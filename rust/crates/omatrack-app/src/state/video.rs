//! The onboard videos and their sync with the telemetry.
//!
//! [`VideoController`] owns two decks: the primary recording's player (the
//! clock) and, when the reference lap has a video, the reference player
//! (the follower). It binds each deck to its role's loaded lap, and keeps
//! the videos, the shared cursor and the reference pacing on one map.
//!
//! - Once per display frame while the primary plays, the video panel calls
//!   [`VideoController::sync_frame`]: the primary clock's estimate becomes a
//!   lap fraction written to `CursorState` alone (nothing else notifies).
//! - A cursor the controller did not write is an explicit jump (a trace
//!   click, a corner focus): both players seek.
//! - The reference is paced by [`crate::sync::pacing`] against
//!   `Comparison::compare_time_for_primary_fraction` and hard-sought on
//!   pause, jumps, lap changes and drift.
//! - At the lap end, per-lap playback counts 3-2-1 into the next lap;
//!   continuous playback adopts the next lap without seeking.
//! - A video whose identity is not trusted plays but never drives or
//!   follows the cursor ([`crate::sync::identity`]).

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use gpui_kit::{
    App, AppContext as _, Context, Entity, EventEmitter, SharedString, Subscription, Task,
};
use mpv_player::{
    EndReason, FrameSource, MediaStatus, MpvError, PlaybackClock, Player, PlayerEvent,
    PlayerOptions,
};
use omatrack_core::playback::{
    self, LAP_ADVANCE_INTERVAL_MS, PAUSED_ALIGNMENT_INTERVAL_MS, REFERENCE_SYNC_INTERVAL_MS,
    ReferencePlayback, SyncState,
};
use omatrack_core::session::LoadedLap;
use omatrack_library::config::HudPosition;
use omatrack_trace::{CursorState, Selection, ViewportState};

use crate::actions::Role;
use crate::state::AppState;
use crate::state::preferences::Preferences;
use crate::state::session::{LapRef, RoleSlot, RoleState, Session, SessionEvent};

use crate::sync::lap_end::{self, LapAdvance, LapEnd};
use crate::sync::pacing::{PacerCommand, PacerInput, PairMap, REFERENCE_MAX_FPS, ReferencePacer};
use crate::sync::{IdentityStatus, LapTimeline, VideoMap};

/// Seek step of the Left/Right keys, seconds.
pub const SEEK_STEP: f64 = 2.0;
/// The `S` slow-motion rate of the primary clock.
pub const SLOW_MOTION: f64 = playback::SLOW_MOTION_RATE;

/// Video compositions (keys 1-5).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ComposeLayout {
    #[default]
    Split,
    PrimaryWithReferenceInset,
    ReferenceWithPrimaryInset,
    PrimaryOnly,
    ReferenceOnly,
}

impl ComposeLayout {
    pub const ALL: [Self; 5] = [
        Self::Split,
        Self::PrimaryWithReferenceInset,
        Self::ReferenceWithPrimaryInset,
        Self::PrimaryOnly,
        Self::ReferenceOnly,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Self::Split => "Split",
            Self::PrimaryWithReferenceInset => "Primary with reference inset",
            Self::ReferenceWithPrimaryInset => "Reference with primary inset",
            Self::PrimaryOnly => "Primary only",
            Self::ReferenceOnly => "Reference only",
        }
    }

    /// The key that selects it (`1`..`5`).
    pub fn key(self) -> &'static str {
        match self {
            Self::Split => "1",
            Self::PrimaryWithReferenceInset => "2",
            Self::ReferenceWithPrimaryInset => "3",
            Self::PrimaryOnly => "4",
            Self::ReferenceOnly => "5",
        }
    }

    /// What is actually shown: with a single video (no reference video)
    /// every composition shows the primary alone.
    pub fn effective(self, dual: bool) -> Self {
        if dual { self } else { Self::PrimaryOnly }
    }

    /// Whether the composition shows `role`'s video.
    pub fn shows(self, role: Role) -> bool {
        !matches!(
            (self, role),
            (Self::PrimaryOnly, Role::Reference) | (Self::ReferenceOnly, Role::Primary)
        )
    }
}

/// Whether video can play in this process.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VideoAvailability {
    /// Playback is switched off (headless tests).
    Disabled,
    /// No video bound.
    Idle,
    /// The player is starting; the bound file opens once it has.
    Starting,
    Ready,
    /// libmpv could not start or open the file (user-facing message).
    Failed(SharedString),
}

/// What changed in the [`VideoController`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VideoEvent {
    /// A different source is bound (views rebuild their frame source).
    SourceChanged,
    /// The bound file could not be opened or played (user-facing message).
    Failed(SharedString),
    /// An explicit identity check finished (`Verify video identity`).
    IdentityChecked {
        role: Role,
        summary: SharedString,
        trusted: bool,
    },
}

/// What drives one deck: a libmpv player, or an external clock (a player
/// owned elsewhere, or a test's mock clock).
enum Transport {
    Player(Player),
    External(PlaybackClock),
}

impl Transport {
    fn clock(&self) -> PlaybackClock {
        match self {
            Self::Player(player) => player.clock(),
            Self::External(clock) => clock.clone(),
        }
    }

    fn player(&self) -> Option<&Player> {
        match self {
            Self::Player(player) => Some(player),
            Self::External(_) => None,
        }
    }

    fn is_paused(&self) -> bool {
        match self {
            Self::Player(player) => player.state().paused,
            Self::External(clock) => clock.sample().paused,
        }
    }

    fn set_paused(&self, paused: bool) {
        match self {
            Self::Player(player) if paused => player.pause(),
            Self::Player(player) => player.play(),
            Self::External(clock) => clock.set_paused(paused, Instant::now()),
        }
    }

    fn seek_exact(&self, seconds: f64) {
        match self {
            Self::Player(player) => player.seek_exact(seconds),
            Self::External(clock) => {
                // An external clock lands at once.
                let now = Instant::now();
                clock.seek_started(seconds, now);
                clock.seek_finished(now);
            }
        }
    }

    /// Where the playhead is or is about to be.
    fn target_position(&self) -> f64 {
        match self {
            Self::Player(player) => player.target_position(),
            Self::External(clock) => clock.estimate(Instant::now()),
        }
    }

    fn set_speed(&self, speed: f64) {
        match self {
            Self::Player(player) => player.set_speed(speed),
            Self::External(clock) => clock.set_speed(speed, Instant::now()),
        }
    }
}

