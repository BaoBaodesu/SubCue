#pragma once

#include "common/app_error.h"
#include "common/media_time.h"

#include <QtCore/QString>
#include <QtCore/QVector>

namespace subcue {

struct FrameIndexEntry final {
    int index = -1;
    MediaTime pts = MediaTime::fromMicroseconds(-1);
    qint64 seekTimestamp = 0;
    bool keyframe = false;
};

class FrameIndex final {
public:
    [[nodiscard]] bool build(const QString &path, AppError *error = nullptr);
    void clear();

    [[nodiscard]] int size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool isEmpty() const noexcept { return entries_.isEmpty(); }
    [[nodiscard]] const FrameIndexEntry &at(int index) const { return entries_.at(index); }
    [[nodiscard]] int findIndexAtOrAfter(MediaTime pts) const;
    [[nodiscard]] int keyframeIndexAtOrBefore(int frameIndex) const;

private:
    QVector<FrameIndexEntry> entries_;
};

} // namespace subcue
