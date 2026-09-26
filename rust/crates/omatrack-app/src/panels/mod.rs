//! The dock panels.
//!
//! Every panel is a real dock [`Panel`](gpui_kit::component::dock::Panel)
//! registered by name with [`register_panel`], so a saved layout rebuilds
//! it. Each panel module exposes `init(cx)` (its registration, actions and
//! key bindings) next to its `Panel` impl; [`init`] only calls them. Each workspace creates one entity per panel ([`WorkspacePanels`]);
//! the registered builders hand those same entities to the dock area while
//! it loads a layout.

pub mod channels;
pub mod corners;
pub mod inspector;
pub mod laps;
pub mod library;
pub mod map;
pub mod time_goes;
pub mod traces;
pub mod video;

use std::sync::Arc;

use gpui_kit::component::dock::{BasePanelView, panel_handle, register_panel};
use gpui_kit::component::{
    ActiveTheme as _, Icon, IconName,
    button::ButtonCustomVariant,
    empty::{Empty, EmptyDescription, EmptyHeader, EmptyMedia, EmptyMediaVariant, EmptyTitle},
};
use gpui_kit::{
    App, AppContext as _, ElementId, Entity, FocusHandle, Focusable, Global,
    InteractiveElement as _, IntoElement, ParentElement as _, SharedString,
    StatefulInteractiveElement as _, Styled as _, TestSupportExt as _, Window, div,
};

use crate::state::AppState;

pub use channels::ChannelsPanel;
pub use corners::CornersPanel;
pub use inspector::InspectorPanel;
pub use laps::LapsPanel;
pub use library::LibraryPanel;
pub use map::MapPanel;
pub use time_goes::TimeGoesPanel;
pub use traces::{TraceMode, TracesPanel};
pub use video::VideoPanel;

/// Which panel.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum PanelKind {
    Library,
    Traces,
    Video,
    Corners,
    Laps,
    Channels,
    Inspector,
    Map,
    TimeGoes,
}

impl PanelKind {
    pub const ALL: [Self; 9] = [
        Self::Library,
        Self::Traces,
        Self::Video,
        Self::Corners,
        Self::Laps,
        Self::Channels,
        Self::Inspector,
        Self::Map,
        Self::TimeGoes,
    ];

    /// The persisted panel name. Never change it: saved layouts use it.
    pub fn name(self) -> &'static str {
        match self {
            Self::Library => "omatrack.library",
            Self::Traces => "omatrack.traces",
            Self::Video => "omatrack.video",
            Self::Corners => "omatrack.corners",
            Self::Laps => "omatrack.laps",
            Self::Channels => "omatrack.channels",
            Self::Inspector => "omatrack.inspector",
            Self::Map => "omatrack.map",
            Self::TimeGoes => "omatrack.time_goes",
        }
    }

    /// The tab title.
    pub fn title(self) -> &'static str {
        match self {
            Self::Library => "Library",
            Self::Traces => "Traces",
            Self::Video => "Video",
            Self::Corners => "Corners",
            Self::Laps => "Laps",
            Self::Channels => "Channels",
            Self::Inspector => "Inspector",
            Self::Map => "Map",
            Self::TimeGoes => "Time lost",
        }
    }

    /// Whether the dock draws a title bar over this panel when it is alone in
    /// its group. The video and the traces carry their own chrome (the
    /// transport row, the corner ruler): the centre reads as one surface
    /// headed by its content, not by tab strips. A group of several panels
    /// still shows its tabs, so every panel stays reachable.
    pub fn has_title_bar(self) -> bool {
        !matches!(self, Self::Traces | Self::Video)
    }
}

/// One entity per panel, owned by a workspace. Built only by
/// [`WorkspacePanels::new`]; the fields are readable handles.
#[derive(Clone)]
#[non_exhaustive]
pub struct WorkspacePanels {
    pub library: Entity<LibraryPanel>,
    pub traces: Entity<TracesPanel>,
    pub video: Entity<VideoPanel>,
    pub corners: Entity<CornersPanel>,
    pub laps: Entity<LapsPanel>,
    pub channels: Entity<ChannelsPanel>,
    pub inspector: Entity<InspectorPanel>,
    pub map: Entity<MapPanel>,
    pub time_goes: Entity<TimeGoesPanel>,
}