/// One player and the lap it shows.
struct Deck {
    /// Identifies this deck's player events across role swaps.
    token: u64,
    transport: Option<Transport>,
    /// libmpv starting on the background executor.
    starting: Option<Task<()>>,
    /// The file (and start, s) to open once the player has started.
    pending_open: Option<(PathBuf, f64)>,
    bound: Option<PathBuf>,
    /// The recording whose video is bound (the file itself for an AiM
    /// MP4, else the recording beside its video).
    recording: Option<PathBuf>,
    /// The lap shown.
    lap: Option<LoadedLap>,
    timeline: Option<LapTimeline>,
    identity: IdentityStatus,
    availability: VideoAvailability,
    /// For an external transport: its clock's relation to telemetry.
    external_map: Option<VideoMap>,
    events: Option<Task<()>>,
    identity_check: Option<Task<()>>,
}

impl Deck {
    fn new(token: u64, enabled: bool) -> Self {
        Self {
            token,
            transport: None,
            starting: None,
            pending_open: None,
            bound: None,
            recording: None,
            lap: None,
            timeline: None,
            identity: IdentityStatus::None,
            availability: if enabled {
                VideoAvailability::Idle
            } else {
                VideoAvailability::Disabled
            },
            external_map: None,
            events: None,
            identity_check: None,
        }
    }

    /// The file bound, or waiting for the player to start.
    fn target(&self) -> Option<&PathBuf> {
        self.bound
            .as_ref()
            .or(self.pending_open.as_ref().map(|(path, _)| path))
    }

    fn is_external(&self) -> bool {
        matches!(self.transport, Some(Transport::External(_)))
    }

    /// The transport of a bound deck (an external clock is always bound).
    fn active(&self) -> Option<&Transport> {
        self.transport
            .as_ref()
            .filter(|transport| matches!(transport, Transport::External(_)) || self.bound.is_some())
    }

    /// Whether the deck's clock drives (or follows) the cursor.
    fn is_synced(&self) -> bool {
        self.identity.is_trusted() && self.timeline.is_some() && self.active().is_some()
    }

    fn frame_source(&self) -> Option<Arc<dyn FrameSource>> {
        self.transport
            .as_ref()
            .and_then(Transport::player)
            .filter(|_| self.bound.is_some())
            .map(Player::frame_source)
    }

    fn holds_lap(&self, lap: &LoadedLap) -> bool {
        self.lap.as_ref().is_some_and(|held| {
            held.lap_id() == lap.lap_id() && held.recording().path() == lap.recording().path()
        })
    }
}

/// Owns both players and the telemetry sync. See the module docs.
pub struct VideoController {
    preferences: Entity<Preferences>,
    session: Entity<Session>,
    cursor: Option<Entity<CursorState>>,
    viewport: Option<Entity<ViewportState>>,
    enabled: bool,
    audio_output: Option<String>,
    primary: Deck,
    reference: Deck,
    slow_motion: bool,
    layout: ComposeLayout,
    pair: Option<PairMap>,
    pacer: Option<ReferencePacer>,
    last_pace: Option<Instant>,
    paused_verify: Option<Task<()>>,
    advance: LapAdvance,
    advance_timer: Option<Task<()>>,
    /// Continuous playback is adopting this lap: bind it without seeking.
    adopting: Option<i32>,
    prefetch: Option<(i32, Task<()>)>,
    prefetched: Option<LapTimeline>,
    /// The cursor fraction this controller wrote last; any other value is
    /// an explicit jump.
    written: Option<f64>,
    seen_focus: Option<Selection>,
    /// The fraction of the previous frame (lap-end edge detection).
    last_fraction: Option<f64>,
    _subscriptions: Vec<Subscription>,
    _cursor: Option<Subscription>,
}

impl EventEmitter<VideoEvent> for VideoController {}

impl VideoController {
    pub fn new(
        preferences: Entity<Preferences>,
        session: &Entity<Session>,
        enabled: bool,
        audio_output: Option<String>,
        cx: &mut Context<Self>,
    ) -> Self {
        let subscriptions = vec![cx.subscribe(session, |this, _, event, cx| {
            this.on_session_event(event, cx);
        })];
        // The shared cursor and viewport are created after this entity; the
        // application state is installed by the time deferred work runs.
        let this = cx.weak_entity();
        cx.defer(move |cx| {
            let Some(state) = AppState::try_global(cx) else {
                return;
            };
            let (cursor, viewport) = (state.cursor.clone(), state.viewport.clone());
            let _ = this.update(cx, |this, cx| {
                if this.cursor.is_none() {
                    this.connect(cursor, viewport, cx);
                }
            });
        });
        Self {
            preferences,
            session: session.clone(),
            cursor: None,
            viewport: None,
            enabled,
            audio_output,
            primary: Deck::new(1, enabled),
            reference: Deck::new(2, enabled),
            slow_motion: false,
            layout: ComposeLayout::default(),
            pair: None,
            pacer: None,
            last_pace: None,
            paused_verify: None,
            advance: LapAdvance::Idle,
            advance_timer: None,
            adopting: None,
            prefetch: None,
            prefetched: None,
            written: None,
            seen_focus: None,
            last_fraction: None,
            _subscriptions: subscriptions,
            _cursor: None,
        }
    }

    /// Follow `cursor` (explicit jumps seek the videos) and write it from
    /// the primary clock; `viewport` follows the playhead in continuous
    /// playback. Done automatically from the installed [`AppState`].
    pub fn connect(
        &mut self,
        cursor: Entity<CursorState>,
        viewport: Entity<ViewportState>,
        cx: &mut Context<Self>,
    ) {
        self.written = cursor.read(cx).fraction();
        self.seen_focus = cursor.read(cx).focus();
        self._cursor = Some(cx.observe(&cursor, |this, _, cx| this.on_cursor_changed(cx)));
        self.cursor = Some(cursor);
        self.viewport = Some(viewport);
    }

    fn deck(&self, role: Role) -> &Deck {
        match role {
            Role::Primary => &self.primary,
            Role::Reference => &self.reference,
        }
    }

    fn deck_mut(&mut self, role: Role) -> &mut Deck {
        match role {
            Role::Primary => &mut self.primary,
            Role::Reference => &mut self.reference,
        }
    }

    fn role_of(&self, token: u64) -> Option<Role> {
        if self.primary.token == token {
            Some(Role::Primary)
        } else if self.reference.token == token {
            Some(Role::Reference)
        } else {
            None
        }
    }

    // ── readers ─────────────────────────────────────────────────────

    /// The primary video's availability.
    pub fn availability(&self) -> &VideoAvailability {
        &self.primary.availability
    }

    pub fn reference_availability(&self) -> &VideoAvailability {
        &self.reference.availability
    }

    /// The video file bound to the primary lap.
    pub fn bound_path(&self) -> Option<&PathBuf> {
        self.primary.bound.as_ref()
    }

    /// The video file bound to the reference lap.
    pub fn reference_bound_path(&self) -> Option<&PathBuf> {
        self.reference.bound.as_ref()
    }

