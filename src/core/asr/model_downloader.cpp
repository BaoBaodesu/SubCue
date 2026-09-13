#include "asr/model_downloader.h"

#include "asr/asr_types.h"
#include "common/logging.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

#include <utility>

namespace subcue {
namespace {

QString partPathFor(const QString &destination)
{
    return destination + QStringLiteral(".part");
}

bool atomicReplace(const QString &partPath, const QString &destination, QString *errorMessage)
{
    if (QFile::exists(destination) && !QFile::remove(destination)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法替换已有模型文件");
        }
        return false;
    }
    if (!QFile::rename(partPath, destination)) {
        // 跨卷 rename 失败时退回 QSaveFile 拷贝。
        QFile part(partPath);
        if (!part.open(QIODevice::ReadOnly)) {
            if (errorMessage) {
                *errorMessage = part.errorString();
            }
            return false;
        }
        QSaveFile file(destination);
        if (!file.open(QIODevice::WriteOnly)) {
            if (errorMessage) {
                *errorMessage = file.errorString();
            }
            return false;
        }
        while (!part.atEnd()) {
            const QByteArray chunk = part.read(1 << 20);
            if (file.write(chunk) != chunk.size()) {
                if (errorMessage) {
                    *errorMessage = file.errorString();
                }
                return false;
            }
        }
        if (!file.commit()) {
            if (errorMessage) {
                *errorMessage = file.errorString();
            }
            return false;
        }
        QFile::remove(partPath);
    }
    return true;
}

} // namespace

ModelDownloader::ModelDownloader(IHttpClient *http)
    : http_(http)
{
}

QByteArray ModelDownloader::sha256HexOfFile(const QString &path, const std::atomic<bool> *cancel)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (asrCancelled(cancel)) return {};
        const QByteArray data = file.read(1 << 20);
        if (data.isEmpty() && file.error() != QFileDevice::NoError) return {};
        hash.addData(data);
    }
    return hash.result().toHex();
}

bool ModelDownloader::matchesSpec(const QString &path, const WhisperModelSpec &spec, const std::atomic<bool> *cancel)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() != spec.size) {
        return false;
    }
    return sha256HexOfFile(path, cancel).toLower() == spec.sha256Hex.toLower();
}

std::variant<QString, AppError> ModelDownloader::ensure(
    const WhisperModelSpec &spec,
    const QString &directory,
    const std::atomic<bool> *cancel,
    const std::function<void(qint64, qint64)> &progress) const
{
    if (!http_) {
        return AppError(ErrorDomain::Network, static_cast<int>(AsrErrorCode::DownloadFailure),
            QStringLiteral("模型下载器未配置 HTTP 客户端"));
    }
    if (spec.url.isEmpty() || spec.size <= 0 || spec.sha256Hex.size() != 64) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::ModelNotFound),
            QStringLiteral("Whisper 模型清单无效"));
    }
    if (!QDir().mkpath(directory)) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::DownloadFailure),
            QStringLiteral("无法创建模型目录"));
    }

    const QString destination = QDir(directory).filePath(spec.fileName);
    if (QFileInfo::exists(destination)) {
        if (matchesSpec(destination, spec)) {
            return destination;
        }
        qCWarning(subcueAsrLog) << "whisper model checksum mismatch, redownloading" << spec.id;
        QFile::remove(destination);
    }

    const QString partPath = partPathFor(destination);
    qint64 resumeFrom = 0;
    if (QFileInfo::exists(partPath)) {
        const qint64 partSize = QFileInfo(partPath).size();
        if (partSize > spec.size) {
            QFile::remove(partPath);
        } else if (partSize == spec.size) {
            if (sha256HexOfFile(partPath).toLower() == spec.sha256Hex.toLower()) {
                QString error;
                if (!atomicReplace(partPath, destination, &error)) {
                    return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::DownloadFailure),
                        QStringLiteral("无法完成模型落盘"), error);
                }
                return destination;
            }
            QFile::remove(partPath);
        } else {
            resumeFrom = partSize;
        }
    }

    if (asrCancelled(cancel)) {
        return asrCancelledError();
    }

    QFile part(partPath);
    const QIODevice::OpenMode mode = resumeFrom > 0
        ? (QIODevice::Append | QIODevice::WriteOnly)
        : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!part.open(mode)) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::DownloadFailure),
            QStringLiteral("无法写入模型临时文件"), part.errorString());
    }

    HttpRequest request;
    request.url = spec.url;
    request.method = QByteArrayLiteral("GET");
    request.connectTimeoutMs = 15'000;
    request.transferTimeoutMs = 0;

    std::variant<qint64, AppError> downloaded =
        http_->download(request, &part, resumeFrom, cancel, progress);
    part.close();
    if (std::holds_alternative<AppError>(downloaded)) {
        return std::get<AppError>(downloaded);
    }

    if (!matchesSpec(partPath, spec)) {
        QFile::remove(partPath);
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::ChecksumMismatch),
            QStringLiteral("Whisper 模型 SHA-256 校验失败"));
    }

    QString error;
    if (!atomicReplace(partPath, destination, &error)) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::DownloadFailure),
            QStringLiteral("无法完成模型落盘"), error);
    }
    return destination;
}

} // namespace subcue
