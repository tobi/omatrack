#include "WindowsIntegration.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QWindow>
#include <QtGui/qpa/qplatformwindow_p.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <dwmapi.h>
#include <shobjidl.h>

namespace omatrack {
namespace {

bool highContrastEnabled() {
    HIGHCONTRASTW highContrast = {};
    highContrast.cbSize = sizeof(highContrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, highContrast.cbSize,
                                 &highContrast, 0) != FALSE &&
           (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

QPalette omatrackDarkPalette() {
    QPalette palette;
    const QColor window(QStringLiteral("#16181d"));
    const QColor base(QStringLiteral("#101216"));
    const QColor button(QStringLiteral("#202329"));
    const QColor foreground(QStringLiteral("#d3c6aa"));
    const QColor muted(QStringLiteral("#9da9a0"));
    const QColor accent(QStringLiteral("#a7c080"));

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, foreground);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase,
                     QColor(QStringLiteral("#111318")));
    palette.setColor(QPalette::ToolTipBase, button);
    palette.setColor(QPalette::ToolTipText, foreground);
    palette.setColor(QPalette::Text, foreground);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, foreground);
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, base);
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#2a2f3a")));
    palette.setColor(QPalette::Midlight, muted);
    palette.setColor(QPalette::Link, accent);
    palette.setColor(QPalette::PlaceholderText, muted);
    return palette;
}

void useDarkTitleBar(QWindow* window) {
    if (!window) return;

    const BOOL enabled = TRUE;
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    constexpr DWORD immersiveDarkMode = 20;
    if (SUCCEEDED(DwmSetWindowAttribute(handle, immersiveDarkMode, &enabled,
                                        sizeof(enabled))))
        return;

    // Windows 10 version 1809 used the provisional value before the public
    // DWMWA_USE_IMMERSIVE_DARK_MODE constant settled at 20.
    constexpr DWORD immersiveDarkMode1809 = 19;
    DwmSetWindowAttribute(handle, immersiveDarkMode1809, &enabled,
                          sizeof(enabled));
}

// Qt implements Window.FullScreen on Windows as a borderless WS_POPUP window
// sized exactly to the monitor. Because the scene graph runs on OpenGL (the
// libmpv render API needs it), DWM and the display driver treat that surface
// like a game's exclusive fullscreen: the display re-modes, the screen goes
// black for a moment, variable-refresh monitors switch rate, and — Qt's
// documented DWM limitation — other top-level windows (Preferences,
// Channels, dialogs) cannot appear above it. Qt's remedy is the platform
// window's HasBorderInFullScreen flag: the popup keeps WS_BORDER, the
// client area is inset by one pixel, and the window never matches the
// screen exactly, so it stays a composited window. Qt still reports it as
// FullScreen (isFullScreen_sys() accounts for the border), so the QML
// visibility round-trip is unchanged. The flag lives on the platform
// window, so it is set once the native handle exists — the first Show.
void keepFullScreenComposited(QWindow* window) {
    if (!window || !window->isTopLevel()) return;
    using QNativeInterface::Private::QWindowsWindow;
    if (auto* native = window->nativeInterface<QWindowsWindow>())
        native->setHasBorderInFullScreen(true);
}

class WindowsWindowAppearance final : public QObject {
public:
    explicit WindowsWindowAppearance(QObject* parent, bool darkTitleBar)
        : QObject(parent), darkTitleBar_(darkTitleBar) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            auto* window = qobject_cast<QWindow*>(watched);
            keepFullScreenComposited(window);
            if (darkTitleBar_) useDarkTitleBar(window);
        }
        return QObject::eventFilter(watched, event);
    }

private:
    const bool darkTitleBar_;
};

}  // namespace

void initializeWindowsIntegration(QGuiApplication& app) {
    SetCurrentProcessExplicitAppUserModelID(L"io.github.tobi.omatrack");
    // High contrast keeps the system palette and title bars; the fullscreen
    // compositing fix applies regardless.
    const bool themed = !highContrastEnabled();
    if (themed) QGuiApplication::setPalette(omatrackDarkPalette());
    app.installEventFilter(new WindowsWindowAppearance(&app, themed));
}

}  // namespace omatrack
