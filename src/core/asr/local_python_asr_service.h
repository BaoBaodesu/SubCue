#pragma once

#include "asr/asr_service.h"

namespace subcue {

// 通过独立 Python 进程调用官方本地 SDK，避免主程序加载推理依赖。
class LocalPythonAsrService final : public IAsrService {
public:
    LocalPythonAsrService(QString providerId, QString modelDirectory, QString forcedAlignerDirectory,
                          QString pythonExecutable);
    [[nodiscard]] QString providerId() const override;
    [[nodiscard]] ProviderTestResult testConnection(const std::atomic<bool> *cancel = nullptr) override;
    [[nodiscard]] AsrResult transcribe(const AsrRequest &request) override;
    [[nodiscard]] static bool modelReady(const QString &directory);
private:
    QString providerId_;
    QString modelDirectory_;
    QString forcedAlignerDirectory_;
    QString pythonExecutable_;
};

} // namespace subcue
