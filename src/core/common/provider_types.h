#pragma once

#include "common/app_error.h"

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <optional>
#include <variant>

namespace subcue {

struct ProviderDescriptor final {
    QString id;
    QString name;
    QString baseUrl;
    QString authMode = QStringLiteral("bearer");
};

struct ModelDescriptor final {
    QString id;
    QString name;
    qint64 size = 0;
    bool ready = true;
};

struct ConnectionTestResult final {
    QVector<ModelDescriptor> models;
    QDateTime verifiedAtUtc;
};

using ProviderTestResult = std::variant<ConnectionTestResult, AppError>;
using ModelListResult = std::variant<QVector<ModelDescriptor>, AppError>;

struct AlignmentCredentials final {
    QString asrApiKey;
    QString aiApiKey;
};

struct PreflightIssue final {
    QString code;
    QString title;
    QString detail;
    QString settingsSection;
};

} // namespace subcue