    /// Why the primary video's timing cannot be trusted, when it cannot.
    pub fn identity_warning(&self) -> Option<&SharedString> {
        self.primary.identity.warning()
    }

    /// What is known about `role`'s video identity.
    pub fn identity(&self, role: Role) -> &IdentityStatus {
        &self.deck(role).identity
    }

    /// The frames of the primary player, for a `VideoView`.
    pub fn frame_source(&self) -> Option<Arc<dyn FrameSource>> {
        self.primary.frame_source()
    }

    /// The frames of the reference player.
    pub fn reference_frame_source(&self) -> Option<Arc<dyn FrameSource>> {
        self.reference.frame_source()
    }

    /// The lap `role`'s video shows.
    pub fn lap(&self, role: Role) -> Option<&LoadedLap> {
        self.deck(role).lap.as_ref()
    }

    /// `role`'s time map (the adopted lap's during continuous playback).
    pub fn timeline(&self, role: Role) -> Option<&LapTimeline> {
        self.deck(role).timeline.as_ref()
    }

    /// The shared map of the pair, when both videos are bound and the
    /// analysis of exactly this pair is ready.
    pub fn pair(&self) -> Option<&PairMap> {
        self.pair.as_ref()
    }

    /// `role`'s clock, when a transport is bound.
    pub fn clock(&self, role: Role) -> Option<PlaybackClock> {
        self.deck(role).active().map(Transport::clock)
    }

    /// Reference presentation time (s) the pair maps `fraction` to.
    pub fn reference_target(&self, fraction: f64) -> Option<f64> {
        self.pair.as_ref()?.reference_target(fraction)
    }

    /// Whether the primary clock drives the cursor.
    pub fn is_synced(&self) -> bool {
        self.primary.is_synced()
    }

    /// Whether the reference follows the primary.
    pub fn is_reference_synced(&self) -> bool {
        self.reference.is_synced() && self.pair.is_some()
    }

    /// Both roles have a video to show.
    pub fn is_dual(&self) -> bool {
        self.primary.active().is_some() && self.reference.active().is_some()
    }

    /// The reference sync indicator.
    pub fn sync_state(&self) -> SyncState {
        if !self.reference.is_synced() {
            return SyncState::Wait;
        }
        if self.pair.is_none() {
            return SyncState::NoMap;
        }
        self.pacer
            .as_ref()
            .map_or(SyncState::Wait, ReferencePacer::state)
    }

    /// The 3-2-1 number while counting into the next lap.
    pub fn countdown(&self) -> Option<u8> {
        self.advance.countdown()
    }

    /// The lap a countdown or resume is heading to.
    pub fn advancing_to(&self) -> Option<i32> {
        self.advance.next_lap()
    }

    pub fn is_muted(&self, cx: &App) -> bool {
        self.preferences.read(cx).config().video.is_muted()
    }

    /// The primary clock is running (or about to).
    pub fn is_playing(&self) -> bool {
        self.primary
            .active()
            .is_some_and(|transport| !transport.is_paused())
    }

    pub fn is_slow_motion(&self) -> bool {
        self.slow_motion
    }

    /// The primary clock rate: 1x, or 0.25x in slow motion.
    pub fn clock_rate(&self) -> f64 {
        if self.slow_motion { SLOW_MOTION } else { 1.0 }
    }

    pub fn layout(&self) -> ComposeLayout {
        self.layout
    }

    pub fn is_continuous(&self, cx: &App) -> bool {
        self.preferences
            .read(cx)
            .config()
            .video
            .is_continuous_playback()
    }

    /// How the reference is paced (`video.reference_playback`).
    pub fn reference_playback(&self, cx: &App) -> ReferencePlayback {
        self.preferences
            .read(cx)
            .config()
            .video
            .reference_playback()
    }

    /// The saved HUD placement (`video.hud_position`), normalized.
    pub fn hud_position(&self, cx: &App) -> Option<(f64, f64)> {
        self.preferences
            .read(cx)
            .config()
            .video
            .hud_position()
            .map(|position| (position.x, position.y))
    }

    // ── session ─────────────────────────────────────────────────────

    fn on_session_event(&mut self, event: &SessionEvent, cx: &mut Context<Self>) {
        match event {
            SessionEvent::AnalysisReady => {
                self.rebuild_pair(cx);
                self.force_reference_sync(cx);
                cx.notify();
            }
            SessionEvent::LoadFailed {
                role: Role::Primary,
                ..
            } => {
                self.cancel_advance(cx);
                self.adopting = None;
                self.reconcile(cx);
            }
            _ => self.reconcile(cx),
        }
    }

    /// Bring both decks in line with the session's roles. A swap exchanges
    /// the players instead of reopening both files.
    fn reconcile(&mut self, cx: &mut Context<Self>) {
        let (primary, reference) = {
            let session = self.session.read(cx);
            (session.primary().cloned(), session.reference().cloned())
        };
        let recording =
            |slot: &Option<RoleSlot>| slot.as_ref().map(|slot| slot.info().path.clone());
        let (want_primary, want_reference) = (recording(&primary), recording(&reference));
        let swapped = want_primary.is_some()
            && want_primary != want_reference
            && want_primary == self.reference.recording
            && want_reference == self.primary.recording;
        if swapped {
            self.swap_decks(cx);
        }
        self.follow(Role::Primary, primary.as_ref(), cx);
        self.follow(Role::Reference, reference.as_ref(), cx);
        self.rebuild_pair(cx);
        // The cursor stays where it was across a swap; the new primary video
        // was on the old pair's mapped station, so bring it onto the cursor.
        if swapped && let Some(fraction) = self.written {
            self.jump_to(fraction, cx);
        }
    }

    /// Follow one role. While another lap of the recording already bound is
    /// loading, the video stays bound and keeps its frame; the finished load
    /// re-parks it. A lap of another recording unbinds it until it loads.
    fn follow(&mut self, role: Role, slot: Option<&RoleSlot>, cx: &mut Context<Self>) {
        let deck = self.deck(role);
        match slot.map(|slot| (slot, slot.state())) {
            Some((_, RoleState::Loaded(lap))) => self.bind_role(role, Some(lap), cx),
            Some((slot, RoleState::Loading))
                if (deck.target().is_some() || deck.is_external())
                    && deck.recording.as_deref() == Some(slot.info().path.as_path()) => {}
            _ => self.bind_role(role, None, cx),
        }
    }

    fn swap_decks(&mut self, cx: &mut Context<Self>) {
        std::mem::swap(&mut self.primary, &mut self.reference);
        let muted = self.is_muted(cx);
        let rate = self.clock_rate();
        if let Some(player) = self.primary.transport.as_ref().and_then(Transport::player) {
            player.set_mute(muted);
            player.set_max_fps(None);
            player.set_speed(rate);
        }
        if let Some(player) = self
            .reference
            .transport
            .as_ref()
            .and_then(Transport::player)
        {
            player.set_mute(true);
            player.set_max_fps(Some(REFERENCE_MAX_FPS));
        }
        self.pair = None;
        self.rebuild_pacer(cx);
        cx.emit(VideoEvent::SourceChanged);
    }

