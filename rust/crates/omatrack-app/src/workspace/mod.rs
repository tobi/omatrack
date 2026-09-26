//! The window’s root view: title bar, lap filmstrip, dock workspace, status
//! bar and the overlay layers, and the one place every workspace action is
//! routed to the entity that owns it.

pub mod filmstrip;
pub mod header;
pub mod layout;
pub mod status;

use std::rc::Rc;
use std::time::Duration;

use gpui_kit::component::{
    ActiveTheme as _, Root, Theme, WindowExt as _,
    dock::{DockArea, DockEvent, DockPlacement, DockSkin},
    notification::Notification,
};
use gpui_kit::{
    AppContext as _, Context, Entity, FocusHandle, Focusable, InteractiveElement as _, IntoElement,
    ParentElement as _, Pixels, Render, SharedString, Styled as _, Subscription, Task,
    TestSupportExt as _, Window, div,
};
use omatrack_trace::{Selection, Viewport};
use omatrack_ui::theme::ThemeStatus;

use crate::actions::{
    ClosePreferences, ComposeLayout1, ComposeLayout2, ComposeLayout3, ComposeLayout4,
    ComposeLayout5, ExitFullscreen, FocusCorner, FocusPanel1, FocusPanel2, FocusPanel3,
    FocusPanel4, FocusPanel5, FocusPanel6, NextCorner, NextLap, OpenFolder, OpenPreferences,
    PrevCorner, PrevLap, Rescan, ResetLayout, ResizeLanes, RevealRecording, Role, SeekBack,
    SeekForward, SelectLap, ShowChannels, ShowInspector, ShowMap, SwapRoles, ToggleContinuous,
    ToggleCornerEdit, ToggleFit, ToggleInspector, ToggleLibrary, ToggleMute, TogglePalette,
    TogglePlay, ToggleSlowMotion, ToggleVideoFullscreen, ToggleXAxis, ZoomIn, ZoomOut, ZoomReset,
};
use crate::commands::Palette;
use crate::keymap::WORKSPACE_CONTEXT;
use crate::panels::{PanelKind, TraceMode, WorkspacePanels};
use crate::preferences::PreferencesView;
use crate::state::{
    AppState, ComposeLayout, LapRef, LibraryEvent, PreferencesEvent, SessionEvent, VideoEvent,
};

pub use filmstrip::{Filmstrip, FilmstripRow};
pub use header::SyncOption;
pub use layout::{DOCK_AREA_ID, LAYOUT_VERSION, LayoutOrigin};
pub use status::StatusView;

/// How long dock edits coalesce before the layout is saved.
const LAYOUT_SAVE_DEBOUNCE: Duration = Duration::from_millis(500);

/// The root view below `Root`.
///
/// Owns the dock area (the panels are entities inside it), the status bar
/// entity and the palette; reads everything else from [`AppState`].
pub struct Workspace {
    app: AppState,
    focus_handle: FocusHandle,
    dock_area: Entity<DockArea>,
    _skin: Rc<DockSkin>,
    panels: WorkspacePanels,
    filmstrip: Entity<Filmstrip>,
    status: Entity<StatusView>,
    /// The sync menu of the title bar: automatic, then the strategies
    /// both laps support; `sync_current` is the one asked for.
    sync_options: Vec<SyncOption>,
    sync_current: Option<omatrack_core::alignment::Strategy>,
    swap_icon: gpui_kit::component::Icon,
    palette: Palette,
    layout_origin: LayoutOrigin,
    layout_save: Option<Task<()>>,
    /// The fullscreen video stage, shown over the whole window in place of
    /// every chrome while open.
    stage: Option<VideoStage>,
    focused_corner: Option<usize>,
    /// The viewport before the first corner focus; Escape returns to it.
    pre_focus_viewport: Option<Viewport>,
    /// The cursor before the corner focus, restored with the viewport.
    pre_focus_cursor: Option<f64>,
    /// The default layout follows the window's width until the user
    /// toggles a dock (a restored layout is the user's and never refits).
    fit_docks: bool,
    last_fit_width: Option<Pixels>,
    /// The Preferences screen, shown in place of the dock area while open.
    preferences: Option<PreferencesScreen>,
    _subscriptions: Vec<Subscription>,
}

/// The open fullscreen video stage: the focus to return to on exit and
/// whether the stage put the window into fullscreen (and so takes it out).
struct VideoStage {
    restore: Option<FocusHandle>,
    window_fullscreen: bool,
}

/// An open Preferences screen and the focus to return to on close.
struct PreferencesScreen {
    view: Entity<PreferencesView>,
    restore: Option<FocusHandle>,
}

