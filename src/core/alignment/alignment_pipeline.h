#pragma once

#include "ai/ai_provider.h"
#include "alignment/alignment_result.h"
#include "asr/asr_service.h"
#include "common/app_error.h"
#include "common/provider_types.h"
#include "media/media_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <atomic>
#include <functional>
#include <utility>
#include <variant>

namespace subcue {

struct AlignmentTaskOutput final {
    AlignmentResult result;
    MediaInfo mediaInfo;
    QStringList outputPaths;
};

struct AlignmentProgressState final {
    QString stage;
    QString message;
    int completed = 0;
    int total = 0;
    [[nodiscard]] int percent() const { return total > 0 ? qBound(0, completed, total) * 100 / total : 0; }
    [[nodiscard]] bool indeterminate() const { return total <= 0; }
};
using AlignmentProgress = std::function<void(const AlignmentProgressState &)>;
using AlignmentRunResult = std::variant<AlignmentTaskOutput, AppError>;

class AlignmentPipeline final {
public:
    AlignmentPipeline(
        QJsonObject settings,
        AlignmentCredentials credentials,
        IAsrService *asrOverride = nullptr,
        IAiProvider *aiOverride = nullptr);

    [[nodiscard]] AlignmentRunResult run(
        const QString &mediaPath,
        const QStringList &lines,
        const std::atomic<bool> *cancel = nullptr,
        const AlignmentProgress &progress = {});

    [[nodiscard]] static std::variant<QStringList, AppError> exportResult(
        const QString &mediaPath,
        const AlignmentResult &result,
        const MediaInfo &mediaInfo,
        const QJsonObject &settings);

    [[nodiscard]] static std::pair<int, int> videoSize(const MediaInfo &mediaInfo);

private:
    QJsonObject settings_;
    AlignmentCredentials credentials_;
    IAsrService *asrOverride_ = nullptr;
    IAiProvider *aiOverride_ = nullptr;
};

} // namespace subcue