impl WorkspacePanels {
    pub fn new(app: &AppState, window: &mut Window, cx: &mut App) -> Self {
        Self {
            library: cx.new(|cx| LibraryPanel::new(app.clone(), window, cx)),
            traces: cx.new(|cx| TracesPanel::new(app.clone(), cx)),
            video: cx.new(|cx| VideoPanel::new(app.clone(), window, cx)),
            corners: cx.new(|cx| CornersPanel::new(app.clone(), cx)),
            laps: cx.new(|cx| LapsPanel::new(app.clone(), cx)),
            channels: cx.new(|cx| ChannelsPanel::new(app.clone(), cx)),
            inspector: cx.new(|cx| InspectorPanel::new(app.clone(), cx)),
            map: cx.new(|cx| MapPanel::new(app.clone(), cx)),
            time_goes: cx.new(|cx| TimeGoesPanel::new(app.clone(), cx)),
        }
    }

    /// The dock handle of one panel (the presentation-aware wrapper).
    pub fn handle(&self, kind: PanelKind) -> Arc<dyn BasePanelView> {
        match kind {
            PanelKind::Library => panel_handle(self.library.clone()),
            PanelKind::Traces => panel_handle(self.traces.clone()),
            PanelKind::Video => panel_handle(self.video.clone()),
            PanelKind::Corners => panel_handle(self.corners.clone()),
            PanelKind::Laps => panel_handle(self.laps.clone()),
            PanelKind::Channels => panel_handle(self.channels.clone()),
            PanelKind::Inspector => panel_handle(self.inspector.clone()),
            PanelKind::Map => panel_handle(self.map.clone()),
            PanelKind::TimeGoes => panel_handle(self.time_goes.clone()),
        }
    }

    pub fn focus_handle(&self, kind: PanelKind, cx: &App) -> FocusHandle {
        match kind {
            PanelKind::Library => Focusable::focus_handle(&self.library, cx),
            PanelKind::Traces => Focusable::focus_handle(&self.traces, cx),
            PanelKind::Video => Focusable::focus_handle(&self.video, cx),
            PanelKind::Corners => Focusable::focus_handle(&self.corners, cx),
            PanelKind::Laps => Focusable::focus_handle(&self.laps, cx),
            PanelKind::Channels => Focusable::focus_handle(&self.channels, cx),
            PanelKind::Inspector => Focusable::focus_handle(&self.inspector, cx),
            PanelKind::Map => Focusable::focus_handle(&self.map, cx),
            PanelKind::TimeGoes => Focusable::focus_handle(&self.time_goes, cx),
        }
    }
}

/// The panels a dock area rebuilding a saved layout adopts (the builders
/// run synchronously inside `DockArea::load`).
struct LoadingPanels(WorkspacePanels);

impl Global for LoadingPanels {}

/// Make `panels` the ones registered builders return.
pub(crate) fn provide(panels: &WorkspacePanels, cx: &mut App) {
    cx.set_global(LoadingPanels(panels.clone()));
}

/// Stop handing out the panels of [`provide`].
pub(crate) fn withdraw(cx: &mut App) {
    if cx.has_global::<LoadingPanels>() {
        cx.remove_global::<LoadingPanels>();
    }
}

/// Initialize every panel module. Each module owns its own `init` (dock
/// registration plus its actions and bindings), so work on one panel never
/// needs to touch this list beyond adding a new module once.
pub(crate) fn init(cx: &mut App) {
    library::init(cx);
    traces::init(cx);
    video::init(cx);
    corners::init(cx);
    laps::init(cx);
    channels::init(cx);
    inspector::init(cx);
    map::init(cx);
    time_goes::init(cx);
}

/// Register `kind`'s dock builder for layout persistence: while a
/// workspace loads a saved layout the builder hands out that workspace's
/// entity (see [`provide`]).
pub(crate) fn register(kind: PanelKind, cx: &mut App) {
    register_panel(cx, kind.name(), move |_, window, cx| {
        if let Some(LoadingPanels(panels)) = cx.try_global::<LoadingPanels>() {
            return panels.handle(kind);
        }
        // No workspace is loading: build a fresh panel (only reachable
        // through a dock area this crate did not create).
        let app = AppState::global(cx).clone();
        WorkspacePanels::new(&app, window, cx).handle(kind)
    });
}

/// Emphasis of the selected segment's fill over the background.
const SELECTED_SEGMENT_FILL: f32 = 0.16;