impl Workspace {
    /// Build the workspace from the installed [`AppState`], restoring the
    /// saved dock layout (or the default one).
    pub fn new(window: &mut Window, cx: &mut Context<'_, Self>) -> Self {
        let app = AppState::global(cx).clone();
        let (dock_area, skin) = DockSkin::dock_area(DOCK_AREA_ID, Some(LAYOUT_VERSION), window, cx);
        let panels = WorkspacePanels::new(&app, window, cx);
        let saved = app.preferences.read(cx).config().workspace.layout.clone();
        let layout_origin = layout::restore(&dock_area, &panels, saved.as_ref(), window, cx);
        if let LayoutOrigin::Replaced(reason) = &layout_origin {
            let message = format!("{reason} The default layout is shown.");
            cx.defer_in(window, move |_, window, cx| {
                window.push_notification(Notification::info(message).id::<LayoutNotice>(), cx);
            });
        }

        let status = cx.new(|cx| StatusView::new(app.clone(), cx));
        let filmstrip = cx.new(|cx| Filmstrip::new(app.clone(), cx));

        let subscriptions = vec![
            cx.observe_window_bounds(window, |this: &mut Self, window, cx| {
                this.fit_docks(window, cx);
            }),
            cx.observe_global::<Theme>(|_, cx| cx.notify()),
            cx.observe_global::<ThemeStatus>(|_, cx| cx.notify()),
            cx.observe(&app.session, |_, _, cx| cx.notify()),
            cx.subscribe(&dock_area, |this, _, event, cx| {
                if matches!(event, DockEvent::LayoutChanged) {
                    this.schedule_layout_save(cx);
                }
            }),
            cx.subscribe_in(&app.session, window, Self::on_session_event),
            cx.subscribe_in(&app.library, window, Self::on_library_event),
            cx.subscribe_in(&app.video, window, |_, _, event, window, cx| {
                if let VideoEvent::Failed(message) = event {
                    window.push_notification(
                        Notification::error(message.clone())
                            .title("Video")
                            .id::<VideoNotice>()
                            .autohide(false),
                        cx,
                    );
                }
            }),
            cx.subscribe_in(&app.preferences, window, |_, _, event, window, cx| {
                if let PreferencesEvent::SaveFailed(message) = event {
                    window.push_notification(
                        Notification::error(message.clone())
                            .id::<PreferencesNotice>()
                            .autohide(false),
                        cx,
                    );
                }
            }),
        ];

        // A quit inside the layout debounce still saves the layout.
        cx.on_app_quit(|this: &mut Self, cx| {
            this.save_layout(cx);
            this.app
                .preferences
                .update(cx, super::state::preferences::Preferences::flush);
            async {}
        })
        .detach();

        // Start in the primary work area so single keys work at once.
        let traces = panels.focus_handle(PanelKind::Traces, cx);
        window.focus(&traces, cx);

        let layout_origin_is_default = layout_origin != LayoutOrigin::Restored;
        let mut workspace = Self {
            app,
            focus_handle: cx.focus_handle(),
            dock_area,
            _skin: skin,
            panels,
            filmstrip,
            status,
            sync_options: vec![SyncOption::automatic()],
            sync_current: None,
            swap_icon: header::swap_icon(),
            palette: Palette::default(),
            layout_origin,
            layout_save: None,
            stage: None,
            focused_corner: None,
            pre_focus_viewport: None,
            pre_focus_cursor: None,
            fit_docks: layout_origin_is_default,
            last_fit_width: None,
            preferences: None,
            _subscriptions: subscriptions,
        };
        workspace.sync_strategies(cx);
        workspace
    }

    /// The dock area holding every panel.
    pub fn dock_area(&self) -> &Entity<DockArea> {
        &self.dock_area
    }

    /// The panel entities of this workspace.
    pub fn panels(&self) -> &WorkspacePanels {
        &self.panels
    }

    /// The lap filmstrip. The workspace renders it full width below the
    /// title bar; a surface that shows it elsewhere (a lane over
    /// fullscreen video) renders this same entity instead.
    pub fn filmstrip(&self) -> &Entity<Filmstrip> {
        &self.filmstrip
    }

    /// The application state this workspace shows.
    pub fn app(&self) -> &AppState {
        &self.app
    }

    /// How the current dock layout came to be.
    pub fn layout_origin(&self) -> &LayoutOrigin {
        &self.layout_origin
    }

    /// The corner the viewport was last focused on, by index.
    pub fn focused_corner(&self) -> Option<usize> {
        self.focused_corner
    }

