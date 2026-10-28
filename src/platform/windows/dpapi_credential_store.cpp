#include "platform/windows/dpapi_credential_store.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStandardPaths>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <dpapi.h>
#include <wincred.h>
#endif

#include <utility>

namespace subcue {
namespace {

const QString kDashScopeCredential = QStringLiteral("SubCue/ASR/dashscope");

}

DpapiCredentialStore::DpapiCredentialStore(QString path)
    : path_(path.isEmpty() ? defaultPath() : std::move(path)),
      productionTargets_(path_.compare(defaultPath(), Qt::CaseInsensitive) == 0)
{
}

QString DpapiCredentialStore::defaultPath()
{
    const QString appData = QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPDATA"));
    const QString base = appData.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        : QDir(appData).filePath(QStringLiteral("SubCue"));
    const QString destination = QDir(base).filePath(QStringLiteral("credentials.dat"));
    return destination;
}

QString DpapiCredentialStore::targetName(const QString &credentialId) const
{
    if (productionTargets_) {
        return credentialId;
    }
    const QByteArray suffix = QCryptographicHash::hash(
        QFileInfo(path_).absoluteFilePath().toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return QStringLiteral("SubCue/Test/%1/%2").arg(QString::fromLatin1(suffix), credentialId);
}

QString DpapiCredentialStore::load(const QString &credentialId, QString *errorMessage) const
{
#ifndef Q_OS_WIN
    Q_UNUSED(credentialId);
    if (errorMessage) *errorMessage = QStringLiteral("Windows Credential Manager 仅支持 Windows");
    return {};
#else
    PCREDENTIALW credential = nullptr;
    const std::wstring target = targetName(credentialId).toStdWString();
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND && errorMessage) {
            *errorMessage = QStringLiteral("读取 Windows 凭据失败：%1").arg(code);
        }
        return {};
    }
    const QByteArray bytes(
        reinterpret_cast<const char *>(credential->CredentialBlob),
        static_cast<qsizetype>(credential->CredentialBlobSize));
    CredFree(credential);
    return QString::fromUtf8(bytes);
#endif
}

bool DpapiCredentialStore::save(
    const QString &credentialId,
    const QString &secret,
    QString *errorMessage) const
{
#ifndef Q_OS_WIN
    Q_UNUSED(credentialId);
    Q_UNUSED(secret);
    if (errorMessage) *errorMessage = QStringLiteral("Windows Credential Manager 仅支持 Windows");
    return false;
#else
    const QByteArray bytes = secret.trimmed().toUtf8();
    if (bytes.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("API Key 不能为空");
        return false;
    }
    const std::wstring target = targetName(credentialId).toStdWString();
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t *>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(bytes.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(bytes.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0)) {
        if (errorMessage) *errorMessage = QStringLiteral("写入 Windows 凭据失败：%1").arg(GetLastError());
        return false;
    }
    return true;
#endif
}

bool DpapiCredentialStore::remove(const QString &credentialId, QString *errorMessage) const
{
#ifndef Q_OS_WIN
    Q_UNUSED(credentialId);
    if (errorMessage) *errorMessage = QStringLiteral("Windows Credential Manager 仅支持 Windows");
    return false;
#else
    const std::wstring target = targetName(credentialId).toStdWString();
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        return true;
    }
    const DWORD code = GetLastError();
    if (code == ERROR_NOT_FOUND) {
        return true;
    }
    if (errorMessage) *errorMessage = QStringLiteral("删除 Windows 凭据失败：%1").arg(code);
    return false;
#endif
}

bool DpapiCredentialStore::exists(const QString &credentialId) const
{
#ifdef Q_OS_WIN
    PCREDENTIALW credential = nullptr;
    const auto target = targetName(credentialId).toStdWString();
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return false;
    const bool present = credential->CredentialBlobSize > 0;
    if (credential->CredentialBlobSize) SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return present;
#else
    Q_UNUSED(credentialId);
    return false;
#endif
}

