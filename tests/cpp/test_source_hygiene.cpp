#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
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

[[nodiscard]] QSet<QString> frozenEditorShortcuts()
{
    return {
        QStringLiteral("Ctrl+O"),
        QStringLiteral("Ctrl+Z"),
        QStringLiteral("Ctrl+Y"),
        QStringLiteral("Delete"),
        QStringLiteral("Ctrl+I"),
        QStringLiteral("Space"),
        QStringLiteral("J"),
        QStringLiteral("K"),
        QStringLiteral("L"),
        QStringLiteral("Left"),
        QStringLiteral("Right"),
        QStringLiteral("Shift+Left"),
        QStringLiteral("Shift+Right"),
        QStringLiteral("3"),
        QStringLiteral("4"),
        QStringLiteral("T"),
        QStringLiteral("["),
        QStringLiteral("]"),
        QStringLiteral("C"),
        QStringLiteral("Return"),
        QStringLiteral("Enter"),
        QStringLiteral("Shift+Return"),
        QStringLiteral("Shift+Enter"),
        QStringLiteral("Up"),
        QStringLiteral("Down"),
        QStringLiteral("Tab"),
        QStringLiteral("I"),
        QStringLiteral("O"),
        QStringLiteral("Alt+I"),
        QStringLiteral("Alt+O"),
        QStringLiteral("S"),
        QStringLiteral("="),
        QStringLiteral("-"),
        QStringLiteral("\\"),
    };
}

[[nodiscard]] QSet<QString> shortcutTokens(const QString &source)
{
    QSet<QString> tokens;
    QRegularExpression sequence(QStringLiteral("sequence:\\s*\"([^\"]+)\""));
    QRegularExpressionMatchIterator it = sequence.globalMatch(source);
    while (it.hasNext()) {
        const QString token = it.next().captured(1);
        tokens.insert(token == QLatin1String("\\\\") ? QStringLiteral("\\") : token);
    }
    QRegularExpression sequences(QStringLiteral("sequences:\\s*\\[([^\\]]+)\\]"));
    QRegularExpressionMatchIterator grouped = sequences.globalMatch(source);
    while (grouped.hasNext()) {
        const QString inner = grouped.next().captured(1);
        QRegularExpression quoted(QStringLiteral("\"([^\"]+)\""));
        QRegularExpressionMatchIterator quotedIt = quoted.globalMatch(inner);
        while (quotedIt.hasNext()) {
            const QString token = quotedIt.next().captured(1);
            tokens.insert(token == QLatin1String("\\\\") ? QStringLiteral("\\") : token);
        }
    }
    return tokens;
}

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

} // namespace

class SourceHygieneTests final : public QObject {
    Q_OBJECT

private slots:
    void pythonRuntimePathsAreGone();
    void goldenFixturesAndGeneratorRemain();
    void qmlKeepsFrozenShortcutMatrix();
    void nativeSourcesDoNotLaunchPythonRuntime();
    void controllerDoesNotDetachBackgroundThreads();
    void applicationDoesNotImportPython();

private:
    [[nodiscard]] QString sourcePath(const QString &relative) const;
    [[nodiscard]] QString readText(const QString &relative) const;
};

QString SourceHygieneTests::sourcePath(const QString &relative) const
{
    return QDir(QString::fromUtf8(SUBCUE_SOURCE_DIR)).filePath(relative);
}

