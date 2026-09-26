//! The dock layout: the default arrangement and its persistence in
//! `omatrack.yml` under `workspace.layout`.

use gpui_kit::component::dock::{DockArea, DockAreaState, DockLayout, DockPlacement};
use gpui_kit::{App, Context, Entity, Window};

use crate::panels::{PanelKind, WorkspacePanels, provide, withdraw};

/// Persistence id of the workspace dock area.
pub const DOCK_AREA_ID: &str = "omatrack.workspace";
/// Written into every saved layout. A saved layout with another version is
/// replaced by the default layout (with a notification saying so).
///
/// 3: the map got its own pane on the right instead of a tab behind the
/// inspector.
pub const LAYOUT_VERSION: usize = 3;

/// Library dock width, in rems (300 px at the default 16 px base).
pub(crate) const LEFT_DOCK_REMS: f32 = 18.75;
/// Inspector dock width, in rems (380 px at the default base).
pub(crate) const RIGHT_DOCK_REMS: f32 = 23.75;
/// Video pane height above the traces, in rems (the traces take the rest).
pub(crate) const VIDEO_REMS: f32 = 22.5;
/// Map pane height in the right dock, in rems (220 px at the default base).
pub(crate) const MAP_REMS: f32 = 13.75;
/// Inspector pane height in the right dock, in rems (240 px at the default
/// base); the tables above take the rest.
pub(crate) const INSPECTOR_REMS: f32 = 15.0;

/// How a layout came to be on screen.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LayoutOrigin {
    /// Nothing was saved: the default layout.
    Default,
    /// The saved layout.
    Restored,
    /// A saved layout could not be used; the default replaced it. The
    /// message says why (user-facing).
    Replaced(String),
}

/// Where a panel lives in the default layout.
pub(crate) fn default_placement(kind: PanelKind) -> DockPlacement {
    match kind {
        PanelKind::Library => DockPlacement::Left,
        PanelKind::Traces | PanelKind::Video => DockPlacement::Center,
        PanelKind::Corners
        | PanelKind::Laps
        | PanelKind::Channels
        | PanelKind::Inspector
        | PanelKind::Map => DockPlacement::Right,
    }
}

/// Library on the left; video over traces in the center;
/// [Corners | Laps | Channels] over Map over Inspector on the right, so the
/// map and the cursor readouts are both visible without switching tabs.
pub(crate) fn apply_default(
    area: &Entity<DockArea>,
    panels: &WorkspacePanels,
    window: &mut Window,
    cx: &mut App,
) {
    // The dock stores pixels; derive them from the rem scale so the default
    // layout follows the theme's base font.
    let rem = window.rem_size();
    let tabs = |kinds: &[PanelKind], cx: &App| {
        kinds.iter().fold(DockLayout::tabs(), |layout, kind| {
            layout.panel_view(panels.handle(*kind), cx)
        })
    };
    let center = DockLayout::v_split()
        .child(tabs(&[PanelKind::Video], cx), Some(rem * VIDEO_REMS))
        .child(tabs(&[PanelKind::Traces], cx), None);
    let left = tabs(&[PanelKind::Library], cx);
    let right = DockLayout::v_split()
        .child(
            tabs(
                &[PanelKind::Corners, PanelKind::Laps, PanelKind::Channels],
                cx,
            ),
            None,
        )
        .child(tabs(&[PanelKind::Map], cx), Some(rem * MAP_REMS))
        .child(
            tabs(&[PanelKind::Inspector], cx),
            Some(rem * INSPECTOR_REMS),
        );
    area.update(cx, |area, cx| {
        area.set_version(Some(LAYOUT_VERSION), cx);
        area.set_center(center, window, cx);
        area.set_dock(DockPlacement::Left, left, window, cx);
        area.set_dock(DockPlacement::Right, right, window, cx);
        area.set_dock_size(DockPlacement::Left, rem * LEFT_DOCK_REMS, window, cx);
        area.set_dock_size(DockPlacement::Right, rem * RIGHT_DOCK_REMS, window, cx);
        if !area.is_dock_open(DockPlacement::Left) {
            area.toggle_dock(DockPlacement::Left, window, cx);
        }
        if !area.is_dock_open(DockPlacement::Right) {
            area.toggle_dock(DockPlacement::Right, window, cx);
        }
    });
}

/// The saved layout, validated: parsed, of this version, with a center.
fn decode(saved: &serde_json::Value) -> Result<DockAreaState, String> {
    let state: DockAreaState = serde_json::from_value(saved.clone())
        .map_err(|_| "The saved panel layout couldn’t be read.".to_string())?;
    if state.version != Some(LAYOUT_VERSION) {
        return Err("The saved panel layout is from another version.".to_string());
    }
    Ok(state)
}

/// Put the saved layout on screen, or the default one.
pub(crate) fn restore(
    area: &Entity<DockArea>,
    panels: &WorkspacePanels,
    saved: Option<&serde_json::Value>,
    window: &mut Window,
    cx: &mut App,
) -> LayoutOrigin {
    let Some(saved) = saved else {
        apply_default(area, panels, window, cx);
        return LayoutOrigin::Default;
    };
    let state = match decode(saved) {
        Ok(state) => state,
        Err(reason) => {
            apply_default(area, panels, window, cx);
            return LayoutOrigin::Replaced(reason);
        }
    };
    provide(panels, cx);
    let loaded = area.update(cx, |area, cx| area.load(state, window, cx));
    // The builders ran synchronously; do not keep this workspace's panels
    // alive through the global.
    withdraw(cx);
    if loaded.is_err() || area.read(cx).layout(DockPlacement::Center).is_none() {
        apply_default(area, panels, window, cx);
        return LayoutOrigin::Replaced("The saved panel layout couldn’t be used.".to_string());
    }
    LayoutOrigin::Restored
}

/// The layout as it is written to `workspace.layout`.
pub(crate) fn encode(area: &DockArea, cx: &App) -> Option<serde_json::Value> {
    serde_json::to_value(area.dump(cx)).ok()
}

/// Whether the dock area currently holds `kind`'s panel.
pub(crate) fn holds(area: &DockArea, panels: &WorkspacePanels, kind: PanelKind, cx: &App) -> bool {
    let id = panels.handle(kind).panel_id(cx);
    area.panel(id).is_some()
}

/// Where `kind`'s panel is now (the user may have moved it), if the dock
/// area holds it.
pub(crate) fn current_placement(
    area: &DockArea,
    panels: &WorkspacePanels,
    kind: PanelKind,
    cx: &App,
) -> Option<DockPlacement> {
    let id = panels.handle(kind).panel_id(cx);
    [
        DockPlacement::Center,
        DockPlacement::Left,
        DockPlacement::Right,
        DockPlacement::Bottom,
    ]
    .into_iter()
    .find(|placement| {
        area.layout(*placement)
            .is_some_and(|tree| tree.find_panel_node(id).is_some())
    })
}

/// Make sure the dock holding `kind`'s panel is open: where the panel is
/// now, else where it goes by default.
pub(crate) fn reveal_dock(
    area: &mut DockArea,
    panels: &WorkspacePanels,
    kind: PanelKind,
    window: &mut Window,
    cx: &mut Context<DockArea>,
) {
    let placement =
        current_placement(area, panels, kind, cx).unwrap_or_else(|| default_placement(kind));
    if placement != DockPlacement::Center
        && area.has_dock(placement)
        && !area.is_dock_open(placement)
    {
        area.toggle_dock(placement, window, cx);
    }
}
