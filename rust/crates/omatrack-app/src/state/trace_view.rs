//! What the traces show besides the two laps ([`TraceViewMode`]) and the
//! one pipeline that mode needs: the primary's session spread
//! (Consistency).
//!
//! The spread loads lazily: only while the mode is Consistency and the
//! primary is loaded, once per primary lap. It owns one pipeline slot
//! (latest wins, a replacement cancels the load in flight) and reports its
//! progress through [`Jobs`]. A spread belongs to the primary lap it was
//! built on ([`SessionSpread::primary`]); consumers show it only on that
//! lap, so a lap change never draws another lap's session.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use gpui_kit::{AppContext as _, Context, Entity, EventEmitter, Subscription, Task};
use omatrack_core::UnifiedLap;
use omatrack_core::session::{Consistency, LoadedLap, SessionLaps};

pub use omatrack_library::config::TraceViewMode;

use crate::state::jobs::Jobs;
use crate::state::preferences::Preferences;
use crate::state::session::{RoleSlot, Session, SessionEvent};

/// The primary's session spread and the lap it was built on.
#[derive(Debug, Clone)]
pub struct SessionSpread {
    primary: Arc<UnifiedLap>,
    consistency: Arc<Consistency>,
}

impl SessionSpread {
    /// The primary lap the spread lies on (compare by `Arc::ptr_eq`).
    pub fn primary(&self) -> &Arc<UnifiedLap> {
        &self.primary
    }
    pub fn consistency(&self) -> &Arc<Consistency> {
        &self.consistency
    }
    /// Whether this spread lies on `lap`.
    pub fn is_for(&self, lap: &LoadedLap) -> bool {
        Arc::ptr_eq(&self.primary, lap.unified())
    }
}

/// What changed in the [`TraceView`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TraceViewEvent {
    ModeChanged,
    /// A new spread is available (or a failed load ended).
    SpreadReady,
}

/// Cancellable spread load in the slot.
struct Pending {
    /// The primary lap the load is for.
    primary: Arc<UnifiedLap>,
    cancel: Arc<AtomicBool>,
    _task: Task<()>,
    _progress: Task<()>,
}

/// The trace view mode (`trace.view_mode`) and the session spread.
pub struct TraceView {
    session: Entity<Session>,
    preferences: Entity<Preferences>,
    jobs: Entity<Jobs>,
    mode: TraceViewMode,
    spread: Option<SessionSpread>,
    load: Option<Pending>,
    /// Spread loads started since creation (tests: one per lap and mode).
    loads_started: usize,
    _subscriptions: Vec<Subscription>,
}

impl EventEmitter<TraceViewEvent> for TraceView {}

impl TraceView {
    pub fn new(
        session: Entity<Session>,
        preferences: Entity<Preferences>,
        jobs: Entity<Jobs>,
        cx: &mut Context<'_, Self>,
    ) -> Self {
        let mode = preferences.read(cx).config().trace.view_mode();
        let subscriptions = vec![cx.subscribe(&session, |this, _, event, cx| {
            if matches!(event, SessionEvent::PrimaryChanged | SessionEvent::Swapped) {
                this.ensure_spread(cx);
            }
        })];
        let mut view = Self {
            session,
            preferences,
            jobs,
            mode,
            spread: None,
            load: None,
            loads_started: 0,
            _subscriptions: subscriptions,
        };
        view.ensure_spread(cx);
        view
    }

    pub fn mode(&self) -> TraceViewMode {
        self.mode
    }

    /// Switch the mode (persisted as `trace.view_mode`). Entering
    /// Consistency starts the spread load when the primary has none yet.
    pub fn set_mode(&mut self, mode: TraceViewMode, cx: &mut Context<'_, Self>) {
        if mode == self.mode {
            return;
        }
        self.mode = mode;
        let stored = (mode != TraceViewMode::Lap).then_some(mode);
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| config.trace.view_mode = stored);
        });
        cx.emit(TraceViewEvent::ModeChanged);
        self.ensure_spread(cx);
        cx.notify();
    }

    /// The spread, when it lies on the current primary lap.
    pub fn spread_for(&self, primary: &LoadedLap) -> Option<&SessionSpread> {
        self.spread.as_ref().filter(|spread| spread.is_for(primary))
    }

    /// The last spread built, whichever lap it lies on.
    pub fn spread(&self) -> Option<&SessionSpread> {
        self.spread.as_ref()
    }

    pub fn is_loading(&self) -> bool {
        self.load.is_some()
    }

    pub fn loads_started(&self) -> usize {
        self.loads_started
    }

    /// Start (or restart) the spread load when the mode needs one and the
    /// current primary has none, finished or in flight.
    fn ensure_spread(&mut self, cx: &mut Context<'_, Self>) {
        if self.mode != TraceViewMode::Consistency {
            return;
        }
        let Some(primary) = self
            .session
            .read(cx)
            .primary()
            .and_then(RoleSlot::loaded)
            .cloned()
        else {
            return;
        };
        if self.spread_for(&primary).is_some() {
            return;
        }
        if self
            .load
            .as_ref()
            .is_some_and(|pending| Arc::ptr_eq(&pending.primary, primary.unified()))
        {
            return;
        }
        self.start_load(primary, cx);
    }

    /// Load every timed lap of the primary's session off the UI thread
    /// (the parsed recording and the primary lap are reused) and build the
    /// spread; the slot's previous load is cancelled.
    fn start_load(&mut self, primary: LoadedLap, cx: &mut Context<'_, Self>) {
        if let Some(pending) = self.load.take() {
            pending.cancel.store(true, Ordering::Relaxed);
        }
        self.loads_started += 1;
        let generation = self.loads_started;
        let cancel = Arc::new(AtomicBool::new(false));
        let background_cancel = cancel.clone();
        let job = self
            .jobs
            .update(cx, |jobs, cx| jobs.start("Loading session laps", cx));
        let job_id = job.id();
        let (progress_tx, progress_rx) = async_channel::unbounded::<(usize, usize)>();
        let unified = primary.unified().clone();
        let work = cx.background_spawn(async move {
            let cancel = background_cancel;
            let laps = SessionLaps::for_consistency(&primary, &cancel, &mut |done, total| {
                let _ = progress_tx.try_send((done, total));
            })?;
            let consistency = laps.consistency(&primary, &cancel)?;
            Ok::<_, omatrack_core::SessionError>(SessionSpread {
                primary: primary.unified().clone(),
                consistency: Arc::new(consistency),
            })
        });
        let progress_jobs = self.jobs.clone();
        let progress = cx.spawn(async move |_, cx| {
            while let Ok((done, total)) = progress_rx.recv().await {
                progress_jobs.update(cx, |jobs, cx| jobs.set_progress(job_id, done, total, cx));
            }
        });
        let jobs = self.jobs.clone();
        let task = cx.spawn(async move |this, cx| {
            let result = work.await;
            jobs.update(cx, |jobs, cx| jobs.finish(job, cx));
            let _ = this.update(cx, |this, cx| {
                if this.loads_started != generation {
                    return;
                }
                this.load = None;
                match result {
                    Ok(spread) => this.spread = Some(spread),
                    Err(omatrack_core::SessionError::Cancelled) => return,
                    Err(error) => log::warn!("session spread failed: {error}"),
                }
                cx.emit(TraceViewEvent::SpreadReady);
                cx.notify();
            });
        });
        self.load = Some(Pending {
            primary: unified,
            cancel,
            _task: task,
            _progress: progress,
        });
    }
}