QString SourceHygieneTests::readText(const QString &relative) const
{
    QFile file(sourcePath(relative));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void SourceHygieneTests::pythonRuntimePathsAreGone()
{
    const QStringList gone = {
        QStringLiteral("app.py"),
        QStringLiteral("requirements.txt"),
        QStringLiteral("build/build_windows.bat"),
        QStringLiteral("ui/editor_controller.py"),
        QStringLiteral("ui/qml/Main.qml"),
        QStringLiteral("services/task_service.py"),
        QStringLiteral("services/ffmpeg_service.py"),
        QStringLiteral("utils/logger.py"),
        QStringLiteral("core/alignment.py"),
        QStringLiteral("models/subtitle.py"),
        QStringLiteral("tests/test_alignment.py"),
        QStringLiteral("tests/test_qml_smoke.py"),
    };
    for (const QString &relative : gone) {
        QVERIFY2(
            !QFileInfo::exists(sourcePath(relative)),
            qPrintable(QStringLiteral("Python runtime path still present: %1").arg(relative)));
    }
    QVERIFY(!QDir(sourcePath(QStringLiteral("ui"))).exists());
    QVERIFY(!QDir(sourcePath(QStringLiteral("services"))).exists());
    QVERIFY(!QDir(sourcePath(QStringLiteral("utils"))).exists());
    QVERIFY(!QDir(sourcePath(QStringLiteral("core"))).exists());
}

void SourceHygieneTests::goldenFixturesAndGeneratorRemain()
{
    QVERIFY(QFileInfo::exists(sourcePath(QStringLiteral("tests/golden/alignment_cases.json"))));
    QVERIFY(QFileInfo::exists(sourcePath(QStringLiteral("tests/golden/sample.srt"))));
    QVERIFY(QFileInfo::exists(sourcePath(QStringLiteral("tests/golden/sample.ass"))));
    QVERIFY(QFileInfo::exists(sourcePath(QStringLiteral("tools/generate_alignment_golden.py"))));
    QVERIFY(QFileInfo::exists(sourcePath(QStringLiteral("tools/python_ref/core/alignment.py"))));
    QVERIFY(QFileInfo::exists(sourcePath(QStringLiteral("tools/python_ref/core/normalizer.py"))));
    QVERIFY(QFileInfo::exists(sourcePath(QStringLiteral("tools/python_ref/models/transcript.py"))));
    QVERIFY(QFileInfo::exists(sourcePath(QStringLiteral("tools/requirements-golden.txt"))));

    const QString requirements = readText(QStringLiteral("tools/requirements-golden.txt"));
    QVERIFY(requirements.contains(QStringLiteral("rapidfuzz")));
    QVERIFY(!requirements.contains(QStringLiteral("PySide6")));
    QVERIFY(!requirements.contains(QStringLiteral("Nuitka")));

    const QString generator = readText(QStringLiteral("tools/generate_alignment_golden.py"));
    QVERIFY(generator.contains(QStringLiteral("python_ref")));
    QVERIFY(!generator.contains(QStringLiteral("PySide6")));
}

void SourceHygieneTests::qmlKeepsFrozenShortcutMatrix()
{
    const QString source = readText(QStringLiteral("src/app/qml/Main.qml"));
    QVERIFY(!source.isEmpty());
    QCOMPARE(shortcutTokens(source), frozenEditorShortcuts());
    QVERIFY(source.contains(QStringLiteral("createNextScriptCue")));
    QVERIFY(source.contains(QStringLiteral("exportSubtitles")));

    const QString settings = readText(QStringLiteral("src/app/qml/SettingsWindow.qml"));
    QVERIFY(!settings.contains(QStringLiteral("ffmpeg.exe")));
    QVERIFY(!settings.contains(QStringLiteral("ffmpegPath")));
}

void SourceHygieneTests::nativeSourcesDoNotLaunchPythonRuntime()
{
    const QStringList forbidden = {
        QStringLiteral("PySide6"),
        QStringLiteral("Nuitka"),
        QStringLiteral("python.exe"),
        QStringLiteral("python311.dll"),
        QStringLiteral("CreateProcess"),
        QStringLiteral("ffmpeg.exe"),
        QStringLiteral("ffprobe.exe"),
    };
    const QDir sourceRoot(QString::fromUtf8(SUBCUE_SOURCE_DIR));
    const QStringList roots = {
        sourcePath(QStringLiteral("src")),
        sourcePath(QStringLiteral("CMakeLists.txt")),
        sourcePath(QStringLiteral("tests/CMakeLists.txt")),
    };
    QStringList hits;
    for (const QString &root : roots) {
        const QFileInfo info(root);
        QStringList files;
        if (info.isFile()) {
            files.append(root);
        } else {
            QDirIterator iterator(
                root,
                QStringList{
                    QStringLiteral("*.cpp"),
                    QStringLiteral("*.h"),
                    QStringLiteral("*.qml"),
                    QStringLiteral("CMakeLists.txt"),
                },
                QDir::Files,
                QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                files.append(iterator.next());
            }
        }
        for (const QString &path : files) {
            if (path.endsWith(QLatin1String("unicode_filter_data.h"))) {
                continue;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                continue;
            }
            const QString text = QString::fromUtf8(file.readAll());
            for (const QString &token : forbidden) {
                if (text.contains(token, Qt::CaseSensitive)) {
                    const QString relative = sourceRoot.relativeFilePath(path);
                    if (relative.startsWith(QLatin1String("src/inference/"))
                        && token == QLatin1String("python311.dll")) {
                        continue;
                    }
                    if (relative == QLatin1String("CMakeLists.txt")
                        && (token == QLatin1String("python.exe")
                            || token == QLatin1String("python311.dll"))
                        && text.contains(QStringLiteral("SUBCUE_PROJECT_INFERENCE_PYTHON"))) {
                        continue;
                    }
                    hits.append(QStringLiteral("%1: %2").arg(relative, token));
                }
            }
        }
    }
    QVERIFY2(hits.isEmpty(), qPrintable(hits.join(QLatin1Char('\n'))));
}

void SourceHygieneTests::applicationDoesNotImportPython()
{
#ifndef SUBCUE_APP_PATH
    QSKIP("SubCue application was not built");
#else
#ifndef Q_OS_WIN
    QSKIP("PE import scan is Windows-only");
#else
    const QString appPath = QString::fromUtf8(SUBCUE_APP_PATH);
    QVERIFY(QFileInfo::exists(appPath));
    const QStringList dlls = peImportedDlls(appPath);
    QVERIFY2(!dlls.isEmpty(), qPrintable(QStringLiteral("failed to parse PE imports: %1").arg(appPath)));
    for (const QString &dll : dlls) {
        const QString lower = dll.toLower();
        QVERIFY2(
            !lower.startsWith(QLatin1String("python")),
            qPrintable(QStringLiteral("native app imports Python: %1").arg(dll)));
        QVERIFY2(
            !lower.contains(QLatin1String("avdevice")),
            qPrintable(QStringLiteral("native app imports avdevice: %1").arg(dll)));
    }
#endif
#endif
}

void SourceHygieneTests::controllerDoesNotDetachBackgroundThreads()
{
    const QString controller = readText(QStringLiteral("src/app/app_controller.cpp"));
    QVERIFY(!controller.isEmpty());
    QVERIFY(!controller.contains(QStringLiteral(".detach()")));
    QVERIFY(controller.contains(QStringLiteral("backgroundTasks_.waitForDone()")));
}

QTEST_MAIN(SourceHygieneTests)
#include "test_source_hygiene.moc"
