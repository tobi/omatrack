//! The comparison being studied: the primary lap, the optional reference
//! lap, and the analysis between them.

use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use gpui_kit::{AppContext as _, Context, Entity, EventEmitter, SharedString, Subscription, Task};
use omatrack_core::alignment::Strategy;
use omatrack_core::corners::CornerZone;
use omatrack_core::format_lap_time;
use omatrack_core::recording::Recording;
use omatrack_core::session::{Analysis, LoadOptions, LoadedLap, StrategyRequest, load_lap};
use omatrack_library::catalog::date_heading;
use omatrack_library::location::OpenMode;

use crate::actions::Role;
use crate::state::jobs::Jobs;
use crate::state::library::{Library, LibraryEvent, RecordingSource};
use crate::state::preferences::Preferences;

/// Identity of a lap in the library: catalog session id plus lap id.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct LapRef {
    session: SharedString,
    lap: i32,
}

impl LapRef {
    pub fn new(session: impl Into<SharedString>, lap: i32) -> Self {
        Self {
            session: session.into(),
            lap,
        }
    }
    pub fn session(&self) -> &SharedString {
        &self.session
    }
    pub fn lap(&self) -> i32 {
        self.lap
    }
    /// The catalog id of the lap row (`<session>/l:<lap>`).
    pub fn row_id(&self) -> SharedString {
        format!("{}/l:{}", self.session, self.lap).into()
    }
}

/// What the interface shows for a selected lap before and after it loads
/// (presentation snapshot taken from the catalog at selection).
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct LapInfo {
    /// `L8`, `Out`, `In`, ...
    pub label: SharedString,
    /// `1:13.644`.
    pub time: SharedString,
    pub driver: Option<SharedString>,
    /// `Session · Driver`, else the file name.
    pub title: SharedString,
    pub track: SharedString,
    pub session_name: Option<SharedString>,
    /// `Wed 2 Sep 2026`.
    pub day: SharedString,
    pub path: std::path::PathBuf,
}

/// Where one role is in its load.
#[derive(Debug, Clone)]
pub enum RoleState {
    Loading,
    Loaded(LoadedLap),
    Failed(SharedString),
}

/// One side of the comparison.
#[derive(Debug, Clone)]
pub struct RoleSlot {
    lap_ref: LapRef,
    info: LapInfo,
    /// The key of the lap's track in `tracks.<key>` (Track Atlas slug, else
    /// the recording's own track name); `None` when the track is unknown.
    track_key: Option<SharedString>,
    state: RoleState,
}

impl RoleSlot {
    pub fn lap_ref(&self) -> &LapRef {
        &self.lap_ref
    }
    pub fn info(&self) -> &LapInfo {
        &self.info
    }
    /// The track identity per-track preferences (corner overrides) are
    /// stored under; `None` when the recording names no track.
    pub fn track_key(&self) -> Option<&SharedString> {
        self.track_key.as_ref()
    }
    pub fn state(&self) -> &RoleState {
        &self.state
    }
    pub fn loaded(&self) -> Option<&LoadedLap> {
        match &self.state {
            RoleState::Loaded(lap) => Some(lap),
            _ => None,
        }
    }
    pub fn is_loading(&self) -> bool {
        matches!(self.state, RoleState::Loading)
    }
}

/// What changed in the [`Session`].
#[derive(Debug, Clone, PartialEq)]
pub enum SessionEvent {
    PrimaryChanged,
    ReferenceChanged,
    /// A new [`Analysis`] is available (or the old one was dropped).
    AnalysisReady,
    /// Roles were exchanged; cursor and viewport stay where they are.
    Swapped,
    LoadFailed {
        role: Role,
        message: SharedString,
    },
}

/// One cancellable background job: the latest request wins.
struct Pending {
    cancel: Arc<AtomicBool>,
    _task: Task<()>,
}

impl Pending {
    fn cancel(self) {
        self.cancel.store(true, Ordering::Relaxed);
    }
}

