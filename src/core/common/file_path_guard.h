#pragma once

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

namespace subcue {

inline bool sameFilePath(const QString &left, const QString &right)
{
    if (left.isEmpty() || right.isEmpty()) return false;
    const QFileInfo a(left), b(right);
    const QString ap = a.canonicalFilePath().isEmpty() ? a.absoluteFilePath() : a.canonicalFilePath();
    const QString bp = b.canonicalFilePath().isEmpty() ? b.absoluteFilePath() : b.canonicalFilePath();
#ifdef Q_OS_WIN
    if (QDir::cleanPath(ap).compare(QDir::cleanPath(bp), Qt::CaseInsensitive) == 0) return true;
    // 路径不同的硬链接也可能指向同一个源文件。
    const HANDLE ah = CreateFileW(reinterpret_cast<LPCWSTR>(ap.utf16()), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    const HANDLE bh = CreateFileW(reinterpret_cast<LPCWSTR>(bp.utf16()), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    BY_HANDLE_FILE_INFORMATION ai{}, bi{};
    const bool same = ah != INVALID_HANDLE_VALUE && bh != INVALID_HANDLE_VALUE
        && GetFileInformationByHandle(ah, &ai) && GetFileInformationByHandle(bh, &bi)
        && ai.dwVolumeSerialNumber == bi.dwVolumeSerialNumber
        && ai.nFileIndexHigh == bi.nFileIndexHigh && ai.nFileIndexLow == bi.nFileIndexLow;
    if (ah != INVALID_HANDLE_VALUE) CloseHandle(ah);
    if (bh != INVALID_HANDLE_VALUE) CloseHandle(bh);
    return same;
#else
    return QDir::cleanPath(ap) == QDir::cleanPath(bp);
#endif
}

inline bool safeOutputPath(const QString &path, const QStringList &inputs, QString *error = nullptr)
{
    if (path.isEmpty()) {
        if (error) *error = QStringLiteral("输出路径为空。");
        return false;
    }
    for (const QString &input : inputs) {
        if (!sameFilePath(path, input)) continue;
        if (error) *error = QStringLiteral("输出不能覆盖输入文件：%1").arg(input);
        return false;
    }
    return true;
}

} // namespace subcue