    /// Rebuild the pair map from the analysis of exactly the bound laps.
    fn rebuild_pair(&mut self, cx: &mut Context<Self>) {
        let pair = {
            let session = self.session.read(cx);
            let analysis = session.analysis();
            match (
                analysis,
                self.primary.timeline.as_ref(),
                self.reference.timeline.as_ref(),
            ) {
                (Some(analysis), Some(primary), Some(reference))
                    if self.primary.holds_lap(analysis.primary())
                        && primary.lap_id() == analysis.primary().lap_id()
                        && analysis
                            .reference()
                            .is_some_and(|lap| self.reference.holds_lap(lap)) =>
                {
                    analysis.comparison().map(|comparison| {
                        PairMap::new(
                            primary.clone(),
                            reference.clone(),
                            comparison.clone(),
                            analysis
                                .corners()
                                .iter()
                                .map(|zone| (zone.start, zone.end))
                                .collect(),
                        )
                    })
                }
                _ => None,
            }
        };
        self.pair = pair;
    }

    fn rebuild_pacer(&mut self, cx: &mut Context<Self>) {
        let mode = self.reference_playback(cx);
        self.pacer = self
            .reference
            .active()
            .map(|transport| ReferencePacer::new(transport.clock(), mode));
        self.last_pace = None;
    }

    /// Bind the primary lap's video (or nothing).
    pub fn bind(&mut self, lap: Option<&LoadedLap>, cx: &mut Context<Self>) {
        self.bind_role(Role::Primary, lap, cx);
    }

    /// Bind `role` to `lap`'s video and park it at the cursor's station (a
    /// lap being adopted by continuous playback is not sought). Binding the
    /// lap already bound does nothing; another lap of the file already
    /// bound only seeks: the file is never reopened.
    fn bind_role(&mut self, role: Role, lap: Option<&LoadedLap>, cx: &mut Context<Self>) {
        let Some(lap) = lap else {
            self.unbind(role, cx);
            return;
        };
        if self.deck(role).holds_lap(lap) {
            return;
        }
        let external = self.deck(role).external_map.clone();
        let binding = lap.video().cloned();
        let map = match (&external, &binding) {
            (Some(map), _) => map.clone(),
            (None, Some(binding)) => VideoMap::from_binding(binding),
            (None, None) => {
                // The lap is known but has no video.
                self.unbind(role, cx);
                self.deck_mut(role).lap = Some(lap.clone());
                self.deck_mut(role).recording = Some(PathBuf::from(lap.recording().path()));
                return;
            }
        };
        let adopting = role == Role::Primary && self.adopting == Some(lap.lap_id());
        let same_video = {
            let deck = self.deck(role);
            deck.is_external()
                || binding
                    .as_ref()
                    .is_some_and(|binding| deck.target() == Some(&binding.path))
        };
        {
            let deck = self.deck_mut(role);
            deck.timeline = Some(LapTimeline::new(lap, map));
            deck.lap = Some(lap.clone());
            deck.recording = Some(PathBuf::from(lap.recording().path()));
            let keep_identity =
                same_video && !matches!(deck.identity, IdentityStatus::None) && !deck.is_external();
            if external.is_some() {
                deck.identity = IdentityStatus::External;
            } else if !keep_identity && let Some(binding) = &binding {
                // A check of the previous video no longer applies.
                deck.identity_check = None;
                deck.identity = IdentityStatus::of_binding(binding);
            }
        }
        if self.deck(role).identity.is_checking() && self.deck(role).identity_check.is_none() {
            self.check_identity(role, false, cx);
        }
        if adopting {
            self.adopting = None;
        }
        let start = self.station(role);
        match (same_video, binding) {
            (true, _) => {
                if !adopting {
                    let deck = self.deck_mut(role);
                    match (deck.transport.as_ref(), deck.pending_open.as_mut()) {
                        (_, Some((_, pending))) => *pending = start,
                        (Some(transport), None) => transport.seek_exact(start),
                        (None, None) => {}
                    }
                }
                cx.notify();
            }
            (false, Some(binding)) => self.open_file_as(role, binding.path, start, cx),
            (false, None) => {}
        }
        if role == Role::Primary {
            self.last_fraction = self.written;
            if self.advance.resume_into(lap.lap_id()) {
                self.set_playing(true, cx);
            }
        }
        if role == Role::Reference {
            self.rebuild_pacer(cx);
        }
    }

    /// Where `role`'s video should be parked for the current cursor.
    fn station(&self, role: Role) -> f64 {
        let cursor = self.written.unwrap_or(0.0);
        let timeline = self.deck(role).timeline.as_ref();
        let by_fraction = timeline
            .and_then(|timeline| timeline.video_at_fraction(cursor))
            .unwrap_or(0.0);
        match role {
            Role::Primary => by_fraction,
            // Until the pair's analysis lands, the lap fraction is the best
            // station; the analysis then hard-syncs the reference.
            Role::Reference => self.reference_target(cursor).unwrap_or(by_fraction),
        }
    }

    fn unbind(&mut self, role: Role, cx: &mut Context<Self>) {
        let enabled = self.enabled;
        let deck = self.deck_mut(role);
        let had = deck.target().is_some() || deck.lap.is_some();
        let availability = deck.availability.clone();
        deck.bound = None;
        deck.pending_open = None;
        deck.recording = None;
        deck.lap = None;
        deck.timeline = None;
        deck.identity_check = None;
        if !deck.is_external() {
            deck.identity = IdentityStatus::None;
            if let Some(transport) = deck.transport.as_ref() {
                transport.set_paused(true);
            }
            if enabled {
                deck.availability = VideoAvailability::Idle;
            }
        }
        let changed = had || deck.availability != availability;
        if role == Role::Primary {
            self.cancel_advance(cx);
            self.prefetched = None;
            self.prefetch = None;
        }
        if had {
            self.pair = None;
            cx.emit(VideoEvent::SourceChanged);
        }
        if changed {
            cx.notify();
        }
    }

    // ── players ─────────────────────────────────────────────────────

    /// Open a video file on the primary deck, parked paused at
    /// `start_seconds` (any previous file is replaced). The first file waits
    /// for the player to start ([`VideoAvailability::Starting`]). A file
    /// libmpv cannot play turns [`Self::availability`] into `Failed` and
    /// emits [`VideoEvent::Failed`] once libmpv reports it.
    pub fn open_file(&mut self, path: PathBuf, start_seconds: f64, cx: &mut Context<Self>) {
        self.open_file_as(Role::Primary, path, start_seconds, cx);
    }

