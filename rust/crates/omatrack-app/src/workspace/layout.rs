//! The dock layout: the default arrangement and its persistence in
//! `omatrack.yml` under `workspace.layout`.

use gpui_kit::component::dock::{DockArea, DockAreaState, DockLayout, DockPlacement};
use gpui_kit::{App, Context, Entity, Pixels, Window, px};

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
/// 5: the Laps sidebar leads the left dock with the Library tree the tab
/// beside it; the right dock is one tab group led by Time lost ("Where the
/// time goes": heat map, loss table, corner card); Corners, Channels, Map
/// and Inspector are tabs behind it.
/// 6: the docks scale with the window (the mockup's 290 / 350 px at
/// 1440 wide) and the focus keys follow the layout left to right.
/// 7: Time lost is the right dock's only panel (no tab strip); Corners,
/// Channels, Map and Inspector join it when opened (palette, Ctrl+4,
/// "Open in detail"). Narrow windows close the right dock first and keep
/// the Laps sidebar.
pub const LAYOUT_VERSION: usize = 7;

/// Left dock (Laps sidebar) share of the window width, between
/// [`LEFT_DOCK_MIN_REMS`] and [`LEFT_DOCK_MAX_REMS`] (288 px at 1440 wide,
/// 256 px at 1280, 360 px from 1800 wide at the default 16 px base).
pub(crate) const LEFT_DOCK_SHARE: f32 = 0.2;
pub(crate) const LEFT_DOCK_MIN_REMS: f32 = 15.0;
pub(crate) const LEFT_DOCK_MAX_REMS: f32 = 22.5;
/// Right dock (Where the time goes) share of the window width, between
/// [`RIGHT_DOCK_MIN_REMS`] and [`RIGHT_DOCK_MAX_REMS`] (360 px at 1440
/// wide, 440 px from 1760 wide), never above [`RIGHT_DOCK_MAX_SHARE`].
pub(crate) const RIGHT_DOCK_SHARE: f32 = 0.25;
pub(crate) const RIGHT_DOCK_MIN_REMS: f32 = 22.0;
pub(crate) const RIGHT_DOCK_MAX_REMS: f32 = 27.5;
/// Tallest default video pane above the traces, in rems (the traces take
/// the rest); see [`default_video_height`].
pub(crate) const VIDEO_REMS: f32 = 22.5;
/// Shortest default video pane, in rems.
pub(crate) const VIDEO_MIN_REMS: f32 = 12.0;
/// The video control row under the pictures, in rems (a small control plus
/// its padding).
pub(crate) const VIDEO_BAR_REMS: f32 = 2.25;
/// The onboard pictures' shape the default pane is fitted to (the panel
/// fits the real one inside it).
const VIDEO_ASPECT: f32 = 16.0 / 9.0;

/// Narrowest window, in rems (1400 px at the default base), whose default
/// layout opens the right dock: below it the traces would get less than
/// half the width with both docks, so the right dock closes first (ctrl-j
/// opens it).
pub(crate) const RIGHT_OPEN_MIN_REMS: f32 = 87.5;
/// Narrowest window, in rems (960 px), whose default layout opens the Laps
/// sidebar.
pub(crate) const LEFT_OPEN_MIN_REMS: f32 = 60.0;
/// The right dock never takes more than this share of the window.
pub(crate) const RIGHT_DOCK_MAX_SHARE: f32 = 0.3;

/// The default docks for one window width.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct DockWidths {
    pub left_open: bool,
    pub left: Pixels,
    pub right_open: bool,
    pub right: Pixels,
}

/// Default dock widths for a window `width` wide.
pub(crate) fn default_dock_widths(width: Pixels, rem: Pixels) -> DockWidths {
    let left = (width * LEFT_DOCK_SHARE).clamp(rem * LEFT_DOCK_MIN_REMS, rem * LEFT_DOCK_MAX_REMS);
    let right = (width * RIGHT_DOCK_SHARE)
        .clamp(rem * RIGHT_DOCK_MIN_REMS, rem * RIGHT_DOCK_MAX_REMS)
        .min(width * RIGHT_DOCK_MAX_SHARE);
    DockWidths {
        left_open: width >= rem * LEFT_OPEN_MIN_REMS,
        left,
        right_open: width >= rem * RIGHT_OPEN_MIN_REMS,
        right,
    }
}

/// Default video pane height for a window `width` wide: two split 16:9
/// pictures across the centre (the window less its open docks) plus the
/// control row, so the pictures meet the row without a band of empty pane,
/// between [`VIDEO_MIN_REMS`] and [`VIDEO_REMS`].
pub(crate) fn default_video_height(width: Pixels, rem: Pixels) -> Pixels {
    let docks = default_dock_widths(width, rem);
    let mut centre = width;
    if docks.left_open {
        centre -= docks.left;
    }
    if docks.right_open {
        centre -= docks.right;
    }
    let pictures = (centre - px(1.)) / 2. / VIDEO_ASPECT;
    (pictures + rem * VIDEO_BAR_REMS).clamp(rem * VIDEO_MIN_REMS, rem * VIDEO_REMS)
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
        PanelKind::Laps | PanelKind::Library => DockPlacement::Left,
        PanelKind::Traces | PanelKind::Video => DockPlacement::Center,
        PanelKind::TimeGoes
        | PanelKind::Corners
        | PanelKind::Channels
        | PanelKind::Inspector
        | PanelKind::Map => DockPlacement::Right,
    }
}

