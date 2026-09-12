#pragma once

#include "common/media_time.h"

#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtQuick/QQuickItem>
#include <QtQuick/QSGNode>

#include <memory>

class QQuickWindow;

namespace subcue {

class IPreviewRenderer {
public:
    virtual ~IPreviewRenderer() = default;

    [[nodiscard]] virtual QString backendId() const = 0;
    virtual void present(const QImage &image, MediaTime pts, quint64 generation) = 0;
    virtual QSGNode *syncNode(QQuickItem *item, QSGNode *oldNode) = 0;
    [[nodiscard]] virtual QImage lastImage() const = 0;
    [[nodiscard]] virtual MediaTime lastPts() const = 0;
    [[nodiscard]] virtual quint64 lastGeneration() const = 0;
    [[nodiscard]] virtual bool hasFrame() const = 0;
};

class SoftwarePreviewRenderer final : public IPreviewRenderer {
public:
    [[nodiscard]] QString backendId() const override;
    void present(const QImage &image, MediaTime pts, quint64 generation) override;
    QSGNode *syncNode(QQuickItem *item, QSGNode *oldNode) override;
    [[nodiscard]] QImage lastImage() const override;
    [[nodiscard]] MediaTime lastPts() const override;
    [[nodiscard]] quint64 lastGeneration() const override;
    [[nodiscard]] bool hasFrame() const override;

private:
    QImage image_;
    MediaTime pts_ = MediaTime::fromMicroseconds(-1);
    quint64 generation_ = 0;
};

class PreviewRendererFactory final {
public:
    [[nodiscard]] static std::unique_ptr<IPreviewRenderer> create(QQuickWindow *window = nullptr);
};

} // namespace subcue
