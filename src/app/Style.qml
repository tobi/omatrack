pragma ComponentBehavior: Bound
pragma Singleton
import Omatrack

// Single source of truth for palette and typography.
//
// Colors come from the active Omarchy theme when the desktop provides one and
// fall back to a complete built-in dark palette (Everforest, which the fixed
// trace colors below already are) otherwise. The fallback is deliberately not
// the platform SystemPalette: a light Windows palette under dark video chrome
// looks broken, and the platform highlight is one color where this app needs
// two — `accent` for text/marks and `selection` as a row background — so
// relying on it painted the selected row's title in its own background color.
// Every component reads these instead of re-deriving them, which keeps one
// visual language and lets qmllint resolve the access.

import QtQuick

QtObject {
    id: style

    readonly property color accentColor: Theme.colors.accent || "#7fbbb3"
    readonly property color backgroundColor: Theme.colors.background || "#2d353b"
    readonly property color blueColor: Theme.colors.blue || "#7fbbb3"
    readonly property color borderColor: Theme.colors.muted || "#475258"
    readonly property color brakeTelemetryColor: "#e67e80"

    // Offered when picking a trace color; deliberately theme-independent so a
    // saved channel color keeps meaning across desktop themes.
    readonly property list<string> colorChoices: ["#a7c080", "#7fbbb3", "#e67e80", "#dbbc7f", "#d699b6", "#e09d7f", "#d3c6aa", "#9da9a0"]

    // ── density ─────────────────────────────────────────────────────
    // Material's own metrics are sized for touch. These are the compact
    // desktop values every control in the app is sized against, so density
    // is one edit here rather than a number per component.
    readonly property int controlHeight: 26
    readonly property int controlPadding: 8
    readonly property color darkBackgroundColor: Theme.colors.dark_background || "#272e33"
    readonly property color dimTextColor: Theme.colors.dark_foreground || "#7a8478"
    readonly property color dropOverlayColor: Qt.rgba(0, 0, 0, 0.72)
    readonly property int fontSize: 11
    readonly property color foregroundColor: Theme.colors.foreground || "#d3c6aa"
    readonly property color graphDimColor: Qt.rgba(0, 0, 0, 0.58)
    readonly property color greenColor: Theme.colors.green || "#a7c080"
    readonly property int iconButtonSize: 24
    readonly property color magentaColor: Theme.colors.magenta || "#d699b6"
    readonly property string monoFontFamily: "Geist Mono"
    readonly property color mutedTextColor: Theme.colors.light_foreground || "#9da9a0"
    readonly property color orangeColor: Theme.colors.orange || "#e09d7f"
    readonly property color redColor: Theme.colors.red || "#e67e80"
    readonly property color referenceSelectionColor: Qt.rgba(style.orangeColor.r, style.orangeColor.g, style.orangeColor.b, 0.14)
    readonly property int scrollBarWidth: 6
    // Always a background tone. A theme without `selection` gets a faint
    // accent tint, never the accent itself, so selected text stays readable.
    readonly property color selectionColor: Theme.colors.selection || Qt.rgba(style.accentColor.r, style.accentColor.g, style.accentColor.b, 0.18)
    readonly property int smallControlHeight: 20
    readonly property int smallFontSize: 9
    readonly property color steeringTelemetryColor: "#dbbc7f"
    readonly property color surfaceColor: Theme.colors.lighter_background || "#3d484d"
    readonly property color throttleTelemetryColor: "#a7c080"
    readonly property color traceBackgroundColor: Theme.colors.darker_background || "#232a2e"
    readonly property string uiFontFamily: "Geist"
    readonly property color videoControlBackgroundColor: Qt.rgba(0, 0, 0, 0.86)
    readonly property color videoControlTrackColor: Qt.rgba(style.foregroundColor.r, style.foregroundColor.g, style.foregroundColor.b, 0.28)

    // Letterbox behind onboard video. Deliberately true black in every theme:
    // it must not tint the frame the driver is looking at.
    readonly property color videoLetterboxColor: "#000000"
    readonly property color yellowColor: Theme.colors.yellow || "#dbbc7f"
}
