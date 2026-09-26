//! The window's root view: title bar, dock workspace, status bar and the
//! overlay layers, and the one place every workspace action is routed to
//! the entity that owns it.

pub mod header;
pub mod layout;
pub mod status;

use std::rc::Rc;
use std::time::Duration;

use gpui_kit::component::{
    ActiveTheme as _, IndexPath, Root, Theme, WindowExt as _,
    dock::{DockArea, DockEvent, DockPlacement, DockSkin},
    notification::Notification,
    select::{SelectEvent, SelectState},
};
use gpui_kit::{
    AppContext as _, Context, Entity, FocusHandle, Focusable, InteractiveElement as _, IntoElement,
    ParentElement as _, Render, SharedString, Styled as _, Subscription, Task, TestSupportExt as _,
    Window, div,
};
use omatrack_trace::{Selection, Viewport};
use omatrack_ui::theme::ThemeStatus;

use crate::actions::*;
use crate::commands::Palette;
use crate::keymap::WORKSPACE_CONTEXT;
use crate::panels::{PanelKind, TraceMode, WorkspacePanels};
use crate::state::{
    AppState, ComposeLayout, LapRef, LibraryEvent, PreferencesEvent, SessionEvent, VideoEvent,
};

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
    status: Entity<StatusView>,
    sync_select: Entity<SelectState<Vec<SyncOption>>>,
    palette: Palette,
    layout_origin: LayoutOrigin,
    layout_save: Option<Task<()>>,
    video_fullscreen: bool,
    focused_corner: Option<usize>,
    /// The viewport before the first corner focus; Escape returns to it.
    pre_focus_viewport: Option<Viewport>,
    /// The cursor before the corner focus, restored with the viewport.
    pre_focus_cursor: Option<f64>,
    _subscriptions: Vec<Subscription>,
}

impl Workspace {
    /// Build the workspace from the installed [`AppState`], restoring the
    /// saved dock layout (or the default one).
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
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
        let sync_select = cx.new(|cx| {
            SelectState::new(
                vec![SyncOption::automatic()],
                Some(IndexPath::default()),
                window,
                cx,
            )
        });

