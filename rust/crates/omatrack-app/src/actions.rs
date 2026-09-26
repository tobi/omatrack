//! Every application command, in the `omatrack` action namespace.
//!
//! Actions are declared here and bound in `keymap.rs`, so a menu item,
//! toolbar button, command palette entry and key binding all dispatch the
//! same command. The workspace root routes each one to the entity that owns
//! it (see `workspace/mod.rs`).

use gpui_kit::SharedString;

gpui_kit::actions!(
    omatrack,
    [
        // Application and workspace.
        Quit,
        OpenFolder,
        Rescan,
        TogglePalette,
        OpenPreferences,
        ClosePreferences,
        PrevPreferencesSection,
        NextPreferencesSection,
        ToggleLibrary,
        ToggleInspector,
        ResetLayout,
        FocusPanel1,
        FocusPanel2,
        FocusPanel3,
        FocusPanel4,
        FocusPanel5,
        FocusPanel6,
        // Panels outside the default layout, opened into the right dock.
        ShowChannels,
        ShowMap,
        ShowInspector,
        // Video.
        TogglePlay,
        SeekBack,
        SeekForward,
        ToggleMute,
        ToggleVideoFullscreen,
        ExitFullscreen,
        ComposeLayout1,
        ComposeLayout2,
        ComposeLayout3,
        ComposeLayout4,
        ComposeLayout5,
        ToggleSlowMotion,
        ToggleContinuous,
        // Laps and traces.
        SwapRoles,
        ToggleCornerEdit,
        PrevCorner,
        NextCorner,
        ZoomIn,
        ZoomOut,
        ZoomReset,
        PrevLap,
        NextLap,
        ToggleXAxis,
        ToggleFit,
        ToggleTraceColorMode,
        ResizeLanes,
        // Trace view modes (`trace.view_mode`).
        ViewLap,
        ViewCorners,
        ViewConsistency,
        ViewEvents,
        SaveEdit,
        CancelEdit,
        // Library rows.
        SetPrimary,
        SetReference,
    ]
);

impl Eq for Quit {}
impl Eq for OpenFolder {}
impl Eq for Rescan {}
impl Eq for TogglePalette {}
impl Eq for OpenPreferences {}
impl Eq for ClosePreferences {}
impl Eq for PrevPreferencesSection {}
impl Eq for NextPreferencesSection {}
impl Eq for ToggleLibrary {}
impl Eq for ToggleInspector {}
impl Eq for ResetLayout {}
impl Eq for FocusPanel1 {}
impl Eq for FocusPanel2 {}
impl Eq for FocusPanel3 {}
impl Eq for FocusPanel4 {}
impl Eq for FocusPanel5 {}
impl Eq for FocusPanel6 {}
impl Eq for ShowChannels {}
impl Eq for ShowMap {}
impl Eq for ShowInspector {}
impl Eq for TogglePlay {}
impl Eq for SeekBack {}
impl Eq for SeekForward {}
impl Eq for ToggleMute {}
impl Eq for ToggleVideoFullscreen {}
impl Eq for ExitFullscreen {}
impl Eq for ComposeLayout1 {}
impl Eq for ComposeLayout2 {}
impl Eq for ComposeLayout3 {}
impl Eq for ComposeLayout4 {}
impl Eq for ComposeLayout5 {}
impl Eq for ToggleSlowMotion {}
impl Eq for ToggleContinuous {}
impl Eq for SwapRoles {}
impl Eq for ToggleCornerEdit {}
impl Eq for PrevCorner {}
impl Eq for NextCorner {}
impl Eq for ZoomIn {}
impl Eq for ZoomOut {}
impl Eq for ZoomReset {}
impl Eq for PrevLap {}
impl Eq for NextLap {}
impl Eq for ToggleXAxis {}
impl Eq for ToggleFit {}
impl Eq for ToggleTraceColorMode {}
impl Eq for ResizeLanes {}
impl Eq for ViewLap {}
impl Eq for ViewCorners {}
impl Eq for ViewConsistency {}
impl Eq for ViewEvents {}
impl Eq for SaveEdit {}
impl Eq for CancelEdit {}
impl Eq for SetPrimary {}
impl Eq for SetReference {}

/// The side of a comparison a lap is loaded into.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Role {
    Primary,
    Reference,
}

/// Load one lap of a library recording into a role. `session` is the
/// catalog session id (`trk:<slug>/d:<date>/s:<hash>`), `lap` the lap id.
#[derive(Debug, Clone, PartialEq, Eq, gpui_kit::Action)]
#[action(namespace = omatrack, no_json)]
pub struct SelectLap {
    pub session: SharedString,
    pub lap: i32,
    pub role: Role,
}

/// Focus one corner of the current analysis by its zone id (`T5`, an atlas
/// range id).
#[derive(Debug, Clone, PartialEq, Eq, gpui_kit::Action)]
#[action(namespace = omatrack, no_json)]
pub struct FocusCorner {
    pub id: SharedString,
}

/// Show a library recording in the platform file manager.
#[derive(Debug, Clone, PartialEq, Eq, gpui_kit::Action)]
#[action(namespace = omatrack, no_json)]
pub struct RevealRecording {
    pub session: SharedString,
}