/// The session entity. Every parse, unify and analysis runs on the
/// background executor with one slot per role plus one for the analysis;
/// a newer request cancels the older one and stale results are dropped.
pub struct Session {
    library: Entity<Library>,
    preferences: Entity<Preferences>,
    jobs: Entity<Jobs>,
    primary: Option<RoleSlot>,
    reference: Option<RoleSlot>,
    primary_load: Option<Pending>,
    reference_load: Option<Pending>,
    analysis: Option<Arc<Analysis>>,
    analysis_load: Option<Pending>,
    /// When the pending analysis is `base.with_manual_offset(..)`: `base`,
    /// current in everything but the offset. A further offset change
    /// derives from it again instead of rebuilding the pair.
    offset_base: Option<Arc<Analysis>>,
    analysis_generation: u64,
    strategy: StrategyRequest,
    manual_offset: f64,
    corner_override: Option<Vec<CornerZone>>,
    restore_pending: bool,
    _subscriptions: Vec<Subscription>,
}

impl EventEmitter<SessionEvent> for Session {}

impl Session {
    pub fn new(
        library: Entity<Library>,
        preferences: Entity<Preferences>,
        jobs: Entity<Jobs>,
        cx: &mut Context<Self>,
    ) -> Self {
        let strategy = preferences
            .read(cx)
            .config()
            .video
            .reference_sync()
            .map_or(StrategyRequest::Auto, StrategyRequest::Prefer);
        let subscriptions = vec![cx.subscribe(&library, |this, _, event, cx| {
            if let LibraryEvent::ScanFinished { .. } = event {
                this.restore_selection(cx);
            }
        })];
        Self {
            library,
            preferences,
            jobs,
            primary: None,
            reference: None,
            primary_load: None,
            reference_load: None,
            analysis: None,
            analysis_load: None,
            offset_base: None,
            analysis_generation: 0,
            strategy,
            manual_offset: 0.0,
            corner_override: None,
            restore_pending: true,
            _subscriptions: subscriptions,
        }
    }

    pub fn primary(&self) -> Option<&RoleSlot> {
        self.primary.as_ref()
    }

    pub fn reference(&self) -> Option<&RoleSlot> {
        self.reference.as_ref()
    }

    pub fn slot(&self, role: Role) -> Option<&RoleSlot> {
        match role {
            Role::Primary => self.primary.as_ref(),
            Role::Reference => self.reference.as_ref(),
        }
    }

    /// The current analysis (primary alone, or the comparison).
    pub fn analysis(&self) -> Option<&Arc<Analysis>> {
        self.analysis.as_ref()
    }

    pub fn is_loading(&self) -> bool {
        self.primary_load.is_some() || self.reference_load.is_some() || self.analysis_load.is_some()
    }

    pub fn strategy(&self) -> StrategyRequest {
        self.strategy
    }

    pub fn manual_offset(&self) -> f64 {
        self.manual_offset
    }

    pub fn set_primary(&mut self, session: SharedString, lap: i32, cx: &mut Context<Self>) {
        self.set_lap(Role::Primary, LapRef::new(session, lap), cx);
    }

    pub fn set_reference(&mut self, session: SharedString, lap: i32, cx: &mut Context<Self>) {
        self.set_lap(Role::Reference, LapRef::new(session, lap), cx);
    }