    /// Whether the fullscreen video stage is showing.
    pub fn is_video_fullscreen(&self) -> bool {
        self.stage.is_some()
    }

    fn on_session_event(
        &mut self,
        _: &Entity<crate::state::Session>,
        event: &SessionEvent,
        window: &mut Window,
        cx: &mut Context<'_, Self>,
    ) {
        match event {
            SessionEvent::LoadFailed { role, message } => {
                let title = match role {
                    Role::Primary => "Primary lap",
                    Role::Reference => "Reference lap",
                };
                window.push_notification(
                    Notification::error(message.clone())
                        .title(title)
                        .autohide(false),
                    cx,
                );
            }
            SessionEvent::AnalysisReady => {
                self.forget_corner_focus(cx);
                self.sync_strategies(cx);
                self.place_initial_cursor(cx);
            }
            SessionEvent::PrimaryChanged => self.forget_corner_focus(cx),
            SessionEvent::SelectionRequested { .. }
            | SessionEvent::ReferenceChanged
            | SessionEvent::Swapped => {}
        }
    }

    #[expect(
        clippy::unused_self,
        reason = "GPUI subscription callbacks require the receiving entity even when this event only uses the context."
    )]
    fn on_library_event(
        &mut self,
        _: &Entity<crate::state::Library>,
        event: &LibraryEvent,
        window: &mut Window,
        cx: &mut Context<'_, Self>,
    ) {
        if let LibraryEvent::ScanFinished {
            unreadable,
            location_errors,
        } = event
        {
            for error in location_errors {
                window.push_notification(
                    Notification::error(format!("Couldn’t read a library folder. {error}"))
                        .autohide(false),
                    cx,
                );
            }
            if *unreadable > 0 {
                let noun = if *unreadable == 1 {
                    "recording"
                } else {
                    "recordings"
                };
                window.push_notification(
                    Notification::info(format!("{unreadable} {noun} couldn’t be read.")),
                    cx,
                );
            }
        }
    }

    /// A lap on screen always has a cursor: the first analysis puts it at
    /// the lap start, where the video and HUD already are, so the traces,
    /// inspector and status bar agree. A cursor that exists (a lap change,
    /// a swap, a later analysis) is never moved.
    fn place_initial_cursor(&mut self, cx: &mut Context<'_, Self>) {
        let has_lap = self.app.session.read(cx).analysis().is_some();
        if has_lap && self.app.cursor.read(cx).fraction().is_none() {
            self.app
                .cursor
                .update(cx, |cursor, cx| cursor.set_fraction(Some(0.0), cx));
        }
    }

    /// Offer exactly the strategies both laps support.
    fn sync_strategies(&mut self, cx: &mut Context<'_, Self>) {
        let session = self.app.session.read(cx);
        let available = session
            .analysis()
            .map(|analysis| analysis.available_strategies().to_vec())
            .unwrap_or_default();
        let current = match session.strategy() {
            omatrack_core::session::StrategyRequest::Prefer(strategy)
                if available.contains(&strategy) =>
            {
                Some(strategy)
            }
            _ => None,
        };
        let resolved = session
            .analysis()
            .and_then(|analysis| analysis.comparison())
            .map(|comparison| SharedString::from(comparison.basis().to_owned()))
            .filter(|basis| !basis.is_empty() && current.is_none());
        let options = std::iter::once(SyncOption::automatic_resolved(resolved))
            .chain(available.into_iter().map(SyncOption::strategy))
            .collect::<Vec<_>>();
        self.sync_options = options;
        self.sync_current = current;
        cx.notify();
    }

    fn schedule_layout_save(&mut self, cx: &mut Context<'_, Self>) {
        self.layout_save = Some(cx.spawn(async move |this, cx| {
            cx.background_executor().timer(LAYOUT_SAVE_DEBOUNCE).await;
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no deferred result; this weak entity/window handle may already be gone.")]
            let _ = this.update(cx, Self::save_layout);
        }));
    }

    /// Write the current dock layout to `workspace.layout` now.
    pub fn save_layout(&mut self, cx: &mut Context<'_, Self>) {
        self.layout_save = None;
        let Some(layout) = layout::encode(self.dock_area.read(cx), cx) else {
            return;
        };
        self.app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| config.workspace.layout = Some(layout));
        });
    }

    pub(crate) fn toggle_palette(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        let keys = self.focus_handle.clone();
        self.palette.toggle(&keys, window, cx);
    }

    /// Show the Preferences screen in place of the dock area (which stays
    /// alive behind it) and focus its section list. Already open, only the
    /// focus moves.
    pub fn open_preferences(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        if self.stage.is_some() {
            self.exit_video_fullscreen(window, cx);
        }
        if let Some(screen) = &self.preferences {
            screen
                .view
                .update(cx, |view, cx| view.focus_nav(window, cx));
            return;
        }
        // From the palette, focus is in its query field, which closes with
        // it: return to the traces instead.
        let restore = if window.has_active_dialog(cx) {
            None
        } else {
            window.focused(cx)
        };
        let app = self.app.clone();
        let view = cx.new(|cx| PreferencesView::new(app, window, cx));
        view.update(cx, |view, cx| view.focus_nav(window, cx));
        self.preferences = Some(PreferencesScreen { view, restore });
        cx.notify();
    }

    /// Leave the Preferences screen and return focus to where it was
    /// (the traces when that is unknown).
    pub fn close_preferences(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        let Some(screen) = self.preferences.take() else {
            return;
        };
        let restore = screen
            .restore
            .unwrap_or_else(|| self.panels.focus_handle(PanelKind::Traces, cx));
        window.focus(&restore, cx);
        cx.notify();
    }

    /// The open Preferences screen, if one is.
    pub fn preferences(&self) -> Option<&Entity<PreferencesView>> {
        self.preferences.as_ref().map(|screen| &screen.view)
    }

    fn focus_panel(&mut self, kind: PanelKind, window: &mut Window, cx: &mut Context<'_, Self>) {
        // A panel is behind the Preferences screen or the video stage:
        // leave them for the panel.
        if self.preferences.take().is_some() {
            cx.notify();
        }
        if self.stage.is_some() {
            self.exit_video_fullscreen(window, cx);
        }
        let handle = self.panels.handle(kind);
        let id = handle.panel_id(cx);
        let present = layout::holds(self.dock_area.read(cx), &self.panels, kind, cx);
        let panels = self.panels.clone();
        self.dock_area.update(cx, |area, cx| {
            if !present {
                area.add_panel_view(handle, layout::default_placement(kind), None, window, cx);
            }
            layout::reveal_dock(area, &panels, kind, window, cx);
            area.select_panel(id, window, cx);
        });
        match kind {
            PanelKind::Library => {
                let tree = self.panels.library.read(cx).tree().clone();
                tree.update(cx, |tree, cx| tree.focus(window, cx));
            }
            kind => {
                let handle = self.panels.focus_handle(kind, cx);
                window.focus(&handle, cx);
            }
        }
    }

    /// Keep the default layout's docks fitted to the window width (the
    /// traces keep at least half of it). Stops once the user toggles a dock.
    fn fit_docks(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        let width = window.viewport_size().width;
        if !self.fit_docks || self.last_fit_width == Some(width) {
            return;
        }
        self.last_fit_width = Some(width);
        layout::fit_default_docks(&self.dock_area, window, cx);
    }

    fn toggle_dock(
        &mut self,
        placement: DockPlacement,
        window: &mut Window,
        cx: &mut Context<'_, Self>,
    ) {
        self.fit_docks = false;
        self.dock_area
            .update(cx, |area, cx| area.toggle_dock(placement, window, cx));
    }

    /// Focus corner `ix` of the analysis: centered in the left half with the
    /// 140 ms motion, its zone kept bright.
    fn focus_corner(&mut self, ix: usize, cx: &mut Context<'_, Self>) {
        let Some(zone) = self
            .app
            .session
            .read(cx)
            .analysis()
            .and_then(|analysis| analysis.corners().get(ix).cloned())
        else {
            return;
        };
        self.focused_corner = Some(ix);
        if self.pre_focus_viewport.is_none() {
            self.pre_focus_viewport = Some(self.app.viewport.read(cx).viewport());
            self.pre_focus_cursor = self.app.cursor.read(cx).fraction();
        }
        self.app.viewport.update(cx, |viewport, cx| {
            viewport.focus(zone.start, zone.end, true, cx);
        });
        // The cursor moves to the corner so every readout (legends,
        // inspector, HUD, video) describes the corner being looked at.
        self.app.cursor.update(cx, |cursor, cx| {
            cursor.set_focus(Some(Selection::new(zone.start, zone.end)), cx);
            cursor.set_fraction(Some(zone.start), cx);
        });
    }

    /// Drop the corner focus (its zone may not exist in a new analysis);
    /// the viewport stays where it is.
    fn forget_corner_focus(&mut self, cx: &mut Context<'_, Self>) {
        self.focused_corner = None;
        self.pre_focus_viewport = None;
        self.pre_focus_cursor = None;
        if self.app.cursor.read(cx).focus().is_some() {
            self.app
                .cursor
                .update(cx, |cursor, cx| cursor.set_focus(None, cx));
        }
    }

    /// Leave the corner focus and return to the viewport it started from.
    /// False when no corner is focused.
    fn unfocus_corner(&mut self, cx: &mut Context<'_, Self>) -> bool {
        let Some(viewport) = self.pre_focus_viewport.take() else {
            return false;
        };
        let cursor = self.pre_focus_cursor.take();
        self.forget_corner_focus(cx);
        self.app
            .viewport
            .update(cx, |state, cx| state.set_viewport(viewport, cx));
        if cursor.is_some() {
            self.app
                .cursor
                .update(cx, |state, cx| state.set_fraction(cursor, cx));
        }
        true
    }

    /// Focus the next (`step` 1) or previous (-1) corner (see
    /// [`step_corner_ix`]).
    fn step_corner(&mut self, step: isize, cx: &mut Context<'_, Self>) {
        let Some(starts) = self.app.session.read(cx).analysis().map(|analysis| {
            analysis
                .corners()
                .iter()
                .map(|zone| zone.start)
                .collect::<Vec<_>>()
        }) else {
            return;
        };
        let cursor = self.app.cursor.read(cx).fraction();
        if let Some(ix) = step_corner_ix(&starts, self.focused_corner, cursor, step) {
            self.focus_corner(ix, cx);
        }
    }

    fn zoom(&mut self, factor: Option<f64>, cx: &mut Context<'_, Self>) {
        let anchor = self.app.cursor.read(cx).fraction();
        self.app.viewport.update(cx, |viewport, cx| match factor {
            Some(factor) if factor < 1.0 => viewport.zoom_in(anchor, cx),
            Some(_) => viewport.zoom_out(anchor, cx),
            None => viewport.reset(cx),
        });
    }

    /// Show the video-only stage over the whole window: the title bar,
    /// filmstrip row, docks and status bar give way to the pictures on
    /// black. The dock layout is not touched (the video panel renders on the
    /// stage instead of in its dock), and the window enters fullscreen as a
    /// best effort (a headless or tiling session may refuse).
    fn toggle_video_fullscreen(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        if self.stage.is_some() {
            self.exit_video_fullscreen(window, cx);
            return;
        }
        if self.preferences.is_some() {
            self.close_preferences(window, cx);
        }
        let restore = if window.has_active_dialog(cx) {
            None
        } else {
            window.focused(cx)
        };
        let window_fullscreen = !window.is_fullscreen();
        if window_fullscreen {
            window.toggle_fullscreen();
        }
        self.stage = Some(VideoStage {
            restore,
            window_fullscreen,
        });
        let filmstrip = self.filmstrip.clone();
        self.panels
            .video
            .update(cx, |video, cx| video.set_stage(Some(filmstrip), window, cx));
        let handle = self.panels.focus_handle(PanelKind::Video, cx);
        window.focus(&handle, cx);
        cx.notify();
    }

    /// Leave the stage: the chrome returns with its dock layout as it was,
    /// focus goes back to where it was (the video panel when unknown), and
    /// the window leaves fullscreen if the stage put it there.
    fn exit_video_fullscreen(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        let Some(stage) = self.stage.take() else {
            if window.is_fullscreen() {
                window.toggle_fullscreen();
            }
            return;
        };
        self.panels
            .video
            .update(cx, |video, cx| video.set_stage(None, window, cx));
        if stage.window_fullscreen && window.is_fullscreen() {
            window.toggle_fullscreen();
        }
        let restore = stage
            .restore
            .unwrap_or_else(|| self.panels.focus_handle(PanelKind::Video, cx));
        window.focus(&restore, cx);
        cx.notify();
    }

    fn set_trace_mode(&mut self, mode: TraceMode, window: &mut Window, cx: &mut Context<'_, Self>) {
        self.panels
            .traces
            .update(cx, |traces, cx| traces.toggle_mode(mode, cx));
        // The editors take keyboard focus so Ctrl+S / Escape reach them.
        if self.panels.traces.read(cx).mode() != TraceMode::Browse {
            self.focus_panel(PanelKind::Traces, window, cx);
        }
    }

    #[expect(
        clippy::too_many_lines,
        reason = "One declarative registry keeps all action bindings and command metadata together."
    )]
    fn register_actions(
        root: gpui_kit::Stateful<gpui_kit::Div>,
        cx: &mut Context<'_, Self>,
    ) -> gpui_kit::Stateful<gpui_kit::Div> {
        root.on_action(
            cx.listener(|this, _: &TogglePalette, window, cx| this.toggle_palette(window, cx)),
        )
        .on_action(
            cx.listener(|this, _: &OpenPreferences, window, cx| this.open_preferences(window, cx)),
        )
        .on_action(cx.listener(|this, _: &ClosePreferences, window, cx| {
            this.close_preferences(window, cx);
        }))
        .on_action(cx.listener(|this, _: &OpenFolder, _, cx| {
            this.app
                .library
                .update(cx, super::state::library::Library::prompt_add_folder);
        }))
        .on_action(cx.listener(|this, _: &Rescan, _, cx| {
            this.app
                .library
                .update(cx, super::state::library::Library::rescan);
        }))
        .on_action(cx.listener(|this, _: &ToggleLibrary, window, cx| {
            this.toggle_dock(DockPlacement::Left, window, cx);
        }))
        .on_action(cx.listener(|this, _: &ToggleInspector, window, cx| {
            this.toggle_dock(DockPlacement::Right, window, cx);
        }))
        .on_action(cx.listener(|this, _: &ResetLayout, window, cx| {
            layout::apply_default(&this.dock_area, &this.panels, window, cx);
            this.layout_origin = LayoutOrigin::Default;
            this.fit_docks = true;
            this.last_fit_width = None;
            window.push_notification(
                Notification::info("The default layout is restored.").id::<LayoutNotice>(),
                cx,
            );
        }))
        .on_action(cx.listener(|this, _: &FocusPanel1, window, cx| {
            this.focus_panel(PanelKind::Laps, window, cx);
        }))
        .on_action(cx.listener(|this, _: &FocusPanel2, window, cx| {
            this.focus_panel(PanelKind::Traces, window, cx);
        }))
        .on_action(cx.listener(|this, _: &FocusPanel3, window, cx| {
            this.focus_panel(PanelKind::Video, window, cx);
        }))
        .on_action(cx.listener(|this, _: &FocusPanel4, window, cx| {
            this.focus_panel(PanelKind::Corners, window, cx);
        }))
        .on_action(cx.listener(|this, _: &FocusPanel5, window, cx| {
            this.focus_panel(PanelKind::TimeGoes, window, cx);
        }))
        .on_action(cx.listener(|this, _: &FocusPanel6, window, cx| {
            this.focus_panel(PanelKind::Library, window, cx);
        }))
        .on_action(cx.listener(|this, _: &ShowChannels, window, cx| {
            this.focus_panel(PanelKind::Channels, window, cx);
        }))
        .on_action(cx.listener(|this, _: &ShowMap, window, cx| {
            this.focus_panel(PanelKind::Map, window, cx);
        }))
        .on_action(cx.listener(|this, _: &ShowInspector, window, cx| {
            this.focus_panel(PanelKind::Inspector, window, cx);
        }))
        // Playback.
        .on_action(cx.listener(|this, _: &TogglePlay, _, cx| {
            this.app
                .video
                .update(cx, super::state::video::VideoController::toggle_play);
        }))
        .on_action(cx.listener(|this, _: &SeekBack, _, cx| {
            this.app.video.update(cx, |video, cx| {
                video.seek_by(-crate::state::video::SEEK_STEP, cx);
            });
        }))
        .on_action(cx.listener(|this, _: &SeekForward, _, cx| {
            this.app.video.update(cx, |video, cx| {
                video.seek_by(crate::state::video::SEEK_STEP, cx);
            });
        }))
        .on_action(cx.listener(|this, _: &ToggleMute, _, cx| {
            this.app
                .video
                .update(cx, super::state::video::VideoController::toggle_mute);
        }))
        .on_action(cx.listener(|this, _: &ToggleSlowMotion, _, cx| {
            this.app
                .video
                .update(cx, super::state::video::VideoController::toggle_slow_motion);
        }))
        .on_action(cx.listener(|this, _: &ToggleContinuous, _, cx| {
            this.app
                .video
                .update(cx, super::state::video::VideoController::toggle_continuous);
        }))
        .on_action(cx.listener(|this, _: &ToggleVideoFullscreen, window, cx| {
            this.toggle_video_fullscreen(window, cx);
        }))
        // Escape: leave fullscreen first, then a corner focus.
        .on_action(cx.listener(|this, _: &ExitFullscreen, window, cx| {
            if this.preferences.is_some() {
                this.close_preferences(window, cx);
            } else if this.stage.is_some() || window.is_fullscreen() {
                this.exit_video_fullscreen(window, cx);
            } else if !this.unfocus_corner(cx) {
                cx.propagate();
            }
        }))
        .on_action(cx.listener(|this, _: &ComposeLayout1, _, cx| {
            this.set_compose(ComposeLayout::Split, cx);
        }))
        .on_action(cx.listener(|this, _: &ComposeLayout2, _, cx| {
            this.set_compose(ComposeLayout::PrimaryWithReferenceInset, cx);
        }))
        .on_action(cx.listener(|this, _: &ComposeLayout3, _, cx| {
            this.set_compose(ComposeLayout::ReferenceWithPrimaryInset, cx);
        }))
        .on_action(cx.listener(|this, _: &ComposeLayout4, _, cx| {
            this.set_compose(ComposeLayout::PrimaryOnly, cx);
        }))
        .on_action(cx.listener(|this, _: &ComposeLayout5, _, cx| {
            this.set_compose(ComposeLayout::ReferenceOnly, cx);
        }))
        // Laps and traces.
        .on_action(cx.listener(|this, _: &SwapRoles, _, cx| {
            this.app
                .session
                .update(cx, super::state::session::Session::swap);
        }))
        .on_action(cx.listener(|this, _: &PrevLap, _, cx| {
            this.app
                .session
                .update(cx, super::state::session::Session::prev_lap);
        }))
        .on_action(cx.listener(|this, _: &NextLap, _, cx| {
            this.app
                .session
                .update(cx, super::state::session::Session::next_lap);
        }))
        .on_action(cx.listener(|this, action: &SelectLap, _, cx| {
            let lap_ref = LapRef::new(action.session.clone(), action.lap);
            this.app
                .session
                .update(cx, |session, cx| session.set_lap(action.role, lap_ref, cx));
        }))
        .on_action(cx.listener(|this, action: &RevealRecording, _, cx| {
            let source = this.app.library.read(cx).source(&action.session);
            if let Some(source) = source {
                cx.reveal_path(source.file.path());
            }
        }))
        .on_action(cx.listener(|this, _: &PrevCorner, _, cx| this.step_corner(-1, cx)))
        .on_action(cx.listener(|this, _: &NextCorner, _, cx| this.step_corner(1, cx)))
        .on_action(cx.listener(|this, action: &FocusCorner, _, cx| {
            let ix = this.app.session.read(cx).analysis().and_then(|analysis| {
                analysis
                    .corners()
                    .iter()
                    .position(|zone| zone.id == action.id.as_ref())
            });
            if let Some(ix) = ix {
                this.focus_corner(ix, cx);
            }
        }))
        .on_action(cx.listener(|this, _: &ZoomIn, _, cx| this.zoom(Some(0.5), cx)))
        .on_action(cx.listener(|this, _: &ZoomOut, _, cx| this.zoom(Some(2.0), cx)))
        .on_action(cx.listener(|this, _: &ZoomReset, _, cx| this.zoom(None, cx)))
        .on_action(cx.listener(|this, _: &ToggleXAxis, _, cx| {
            this.app
                .viewport
                .update(cx, omatrack_trace::ViewportState::toggle_axis);
            let axis = match this.app.viewport.read(cx).axis() {
                omatrack_trace::XAxis::Distance => omatrack_library::config::XAxis::Distance,
                omatrack_trace::XAxis::Time => omatrack_library::config::XAxis::Time,
            };
            this.app.preferences.update(cx, |preferences, cx| {
                preferences.update(cx, |config| config.trace.x_axis = Some(axis));
            });
        }))
        .on_action(cx.listener(|this, _: &ToggleFit, _, cx| {
            this.app.preferences.update(cx, |preferences, cx| {
                preferences.update(cx, |config| {
                    let fit = config.trace.is_fitting_channels();
                    config.trace.fit_channels = Some(!fit);
                });
            });
        }))
        .on_action(cx.listener(|this, _: &ResizeLanes, window, cx| {
            this.set_trace_mode(TraceMode::ResizingLanes, window, cx);
        }))
        .on_action(cx.listener(|this, _: &ToggleCornerEdit, window, cx| {
            this.set_trace_mode(TraceMode::EditingCorners, window, cx);
        }))
    }

    fn set_compose(&mut self, layout: ComposeLayout, cx: &mut Context<'_, Self>) {
        self.app
            .video
            .update(cx, |video, cx| video.set_layout(layout, cx));
    }
}

