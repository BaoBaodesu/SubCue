#include "app_controller.h"
#include "application_context.h"
#include "common/logging.h"
#include "native_theme.h"
#include "ui_theme.h"

#include <QtCore/QTimer>
#include <QtGui/QFont>
#include <QtGui/QFontInfo>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtGui/QWindow>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQuickControls2/QQuickStyle>

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SubCue"));
    QCoreApplication::setApplicationName(QStringLiteral("SubCue"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QFont uiFont(QStringLiteral("Microsoft YaHei UI"));
    if (QFontInfo(uiFont).family() != QStringLiteral("Microsoft YaHei UI")) {
        uiFont = QFont(QStringLiteral("Segoe UI"));
    }
    uiFont.setPixelSize(13);
    application.setFont(uiFont);

    QPalette palette;
    palette.setColor(QPalette::Window, subcue::UiTheme::kBackground);
    palette.setColor(QPalette::WindowText, subcue::UiTheme::kPrimaryText);
    palette.setColor(QPalette::Base, subcue::UiTheme::kInput);
    palette.setColor(QPalette::AlternateBase, subcue::UiTheme::kPanelSecondary);
    palette.setColor(QPalette::Text, subcue::UiTheme::kPrimaryText);
    palette.setColor(QPalette::Button, subcue::UiTheme::kButton);
    palette.setColor(QPalette::ButtonText, subcue::UiTheme::kPrimaryText);
    palette.setColor(QPalette::Highlight, subcue::UiTheme::kSelection);
    palette.setColor(QPalette::HighlightedText, subcue::UiTheme::kPrimaryText);
    palette.setColor(QPalette::PlaceholderText, subcue::UiTheme::kPlaceholder);
    palette.setColor(QPalette::Mid, subcue::UiTheme::kDivider);
    palette.setColor(QPalette::Midlight, subcue::UiTheme::kPanelRaised);
    palette.setColor(QPalette::Dark, subcue::UiTheme::kBorder);
    palette.setColor(QPalette::Light, subcue::UiTheme::kPanelSecondary);
    palette.setColor(QPalette::Shadow, subcue::UiTheme::kDivider);
    palette.setColor(QPalette::ToolTipBase, subcue::UiTheme::kPanelRaised);
    palette.setColor(QPalette::ToolTipText, subcue::UiTheme::kPrimaryText);
    application.setPalette(palette);

    subcue::ApplicationContext context;
    subcue::AppController controller(&context);
    subcue::NativeTheme nativeTheme;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("editor"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("nativeTheme"), &nativeTheme);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &application,
        [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreated,
        &application,
        [&nativeTheme](QObject *object, const QUrl &) {
            if (auto *window = qobject_cast<QWindow *>(object)) {
                nativeTheme.applyDarkTitleBar(window);
            }
        });
    engine.loadFromModule(QStringLiteral("SubCue"), QStringLiteral("Main"));

    if (application.arguments().contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }

    qCInfo(subcueLog) << "SubCue native application started";
    return application.exec();
}