    /// Load `lap_ref` into `role`. Selecting the lap a role already holds
    /// does nothing; a pending load for the role is cancelled and the
    /// pair-specific manual offset is cleared. Cursor and viewport stay.
    pub fn set_lap(&mut self, role: Role, lap_ref: LapRef, cx: &mut Context<Self>) {
        if self.slot(role).is_some_and(|slot| {
            slot.lap_ref == lap_ref && !matches!(slot.state, RoleState::Failed(_))
        }) {
            return;
        }
        self.restore_pending = false;
        if let Some(pending) = self.take_load(role) {
            pending.cancel();
        }
        // An analysis still running is for the old pair (and would bring
        // its offset back when it lands); the new pair's comes after load.
        self.cancel_analysis();
        // A new pair: the manual damper offset was tuned for the old one.
        self.manual_offset = 0.0;
        let source = self.library.read(cx).source(&lap_ref.session);
        let Some(source) = source else {
            let message: SharedString = "This recording is no longer in the library.".into();
            let info = LapInfo {
                label: format!("L{}", lap_ref.lap).into(),
                time: SharedString::default(),
                driver: None,
                title: lap_ref.session.clone(),
                track: SharedString::default(),
                session_name: None,
                day: SharedString::default(),
                path: Default::default(),
            };
            self.put_slot(
                role,
                RoleSlot {
                    lap_ref,
                    info,
                    track_key: None,
                    state: RoleState::Failed(message.clone()),
                },
            );
            self.emit_changed(role, cx);
            cx.emit(SessionEvent::LoadFailed { role, message });
            cx.notify();
            return;
        };
        let info = lap_info(&source, lap_ref.lap);
        let label = format!("Loading {} · {}", info.label, info.title);
        let metadata = &source.node.metadata;
        let track_key = metadata
            .track_slug()
            .or(metadata.track_name())
            .map(|key| SharedString::from(key.to_string()));
        // The recording this role held, if it was parsed: a load of another
        // lap of it reuses the parsed file instead of reading it again.
        let previous = self
            .slot(role)
            .and_then(RoleSlot::loaded)
            .map(|lap| lap.recording().clone());
        self.put_slot(
            role,
            RoleSlot {
                lap_ref: lap_ref.clone(),
                info,
                track_key,
                state: RoleState::Loading,
            },
        );
        self.emit_changed(role, cx);
        cx.notify();

        // Two roles on one recording, or two laps of it in turn, share the
        // parsed file.
        let shared = [&self.primary, &self.reference]
            .into_iter()
            .flatten()
            .filter_map(RoleSlot::loaded)
            .map(|lap| lap.recording().clone())
            .chain(previous)
            .find(|recording| Path::new(recording.path()) == source.file.path());

        let cancel = Arc::new(AtomicBool::new(false));
        let job = self.jobs.update(cx, |jobs, cx| jobs.start(label, cx));
        let background_cancel = cancel.clone();
        let load = cx.background_spawn(async move {
            let cancel = background_cancel;
            let recording = match shared {
                Some(recording) => recording,
                None => Arc::new(
                    source
                        .location
                        .open(&source.file, OpenMode::Full)
                        .map_err(|error| format!("Couldn’t open the recording. {error}"))?,
                ),
            };
            load_recording_lap(recording, &source, lap_ref.lap, &cancel)
        });
        let jobs = self.jobs.clone();
        let requested = lap_ref;
        let task = cx.spawn(async move |this, cx| {
            let result = load.await;
            jobs.update(cx, |jobs, cx| jobs.finish(job, cx));
            let _ = this.update(cx, |this, cx| this.finish_load(role, requested, result, cx));
        });
        self.put_load(
            role,
            Pending {
                cancel,
                _task: task,
            },
        );
    }

    fn finish_load(
        &mut self,
        role: Role,
        requested: LapRef,
        result: Result<LoadedLap, String>,
        cx: &mut Context<Self>,
    ) {
        if self.slot(role).is_none_or(|slot| slot.lap_ref != requested) {
            return;
        }
        self.take_load(role);
        let slot = match role {
            Role::Primary => self.primary.as_mut(),
            Role::Reference => self.reference.as_mut(),
        };
        let Some(slot) = slot else {
            return;
        };
        match result {
            Ok(lap) => {
                slot.state = RoleState::Loaded(lap);
                self.persist_selection(cx);
                if role == Role::Primary {
                    self.corner_override = self.stored_corner_override(cx);
                }
                self.emit_changed(role, cx);
                self.rebuild_analysis(cx);
            }
            Err(message) if message == CANCELLED => {}
            Err(message) => {
                let message = SharedString::from(message);
                slot.state = RoleState::Failed(message.clone());
                if role == Role::Primary {
                    self.drop_analysis(cx);
                }
                self.emit_changed(role, cx);
                cx.emit(SessionEvent::LoadFailed { role, message });
                // A failed reference still leaves the primary to analyse.
                if role == Role::Reference {
                    self.rebuild_analysis(cx);
                }
            }
        }
        cx.notify();
    }