QStringList DpapiCredentialStore::list(const QString &prefix, QString *errorMessage) const
{
    QStringList result;
#ifdef Q_OS_WIN
    if (!prefix.startsWith(QStringLiteral("SubCue/Cloud/"))
        && !QStringList{QStringLiteral("SubCue/ASR/"), QStringLiteral("SubCue/AIReview/"),
            QStringLiteral("SubCue/AI/"), QStringLiteral("SubCue/VoiceStudio/")}.contains(prefix)) {
        if (errorMessage) *errorMessage = QStringLiteral("拒绝枚举非云端凭据命名空间");
        return result;
    }
    DWORD count = 0;
    PCREDENTIALW *credentials = nullptr;
    const auto filter = (targetName(prefix) + QLatin1Char('*')).toStdWString();
    if (!CredEnumerateW(filter.c_str(), 0, &count, &credentials)) {
        if (GetLastError() != ERROR_NOT_FOUND && errorMessage) *errorMessage = QStringLiteral("无法枚举云端凭据");
        return result;
    }
    const auto namespacePrefix = productionTargets_ ? QString() : targetName(QString());
    for (DWORD index = 0; index < count; ++index) {
        const auto target = QString::fromWCharArray(credentials[index]->TargetName);
        if (credentials[index]->Type == CRED_TYPE_GENERIC && target.startsWith(namespacePrefix + prefix))
            result.append(target.mid(namespacePrefix.size()));
        if (credentials[index]->CredentialBlobSize) SecureZeroMemory(credentials[index]->CredentialBlob, credentials[index]->CredentialBlobSize);
    }
    CredFree(credentials);
#else
    Q_UNUSED(prefix);
    if (errorMessage) *errorMessage = QStringLiteral("Windows Credential Manager 仅支持 Windows");
#endif
    return result;
}

QString DpapiCredentialStore::loadLegacy(QString *errorMessage) const
{
#ifndef Q_OS_WIN
    if (errorMessage) *errorMessage = QStringLiteral("DPAPI 仅支持 Windows");
    return {};
#else
    QFile file(path_);
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return {};
    }
    const QByteArray encrypted = file.readAll();
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(encrypted.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        if (errorMessage) *errorMessage = QStringLiteral("旧 DPAPI 凭据解密失败：%1").arg(GetLastError());
        return {};
    }
    const QByteArray plain(reinterpret_cast<const char *>(output.pbData),
                           static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return QString::fromUtf8(plain);
#endif
}

bool DpapiCredentialStore::migrateLegacyDashScope(QString *errorMessage) const
{
    // 生产环境不再自动导入旧凭据，防止清除后在重启时恢复。
    if (productionTargets_) return true;
    if (exists(kDashScopeCredential) || !QFileInfo::exists(path_)) {
        return true;
    }
    const QString legacy = loadLegacy(errorMessage);
    if (legacy.isEmpty()) {
        return false;
    }
    if (!save(kDashScopeCredential, legacy, errorMessage)
        || load(kDashScopeCredential) != legacy.trimmed()) {
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("旧凭据迁移后校验失败");
        }
        return false;
    }
    if (!QFile::remove(path_)) {
        if (errorMessage) *errorMessage = QStringLiteral("旧凭据已迁移，但无法删除 credentials.dat");
        return false;
    }
    return true;
}

QString DpapiCredentialStore::load(QString *errorMessage) const
{
    return load(kDashScopeCredential, errorMessage);
}

bool DpapiCredentialStore::save(const QString &apiKey, QString *errorMessage) const
{
    return save(kDashScopeCredential, apiKey, errorMessage);
}

bool DpapiCredentialStore::hasSavedKey() const
{
    return exists(kDashScopeCredential);
}

QString DpapiCredentialStore::getKey(const QString &userInput) const
{
    const QString saved = load(kDashScopeCredential);
    return saved.isEmpty() ? userInput.trimmed() : saved;
}

} // namespace subcue
