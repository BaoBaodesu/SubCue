#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtGui/QColor>
#include <QtGui/QRgb>

class QWindow;

namespace subcue {

class NativeTheme final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool expandedClientArea READ expandedClientArea CONSTANT)

public:
    explicit NativeTheme(QObject *parent = nullptr);

    [[nodiscard]] bool expandedClientArea() const noexcept { return expandedClientArea_; }

    Q_INVOKABLE void applyDarkTitleBar(
        QObject *windowObject,
        const QColor &caption = QColor(0x1B, 0x1E, 0x24),
        const QColor &text = QColor(0xE7, 0xEA, 0xF0),
        const QColor &border = QColor(0x34, 0x39, 0x44));

private:
#ifdef Q_OS_WIN
    struct AppliedState {
        quintptr hwnd = 0;
        QRgb caption = 0;
        QRgb text = 0;
        QRgb border = 0;
        bool expanded = false;
    };

    void applyNow(QWindow *window, const QColor &caption, const QColor &text, const QColor &border);
    void hookWindow(QWindow *window);
    void applyExpandedClientArea(QWindow *window);
    [[nodiscard]] static bool isWindows11OrNewer();
    QHash<QWindow *, AppliedState> applied_;
#endif
    bool expandedClientArea_ = false;
};

} // namespace subcue