    /// Drop the reference lap and compare nothing.
    pub fn clear_reference(&mut self, cx: &mut Context<Self>) {
        if let Some(pending) = self.reference_load.take() {
            pending.cancel();
        }
        if self.reference.take().is_some() {
            self.persist_selection(cx);
            cx.emit(SessionEvent::ReferenceChanged);
            self.rebuild_analysis(cx);
            cx.notify();
        }
    }

    /// Exchange the roles. A loaded lap moves to its new role as it is;
    /// pending (or failed) loads are cancelled and restarted in their new
    /// roles. The manual offset inverts. Cursor and viewport are not
    /// touched: the playhead never moves on a swap.
    pub fn swap(&mut self, cx: &mut Context<Self>) {
        let (Some(primary), Some(reference)) = (self.primary.clone(), self.reference.clone())
        else {
            return;
        };
        let both_loaded = primary.loaded().is_some() && reference.loaded().is_some();
        // Only a settled analysis of exactly this pair can be swapped in
        // place; while a rebuild is in flight (new strategy, new lap, new
        // corners) the current one is stale and swapping it would show the
        // old inputs under the new request.
        let settled = self.settled_analysis();
        if !both_loaded {
            for pending in [self.primary_load.take(), self.reference_load.take()]
                .into_iter()
                .flatten()
            {
                pending.cancel();
            }
            self.primary = None;
            self.reference = None;
            self.cancel_analysis();
            let offset = -self.manual_offset;
            let (loaded, unloaded): (Vec<_>, Vec<_>) =
                [(Role::Primary, reference), (Role::Reference, primary)]
                    .into_iter()
                    .partition(|(_, slot)| slot.loaded().is_some());
            // The loaded lap first: the restarted load of the other role
            // reuses its parsed recording when both share one.
            for (role, slot) in loaded {
                self.put_slot(role, slot);
                if role == Role::Primary {
                    self.corner_override = self.stored_corner_override(cx);
                }
                self.emit_changed(role, cx);
            }
            for (role, slot) in unloaded {
                self.set_lap(role, slot.lap_ref, cx);
            }
            // Same pair, other way round: keep the tuning, inverted.
            self.manual_offset = offset;
            cx.emit(SessionEvent::Swapped);
            cx.notify();
            return;
        }
        self.primary = Some(reference);
        self.reference = Some(primary);
        self.manual_offset = -self.manual_offset;
        self.corner_override = self.stored_corner_override(cx);
        self.persist_selection(cx);
        cx.emit(SessionEvent::PrimaryChanged);
        cx.emit(SessionEvent::ReferenceChanged);
        cx.emit(SessionEvent::Swapped);
        match settled {
            Some(analysis) if self.corner_override.is_none() && !analysis.has_corner_override() => {
                self.run_analysis(cx, move |cancel| analysis.swapped(cancel));
            }
            _ => self.rebuild_analysis(cx),
        }
        cx.notify();
    }

    /// Select the neighbouring lap of the primary recording (`offset` -1/+1).
    pub fn step_lap(&mut self, offset: isize, cx: &mut Context<Self>) {
        let Some(slot) = self.primary.as_ref() else {
            return;
        };
        let session = slot.lap_ref.session.clone();
        let next = match slot.loaded() {
            Some(lap) => lap.neighbour_lap(offset).map(|lap| lap.id),
            None => {
                let library = self.library.read(cx);
                library.snapshot().session(&session).and_then(|node| {
                    let ix = node
                        .laps
                        .iter()
                        .position(|l| l.lap_id == slot.lap_ref.lap)?;
                    node.laps
                        .get(ix.checked_add_signed(offset)?)
                        .map(|l| l.lap_id)
                })
            }
        };
        if let Some(lap) = next {
            self.set_lap(Role::Primary, LapRef::new(session, lap), cx);
        }
    }

