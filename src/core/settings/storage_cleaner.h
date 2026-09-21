#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <atomic>

namespace subcue {

struct StorageTarget final {
    QString id;
    QString group;
    QString label;
    QString path;
    qint64 bytes = 0;
};

struct StorageCleanupResult final {
    bool ok = true;
    qint64 bytesRemoved = 0;
    QString error;
    QStringList removed;
};

class StorageCleaner final {
public:
    StorageCleaner(QString projectRoot, QString modelsRoot, QString keepBuildDirectory);

    [[nodiscard]] QVector<StorageTarget> scan() const;
    [[nodiscard]] StorageCleanupResult remove(
        const QStringList &ids,
        const std::atomic<bool> *cancel = nullptr) const;
    [[nodiscard]] bool isPathAllowed(const QString &path) const;
    [[nodiscard]] QString keepBuildName() const;
    [[nodiscard]] bool developmentTreeVisible() const;

private:
    [[nodiscard]] QString normalize(const QString &path) const;
    [[nodiscard]] bool under(const QString &path, const QString &root) const;
    [[nodiscard]] qint64 directoryBytes(const QString &path) const;
    void appendIfExists(QVector<StorageTarget> *targets, StorageTarget target) const;

    QString projectRoot_;
    QString modelsRoot_;
    QString keepBuildDirectory_;
};

} // namespace subcue
