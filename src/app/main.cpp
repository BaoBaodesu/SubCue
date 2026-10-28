#include "app_controller.h"
#include "application_context.h"
#include "project_session.h"
#include "tts_task_controller.h"
#include "voice_library_controller.h"
#include "voice_studio_controller.h"
#include "voice_audio_controller.h"
#include "audio_preview_controller.h"
#include "inference/inference_manager.h"
#include "common/logging.h"
#include "native_theme.h"
#include "rough_cut_controller.h"
#include "ui_theme.h"
#include "workspace_router.h"

#include <QtCore/QTimer>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QSaveFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QElapsedTimer>
#include <QtGui/QFont>
#include <QtGui/QFontInfo>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
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
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.1"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    application.setWindowIcon(QIcon(QStringLiteral(":/qt/qml/SubCue/src/app/qml/icons/app-icon.png")));

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

    if (application.arguments().contains(QStringLiteral("--preview-legacy-cloud"))
        || application.arguments().contains(QStringLiteral("--retire-legacy-cloud"))) {
        const auto arguments = application.arguments();
        // 清理预览不构造应用上下文，避免触发迁移或孤立凭据自动清理。
        const auto appData = qEnvironmentVariable("APPDATA");
        subcue::SettingsManager settings(appData.isEmpty() ? subcue::SettingsManager::defaultPath()
            : QDir(appData).filePath(QStringLiteral("SubCue/settings.json")));
        subcue::DpapiCredentialStore credentials;
        subcue::CloudConfigStore config(QFileInfo(settings.path()).absoluteDir().filePath(QStringLiteral("connections.sqlite")), &credentials);
        QString error; QJsonArray previews;
        const auto unified = config.load(&error);
        if (unified.isEmpty()) error = QStringLiteral("统一配置不可读，已停止旧数据清理。");
        const auto legacyPath = QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("AutoSubtitleAligner/settings.json"));
        const QStringList paths = QFileInfo::exists(legacyPath)
            ? QStringList{settings.path(), legacyPath} : QStringList{settings.path()};
        if (error.isEmpty()) for (const auto &path : paths) {
            const auto preview = subcue::SettingsManager(path).legacyCloudPreview(credentials, &error);
            if (preview.isEmpty()) break; previews.append(preview);
        }
        const auto token = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(previews).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        bool success = error.isEmpty();
        if (success && arguments.contains(QStringLiteral("--retire-legacy-cloud"))) {
            const int index = arguments.indexOf(QStringLiteral("--retire-legacy-cloud"));
#ifdef Q_OS_WIN
            if (subcue::ApplicationContext::otherCloudInstancesRunning()) { success = false; error = QStringLiteral("存在其他 SubCue 实例或无法验证进程状态，未执行清理。请先正常关闭后重试。"); }
            else
#endif
            if (index + 1 >= arguments.size() || arguments.at(index + 1) != token) { success = false; error = QStringLiteral("清理预览已变化，请重新预览。"); }
            else for (const auto &value : previews) {
                const auto before = value.toObject(); subcue::SettingsManager manager(before.value(QStringLiteral("path")).toString());
                const auto preview = manager.legacyCloudPreview(credentials, &error);
                if (preview.isEmpty() || preview.value(QStringLiteral("metadataSnapshot")) != before.value(QStringLiteral("metadataSnapshot"))) { success = false; error = QStringLiteral("旧配置已变化，请重新预览。"); break; }
                for (const auto &target : preview.value(QStringLiteral("credentials")).toArray())
                    if (!before.value(QStringLiteral("credentials")).toArray().contains(target)) { success = false; error = QStringLiteral("旧凭据目标已变化，请重新预览。"); }
                if (!success || !manager.retireLegacyCloud(credentials, preview, &error)) { success = false; break; }
            }
            if (success && config.load(&error) != unified) { success = false; error = QStringLiteral("统一配置已变化，请检查；未主动删除统一账号或模型。"); }
        }
        const QJsonObject result{{QStringLiteral("success"), success}, {QStringLiteral("error"), error},
            {QStringLiteral("token"), token}, {QStringLiteral("previews"), previews}};
        const int outputIndex = arguments.indexOf(QStringLiteral("--legacy-cloud-output"));
        if (outputIndex < 0 || outputIndex + 1 >= arguments.size()) return EXIT_FAILURE;
        QSaveFile output(arguments.at(outputIndex + 1));
        if (!output.open(QIODevice::WriteOnly)) return EXIT_FAILURE;
        const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
        if (output.write(bytes) != bytes.size() || !output.commit()) return EXIT_FAILURE;
        return success ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    subcue::ApplicationContext context;
    subcue::AppController controller(&context);
    subcue::RoughCutController roughCutController(&context);
    subcue::ProjectSession projectSession(&context, &controller, &roughCutController);
    subcue::VoiceStudioController voiceStudio(&context, &projectSession);
    projectSession.newProject();
    subcue::TtsTaskController ttsTasks(&context, &projectSession);
    voiceStudio.setTasks(&ttsTasks);
    subcue::VoiceLibraryController voiceLibrary(&context, &projectSession);
    subcue::AudioPreviewController audioPreview(&context, &projectSession);
    subcue::VoiceAudioController voiceAudio(&context, &projectSession, &voiceStudio, &audioPreview);
    projectSession.setVoiceServices(&ttsTasks, &voiceLibrary, &voiceAudio);
    subcue::WorkspaceRouter workspaceRouter(&controller, &roughCutController);
    workspaceRouter.setProjectSession(&projectSession);
    workspaceRouter.setAudioPreview(&audioPreview);
    if (application.arguments().contains(QStringLiteral("--roughcut-workspace")))
        workspaceRouter.switchTo(QStringLiteral("roughcut"));
    QElapsedTimer heartbeat;
    heartbeat.start();
    QTimer responsivenessTimer;
    QObject::connect(&responsivenessTimer, &QTimer::timeout, &application, [&heartbeat] {
        const qint64 elapsed = heartbeat.restart();
        if (elapsed > 450) qCWarning(subcueLog) << "ui_event_delay_ms=" << elapsed - 250;
    });
    responsivenessTimer.start(250);
    subcue::NativeTheme nativeTheme;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("editor"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("roughCut"), &roughCutController);
    engine.rootContext()->setContextProperty(QStringLiteral("appRouter"), &workspaceRouter);
    engine.rootContext()->setContextProperty(QStringLiteral("projectSession"), &projectSession);
    engine.rootContext()->setContextProperty(QStringLiteral("ttsTasks"), &ttsTasks);
    engine.rootContext()->setContextProperty(QStringLiteral("voiceLibrary"), &voiceLibrary);
    engine.rootContext()->setContextProperty(QStringLiteral("voiceStudio"), &voiceStudio);
    engine.rootContext()->setContextProperty(QStringLiteral("voiceAudio"), &voiceAudio);
    engine.rootContext()->setContextProperty(QStringLiteral("audioPreview"), &audioPreview);
    engine.rootContext()->setContextProperty(QStringLiteral("gpuQueue"), &subcue::InferenceManager::instance());
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
        [&nativeTheme, &application](QObject *object, const QUrl &) {
            if (auto *window = qobject_cast<QWindow *>(object)) {
                window->setIcon(application.windowIcon());
                nativeTheme.applyDarkTitleBar(window);
            }
        });
    engine.loadFromModule(QStringLiteral("SubCue"), QStringLiteral("AppRouter"));
    // 加载失败必须直接失败，不能被 smoke-test 的 quit() 覆盖退出码。
    if (engine.rootObjects().isEmpty()) return EXIT_FAILURE;

    if (application.arguments().contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }

    qCInfo(subcueLog) << "SubCue native application started" << QCoreApplication::applicationVersion()
        << QCoreApplication::applicationFilePath() << "built=" << __DATE__ << __TIME__;
    const int result = application.exec();
    projectSession.shutdown(result == EXIT_SUCCESS);
    return result;
}