    pub fn prev_lap(&mut self, cx: &mut Context<Self>) {
        self.step_lap(-1, cx);
    }

    pub fn next_lap(&mut self, cx: &mut Context<Self>) {
        self.step_lap(1, cx);
    }

    /// Ask for an alignment strategy (persisted as `video.reference_sync`).
    pub fn set_strategy(&mut self, strategy: StrategyRequest, cx: &mut Context<Self>) {
        if strategy == self.strategy {
            return;
        }
        self.strategy = strategy;
        let key = match strategy {
            StrategyRequest::Auto => None,
            StrategyRequest::Prefer(strategy) => Some(strategy.key().to_string()),
        };
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| config.video.reference_sync = key);
        });
        self.rebuild_analysis(cx);
        cx.notify();
    }

    /// The manual damper offset (primary lap fraction); applies only to
    /// manual damper alignment.
    pub fn set_manual_offset(&mut self, offset: f64, cx: &mut Context<Self>) {
        if !offset.is_finite() || offset == self.manual_offset {
            return;
        }
        self.manual_offset = offset;
        // Derive from an analysis whose every other input is current: the
        // settled one, or the base of an offset change still in flight (a
        // damper drag). Anything else in flight is rebuilt with the offset.
        let base = match &self.analysis_load {
            None => self.settled_analysis(),
            Some(_) => self.offset_base.clone(),
        };
        match base {
            Some(base) if base.strategy() == Some(Strategy::ManualDampers) => {
                let derived = base.clone();
                self.run_analysis(cx, move |cancel| derived.with_manual_offset(offset, cancel));
                self.offset_base = Some(base);
            }
            // Another strategy is in effect: the offset has no effect.
            Some(_) => {}
            None if self.analysis_load.is_some() => self.rebuild_analysis(cx),
            None => {}
        }
        cx.notify();
    }

    /// Replace the user's corner zones for the primary's track (`None`
    /// returns to Track Atlas). Persisted under `tracks.<track>.corners`,
    /// keyed by [`RoleSlot::track_key`]. Does nothing when the primary's
    /// track is unknown: zones of one unnamed track must never apply to
    /// every other unnamed one.
    pub fn set_corner_override(&mut self, zones: Option<Vec<CornerZone>>, cx: &mut Context<Self>) {
        let Some(track) = self
            .primary
            .as_ref()
            .and_then(|slot| slot.track_key.as_ref())
            .map(SharedString::to_string)
        else {
            return;
        };
        self.corner_override = zones.clone();
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| {
                config.set_track_corners(&track, zones.as_deref())
            });
        });
        self.rebuild_analysis(cx);
        cx.notify();
    }

    fn stored_corner_override(&self, cx: &Context<Self>) -> Option<Vec<CornerZone>> {
        let track = self.primary.as_ref()?.track_key.as_ref()?;
        self.preferences.read(cx).config().track_corners(track)
    }

    /// The current analysis when nothing is rebuilding it and it was built
    /// from exactly the loaded pair and the requested strategy.
    fn settled_analysis(&self) -> Option<Arc<Analysis>> {
        if self.analysis_load.is_some() {
            return None;
        }
        self.analysis
            .clone()
            .filter(|analysis| self.is_current(analysis))
    }

    /// Whether `analysis` was built from the loaded roles and the requested
    /// strategy (by identity of the loaded laps, not by value).
    fn is_current(&self, analysis: &Analysis) -> bool {
        let same = |slot: &Option<RoleSlot>, lap: Option<&LoadedLap>| match (
            slot.as_ref().and_then(RoleSlot::loaded),
            lap,
        ) {
            (Some(a), Some(b)) => Arc::ptr_eq(a.unified(), b.unified()),
            (None, None) => true,
            _ => false,
        };
        analysis.request() == self.strategy
            && same(&self.primary, Some(analysis.primary()))
            && same(&self.reference, analysis.reference())
    }

    /// Cancel the analysis in flight, if any.
    fn cancel_analysis(&mut self) {
        self.offset_base = None;
        if let Some(pending) = self.analysis_load.take() {
            pending.cancel();
        }
    }

    fn drop_analysis(&mut self, cx: &mut Context<Self>) {
        self.cancel_analysis();
        if self.analysis.take().is_some() {
            cx.emit(SessionEvent::AnalysisReady);
        }
    }

    /// Rebuild the analysis from the loaded roles once no role is loading.
    fn rebuild_analysis(&mut self, cx: &mut Context<Self>) {
        let Some(primary) = self.primary.as_ref().and_then(RoleSlot::loaded).cloned() else {
            self.drop_analysis(cx);
            return;
        };
        if self.primary_load.is_some() || self.reference_load.is_some() {
            // Its inputs are stale; the load's completion rebuilds.
            self.cancel_analysis();
            return;
        }
        let reference = self.reference.as_ref().and_then(RoleSlot::loaded).cloned();
        let (strategy, offset, zones) = (
            self.strategy,
            self.manual_offset,
            self.corner_override.clone(),
        );
        self.run_analysis(cx, move |cancel| {
            Analysis::build(
                &primary,
                reference.as_ref(),
                strategy,
                offset,
                zones,
                cancel,
            )
        });
    }

    fn run_analysis(
        &mut self,
        cx: &mut Context<Self>,
        build: impl FnOnce(&AtomicBool) -> Result<Analysis, omatrack_core::SessionError>
        + Send
        + 'static,
    ) {
        self.cancel_analysis();
        self.analysis_generation += 1;
        let generation = self.analysis_generation;
        let cancel = Arc::new(AtomicBool::new(false));
        let background_cancel = cancel.clone();
        let job = self
            .jobs
            .update(cx, |jobs, cx| jobs.start("Analysing laps", cx));
        let work = cx.background_spawn(async move { build(&background_cancel) });
        let jobs = self.jobs.clone();
        let task = cx.spawn(async move |this, cx| {
            let result = work.await;
            jobs.update(cx, |jobs, cx| jobs.finish(job, cx));
            let _ = this.update(cx, |this, cx| {
                if this.analysis_generation != generation {
                    return;
                }
                this.analysis_load = None;
                this.offset_base = None;
                match result {
                    Ok(analysis) if !this.is_current(&analysis) => {
                        // Defensive: every input change supersedes the
                        // build, so this should not happen. Never show an
                        // analysis of another pair or strategy.
                        log::warn!("discarding an analysis of stale inputs");
                        this.rebuild_analysis(cx);
                        return;
                    }
                    Ok(analysis) => {
                        if let Some(comparison) = analysis.comparison() {
                            this.manual_offset = comparison.manual_offset();
                        }
                        this.analysis = Some(Arc::new(analysis));
                    }
                    Err(omatrack_core::SessionError::Cancelled) => return,
                    Err(error) => {
                        this.analysis = None;
                        cx.emit(SessionEvent::LoadFailed {
                            role: Role::Primary,
                            message: format!("Couldn’t analyse the laps. {error}").into(),
                        });
                    }
                }
                cx.emit(SessionEvent::AnalysisReady);
                cx.notify();
            });
        });
        self.analysis_load = Some(Pending {
            cancel,
            _task: task,
        });
    }

    fn take_load(&mut self, role: Role) -> Option<Pending> {
        match role {
            Role::Primary => self.primary_load.take(),
            Role::Reference => self.reference_load.take(),
        }
    }

    fn put_load(&mut self, role: Role, pending: Pending) {
        match role {
            Role::Primary => self.primary_load = Some(pending),
            Role::Reference => self.reference_load = Some(pending),
        }
    }

    fn put_slot(&mut self, role: Role, slot: RoleSlot) {
        match role {
            Role::Primary => self.primary = Some(slot),
            Role::Reference => self.reference = Some(slot),
        }
    }

    fn emit_changed(&self, role: Role, cx: &mut Context<Self>) {
        cx.emit(match role {
            Role::Primary => SessionEvent::PrimaryChanged,
            Role::Reference => SessionEvent::ReferenceChanged,
        });
    }

    /// Remember the loaded pair in `selection` (recording path + lap id).
    fn persist_selection(&self, cx: &mut Context<Self>) {
        let key = |slot: &Option<RoleSlot>| {
            slot.as_ref()
                .filter(|slot| slot.loaded().is_some())
                .map(|slot| {
                    (
                        slot.info.path.to_string_lossy().into_owned(),
                        slot.lap_ref.lap,
                    )
                })
        };
        let (primary, reference) = (key(&self.primary), key(&self.reference));
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| {
                if let Some((path, lap)) = primary {
                    config.selection.primary_key = Some(path);
                    config.selection.primary_lap = Some(lap);
                }
                match reference {
                    Some((path, lap)) => {
                        config.selection.compare_key = Some(path);
                        config.selection.compare_lap = Some(lap);
                    }
                    None => {
                        config.selection.compare_key = None;
                        config.selection.compare_lap = None;
                    }
                }
            });
        });
    }

    /// After the first scan: reselect the pair the last session ended with.
    fn restore_selection(&mut self, cx: &mut Context<Self>) {
        if !std::mem::take(&mut self.restore_pending) || self.primary.is_some() {
            return;
        }
        let selection = self.preferences.read(cx).config().selection.clone();
        let find = |key: &str, lap: i32, cx: &Context<Self>| {
            let library = self.library.read(cx);
            library
                .snapshot()
                .session_for_path(Path::new(key))
                .filter(|node| node.lap(lap).is_some())
                .map(|node| LapRef::new(node.id.clone(), lap))
        };
        let primary = selection
            .primary()
            .and_then(|(key, lap)| find(key, lap, cx));
        let reference = selection
            .reference()
            .and_then(|(key, lap)| find(key, lap, cx));
        if let Some(primary) = primary {
            self.set_lap(Role::Primary, primary, cx);
            if let Some(reference) = reference {
                self.set_lap(Role::Reference, reference, cx);
            }
        }
    }
}

