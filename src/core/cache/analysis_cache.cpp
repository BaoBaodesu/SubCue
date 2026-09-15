#include "cache/analysis_cache.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>

#include <utility>

namespace subcue {

AnalysisCache::AnalysisCache(QString directory)
    : directory_(directory.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
              .filePath(QStringLiteral("analysis"))
        : std::move(directory))
{
}

std::variant<QString, AppError> AnalysisCache::keyFor(
    const QString &mediaPath,
    const QString &modelId,
    const QString &language,
    const QJsonObject &parameters,
    const QString &algorithmVersion,
    const std::atomic<bool> *cancel) const
{
    QFile file(mediaPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return AppError(ErrorDomain::Asr, 4, QStringLiteral("无法读取媒体以计算分析缓存"), mediaPath);
    }
    QCryptographicHash mediaHash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancel && cancel->load(std::memory_order_acquire)) {
            return AppError(ErrorDomain::Asr, 1, QStringLiteral("任务已取消"));
        }
        const QByteArray block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFile::NoError) {
            return AppError(ErrorDomain::Asr, 4, QStringLiteral("读取媒体缓存内容失败"), mediaPath);
        }
        mediaHash.addData(block);
    }
    QCryptographicHash keyHash(QCryptographicHash::Sha256);
    keyHash.addData(mediaHash.result());
    keyHash.addData(modelId.toUtf8());
    keyHash.addData(language.toUtf8());
    keyHash.addData(QJsonDocument(parameters).toJson(QJsonDocument::Compact));
    keyHash.addData(algorithmVersion.toUtf8());
    return QString::fromLatin1(keyHash.result().toHex());
}

std::optional<QJsonObject> AnalysisCache::load(const QString &key) const
{
    QFile file(QDir(directory_).filePath(key + QStringLiteral(".json")));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    return document.object();
}

bool AnalysisCache::store(const QString &key, const QJsonObject &result, AppError *error) const
{
    if (!QDir().mkpath(directory_)) {
        if (error) *error = AppError(ErrorDomain::Asr, 4, QStringLiteral("无法创建分析缓存目录"), directory_);
        return false;
    }
    QSaveFile file(QDir(directory_).filePath(key + QStringLiteral(".json")));
    const QByteArray data = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = AppError(ErrorDomain::Asr, 4, QStringLiteral("无法写入分析缓存"), file.fileName());
        return false;
    }
    return true;
}

} // namespace subcue