/// [Laps | Library] on the left; video over traces in the center; Time
/// lost alone on the right, the whole dock height for its map, table and
/// card. Corners, Channels, Map and Inspector are not placed until opened
/// (they then join the right dock as tabs).
pub(crate) fn apply_default(
    area: &Entity<DockArea>,
    panels: &WorkspacePanels,
    window: &mut Window,
    cx: &mut App,
) {
    // The dock stores pixels; derive them from the rem scale so the default
    // layout follows the theme's base font.
    let rem = window.rem_size();
    let width = window.viewport_size().width;
    let docks = default_dock_widths(width, rem);
    let tabs = |kinds: &[PanelKind], cx: &App| {
        kinds.iter().fold(DockLayout::tabs(), |layout, kind| {
            layout.panel_view(panels.handle(*kind), cx)
        })
    };
    let center = DockLayout::v_split()
        .child(
            tabs(&[PanelKind::Video], cx),
            Some(default_video_height(width, rem)),
        )
        .child(tabs(&[PanelKind::Traces], cx), None);
    let left = tabs(&[PanelKind::Laps, PanelKind::Library], cx);
    let right = tabs(&[PanelKind::TimeGoes], cx);
    area.update(cx, |area, cx| {
        area.set_version(Some(LAYOUT_VERSION), cx);
        area.set_center(center, window, cx);
        area.set_dock(DockPlacement::Left, left, window, cx);
        area.set_dock(DockPlacement::Right, right, window, cx);
        set_docks(area, docks, window, cx);
    });
}

/// Size the docks and open or close them to `docks`.
fn set_docks(
    area: &mut DockArea,
    docks: DockWidths,
    window: &mut Window,
    cx: &mut Context<DockArea>,
) {
    area.set_dock_size(DockPlacement::Left, docks.left, window, cx);
    area.set_dock_size(DockPlacement::Right, docks.right, window, cx);
    for (placement, open) in [
        (DockPlacement::Left, docks.left_open),
        (DockPlacement::Right, docks.right_open),
    ] {
        if area.has_dock(placement) && area.is_dock_open(placement) != open {
            area.toggle_dock(placement, window, cx);
        }
    }
}

/// Re-fit the default layout's docks to the window's width (see
/// [`default_dock_widths`]): the window may open at one size and settle at
/// another before the user has touched a dock.
pub(crate) fn fit_default_docks(area: &Entity<DockArea>, window: &mut Window, cx: &mut App) {
    let docks = default_dock_widths(window.viewport_size().width, window.rem_size());
    area.update(cx, |area, cx| set_docks(area, docks, window, cx));
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

    #[test]
    fn the_video_pane_holds_its_pictures_and_control_row() {
        let rem = px(16.);
        let at = |width: f32| default_video_height(px(width), rem);
        // 1440 wide: 792 px of centre, two 395.5 x 222.5 pictures + the row.
        let expected = (px(792.) - px(1.)) / 2. / VIDEO_ASPECT + rem * VIDEO_BAR_REMS;
        assert!((at(1440.) - expected).abs() < px(0.01));
        assert!(at(1440.) < rem * VIDEO_REMS, "no empty band at 1440");
        // Wide windows stop at the cap; narrow ones at the floor.
        assert_eq!(at(3840.), rem * VIDEO_REMS);
        assert_eq!(at(400.), rem * VIDEO_MIN_REMS);
        for width in [1280., 1440., 1920., 2560.] {
            assert!(at(width) >= rem * VIDEO_MIN_REMS && at(width) <= rem * VIDEO_REMS);
        }
    }

    #[test]
    fn narrow_windows_close_the_right_dock_first_and_keep_laps() {
        let rem = px(16.);
        for width in [1024., 1280., 1366., 1400., 1440., 1920., 2560.] {
            let width = px(width);
            let docks = default_dock_widths(width, rem);
            let taken = if docks.left_open { docks.left } else { px(0.) }
                + if docks.right_open {
                    docks.right
                } else {
                    px(0.)
                };
            assert!(
                width - taken >= width * 0.5,
                "{width:?}: the center keeps half the window"
            );
            assert!(docks.left_open, "{width:?}: Laps open");
            assert!(docks.left >= rem * LEFT_DOCK_MIN_REMS);
        }
        let at = |width: f32| default_dock_widths(px(width), rem);
        assert!(!at(1280.).right_open, "right dock closed at 1280");
        assert!(!at(1366.).right_open);
        assert!(at(1440.).right_open, "right dock open at 1440");
        assert_eq!(at(1280.).left, px(256.));
        // The mockup's proportions at 1440 wide; capped on wide windows.
        assert_eq!((at(1440.).left, at(1440.).right), (px(288.), px(360.)));
        assert_eq!(
            (at(1920.).left, at(1920.).right),
            (rem * LEFT_DOCK_MAX_REMS, rem * RIGHT_DOCK_MAX_REMS)
        );
    }
}