const CANCELLED: &str = "cancelled";

fn load_recording_lap(
    recording: Arc<Recording>,
    source: &RecordingSource,
    lap: i32,
    cancel: &AtomicBool,
) -> Result<LoadedLap, String> {
    let metadata = &source.node.metadata;
    let options = LoadOptions::default()
        .with_overrides(metadata.channel_overrides.clone())
        .with_track_hint(metadata.track_slug().map(str::to_string))
        .with_video_path(source.location.media_path(&source.file));
    load_lap(recording, lap, &options, cancel).map_err(|error| match error {
        omatrack_core::SessionError::Cancelled => CANCELLED.to_string(),
        error => format!("Couldn’t load the lap. {error}"),
    })
}

fn lap_info(source: &RecordingSource, lap: i32) -> LapInfo {
    let node = &source.node;
    let row = node.lap(lap);
    LapInfo {
        label: row
            .map(|row| row.label.clone())
            .unwrap_or_else(|| format!("L{lap}"))
            .into(),
        time: row
            .map(|row| format_lap_time(row.time_ms))
            .unwrap_or_default()
            .into(),
        driver: node.driver.clone().map(SharedString::from),
        title: node.title.clone().into(),
        track: node
            .metadata
            .track_name()
            .unwrap_or("Unknown track")
            .to_string()
            .into(),
        session_name: node.session_name.clone().map(SharedString::from),
        day: date_heading(node.start.date).into(),
        path: node.file.path().to_path_buf(),
    }
}
