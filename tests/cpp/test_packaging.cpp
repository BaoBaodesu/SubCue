#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtTest/QTest>

#include <cstring>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {

#ifdef Q_OS_WIN
[[nodiscard]] QStringList peImportedDlls(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = file.readAll();
    if (data.size() < static_cast<int>(sizeof(IMAGE_DOS_HEADER))) {
        return {};
    }
    const auto *base = reinterpret_cast<const quint8 *>(data.constData());
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return {};
    }
    const qint64 ntOffset = dos->e_lfanew;
    if (ntOffset <= 0
        || ntOffset + static_cast<qint64>(sizeof(IMAGE_NT_HEADERS64)) > data.size()) {
        return {};
    }
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + ntOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return {};
    }
    const IMAGE_DATA_DIRECTORY importDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
        return {};
    }

    const auto rvaToOffset = [&](quint32 rva) -> qint64 {
        const auto *section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const quint32 start = section[i].VirtualAddress;
            const quint32 raw = section[i].SizeOfRawData;
            const quint32 virtualSize = section[i].Misc.VirtualSize;
            const quint32 span = raw > virtualSize ? raw : virtualSize;
            if (rva >= start && rva < start + span) {
                return static_cast<qint64>(section[i].PointerToRawData) + (rva - start);
            }
        }
        return -1;
    };

    const qint64 importOff = rvaToOffset(importDir.VirtualAddress);
    if (importOff < 0) {
        return {};
    }

    QStringList dlls;
    for (int i = 0; ; ++i) {
        const qint64 descOff =
            importOff + i * static_cast<qint64>(sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (descOff + static_cast<qint64>(sizeof(IMAGE_IMPORT_DESCRIPTOR)) > data.size()) {
            break;
        }
        IMAGE_IMPORT_DESCRIPTOR desc{};
        std::memcpy(&desc, base + descOff, sizeof(desc));
        if (desc.Name == 0) {
            break;
        }
        const qint64 nameOff = rvaToOffset(desc.Name);
        if (nameOff < 0 || nameOff >= data.size()) {
            break;
        }
        const char *name = reinterpret_cast<const char *>(base + nameOff);
        const int maxLen = static_cast<int>(data.size() - nameOff);
        dlls.append(QString::fromLatin1(name, static_cast<qsizetype>(strnlen(name, maxLen))));
    }
    return dlls;
}
#endif

[[nodiscard]] QString packageDir()
{
    return QString::fromUtf8(SUBCUE_PACKAGE_DIR);
}

[[nodiscard]] QString packageFile(const QString &relative)
{
    return QDir(packageDir()).filePath(relative);
}

[[nodiscard]] bool packageHasAny(const QStringList &relatives)
{
    for (const QString &relative : relatives) {
        if (QFileInfo::exists(packageFile(relative))) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] QString readPackageText(const QString &relative)
{
    QFile file(packageFile(relative));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace

class PackagingTests final : public QObject {
    Q_OBJECT

private slots:
    void packageLayoutContainsRuntime();
    void packageExcludesPythonAndCliTools();
    void packageShipsThirdPartyLicenses();
    void aboutWindowDocumentsFfmpegLgpl();
    void packagedBinaryImportsAreClean();
    void packagedAppStartsWithIsolatedPath();
};

void PackagingTests::packageLayoutContainsRuntime()
{
    QVERIFY2(QFileInfo::exists(packageFile(QStringLiteral("SubCue.exe"))),
        qPrintable(packageFile(QStringLiteral("SubCue.exe"))));
    QVERIFY(packageHasAny({QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Cored.dll")}));
    QVERIFY(packageHasAny({QStringLiteral("Qt6Quick.dll"), QStringLiteral("Qt6Quickd.dll")}));
    QVERIFY(packageHasAny({QStringLiteral("Qt6Network.dll"), QStringLiteral("Qt6Networkd.dll")}));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("avcodec-63.dll"))));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("avformat-63.dll"))));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("avutil-61.dll"))));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("swresample-7.dll"))));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("swscale-10.dll"))));
    QVERIFY(packageHasAny({
        QStringLiteral("vcruntime140.dll"),
        QStringLiteral("VCRUNTIME140.dll"),
        QStringLiteral("vcruntime140d.dll"),
        QStringLiteral("VCRUNTIME140D.dll"),
    }));
    QVERIFY(packageHasAny({
        QStringLiteral("platforms/qwindows.dll"),
        QStringLiteral("platforms/qwindowsd.dll"),
        QStringLiteral("plugins/platforms/qwindows.dll"),
        QStringLiteral("plugins/platforms/qwindowsd.dll"),
    }));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("package.stamp"))));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("package-manifest.txt"))));
}

void PackagingTests::packageExcludesPythonAndCliTools()
{
    QDirIterator iterator(packageDir(), QDir::Files, QDirIterator::Subdirectories);
    QStringList hits;
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString name = QFileInfo(path).fileName().toLower();
        if (name.startsWith(QLatin1String("python"))
            || name.contains(QLatin1String("avdevice"))
            || name.contains(QLatin1String("pyside"))
            || name == QLatin1String("avfilter-12.dll")
            || name == QLatin1String("ffmpeg.exe")
            || name == QLatin1String("ffprobe.exe")
            || name == QLatin1String("ffplay.exe")) {
            hits.append(QDir(packageDir()).relativeFilePath(path));
        }
    }
    QVERIFY2(hits.isEmpty(), qPrintable(hits.join(QLatin1Char('\n'))));
}

