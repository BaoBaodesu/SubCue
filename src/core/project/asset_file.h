#pragma once
#include <QtCore/QString>
#include <QtCore/QPair>
#include <QtCore/QByteArray>
#include <atomic>
#include <optional>
namespace subcue {
std::optional<QPair<QString, QByteArray>> copyAssetFile(const QString &sourcePath, const QString &directory,
    QString *error, const std::atomic<bool> *cancel);
std::optional<QPair<QString, QByteArray>> copyAssetFile(const QString &sourcePath, const QString &directory,
    QString *error, const std::atomic<bool> *cancel, bool *created);
}