        let subscriptions = vec![
            cx.observe_global::<Theme>(|_, cx| cx.notify()),
            cx.observe_global::<ThemeStatus>(|_, cx| cx.notify()),
            cx.observe(&app.session, |_, _, cx| cx.notify()),
            cx.subscribe(&dock_area, |this, _, event, cx| {
                if let DockEvent::LayoutChanged = event {
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
            cx.subscribe(&sync_select, |this, _, event, cx| {
                let SelectEvent::Confirm(value) = event;
                let request = SyncOption::request((*value).flatten());
                this.app
                    .session
                    .update(cx, |session, cx| session.set_strategy(request, cx));
            }),
        ];

        // A quit inside the layout debounce still saves the layout.
        cx.on_app_quit(|this: &mut Self, cx| {
            this.save_layout(cx);
            this.app
                .preferences
                .update(cx, |preferences, cx| preferences.flush(cx));
            async {}
        })
        .detach();

        // Start in the primary work area so single keys work at once.
        let traces = panels.focus_handle(PanelKind::Traces, cx);
        window.focus(&traces, cx);

        let mut workspace = Self {
            app,
            focus_handle: cx.focus_handle(),
            dock_area,
            _skin: skin,
            panels,
            status,
            sync_select,
            palette: Palette::default(),
            layout_origin,
            layout_save: None,
            video_fullscreen: false,
            focused_corner: None,
            pre_focus_viewport: None,
            pre_focus_cursor: None,
            _subscriptions: subscriptions,
        };
        workspace.sync_strategies(window, cx);
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

    pub fn is_video_fullscreen(&self) -> bool {
        self.video_fullscreen
    }

    fn on_session_event(
        &mut self,
        _: &Entity<crate::state::Session>,
        event: &SessionEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
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
                self.sync_strategies(window, cx);
                self.place_initial_cursor(cx);
            }
            SessionEvent::PrimaryChanged => self.forget_corner_focus(cx),
            SessionEvent::ReferenceChanged | SessionEvent::Swapped => {}
        }
    }

    fn on_library_event(
        &mut self,
        _: &Entity<crate::state::Library>,
        event: &LibraryEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
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
    fn place_initial_cursor(&mut self, cx: &mut Context<Self>) {
        let has_lap = self.app.session.read(cx).analysis().is_some();
        if has_lap && self.app.cursor.read(cx).fraction().is_none() {
            self.app
                .cursor
                .update(cx, |cursor, cx| cursor.set_fraction(Some(0.0), cx));
        }
    }

    /// Offer exactly the strategies both laps support.
    fn sync_strategies(&mut self, window: &mut Window, cx: &mut Context<Self>) {
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
        self.sync_select.update(cx, |select, cx| {
            select.set_items(options, window, cx);
            select.set_selected_value(&current, window, cx);
        });
    }

    fn schedule_layout_save(&mut self, cx: &mut Context<Self>) {
        self.layout_save = Some(cx.spawn(async move |this, cx| {
            cx.background_executor().timer(LAYOUT_SAVE_DEBOUNCE).await;
            let _ = this.update(cx, |this, cx| this.save_layout(cx));
        }));
    }

    /// Write the current dock layout to `workspace.layout` now.
    pub fn save_layout(&mut self, cx: &mut Context<Self>) {
        self.layout_save = None;
        let Some(layout) = layout::encode(self.dock_area.read(cx), cx) else {
            return;
        };
        self.app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| config.workspace.layout = Some(layout));
        });
    }

    pub(crate) fn toggle_palette(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let keys = self.focus_handle.clone();
        self.palette.toggle(&keys, window, cx);
    }

    /// Preferences open in a sheet ([`crate::preferences::open`]).
    pub(crate) fn open_preferences(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        crate::preferences::open(window, cx);
    }

    fn focus_panel(&mut self, kind: PanelKind, window: &mut Window, cx: &mut Context<Self>) {
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

    fn toggle_dock(
        &mut self,
        placement: DockPlacement,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.dock_area
            .update(cx, |area, cx| area.toggle_dock(placement, window, cx));
    }

    /// Focus corner `ix` of the analysis: centered in the left half with the
    /// 140 ms motion, its zone kept bright.
    fn focus_corner(&mut self, ix: usize, cx: &mut Context<Self>) {
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
            viewport.focus(zone.start, zone.end, true, cx)
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
    fn forget_corner_focus(&mut self, cx: &mut Context<Self>) {
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
    fn unfocus_corner(&mut self, cx: &mut Context<Self>) -> bool {
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
    fn step_corner(&mut self, step: isize, cx: &mut Context<Self>) {
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

    fn zoom(&mut self, factor: Option<f64>, cx: &mut Context<Self>) {
        let anchor = self.app.cursor.read(cx).fraction();
        self.app.viewport.update(cx, |viewport, cx| match factor {
            Some(factor) if factor < 1.0 => viewport.zoom_in(anchor, cx),
            Some(_) => viewport.zoom_out(anchor, cx),
            None => viewport.reset(cx),
        });
    }

    fn toggle_video_fullscreen(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.video_fullscreen {
            self.exit_video_fullscreen(window, cx);
            return;
        }
        let Some(group) = self.panels.video.read(cx).group() else {
            return;
        };
        let node = group.read(cx).node();
        self.dock_area
            .update(cx, |area, cx| area.set_zoomed_in(node, window, cx));
        self.video_fullscreen = self.dock_area.read(cx).is_zoomed();
        if self.video_fullscreen && !window.is_fullscreen() {
            window.toggle_fullscreen();
        }
        cx.notify();
    }

    fn exit_video_fullscreen(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.dock_area
            .update(cx, |area, cx| area.set_zoomed_out(window, cx));
        if window.is_fullscreen() {
            window.toggle_fullscreen();
        }
        self.video_fullscreen = false;
        cx.notify();
    }

    fn set_trace_mode(&mut self, mode: TraceMode, window: &mut Window, cx: &mut Context<Self>) {
        self.panels
            .traces
            .update(cx, |traces, cx| traces.toggle_mode(mode, cx));
        // The editors take keyboard focus so Ctrl+S / Escape reach them.
        if self.panels.traces.read(cx).mode() != TraceMode::Browse {
            self.focus_panel(PanelKind::Traces, window, cx);
        }
    }

    fn register_actions(
        &self,
        root: gpui_kit::Stateful<gpui_kit::Div>,
        cx: &mut Context<Self>,
    ) -> gpui_kit::Stateful<gpui_kit::Div> {
        root.on_action(
            cx.listener(|this, _: &TogglePalette, window, cx| this.toggle_palette(window, cx)),
        )
        .on_action(
            cx.listener(|this, _: &OpenPreferences, window, cx| this.open_preferences(window, cx)),
        )
        .on_action(cx.listener(|this, _: &OpenFolder, _, cx| {
            this.app
                .library
                .update(cx, |library, cx| library.prompt_add_folder(cx))
        }))
        .on_action(cx.listener(|this, _: &Rescan, _, cx| {
            this.app
                .library
                .update(cx, |library, cx| library.rescan(cx))
        }))
        .on_action(cx.listener(|this, _: &ToggleLibrary, window, cx| {
            this.toggle_dock(DockPlacement::Left, window, cx)
        }))
        .on_action(cx.listener(|this, _: &ToggleInspector, window, cx| {
            this.toggle_dock(DockPlacement::Right, window, cx)
        }))
        .on_action(cx.listener(|this, _: &ResetLayout, window, cx| {
            layout::apply_default(&this.dock_area, &this.panels, window, cx);
            this.layout_origin = LayoutOrigin::Default;
            window.push_notification(
                Notification::info("The default layout is restored.").id::<LayoutNotice>(),
                cx,
            );
        }))
        .on_action(cx.listener(|this, _: &FocusPanel1, window, cx| {
            this.focus_panel(PanelKind::Library, window, cx)
        }))
        .on_action(cx.listener(|this, _: &FocusPanel2, window, cx| {
            this.focus_panel(PanelKind::Traces, window, cx)
        }))
        .on_action(cx.listener(|this, _: &FocusPanel3, window, cx| {
            this.focus_panel(PanelKind::Video, window, cx)
        }))
        .on_action(cx.listener(|this, _: &FocusPanel4, window, cx| {
            this.focus_panel(PanelKind::Corners, window, cx)
        }))
        .on_action(cx.listener(|this, _: &FocusPanel5, window, cx| {
            this.focus_panel(PanelKind::Laps, window, cx)
        }))
        .on_action(cx.listener(|this, _: &FocusPanel6, window, cx| {
            this.focus_panel(PanelKind::Map, window, cx)
        }))
        // Playback.
        .on_action(cx.listener(|this, _: &TogglePlay, _, cx| {
            this.app.video.update(cx, |video, cx| video.toggle_play(cx))
        }))
        .on_action(cx.listener(|this, _: &SeekBack, _, cx| {
            this.app.video.update(cx, |video, cx| {
                video.seek_by(-crate::state::video::SEEK_STEP, cx)
            })
        }))
        .on_action(cx.listener(|this, _: &SeekForward, _, cx| {
            this.app.video.update(cx, |video, cx| {
                video.seek_by(crate::state::video::SEEK_STEP, cx)
            })
        }))
        .on_action(cx.listener(|this, _: &ToggleMute, _, cx| {
            this.app.video.update(cx, |video, cx| video.toggle_mute(cx))
        }))
        .on_action(cx.listener(|this, _: &ToggleSlowMotion, _, cx| {
            this.app
                .video
                .update(cx, |video, cx| video.toggle_slow_motion(cx))
        }))
        .on_action(cx.listener(|this, _: &ToggleContinuous, _, cx| {
            this.app
                .video
                .update(cx, |video, cx| video.toggle_continuous(cx))
        }))
        .on_action(cx.listener(|this, _: &ToggleVideoFullscreen, window, cx| {
            this.toggle_video_fullscreen(window, cx)
        }))
        // Escape: leave fullscreen first, then a corner focus.
        .on_action(cx.listener(|this, _: &ExitFullscreen, window, cx| {
            if this.video_fullscreen || window.is_fullscreen() {
                this.exit_video_fullscreen(window, cx);
            } else if !this.unfocus_corner(cx) {
                cx.propagate();
            }
        }))
        .on_action(
            cx.listener(|this, _: &ComposeLayout1, _, cx| {
                this.set_compose(ComposeLayout::Split, cx)
            }),
        )
        .on_action(cx.listener(|this, _: &ComposeLayout2, _, cx| {
            this.set_compose(ComposeLayout::PrimaryWithReferenceInset, cx)
        }))
        .on_action(cx.listener(|this, _: &ComposeLayout3, _, cx| {
            this.set_compose(ComposeLayout::ReferenceWithPrimaryInset, cx)
        }))
        .on_action(cx.listener(|this, _: &ComposeLayout4, _, cx| {
            this.set_compose(ComposeLayout::PrimaryOnly, cx)
        }))
        .on_action(cx.listener(|this, _: &ComposeLayout5, _, cx| {
            this.set_compose(ComposeLayout::ReferenceOnly, cx)
        }))
        // Laps and traces.
        .on_action(cx.listener(|this, _: &SwapRoles, _, cx| {
            this.app.session.update(cx, |session, cx| session.swap(cx))
        }))
        .on_action(cx.listener(|this, _: &PrevLap, _, cx| {
            this.app
                .session
                .update(cx, |session, cx| session.prev_lap(cx))
        }))
        .on_action(cx.listener(|this, _: &NextLap, _, cx| {
            this.app
                .session
                .update(cx, |session, cx| session.next_lap(cx))
        }))
        .on_action(cx.listener(|this, action: &SelectLap, _, cx| {
            let lap_ref = LapRef::new(action.session.clone(), action.lap);
            this.app
                .session
                .update(cx, |session, cx| session.set_lap(action.role, lap_ref, cx))
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
                .update(cx, |viewport, cx| viewport.toggle_axis(cx));
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
            this.set_trace_mode(TraceMode::ResizingLanes, window, cx)
        }))
        .on_action(cx.listener(|this, _: &ToggleCornerEdit, window, cx| {
            this.set_trace_mode(TraceMode::EditingCorners, window, cx)
        }))
    }

    fn set_compose(&mut self, layout: ComposeLayout, cx: &mut Context<Self>) {
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
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        // gpui-component 0.6.6's `Root` does not mount its overlay layers; the
        // window's first view renders them, above everything else.
        let sheets = Root::render_sheet_layer(window, cx);
        let dialogs = Root::render_dialog_layer(window, cx);
        let notifications = Root::render_notification_layer(window, cx);
        let root = div()
            .id("workspace")
            .key_context(WORKSPACE_CONTEXT)
            .track_focus(&self.focus_handle);
        self.register_actions(root, cx)
            .relative()
            .flex()
            .flex_col()
            .size_full()
            .bg(cx.theme().background)
            .text_color(cx.theme().foreground)
            .child(self.render_header(window, cx))
            .child(
                div()
                    .id("workspace-dock")
                    .test_support()
                    .flex_1()
                    .min_h_0()
                    .child(self.dock_area.clone()),
            )
            .child(self.status.clone())
            .children(sheets)
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
