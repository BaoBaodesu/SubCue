#include "project/asset_file.h"
#include "roughcut/rough_cut_project.h"
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QTemporaryFile>
#include <QtCore/QCryptographicHash>
namespace subcue {
namespace {
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
}
std::optional<QPair<QString, QByteArray>> copyAssetFile(const QString &sourcePath, const QString &directory,
    QString *error, const std::atomic<bool> *cancel)
{
    return copyAssetFile(sourcePath, directory, error, cancel, nullptr);
}
// 文件发布先于数据库引用；固定缓冲复制并验证，失败不会留下半个资产。
std::optional<QPair<QString, QByteArray>> copyAssetFile(const QString &sourcePath, const QString &directory,
    QString *error, const std::atomic<bool> *cancel, bool *created)
{
    if (created) *created = false;
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly) || source.size() <= 0) {
        fail(error, QStringLiteral("资产不存在、无法读取或为空：%1").arg(sourcePath)); return {};
    }
    if (!QDir().mkpath(directory)) { fail(error, QStringLiteral("无法创建资产目录。")); return {}; }
    QTemporaryFile temporary(QDir(directory).filePath(QStringLiteral(".asset-XXXXXX")));
    if (!temporary.open()) { fail(error, temporary.errorString()); return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!source.atEnd()) {
        if (cancel && cancel->load()) { fail(error, QStringLiteral("资产处理已取消。")); return {}; }
        const auto bytes = source.read(1024 * 1024);
        if (bytes.isEmpty() || temporary.write(bytes) != bytes.size()) {
            fail(error, QStringLiteral("资产读取或写入失败。")); return {};
        }
        hash.addData(bytes);
    }
    if (cancel && cancel->load()) { fail(error, QStringLiteral("资产处理已取消。")); return {}; }
    if (!temporary.flush()) { fail(error, temporary.errorString()); return {}; }
    temporary.close();
    if (RoughCutProjectSerializer::mediaSha256(temporary.fileName(), error, cancel) != hash.result()
        || RoughCutProjectSerializer::mediaSha256(sourcePath, error, cancel) != hash.result()) {
        fail(error, QStringLiteral("资产复制校验失败或源文件发生变化。")); return {};
    }
    const QString name = QString::fromLatin1(hash.result().toHex()) + QStringLiteral(".asset");
    const QString destination = QDir(directory).filePath(name);
    if (QFileInfo::exists(destination)) {
        if (RoughCutProjectSerializer::mediaSha256(destination, error, cancel) != hash.result()) {
            fail(error, QStringLiteral("已有资产损坏，未覆盖：%1").arg(destination)); return {};
        }
    } else {
        if (!temporary.rename(destination)) {
            // 另一个提交可能刚发布相同内容；只接受完整、同哈希的文件。
            if (RoughCutProjectSerializer::mediaSha256(destination, error, cancel) != hash.result()) {
                fail(error, QStringLiteral("无法发布资产：%1").arg(temporary.errorString())); return {};
            }
        } else {
            temporary.setAutoRemove(false);
            if (created) *created = true;
        }
    }
    return QPair<QString, QByteArray>{name, hash.result()};
}
}