    fn open_file_as(&mut self, role: Role, path: PathBuf, start: f64, cx: &mut Context<Self>) {
        let enabled = self.enabled;
        let deck = self.deck_mut(role);
        deck.bound = None;
        deck.pending_open = None;
        if deck.is_external() {
            // The external clock's owner shows the picture.
            deck.bound = Some(path);
            if let Some(transport) = &deck.transport {
                transport.seek_exact(start);
            }
            cx.emit(VideoEvent::SourceChanged);
            cx.notify();
            return;
        }
        if !enabled {
            cx.emit(VideoEvent::SourceChanged);
            cx.notify();
            return;
        }
        if deck.transport.is_none() {
            deck.pending_open = Some((path, start));
            deck.availability = VideoAvailability::Starting;
            self.start_player(role, cx);
            cx.emit(VideoEvent::SourceChanged);
            cx.notify();
            return;
        }
        self.load(role, path, start, cx);
    }

    /// Start libmpv on the background executor (`mpv_initialize` and the
    /// player's threads are too slow for the UI thread); a start already in
    /// flight is reused. [`Self::player_started`] opens the waiting file.
    fn start_player(&mut self, role: Role, cx: &mut Context<Self>) {
        if self.deck(role).starting.is_some() {
            return;
        }
        let mut options = match role {
            Role::Primary => PlayerOptions::default().muted(self.is_muted(cx)),
            // The reference never plays sound and caps its frame rate.
            Role::Reference => PlayerOptions::default()
                .muted(true)
                .max_fps(Some(REFERENCE_MAX_FPS)),
        }
        .client_name("omatrack");
        if let Some(output) = &self.audio_output {
            options = options.audio_output(output.clone());
        }
        let token = self.deck(role).token;
        let created = cx.background_spawn(async move { Player::new(options) });
        self.deck_mut(role).starting = Some(cx.spawn(async move |this, cx| {
            let result = created.await;
            let _ = this.update(cx, |this, cx| this.player_started(token, result, cx));
        }));
    }

    fn player_started(
        &mut self,
        token: u64,
        result: Result<Player, MpvError>,
        cx: &mut Context<Self>,
    ) {
        let Some(role) = self.role_of(token) else {
            return;
        };
        self.deck_mut(role).starting = None;
        let player = match result {
            Ok(player) => player,
            Err(error) => {
                let message: SharedString =
                    format!("Video playback is unavailable. {error}").into();
                let deck = self.deck_mut(role);
                deck.pending_open = None;
                deck.availability = VideoAvailability::Failed(message.clone());
                cx.emit(VideoEvent::SourceChanged);
                cx.emit(VideoEvent::Failed(message));
                cx.notify();
                return;
            }
        };
        // `load` only queues the file; opening it can still fail, and only
        // the event stream says so.
        let events = player.events();
        let task = cx.spawn(async move |this, cx| {
            while let Ok(event) = events.recv().await {
                if this
                    .update(cx, |this, cx| this.on_player_event(token, event, cx))
                    .is_err()
                {
                    break;
                }
            }
        });
        // Mute, role or slow motion may have changed while it started.
        match role {
            Role::Primary => {
                player.set_mute(self.is_muted(cx));
                player.set_speed(self.clock_rate());
            }
            Role::Reference => {
                player.set_mute(true);
                player.set_max_fps(Some(REFERENCE_MAX_FPS));
            }
        }
        let deck = self.deck_mut(role);
        deck.events = Some(task);
        deck.transport = Some(Transport::Player(player));
        match deck.pending_open.take() {
            Some((path, start)) => self.load(role, path, start, cx),
            None => {
                deck.availability = VideoAvailability::Idle;
                cx.notify();
            }
        }
        if role == Role::Reference {
            self.rebuild_pacer(cx);
        }
    }

    /// Queue `path` on the started player, parked paused at `start`.
    fn load(&mut self, role: Role, path: PathBuf, start: f64, cx: &mut Context<Self>) {
        let deck = self.deck_mut(role);
        let Some(player) = deck.transport.as_ref().and_then(Transport::player) else {
            return;
        };
        match player.load(&path) {
            Ok(()) => {
                player.pause();
                player.seek_exact(start);
                deck.bound = Some(path);
                deck.availability = VideoAvailability::Ready;
            }
            Err(error) => {
                let message: SharedString = format!("Couldn’t open the video. {error}").into();
                deck.availability = VideoAvailability::Failed(message.clone());
                cx.emit(VideoEvent::Failed(message));
            }
        }
        cx.emit(VideoEvent::SourceChanged);
        cx.notify();
    }

    fn on_player_event(&mut self, token: u64, event: PlayerEvent, cx: &mut Context<Self>) {
        let Some(role) = self.role_of(token) else {
            return;
        };
        match event {
            PlayerEvent::EndFile {
                reason: EndReason::Error,
                ..
            } => {
                // The player state is authoritative: an error queued for a
                // file that was since replaced finds the new file loading.
                let deck = self.deck(role);
                let status = deck
                    .transport
                    .as_ref()
                    .and_then(Transport::player)
                    .map(|player| player.state().status);
                if let (Some(_), Some(MediaStatus::Failed(message))) = (&deck.bound, status) {
                    self.fail(role, message.into(), cx);
                }
            }
            // The reference opened: put it on the primary's station.
            PlayerEvent::FileLoaded { .. } if role == Role::Reference => {
                self.mirror_pause(cx);
                self.force_reference_sync(cx);
            }
            PlayerEvent::EndOfFile(true) if role == Role::Primary => {
                if let Some(reference) = self.reference.active() {
                    reference.set_paused(true);
                }
                cx.notify();
            }
            // Play state changes restart or stop the display-frame pull.
            PlayerEvent::Paused(_) if role == Role::Primary => cx.notify(),
            _ => {}
        }
    }

    /// The bound file failed: drop it and say why. The lap stays held, so
    /// later session changes do not reopen the same failing file; selecting
    /// another lap tries again.
    fn fail(&mut self, role: Role, message: SharedString, cx: &mut Context<Self>) {
        let deck = self.deck_mut(role);
        if let Some(transport) = deck.transport.as_ref() {
            transport.set_paused(true);
        }
        deck.bound = None;
        deck.timeline = None;
        deck.pending_open = None;
        deck.availability = VideoAvailability::Failed(message.clone());
        self.pair = None;
        cx.emit(VideoEvent::SourceChanged);
        cx.emit(VideoEvent::Failed(message));
        cx.notify();
    }

