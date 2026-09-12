#include "native_theme.h"
#include "ui_theme.h"

#include "common/logging.h"

#include <QtCore/QVariant>
#include <QtGui/QWindow>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <dwmapi.h>
#endif

namespace subcue {

NativeTheme::NativeTheme(QObject *parent)
    : QObject(parent)
{
    // 保留系统标题栏，避免 Windows 10 扩展客户区使用黑色标题文字。
}

void NativeTheme::applyDarkTitleBar(QObject *windowObject, const QColor &caption, const QColor &text,
                                    const QColor &border)
{
    auto *window = qobject_cast<QWindow *>(windowObject);
    if (window == nullptr) {
        return;
    }

    const QColor captionColor = caption.isValid() ? caption : UiTheme::kTitleBar;
    const QColor textColor = text.isValid() ? text : UiTheme::kPrimaryText;
    const QColor borderColor = border.isValid() ? border : UiTheme::kBorder;

#ifdef Q_OS_WIN
    hookWindow(window);
    window->setProperty("_subcueCaption", captionColor);
    window->setProperty("_subcueText", textColor);
    window->setProperty("_subcueBorder", borderColor);
    applyExpandedClientArea(window);
    applyNow(window, captionColor, textColor, borderColor);
#else
    Q_UNUSED(captionColor);
    Q_UNUSED(textColor);
    Q_UNUSED(borderColor);
#endif
}

#ifdef Q_OS_WIN
namespace {

COLORREF toColorRef(const QColor &color)
{
    return RGB(static_cast<BYTE>(color.red()), static_cast<BYTE>(color.green()),
               static_cast<BYTE>(color.blue()));
}

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kImmersiveDarkMode = 20;
#else
constexpr DWORD kImmersiveDarkMode = static_cast<DWORD>(DWMWA_USE_IMMERSIVE_DARK_MODE);
#endif

// Windows 10 1809–1909 used 19 before the official attribute became 20.
constexpr DWORD kImmersiveDarkModeLegacy = 19;

} // namespace

bool NativeTheme::isWindows11OrNewer()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return false;
    }
    using RtlGetVersionFn = LONG (WINAPI *)(OSVERSIONINFOW *);
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtlGetVersion == nullptr) {
        return false;
    }
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0) {
        return false;
    }
    return info.dwBuildNumber >= 22000;
}

void NativeTheme::applyExpandedClientArea(QWindow *window)
{
    if (!expandedClientArea_ || window == nullptr) {
        return;
    }
    // Dialogs keep standard caption chrome; only the main window extends #1B1E24.
    if (window->flags().testFlag(Qt::Dialog) || window->flags().testFlag(Qt::Tool)) {
        return;
    }
    const Qt::WindowFlags extra = Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint;
    if ((window->flags() & extra) == extra) {
        return;
    }
    window->setFlags(window->flags() | extra);
}

void NativeTheme::hookWindow(QWindow *window)
{
    if (window->property("_subcueDarkTitleBarHooked").toBool()) {
        return;
    }
    window->setProperty("_subcueDarkTitleBarHooked", true);

    const auto reapply = [this, window] {
        if (window == nullptr) {
            return;
        }
        const QColor caption = window->property("_subcueCaption").value<QColor>();
        const QColor text = window->property("_subcueText").value<QColor>();
        const QColor border = window->property("_subcueBorder").value<QColor>();
        applyExpandedClientArea(window);
        applyNow(window, caption.isValid() ? caption : UiTheme::kTitleBar,
                 text.isValid() ? text : UiTheme::kPrimaryText,
                 border.isValid() ? border : UiTheme::kBorder);
    };

    QObject::connect(window, &QWindow::visibleChanged, this, reapply);
    QObject::connect(window, &QWindow::windowStateChanged, this, reapply);
    QObject::connect(window, &QObject::destroyed, this, [this, window] { applied_.remove(window); });
}

void NativeTheme::applyNow(QWindow *window, const QColor &caption, const QColor &text, const QColor &border)
{
    if (window == nullptr) {
        return;
    }

    const WId wid = window->winId();
    if (wid == 0) {
        return;
    }

    HWND hwnd = reinterpret_cast<HWND>(wid);
    if (hwnd == nullptr) {
        return;
    }

    AppliedState &state = applied_[window];
    const quintptr hwndValue = reinterpret_cast<quintptr>(hwnd);
    if (state.hwnd == hwndValue && state.caption == caption.rgb() && state.text == text.rgb()
        && state.border == border.rgb() && state.expanded == expandedClientArea_) {
        return;
    }

    BOOL dark = TRUE;
    HRESULT hr = DwmSetWindowAttribute(hwnd, kImmersiveDarkMode, &dark, sizeof(dark));
    if (FAILED(hr)) {
        hr = DwmSetWindowAttribute(hwnd, kImmersiveDarkModeLegacy, &dark, sizeof(dark));
        if (FAILED(hr)) {
            qCDebug(subcueAppLog) << "DWM immersive dark mode is unavailable";
        }
    }

    if (!expandedClientArea_ && isWindows11OrNewer()) {
        const COLORREF captionRef = toColorRef(caption);
        hr = DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionRef, sizeof(captionRef));
        if (FAILED(hr)) {
            qCDebug(subcueAppLog) << "DWM caption color is unavailable";
        }

        const COLORREF textRef = toColorRef(text);
        hr = DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textRef, sizeof(textRef));
        if (FAILED(hr)) {
            qCDebug(subcueAppLog) << "DWM text color is unavailable";
        }

        const COLORREF borderRef = toColorRef(border);
        hr = DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderRef, sizeof(borderRef));
        if (FAILED(hr)) {
            qCDebug(subcueAppLog) << "DWM border color is unavailable";
        }
    }

    state.hwnd = hwndValue;
    state.caption = caption.rgb();
    state.text = text.rgb();
    state.border = border.rgb();
    state.expanded = expandedClientArea_;
}
#endif

} // namespace subcue