/// The selected segment of an outline segmented control (`Lap | Corners`,
/// `Per lap | Continuous`): a clear neutral fill and full-strength label,
/// where the kit's outline selection is only a faint tint. Colour stays
/// for the lap roles and Δ.
pub(crate) fn selected_segment(cx: &App) -> ButtonCustomVariant {
    let theme = cx.theme();
    let fill = theme
        .background
        .blend(theme.foreground.opacity(SELECTED_SEGMENT_FILL));
    ButtonCustomVariant::new(cx)
        .color(theme.muted_foreground)
        .foreground(theme.foreground)
        .hover(fill)
        .active(fill)
}

/// An empty or waiting state: icon, title and one explanatory sentence.
pub(crate) fn empty_state(
    icon: IconName,
    title: impl Into<SharedString>,
    description: impl Into<SharedString>,
) -> Empty {
    Empty::new().header(
        EmptyHeader::new()
            .media(
                EmptyMedia::new()
                    .with_variant(EmptyMediaVariant::Icon)
                    .child(Icon::new(icon)),
            )
            .title(EmptyTitle::new().child(title.into()))
            .description(EmptyDescription::new().child(description.into())),
    )
}

/// A centered panel body with a stable id and an accessible status label.
pub(crate) fn panel_body(
    id: impl Into<ElementId>,
    label: impl Into<SharedString>,
    content: impl IntoElement,
    cx: &App,
) -> impl IntoElement {
    div()
        .id(id)
        .test_support()
        .aria_label(label.into())
        .size_full()
        .flex()
        .items_center()
        .justify_center()
        .p_4()
        .text_sm()
        .text_color(cx.theme().foreground)
        .child(content)
}

/// The honest empty state every analysis panel shows before a lap loads.
pub(crate) const SELECT_A_LAP: &str = "Select a primary lap in the Library.";

/// Implements the dock traits every simple panel shares.
macro_rules! simple_panel {
    ($ty:ty, $kind:expr) => {
        impl gpui_kit::component::dock::BasePanel for $ty {
            fn panel_name(&self) -> &'static str {
                $kind.name()
            }

            fn closable(&self, _: &gpui_kit::App) -> bool {
                false
            }
        }

        impl gpui_kit::component::dock::Panel for $ty {
            fn tab_name(&self, _: &gpui_kit::App) -> Option<gpui_kit::SharedString> {
                Some($kind.title().into())
            }

            fn title(
                &mut self,
                _: &mut gpui_kit::Window,
                _: &mut gpui_kit::Context<'_, Self>,
            ) -> impl gpui_kit::IntoElement {
                $kind.title()
            }

            fn title_bar(&self, _: &gpui_kit::App) -> bool {
                $kind.has_title_bar()
            }
        }

        impl gpui_kit::EventEmitter<gpui_kit::component::dock::PanelEvent> for $ty {}

        impl gpui_kit::Focusable for $ty {
            fn focus_handle(&self, _: &gpui_kit::App) -> gpui_kit::FocusHandle {
                self.focus_handle.clone()
            }
        }
    };
}

pub(crate) use simple_panel;

/// Render a panel body: `ready` with the current analysis, else the
/// loading, failure or empty state of the primary lap.
pub(crate) fn analysis_body(
    id: &'static str,
    app: &AppState,
    cx: &App,
    ready: impl FnOnce(&omatrack_core::session::Analysis) -> SharedString,
) -> gpui_kit::AnyElement {
    use crate::state::RoleState;
    let session = app.session.read(cx);
    if let Some(analysis) = session.analysis() {
        let summary = ready(analysis);
        return panel_body(id, summary.clone(), div().child(summary), cx).into_any_element();
    }
    let (icon, title, description): (IconName, SharedString, SharedString) =
        match session.primary().map(|slot| (slot, slot.state())) {
            Some((slot, RoleState::Loading)) => (
                IconName::LoaderCircle,
                format!("Loading {}", slot.info().label).into(),
                slot.info().title.clone(),
            ),
            Some((_, RoleState::Failed(message))) => (
                IconName::TriangleAlert,
                "The lap couldn’t be loaded".into(),
                message.clone(),
            ),
            Some((slot, RoleState::Loaded(_))) => (
                IconName::LoaderCircle,
                format!("Analysing {}", slot.info().label).into(),
                slot.info().title.clone(),
            ),
            None => (
                IconName::Inbox,
                "No lap selected".into(),
                SELECT_A_LAP.into(),
            ),
        };
    let label = SharedString::from(format!("{title}. {description}"));
    panel_body(id, label, empty_state(icon, title, description), cx).into_any_element()
}
