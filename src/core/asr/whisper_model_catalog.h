#pragma once

#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVector>

#include <optional>

namespace subcue {

struct WhisperModelSpec final {
    QString id;
    QUrl url;
    qint64 size = 0;
    QByteArray sha256Hex;
    QString fileName;
};

class WhisperModelCatalog final {
public:
    // 固定清单：Hugging Face ggerganov/whisper.cpp 的 ggml 模型。
    // SHA-256 取自 Hugging Face LFS oid，大小为字节数。模型本身不随程序分发。
    [[nodiscard]] static QVector<WhisperModelSpec> all();
    [[nodiscard]] static std::optional<WhisperModelSpec> find(QStringView id);
    [[nodiscard]] static QString fileNameFor(QStringView id);
};

} // namespace subcue