    /// Drive the primary deck from an external clock instead of libmpv: a
    /// player the host owns elsewhere, or a test's mock clock. `map` relates
    /// the clock's media time to the recording's telemetry time; the
    /// clock's owner vouches for its identity. The current lap is re-bound.
    pub fn attach_external_clock(
        &mut self,
        clock: PlaybackClock,
        map: VideoMap,
        cx: &mut Context<Self>,
    ) {
        let lap = self.primary.lap.take();
        let deck = &mut self.primary;
        deck.starting = None;
        deck.events = None;
        deck.pending_open = None;
        deck.bound = None;
        deck.transport = Some(Transport::External(clock));
        deck.external_map = Some(map);
        deck.identity = IdentityStatus::External;
        deck.availability = VideoAvailability::Ready;
        deck.timeline = None;
        if let Some(lap) = lap {
            self.bind_role(Role::Primary, Some(&lap), cx);
        }
        cx.emit(VideoEvent::SourceChanged);
        cx.notify();
    }

    // ── identity ────────────────────────────────────────────────────

    /// Check the identity of every bound video again on a worker (BLAKE3 of
    /// a companion file) and report each result through
    /// [`VideoEvent::IdentityChecked`].
    pub fn verify_identity(&mut self, cx: &mut Context<Self>) {
        let mut any = false;
        for role in [Role::Primary, Role::Reference] {
            let deck = self.deck(role);
            if deck.lap.is_none() {
                continue;
            }
            any = true;
            if deck.is_external() {
                cx.emit(VideoEvent::IdentityChecked {
                    role,
                    summary: IdentityStatus::External.summary(),
                    trusted: true,
                });
            } else if deck.lap.as_ref().is_some_and(|lap| lap.video().is_some()) {
                self.check_identity(role, true, cx);
            }
        }
        if !any {
            cx.emit(VideoEvent::IdentityChecked {
                role: Role::Primary,
                summary: IdentityStatus::None.summary(),
                trusted: false,
            });
        }
        cx.notify();
    }

    fn check_identity(&mut self, role: Role, explicit: bool, cx: &mut Context<Self>) {
        let deck = self.deck(role);
        let (Some(lap), token) = (deck.lap.as_ref(), deck.token) else {
            return;
        };
        let Some(path) = lap.video().map(|binding| binding.path.clone()) else {
            return;
        };
        let recording = lap.recording().clone();
        let lap_id = lap.lap_id();
        let checked_path = path.clone();
        let work =
            cx.background_spawn(async move { crate::sync::identity::check(&recording, &path) });
        let deck = self.deck_mut(role);
        if !explicit {
            deck.identity = IdentityStatus::Checking;
        }
        deck.identity_check = Some(cx.spawn(async move |this, cx| {
            let status = work.await;
            let _ = this.update(cx, |this, cx| {
                this.identity_checked(token, lap_id, &checked_path, status, explicit, cx)
            });
        }));
    }

    fn identity_checked(
        &mut self,
        token: u64,
        lap_id: i32,
        path: &Path,
        status: IdentityStatus,
        explicit: bool,
        cx: &mut Context<Self>,
    ) {
        let Some(role) = self.role_of(token) else {
            return;
        };
        let deck = self.deck_mut(role);
        let current = deck.lap.as_ref().is_some_and(|lap| {
            lap.lap_id() == lap_id && lap.video().is_some_and(|binding| binding.path == path)
        });
        if !current {
            return;
        }
        deck.identity_check = None;
        let was_trusted = deck.identity.is_trusted();
        deck.identity = status.clone();
        if explicit {
            cx.emit(VideoEvent::IdentityChecked {
                role,
                summary: status.summary(),
                trusted: status.is_trusted(),
            });
        }
        // Newly trusted: bring the video onto the cursor.
        if status.is_trusted() && !was_trusted {
            match role {
                Role::Primary => {
                    if let Some(fraction) = self.written {
                        self.jump_to(fraction, cx);
                    }
                }
                Role::Reference => self.force_reference_sync(cx),
            }
        }
        cx.notify();
    }

    // ── the cursor ──────────────────────────────────────────────────