/// Notification identities (a newer one replaces an older one).
struct LayoutNotice;
struct PreferencesNotice;
struct VideoNotice;

impl Focusable for Workspace {
    fn focus_handle(&self, _: &gpui_kit::App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for Workspace {
    fn render(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        // gpui-component 0.6.6's `Root` does not mount its overlay layers; the
        // window's first view renders them, above everything else.
        let sheets = Root::render_sheet_layer(window, cx);
        let dialogs = Root::render_dialog_layer(window, cx);
        let notifications = Root::render_notification_layer(window, cx);
        let root = div()
            .id("workspace")
            .key_context(WORKSPACE_CONTEXT)
            .track_focus(&self.focus_handle);
        let root = Self::register_actions(root, cx)
            .relative()
            .flex()
            .flex_col()
            .size_full()
            .bg(cx.theme().background)
            .text_color(cx.theme().foreground);
        // Preferences replace the dock area and status bar; both entities
        // stay alive (and untouched) behind the screen.
        // The video stage replaces every chrome, the Preferences screen the
        // dock area and status bar; the entities behind stay alive and
        // untouched.
        let root = if self.stage.is_some() {
            root.bg(gpui_kit::black()).child(
                div()
                    .id("workspace-stage")
                    .test_support()
                    .flex_1()
                    .min_h_0()
                    .child(self.panels.video.clone()),
            )
        } else {
            match &self.preferences {
                Some(screen) => root
                    .child(crate::preferences::title_bar(
                        cx.listener(|this, _, window, cx| this.close_preferences(window, cx)),
                        window,
                        cx,
                    ))
                    .child(div().flex_1().min_h_0().child(screen.view.clone())),
                None => root
                    .child(self.render_header(window, cx))
                    .child(self.filmstrip.clone())
                    .child(
                        div()
                            .id("workspace-dock")
                            .test_support()
                            .flex_1()
                            .min_h_0()
                            .child(self.dock_area.clone()),
                    )
                    .child(self.status.clone()),
            }
        };
        root.children(sheets)
            .children(dialogs)
            .children(notifications)
    }
}

/// The corner H (`step` -1) or J (`step` 1) moves to, given the corners'
/// start fractions in lap order: the neighbour of the focused corner, else
/// the first corner after (J) or before (H) the cursor. There is no wrap:
/// past the last corner J does nothing, before the first H does nothing.
/// Without a cursor the playhead is at the lap start, so J focuses the
/// first corner and H has nowhere to go.
fn step_corner_ix(
    starts: &[f64],
    focused: Option<usize>,
    cursor: Option<f64>,
    step: isize,
) -> Option<usize> {
    if let Some(ix) = focused.filter(|ix| *ix < starts.len()) {
        return ix.checked_add_signed(step).filter(|ix| *ix < starts.len());
    }
    match (cursor, step > 0) {
        (None, true) => (!starts.is_empty()).then_some(0),
        (None, false) => None,
        (Some(cursor), true) => starts.iter().position(|start| *start > cursor),
        (Some(cursor), false) => starts.iter().rposition(|start| *start < cursor),
    }
}

#[cfg(test)]
mod tests {
    use super::step_corner_ix;

    const STARTS: [f64; 3] = [0.1, 0.4, 0.7];

    #[test]
    fn stepping_from_a_focused_corner_does_not_wrap() {
        assert_eq!(step_corner_ix(&STARTS, Some(0), Some(0.5), 1), Some(1));
        assert_eq!(step_corner_ix(&STARTS, Some(1), None, -1), Some(0));
        assert_eq!(step_corner_ix(&STARTS, Some(2), Some(0.5), 1), None);
        assert_eq!(step_corner_ix(&STARTS, Some(0), Some(0.5), -1), None);
    }

    #[test]
    fn stepping_from_the_cursor_does_not_wrap() {
        assert_eq!(step_corner_ix(&STARTS, None, Some(0.5), 1), Some(2));
        assert_eq!(step_corner_ix(&STARTS, None, Some(0.5), -1), Some(1));
        // Past the last corner J stays; before the first H stays.
        assert_eq!(step_corner_ix(&STARTS, None, Some(0.9), 1), None);
        assert_eq!(step_corner_ix(&STARTS, None, Some(0.05), -1), None);
        // A cursor exactly on a corner start moves past it.
        assert_eq!(step_corner_ix(&STARTS, None, Some(0.4), 1), Some(2));
        assert_eq!(step_corner_ix(&STARTS, None, Some(0.4), -1), Some(0));
    }

    #[test]
    fn without_a_cursor_the_lap_start_is_the_playhead() {
        assert_eq!(step_corner_ix(&STARTS, None, None, 1), Some(0));
        assert_eq!(step_corner_ix(&STARTS, None, None, -1), None);
        assert_eq!(step_corner_ix(&[], None, None, 1), None);
    }

    #[test]
    fn a_stale_focus_falls_back_to_the_cursor() {
        // The focused index is from an analysis with more corners.
        assert_eq!(step_corner_ix(&STARTS, Some(5), Some(0.5), 1), Some(2));
    }
}
