//! The dock layout: the default arrangement and its persistence in
//! `omatrack.yml` under `workspace.layout`.

use gpui_kit::component::dock::{DockArea, DockAreaState, DockLayout, DockPlacement};
use gpui_kit::{App, Context, Entity, Pixels, Window};

use crate::panels::{PanelKind, WorkspacePanels, provide, withdraw};

/// Persistence id of the workspace dock area.
pub const DOCK_AREA_ID: &str = "omatrack.workspace";
/// Written into every saved layout. A saved layout with another version is
/// replaced by the default layout (with a notification saying so).
///
/// 3: the map got its own pane on the right instead of a tab behind the
/// inspector.
/// 4: the inspector is a tab beside the map (the trace gutter already reads
/// the cursor), so the right dock holds two surfaces, not three.
pub const LAYOUT_VERSION: usize = 4;

/// Library dock width, in rems (300 px at the default 16 px base).
pub(crate) const LEFT_DOCK_REMS: f32 = 18.75;
/// Inspector dock width, in rems (380 px at the default base).
pub(crate) const RIGHT_DOCK_REMS: f32 = 23.75;
/// Video pane height above the traces, in rems (the traces take the rest).
pub(crate) const VIDEO_REMS: f32 = 22.5;
/// Map | Inspector pane height in the right dock, in rems (400 px at the
/// default base); the tables above take the rest.
pub(crate) const MAP_REMS: f32 = 25.0;

/// Narrowest window, in rems (1440 px at the default base), whose default
/// layout opens the Library dock too: below it the traces would get less
/// than half the width, so the Library starts closed (ctrl-b opens it).
pub(crate) const LIBRARY_OPEN_MIN_REMS: f32 = 90.0;
/// The right dock never takes more than this share of the window.
pub(crate) const RIGHT_DOCK_MAX_SHARE: f32 = 0.3;

/// Default dock widths for a window `width` wide: (library open, library
/// width, right dock width).
pub(crate) fn default_dock_widths(width: Pixels, rem: Pixels) -> (bool, Pixels, Pixels) {
    let library = width >= rem * LIBRARY_OPEN_MIN_REMS;
    let right = (rem * RIGHT_DOCK_REMS).min(width * RIGHT_DOCK_MAX_SHARE);
    (library, rem * LEFT_DOCK_REMS, right)
}

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
/// [Corners | Laps | Channels] over [Map | Inspector] on the right: two
/// surfaces, each tall enough not to clip (the trace gutter carries the
/// cursor readouts, so the inspector can sit behind the map).
pub(crate) fn apply_default(
    area: &Entity<DockArea>,
    panels: &WorkspacePanels,
    window: &mut Window,
    cx: &mut App,
) {
    // The dock stores pixels; derive them from the rem scale so the default
    // layout follows the theme's base font.
    let rem = window.rem_size();
    let (library_open, left_width, right_width) =
        default_dock_widths(window.viewport_size().width, rem);
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
        .child(
            tabs(&[PanelKind::Map, PanelKind::Inspector], cx),
            Some(rem * MAP_REMS),
        );
    area.update(cx, |area, cx| {
        area.set_version(Some(LAYOUT_VERSION), cx);
        area.set_center(center, window, cx);
        area.set_dock(DockPlacement::Left, left, window, cx);
        area.set_dock(DockPlacement::Right, right, window, cx);
        area.set_dock_size(DockPlacement::Left, left_width, window, cx);
        area.set_dock_size(DockPlacement::Right, right_width, window, cx);
        if area.is_dock_open(DockPlacement::Left) != library_open {
            area.toggle_dock(DockPlacement::Left, window, cx);
        }
        if !area.is_dock_open(DockPlacement::Right) {
            area.toggle_dock(DockPlacement::Right, window, cx);
        }
    });
}

/// Re-fit the default layout's docks to the window's width (see
/// [`default_dock_widths`]): the window may open at one size and settle at
/// another before the user has touched a dock.
pub(crate) fn fit_default_docks(area: &Entity<DockArea>, window: &mut Window, cx: &mut App) {
    let (library_open, left_width, right_width) =
        default_dock_widths(window.viewport_size().width, window.rem_size());
    area.update(cx, |area, cx| {
        area.set_dock_size(DockPlacement::Left, left_width, window, cx);
        area.set_dock_size(DockPlacement::Right, right_width, window, cx);
        if area.has_dock(DockPlacement::Left)
            && area.is_dock_open(DockPlacement::Left) != library_open
        {
            area.toggle_dock(DockPlacement::Left, window, cx);
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

#[cfg(test)]
mod tests {
    use super::*;
    use gpui_kit::px;

    #[test]
    fn narrow_windows_keep_at_least_half_the_width_for_the_center() {
        let rem = px(16.);
        for width in [1024., 1280., 1366., 1440., 1920., 2560.] {
            let width = px(width);
            let (library, left, right) = default_dock_widths(width, rem);
            let docks = right + if library { left } else { px(0.) };
            assert!(
                width - docks >= width * 0.5,
                "{width:?}: the center keeps half the window"
            );
        }
        assert!(
            !default_dock_widths(px(1280.), rem).0,
            "Library closed at 1280"
        );
        assert!(
            default_dock_widths(px(1920.), rem).0,
            "Library open at 1920"
        );
        assert_eq!(default_dock_widths(px(1920.), rem).2, rem * RIGHT_DOCK_REMS);
    }
}
