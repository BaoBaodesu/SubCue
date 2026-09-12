#pragma once

#include "asr/asr_types.h"

#include <QtCore/QString>

#include <memory>

namespace subcue {

class IWhisperEngine {
public:
    virtual ~IWhisperEngine() = default;
    [[nodiscard]] virtual bool isAvailable() const noexcept { return true; }
    [[nodiscard]] virtual bool setModelPath(
        const QString &path,
        QString *errorMessage = nullptr)
    {
        Q_UNUSED(path);
        Q_UNUSED(errorMessage);
        return true;
    }

    [[nodiscard]] virtual AsrResult transcribePcm(
        const QVector<float> &pcm16kMono,
        const std::atomic<bool> *cancel = nullptr) = 0;
};

// 未链接 whisper.cpp 核心库时的占位实现。CTest 注入 FakeWhisperEngine，
// 真正 ggml 推理在提供 SUBCUE_WHISPER_ROOT 后接入。
class UnlinkedWhisperEngine final : public IWhisperEngine {
public:
    [[nodiscard]] bool isAvailable() const noexcept override { return false; }
    AsrResult transcribePcm(
        const QVector<float> &pcm16kMono,
        const std::atomic<bool> *cancel = nullptr) override;
};

#ifdef SUBCUE_HAS_WHISPER
class LinkedWhisperEngine final : public IWhisperEngine {
public:
    explicit LinkedWhisperEngine(QString device = QStringLiteral("auto"));
    ~LinkedWhisperEngine() override;

    [[nodiscard]] bool isAvailable() const noexcept override { return true; }
    [[nodiscard]] bool setModelPath(const QString &path, QString *errorMessage = nullptr) override;
    AsrResult transcribePcm(
        const QVector<float> &pcm16kMono,
        const std::atomic<bool> *cancel = nullptr) override;

private:
    struct Private;
    std::unique_ptr<Private> d_;
};
#endif

} // namespace subcue
