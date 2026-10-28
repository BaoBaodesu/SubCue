#pragma once
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QString>

namespace subcue {
// 文稿是编辑模型；JSON 用于快照／交换，工程内由关系表保存各实体。
class VoiceDocument final {
public:
    static QJsonObject empty();
    static bool validate(const QJsonObject &document, QString *error = nullptr);
    static QStringList sentences(const QString &text);
    static QString normalizeChinese(const QString &text);
    static QJsonObject segment(const QString &paragraphId, const QString &text);
    static QString spokenText(const QJsonObject &document, const QJsonObject &segment);
    static QString effectiveVoiceVersion(const QJsonObject &document, const QJsonObject &segment);
    static QJsonObject setDefaultVoice(QJsonObject document, const QString &versionId);
    static QByteArray inputKey(const QJsonObject &document, const QJsonObject &segment);
    static QJsonObject appendText(QJsonObject document, const QString &text);
    static QJsonObject appendStructured(QJsonObject document, const QJsonObject &incoming, QString *error);
    static QJsonObject edit(QJsonObject document, const QString &id, const QString &text,
        const QString &spoken, bool manual, const QString &notes);
    static QJsonObject split(QJsonObject document, const QString &id, int offset, QString *error);
    static QJsonObject merge(QJsonObject document, const QString &id, QString *error);
    static QJsonObject remove(QJsonObject document, const QString &id);
    static QJsonObject resegment(QJsonObject document, const QString &paragraphId, const QString &text);
    static QJsonObject targetedUpdate(QJsonObject document, const QJsonObject &incoming,
        const QString &projectId, QJsonArray *differences, QString *error);
    static QJsonObject applyTake(QJsonObject document, const QString &id, const QJsonObject &take,
        const QString &cropPolicy, QString *error);
};
}
