#include "WindowsIntegration.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QWindow>

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

class WindowsWindowAppearance final : public QObject {
public:
    explicit WindowsWindowAppearance(QObject* parent) : QObject(parent) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show)
            useDarkTitleBar(qobject_cast<QWindow*>(watched));
        return QObject::eventFilter(watched, event);
    }
};

}  // namespace

void initializeWindowsIntegration(QGuiApplication& app) {
    SetCurrentProcessExplicitAppUserModelID(L"io.github.tobi.omatrack");
    if (highContrastEnabled()) return;

    QGuiApplication::setPalette(omatrackDarkPalette());
    app.installEventFilter(new WindowsWindowAppearance(&app));
}

}  // namespace omatrack
