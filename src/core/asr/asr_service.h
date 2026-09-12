#pragma once

#include "asr/asr_types.h"
#include "common/provider_types.h"

namespace subcue {

class IAsrService {
public:
    virtual ~IAsrService() = default;

    [[nodiscard]] virtual QString providerId() const = 0;
    [[nodiscard]] virtual ProviderTestResult testConnection(
        const std::atomic<bool> *cancel = nullptr)
    {
        Q_UNUSED(cancel);
        return ConnectionTestResult{{}, QDateTime::currentDateTimeUtc()};
    }
    [[nodiscard]] virtual AsrResult transcribe(const AsrRequest &request) = 0;
};

} // namespace subcue
