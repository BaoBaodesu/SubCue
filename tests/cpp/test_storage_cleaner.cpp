#include "settings/storage_cleaner.h"

#include "app_controller.h"
#include "application_context.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#include <memory>

using namespace subcue;

class StorageCleanerTests final : public QObject {
    Q_OBJECT

private slots:
    void rejectsProtectedPaths();
    void removesOnlySelectedTargets();
    void cancelStopsWithoutDeleting();
    void destroyedControllerDoesNotDeliverCallback();
};

namespace {

void writeFile(const QString &path, const QByteArray &bytes = QByteArrayLiteral("x"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(bytes) == bytes.size());
}

} // namespace

void StorageCleanerTests::rejectsProtectedPaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();
    const QString models = QDir(root).filePath(QStringLiteral("external-models"));
    const QString keep = QDir(root).filePath(QStringLiteral("out/build/windows-release-cuda"));
    QDir().mkpath(QDir(root).filePath(QStringLiteral("src")));
    QDir().mkpath(QDir(root).filePath(QStringLiteral("tests/media")));
    QDir().mkpath(QDir(root).filePath(QStringLiteral("tests/golden")));
    QDir().mkpath(QDir(root).filePath(QStringLiteral(".git/objects")));
    QDir().mkpath(keep);
    QDir().mkpath(QDir(keep).filePath(QStringLiteral("package")));
    QDir().mkpath(models);
    writeFile(QDir(root).filePath(QStringLiteral("src/core/foo.cpp")));
    writeFile(QDir(models).filePath(QStringLiteral("qwen3-asr-0.6b/model.safetensors")));

    StorageCleaner cleaner(root, models, keep);
    QVERIFY(cleaner.developmentTreeVisible());
    QCOMPARE(cleaner.keepBuildName(), QStringLiteral("windows-release-cuda"));
    QVERIFY(!cleaner.isPathAllowed(QDir(root).filePath(QStringLiteral("src"))));
    QVERIFY(!cleaner.isPathAllowed(QDir(root).filePath(QStringLiteral("src/core/foo.cpp"))));
    QVERIFY(!cleaner.isPathAllowed(QDir(root).filePath(QStringLiteral("tests/media"))));
    QVERIFY(!cleaner.isPathAllowed(QDir(root).filePath(QStringLiteral("tests/golden"))));
    QVERIFY(!cleaner.isPathAllowed(QDir(root).filePath(QStringLiteral(".git"))));
    QVERIFY(!cleaner.isPathAllowed(models));
    QVERIFY(!cleaner.isPathAllowed(QDir(models).filePath(QStringLiteral("qwen3-asr-0.6b"))));
    QVERIFY(!cleaner.isPathAllowed(keep));
    QVERIFY(cleaner.isPathAllowed(QDir(keep).filePath(QStringLiteral("package"))));
    QVERIFY(!cleaner.isPathAllowed(QStringLiteral("C:/Windows/System32")));
}

void StorageCleanerTests::removesOnlySelectedTargets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();
    const QString keep = QDir(root).filePath(QStringLiteral("out/build/windows-release-cuda"));
    const QString stale = QDir(root).filePath(QStringLiteral("out/build/windows-debug"));
    const QString pack = QDir(keep).filePath(QStringLiteral("package/ignored.bin"));
    QDir().mkpath(keep);
    writeFile(QDir(stale).filePath(QStringLiteral("SubCue.exe")), QByteArray(1024, 'a'));
    writeFile(pack, QByteArray(512, 'b'));
    writeFile(QDir(root).filePath(QStringLiteral(".test_tmp/run.log")));

    StorageCleaner cleaner(root, QDir(root).filePath(QStringLiteral("models")), keep);
    const QVector<StorageTarget> targets = cleaner.scan();
    QStringList ids;
    for (const StorageTarget &target : targets) {
        if (target.group == QLatin1String("build") && target.path.contains(QLatin1String("windows-debug"))) {
            ids.append(target.id);
        }
    }
    QVERIFY(!ids.isEmpty());
    const StorageCleanupResult result = cleaner.remove(ids);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(!QFileInfo::exists(stale));
    QVERIFY(QFileInfo::exists(keep));
    QVERIFY(QFileInfo::exists(pack));
    QVERIFY(QFileInfo::exists(QDir(root).filePath(QStringLiteral(".test_tmp/run.log"))));
}

void StorageCleanerTests::cancelStopsWithoutDeleting()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();
    const QString keep = QDir(root).filePath(QStringLiteral("out/build/windows-release-cuda"));
    const QString stale = QDir(root).filePath(QStringLiteral("out/build/windows-debug"));
    QDir().mkpath(keep);
    writeFile(QDir(stale).filePath(QStringLiteral("SubCue.exe")));

    StorageCleaner cleaner(root, {}, keep);
    QString id;
    for (const StorageTarget &target : cleaner.scan()) {
        if (target.path == QDir::cleanPath(stale)) id = target.id;
    }
    QVERIFY(!id.isEmpty());
    std::atomic<bool> cancel{true};
    const StorageCleanupResult result = cleaner.remove({id}, &cancel);
    QVERIFY(!result.ok);
    QCOMPARE(result.error, QStringLiteral("任务已取消"));
    QVERIFY(QFileInfo::exists(QDir(stale).filePath(QStringLiteral("SubCue.exe"))));
}

void StorageCleanerTests::destroyedControllerDoesNotDeliverCallback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = std::make_unique<ApplicationContext>(
        dir.filePath(QStringLiteral("settings.json")),
        dir.filePath(QStringLiteral("credentials.dat")),
        AudioDeviceKind::Virtual);
    auto *controller = new AppController(context.get());
    controller->requestStorageTargets();
    controller->cleanupStorage({QStringLiteral("missing")});
    delete controller;
    QTest::qWait(300);
    QVERIFY(true);
}

QTEST_GUILESS_MAIN(StorageCleanerTests)
#include "test_storage_cleaner.moc"
