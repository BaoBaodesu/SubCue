#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>

namespace subcue {

// 对应 models/transcript.py。word id 语义与原后端一致：sortAndReindex 后从 1 编号。
struct TranscriptWord final {
    qint64 id = -1;
    QString text;
    qint64 startMs = 0;
    qint64 endMs = 0;
    bool preciseTiming = true;
};

struct Transcript final {
    QVector<TranscriptWord> words;

    // 按 (startMs, endMs) 稳定排序并重新编号 id（从 1 开始）。
    void sortAndReindex();
};

} // namespace subcue
