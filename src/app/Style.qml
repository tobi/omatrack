pragma ComponentBehavior: Bound
pragma Singleton

// Single source of truth for palette and typography.
//
// Colors come from the active Omarchy theme when the desktop provides one and
// fall back to the platform SystemPalette otherwise, so the app still looks
// correct outside Omarchy. Every component reads these instead of re-deriving
// them, which keeps one visual language and lets qmllint resolve the access.

import QtQuick
import Racecraft

QtObject {
    id: style

    readonly property color accentColor: Theme.colors.accent || desktopPalette.highlight
    readonly property color backgroundColor: Theme.colors.background || desktopPalette.window
    readonly property color borderColor: Theme.colors.muted || desktopPalette.mid

    // Offered when picking a trace color; deliberately theme-independent so a
    // saved channel color keeps meaning across desktop themes.
    readonly property list<string> colorChoices: ["#a7c080", "#7fbbb3", "#e67e80", "#dbbc7f", "#d699b6", "#e09d7f", "#d3c6aa", "#9da9a0"]

    // ── density ─────────────────────────────────────────────────────
    // Material's own metrics are sized for touch. These are the compact
    // desktop values every control in the app is sized against, so density
    // is one edit here rather than a number per component.
    readonly property int controlHeight: 26
    readonly property int controlPadding: 8
    readonly property color darkBackgroundColor: Theme.colors.dark_background || desktopPalette.alternateBase
    readonly property SystemPalette desktopPalette: SystemPalette {
        colorGroup: SystemPalette.Active
    }
    readonly property color dimTextColor: Theme.colors.dark_foreground || desktopPalette.mid
    readonly property int fontSize: 11
    readonly property color foregroundColor: Theme.colors.foreground || desktopPalette.windowText
    readonly property color greenColor: Theme.colors.green || "#a7c080"
    readonly property int iconButtonSize: 24
    readonly property color magentaColor: Theme.colors.magenta || "#d699b6"
    readonly property string monoFontFamily: "Geist Mono"
    readonly property color mutedTextColor: Theme.colors.light_foreground || desktopPalette.midlight
    readonly property color orangeColor: Theme.colors.orange || "#e09d7f"
    readonly property color redColor: Theme.colors.red || "#e67e80"
    readonly property int scrollBarWidth: 6
    readonly property color selectionColor: Theme.colors.selection || desktopPalette.highlight
    readonly property int smallControlHeight: 20
    readonly property int smallFontSize: 9
    readonly property color surfaceColor: Theme.colors.lighter_background || desktopPalette.button
    readonly property color traceBackgroundColor: Theme.colors.darker_background || desktopPalette.base
    readonly property string uiFontFamily: "Geist"

    // Letterbox behind onboard video. Deliberately true black in every theme:
    // it must not tint the frame the driver is looking at.
    readonly property color videoLetterboxColor: "#000000"
    readonly property color yellowColor: Theme.colors.yellow || "#dbbc7f"
}
