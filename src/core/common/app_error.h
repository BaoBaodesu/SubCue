#pragma once

#include <QtCore/QString>

namespace subcue {

enum class ErrorDomain {
    Media,
    Decoder,
    Project,
    Asr,
    Ai,
    Network,
    Settings,
    Validation
};

class AppError final {
public:
    AppError(ErrorDomain domain, int code, QString userMessage, QString technicalDetails = {});

    [[nodiscard]] ErrorDomain domain() const noexcept { return domain_; }
    [[nodiscard]] int code() const noexcept { return code_; }
    [[nodiscard]] const QString &userMessage() const noexcept { return userMessage_; }
    [[nodiscard]] const QString &technicalDetails() const noexcept { return technicalDetails_; }

private:
    ErrorDomain domain_;
    int code_;
    QString userMessage_;
    QString technicalDetails_;
};

} // namespace subcue
