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

/// The side of a comparison a lap is loaded into.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Role {
    Primary,
    Reference,
}

/// Load one lap of a library recording into a role. `session` is the
/// catalog session id (`trk:<slug>/d:<date>/s:<hash>`), `lap` the lap id.
#[derive(Debug, Clone, PartialEq, gpui_kit::Action)]
#[action(namespace = omatrack, no_json)]
pub struct SelectLap {
    pub session: SharedString,
    pub lap: i32,
    pub role: Role,
}

/// Focus one corner of the current analysis by its zone id (`T5`, an atlas
/// range id).
#[derive(Debug, Clone, PartialEq, gpui_kit::Action)]
#[action(namespace = omatrack, no_json)]
pub struct FocusCorner {
    pub id: SharedString,
}

/// Show a library recording in the platform file manager.
#[derive(Debug, Clone, PartialEq, gpui_kit::Action)]
#[action(namespace = omatrack, no_json)]
pub struct RevealRecording {
    pub session: SharedString,
}