    fn write_cursor(&mut self, fraction: f64, cx: &mut Context<Self>) {
        let Some(cursor) = self.cursor.clone() else {
            return;
        };
        let fraction = fraction.clamp(0.0, 1.0);
        self.written = Some(fraction);
        cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(fraction), cx));
    }

    fn cursor_fraction(&self, cx: &App) -> Option<f64> {
        self.cursor
            .as_ref()
            .and_then(|cursor| cursor.read(cx).fraction())
    }

    /// The shared cursor changed. A value this controller did not write is
    /// an explicit jump; a newly focused corner is a jump to its start.
    fn on_cursor_changed(&mut self, cx: &mut Context<Self>) {
        let Some(cursor) = self.cursor.as_ref() else {
            return;
        };
        let (fraction, focus) = {
            let cursor = cursor.read(cx);
            (cursor.fraction(), cursor.focus())
        };
        if focus != self.seen_focus {
            self.seen_focus = focus;
            if let Some(zone) = focus
                && self.is_synced()
            {
                self.write_cursor(zone.start, cx);
                self.jump_to(zone.start, cx);
                return;
            }
        }
        if fraction != self.written {
            self.written = fraction;
            if let Some(fraction) = fraction {
                self.jump_to(fraction, cx);
            }
        }
    }

    /// An explicit cursor jump (trace click or scrub, corner focus): seek
    /// both players. The playing primary is sought only beyond
    /// `PLAYING_SEEK_ERROR`; it is the clock.
    pub fn seek_to_fraction(&mut self, fraction: f64, cx: &mut Context<Self>) {
        if !fraction.is_finite() {
            return;
        }
        let fraction = fraction.clamp(0.0, 1.0);
        if self.cursor_fraction(cx) != Some(fraction) && self.cursor.is_some() {
            self.write_cursor(fraction, cx);
        }
        self.jump_to(fraction, cx);
    }

    fn jump_to(&mut self, fraction: f64, cx: &mut Context<Self>) {
        if !self.primary.is_synced() {
            return;
        }
        if self.advance.is_counting() {
            self.cancel_advance(cx);
        }
        let (Some(transport), Some(timeline)) =
            (self.primary.active(), self.primary.timeline.as_ref())
        else {
            return;
        };
        let Some(target) = timeline.video_at_fraction(fraction) else {
            return;
        };
        let paused = transport.is_paused();
        let position = transport.clock().estimate(Instant::now());
        if playback::primary_needs_seek(position, target, paused) {
            transport.seek_exact(target);
        }
        self.last_fraction = Some(fraction);
        self.force_reference_sync(cx);
    }

    // ── the display-frame pull ──────────────────────────────────────

    /// Once per display frame while the primary plays: map the primary
    /// clock to a lap fraction and write it to the cursor (the only
    /// notification), pace the reference, handle the lap end and, in
    /// continuous playback, keep the playhead at 33% of the viewport.
    pub fn sync_frame(&mut self, now: Instant, cx: &mut Context<Self>) {
        if !self.primary.is_synced() || self.advance.is_counting() {
            return;
        }
        let (Some(transport), Some(timeline)) =
            (self.primary.active(), self.primary.timeline.as_ref())
        else {
            return;
        };
        let sample = transport.clock().sample();
        if sample.paused {
            return;
        }
        let video = sample.estimate(now);
        let Some(position) = timeline.position_at_video(video) else {
            return;
        };
        let fraction = position.fraction;
        self.write_cursor(fraction, cx);
        let previous = self.last_fraction.replace(fraction);
        let continuous = self.is_continuous(cx);
        if continuous {
            if lap_end::should_prefetch(fraction) {
                self.prefetch_next(cx);
            }
            self.follow_playhead(fraction, cx);
        }
        if !sample.seeking && lap_end::reached_lap_end(previous, fraction) {
            self.on_lap_end(video, continuous, cx);
            return;
        }
        self.pace_reference(now, false, cx);
    }

    fn follow_playhead(&mut self, fraction: f64, cx: &mut Context<Self>) {
        let focused = self
            .cursor
            .as_ref()
            .is_some_and(|cursor| cursor.read(cx).focus().is_some());
        let Some(viewport) = self.viewport.as_ref().filter(|_| !focused) else {
            return;
        };
        let next = lap_end::follow_viewport(viewport.read(cx).viewport(), fraction);
        if next != viewport.read(cx).viewport() {
            viewport.update(cx, |viewport, cx| viewport.set_viewport(next, cx));
        }
    }

    // ── the reference ───────────────────────────────────────────────

    fn force_reference_sync(&mut self, cx: &mut Context<Self>) {
        self.last_pace = None;
        self.pace_reference(Instant::now(), true, cx);
    }

    /// One pacing decision, at most every `REFERENCE_SYNC_INTERVAL_MS`
    /// unless forced.
    fn pace_reference(&mut self, now: Instant, force: bool, cx: &mut Context<Self>) {
        if !self.reference.is_synced() || !self.primary.is_synced() {
            return;
        }
        let interval = Duration::from_millis(REFERENCE_SYNC_INTERVAL_MS);
        if !force
            && self
                .last_pace
                .is_some_and(|last| now.saturating_duration_since(last) < interval)
        {
            return;
        }
        let (Some(pair), Some(pacer), Some(primary), Some(reference)) = (
            self.pair.as_ref(),
            self.pacer.as_mut(),
            self.primary.active(),
            self.reference.active(),
        ) else {
            return;
        };
        self.last_pace = Some(now);
        let primary_sample = primary.clock().sample();
        let cursor = self.written.unwrap_or(0.0);
        let input = PacerInput::new(
            cursor,
            primary_sample.estimate(now),
            reference.clock().estimate(now),
        )
        .paused(primary_sample.paused)
        .clock_rate(if self.slow_motion { SLOW_MOTION } else { 1.0 })
        .force(force);
        match pacer.step(pair, input) {
            PacerCommand::None => {}
            PacerCommand::SetSpeed(speed) => reference.set_speed(speed),
            PacerCommand::Seek {
                target,
                rate,
                verify,
            } => {
                reference.set_speed(rate);
                reference.seek_exact(target);
                if verify {
                    self.schedule_paused_verify(cx);
                }
            }
        }
    }

    /// Re-check a paused aligning seek after it had time to land.
    fn schedule_paused_verify(&mut self, cx: &mut Context<Self>) {
        let delay = Duration::from_millis(PAUSED_ALIGNMENT_INTERVAL_MS);
        self.paused_verify = Some(cx.spawn(async move |this, cx| {
            cx.background_executor().timer(delay).await;
            let _ = this.update(cx, |this, cx| this.verify_paused_reference(cx));
        }));
    }

    fn verify_paused_reference(&mut self, cx: &mut Context<Self>) {
        self.paused_verify = None;
        let playing = self.is_playing();
        let cursor = self.written.unwrap_or(0.0);
        let (Some(pair), Some(pacer), Some(reference)) = (
            self.pair.as_ref(),
            self.pacer.as_mut(),
            self.reference.active(),
        ) else {
            return;
        };
        if playing {
            return;
        }
        let sample = reference.clock().sample();
        if sample.seeking {
            self.schedule_paused_verify(cx);
            return;
        }
        if let Some(target) = pacer.verify_paused(pair, cursor, sample.position) {
            reference.seek_exact(target);
            self.schedule_paused_verify(cx);
        }
        cx.notify();
    }

    /// The reference plays exactly when the primary does.
    fn mirror_pause(&mut self, _cx: &mut Context<Self>) {
        if !self.reference.is_synced() {
            return;
        }
        let paused = !self.is_playing();
        if let Some(reference) = self.reference.active()
            && reference.is_paused() != paused
        {
            reference.set_paused(paused);
        }
    }

    // ── lap end ─────────────────────────────────────────────────────

    fn on_lap_end(&mut self, video: f64, continuous: bool, cx: &mut Context<Self>) {
        let next = self
            .primary
            .lap
            .as_ref()
            .and_then(|lap| lap.neighbour_lap(1))
            .map(|lap| lap.id);
        match lap_end::lap_end(continuous, next) {
            LapEnd::Adopt { next_lap } => self.adopt(next_lap, video, cx),
            LapEnd::Countdown { next_lap } => self.start_countdown(next_lap, cx),
            LapEnd::Stop if !continuous => {
                self.set_playing(false, cx);
                cx.notify();
            }
            LapEnd::Stop => {}
        }
    }

    /// Continuous playback: select the next lap and take the cursor from
    /// the current video position. The playing video is never sought.
    fn adopt(&mut self, next_lap: i32, video: f64, cx: &mut Context<Self>) {
        self.adopting = Some(next_lap);
        if let Some(timeline) = self
            .prefetched
            .take()
            .filter(|timeline| timeline.lap_id() == next_lap)
        {
            if let Some(cursor) = lap_end::adopted_cursor(&timeline, video) {
                self.write_cursor(cursor, cx);
                self.last_fraction = Some(cursor);
            }
            self.primary.timeline = Some(timeline);
            self.pair = None;
        }
        self.prefetch = None;
        self.select_primary_lap(next_lap, cx);
    }

    fn select_primary_lap(&mut self, lap: i32, cx: &mut Context<Self>) {
        let Some(session_id) = self
            .session
            .read(cx)
            .primary()
            .map(|slot| slot.lap_ref().session().clone())
        else {
            return;
        };
        self.session.update(cx, |session, cx| {
            session.set_lap(Role::Primary, LapRef::new(session_id, lap), cx)
        });
    }

    /// Past 70% of the lap in continuous playback: build the next lap's
    /// timeline in the background so the hand-over maps the cursor at once.
    fn prefetch_next(&mut self, cx: &mut Context<Self>) {
        let (Some(lap), Some(timeline)) = (self.primary.lap.as_ref(), &self.primary.timeline)
        else {
            return;
        };
        let Some(next) = lap.neighbour_lap(1).cloned() else {
            return;
        };
        let fetched = self
            .prefetched
            .as_ref()
            .is_some_and(|prefetched| prefetched.lap_id() == next.id);
        let fetching = self.prefetch.as_ref().is_some_and(|(id, _)| *id == next.id);
        if fetched || fetching {
            return;
        }
        let recording = lap.recording().clone();
        let overrides = lap.overrides().clone();
        let map = timeline.map().clone();
        let work = cx.background_spawn(async move {
            let unified = recording.unify_lap(next.start_time, next.end_time, &overrides);
            LapTimeline::from_unified(next.id, Arc::new(unified), map)
        });
        let task = cx.spawn(async move |this, cx| {
            let timeline = work.await;
            let _ = this.update(cx, |this, _| {
                this.prefetch = None;
                this.prefetched = Some(timeline);
            });
        });
        self.prefetch = Some((next.id, task));
    }

    /// Per-lap playback: pause both and count 3-2-1 into `next_lap`.
    fn start_countdown(&mut self, next_lap: i32, cx: &mut Context<Self>) {
        self.set_playing(false, cx);
        self.advance = LapAdvance::start(next_lap);
        let interval = Duration::from_millis(LAP_ADVANCE_INTERVAL_MS);
        self.advance_timer = Some(cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor().timer(interval).await;
                let Ok(counting) = this.update(cx, |this, cx| this.tick_countdown(cx)) else {
                    break;
                };
                if !counting {
                    break;
                }
            }
        }));
        cx.notify();
    }

    /// One countdown step; false once the countdown is over.
    fn tick_countdown(&mut self, cx: &mut Context<Self>) -> bool {
        if !self.advance.is_counting() {
            return false;
        }
        let finished = self.advance.tick();
        cx.notify();
        match finished {
            Some(next_lap) => {
                self.advance_timer = None;
                // The next lap starts at its beginning.
                self.write_cursor(0.0, cx);
                self.last_fraction = Some(0.0);
                self.select_primary_lap(next_lap, cx);
                false
            }
            None => true,
        }
    }

    fn cancel_advance(&mut self, cx: &mut Context<Self>) {
        if self.advance != LapAdvance::Idle {
            self.advance.cancel();
            self.advance_timer = None;
            cx.notify();
        }
    }

    // ── commands ────────────────────────────────────────────────────

    /// Play or pause both videos together.
    fn set_playing(&mut self, playing: bool, cx: &mut Context<Self>) {
        let Some(primary) = self.primary.active() else {
            return;
        };
        if playing {
            primary.set_speed(self.clock_rate());
        }
        primary.set_paused(!playing);
        self.mirror_pause(cx);
        // Paused: align the reference on the station (then verify); playing:
        // lock it there and pace from it.
        self.force_reference_sync(cx);
    }

    /// Play/pause. During the lap countdown it cancels the countdown.
    pub fn toggle_play(&mut self, cx: &mut Context<Self>) {
        if self.advance != LapAdvance::Idle {
            self.cancel_advance(cx);
            return;
        }
        let playing = self.is_playing();
        self.set_playing(!playing, cx);
        cx.notify();
    }

    /// Seek the primary by `seconds` (from a seek still in flight, so taps
    /// add up); the cursor and the reference follow.
    pub fn seek_by(&mut self, seconds: f64, cx: &mut Context<Self>) {
        self.cancel_advance(cx);
        let Some(transport) = self.primary.active() else {
            return;
        };
        let target = (transport.target_position() + seconds).max(0.0);
        transport.seek_exact(target);
        let fraction = self
            .primary
            .timeline
            .as_ref()
            .filter(|_| self.primary.is_synced())
            .and_then(|timeline| timeline.fraction_at_video(target));
        if let Some(fraction) = fraction {
            self.write_cursor(fraction, cx);
            self.last_fraction = Some(fraction);
            self.force_reference_sync(cx);
        }
        cx.notify();
    }

    /// Toggle audio and persist `video.muted` (the reference never plays
    /// sound).
    pub fn toggle_mute(&mut self, cx: &mut Context<Self>) {
        let muted = !self.is_muted(cx);
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| config.video.muted = Some(muted));
        });
        if let Some(player) = self.primary.transport.as_ref().and_then(Transport::player) {
            player.set_mute(muted);
        }
        cx.notify();
    }

    /// 0.25x on the primary clock, or back to 1x; the reference follows.
    pub fn toggle_slow_motion(&mut self, cx: &mut Context<Self>) {
        self.slow_motion = !self.slow_motion;
        let rate = self.clock_rate();
        if let Some(transport) = &self.primary.transport {
            transport.set_speed(rate);
        }
        self.last_pace = None;
        self.pace_reference(Instant::now(), false, cx);
        cx.notify();
    }

    /// Per-lap or continuous playback (`video.continuous_playback`).
    /// Switching to continuous during a countdown goes on at once.
    pub fn toggle_continuous(&mut self, cx: &mut Context<Self>) {
        let continuous = !self.is_continuous(cx);
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| {
                config.video.continuous_playback = Some(continuous)
            });
        });
        if continuous && let Some(next_lap) = self.advance.countdown().and(self.advancing_to()) {
            self.advance = LapAdvance::Resuming { next_lap };
            self.advance_timer = None;
            self.write_cursor(0.0, cx);
            self.last_fraction = Some(0.0);
            self.select_primary_lap(next_lap, cx);
        }
        cx.notify();
    }

    /// How the reference is paced; persisted as `video.reference_playback`.
    pub fn set_reference_playback(&mut self, mode: ReferencePlayback, cx: &mut Context<Self>) {
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| {
                config.video.reference_playback = Some(mode.key().to_string())
            });
        });
        if let Some(pacer) = self.pacer.as_mut() {
            pacer.set_mode(mode);
        }
        self.force_reference_sync(cx);
        cx.notify();
    }

    pub fn set_layout(&mut self, layout: ComposeLayout, cx: &mut Context<Self>) {
        if layout != self.layout {
            self.layout = layout;
            cx.notify();
        }
    }

    /// Persist the HUD placement (normalized; written at drag end).
    pub fn set_hud_position(&mut self, x: f64, y: f64, cx: &mut Context<Self>) {
        let position = HudPosition::new(x.clamp(0.0, 1.0), y.clamp(0.0, 1.0));
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| config.video.hud_position = Some(position));
        });
        cx.notify();
    }
}