void PackagingTests::packageShipsThirdPartyLicenses()
{
    const QString notice = readPackageText(QStringLiteral("licenses/NOTICE.txt"));
    QVERIFY(!notice.isEmpty());
    QVERIFY(notice.contains(QStringLiteral("Qt")));
    QVERIFY(notice.contains(QStringLiteral("FFmpeg")));
    QVERIFY(notice.contains(QStringLiteral("LGPL")));
    QVERIFY(notice.contains(QStringLiteral("whisper.cpp")));
    QVERIFY(notice.contains(QStringLiteral("Python")));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("licenses/qt/LICENSE.LGPLv3"))));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("licenses/whisper.cpp/LICENSE"))));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("licenses/FFmpeg/copyright"))));
    QVERIFY(QFileInfo::exists(packageFile(QStringLiteral("licenses/third-party-licenses.md"))));
}

void PackagingTests::aboutWindowDocumentsFfmpegLgpl()
{
    QFile about(QDir(QString::fromUtf8(SUBCUE_SOURCE_DIR)).filePath(QStringLiteral("src/app/qml/AboutWindow.qml")));
    QVERIFY(about.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(about.readAll());
    QVERIFY(source.contains(QStringLiteral("FFmpeg")));
    QVERIFY(source.contains(QStringLiteral("LGPL")));
    QVERIFY(source.contains(QStringLiteral("whisper.cpp")));
    QVERIFY(!source.contains(QStringLiteral("未链接 whisper.cpp")));
    QVERIFY(!source.contains(QStringLiteral("ffmpeg.exe")));
    QVERIFY(!source.contains(QStringLiteral("python.exe")));

    QFile mainFile(QDir(QString::fromUtf8(SUBCUE_SOURCE_DIR)).filePath(QStringLiteral("src/app/qml/Main.qml")));
    QVERIFY(mainFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString mainSource = QString::fromUtf8(mainFile.readAll());
    QVERIFY(mainSource.contains(QStringLiteral("openAbout")));
    QVERIFY(mainSource.contains(QStringLiteral("AboutWindow")));
}

void PackagingTests::packagedBinaryImportsAreClean()
{
#ifndef Q_OS_WIN
    QSKIP("PE import scan is Windows-only");
#else
    const QString appPath = packageFile(QStringLiteral("SubCue.exe"));
    QVERIFY(QFileInfo::exists(appPath));
    const QStringList dlls = peImportedDlls(appPath);
    QVERIFY2(!dlls.isEmpty(), qPrintable(QStringLiteral("failed to parse PE imports: %1").arg(appPath)));
    for (const QString &dll : dlls) {
        const QString lower = dll.toLower();
        QVERIFY2(
            !lower.startsWith(QLatin1String("python")),
            qPrintable(QStringLiteral("packaged app imports Python: %1").arg(dll)));
        QVERIFY2(
            !lower.contains(QLatin1String("avdevice")),
            qPrintable(QStringLiteral("packaged app imports avdevice: %1").arg(dll)));
    }

    const QStringList runtime = {
        QStringLiteral("avcodec-63.dll"),
        QStringLiteral("avformat-63.dll"),
        QStringLiteral("avutil-61.dll"),
        QStringLiteral("swresample-7.dll"),
        QStringLiteral("swscale-10.dll"),
    };
    for (const QString &relative : runtime) {
        const QStringList imported = peImportedDlls(packageFile(relative));
        QVERIFY2(!imported.isEmpty(), qPrintable(relative));
        for (const QString &dll : imported) {
            const QString lower = dll.toLower();
            QVERIFY2(
                !lower.startsWith(QLatin1String("python")),
                qPrintable(QStringLiteral("%1 imports Python: %2").arg(relative, dll)));
            QVERIFY2(
                !lower.contains(QLatin1String("avdevice")),
                qPrintable(QStringLiteral("%1 imports avdevice: %2").arg(relative, dll)));
        }
    }
#endif
}

void PackagingTests::packagedAppStartsWithIsolatedPath()
{
    const QString appPath = packageFile(QStringLiteral("SubCue.exe"));
    QVERIFY(QFileInfo::exists(appPath));

    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString systemRoot = env.value(QStringLiteral("SystemRoot"), QStringLiteral("C:/Windows"));
    const QString isolatedPath = QDir::toNativeSeparators(packageDir())
        + QLatin1Char(';')
        + QDir::toNativeSeparators(QDir(systemRoot).filePath(QStringLiteral("System32")));
    env.insert(QStringLiteral("PATH"), isolatedPath);
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.remove(QStringLiteral("PYTHONPATH"));
    env.remove(QStringLiteral("PYTHONHOME"));
    env.remove(QStringLiteral("VIRTUAL_ENV"));
    env.remove(QStringLiteral("QML_IMPORT_PATH"));
    env.remove(QStringLiteral("QML2_IMPORT_PATH"));
    env.remove(QStringLiteral("QT_PLUGIN_PATH"));
    env.remove(QStringLiteral("Qt6_ROOT"));
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(packageDir());
    process.start(appPath, QStringList{QStringLiteral("--smoke-test")});
    QVERIFY2(process.waitForStarted(10000), qPrintable(process.errorString()));
    QVERIFY2(
        process.waitForFinished(20000),
        qPrintable(QStringLiteral("packaged smoke timed out:\n%1\n%2")
            .arg(QString::fromLocal8Bit(process.readAllStandardOutput()),
                QString::fromLocal8Bit(process.readAllStandardError()))));
    QVERIFY2(
        process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
        qPrintable(QStringLiteral("packaged smoke exit %1:\n%2\n%3")
            .arg(process.exitCode())
            .arg(QString::fromLocal8Bit(process.readAllStandardOutput()),
                QString::fromLocal8Bit(process.readAllStandardError()))));
}

QTEST_MAIN(PackagingTests)
#include "test_packaging.moc"
