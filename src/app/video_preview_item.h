#pragma once

#include "preview/preview_renderer.h"

#include <QtGui/QImage>
#include <QtQml/qqmlregistration.h>
#include <QtQuick/QQuickItem>

#include <memory>

namespace subcue {

class VideoPreviewItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    QML_NAMED_ELEMENT(VideoPreviewItem)
    Q_PROPERTY(QString backendId READ backendId NOTIFY backendIdChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)
    Q_PROPERTY(qint64 framePtsUs READ framePtsUs NOTIFY frameChanged)

public:
    explicit VideoPreviewItem(QQuickItem *parent = nullptr);

    [[nodiscard]] QString backendId() const;
    [[nodiscard]] bool hasFrame() const;
    [[nodiscard]] qint64 framePtsUs() const;
    [[nodiscard]] QImage lastImage() const;

    Q_INVOKABLE void presentImage(const QImage &image, qint64 ptsUs = 0, quint64 generation = 0);
    void present(const QImage &image, MediaTime pts, quint64 generation);
    void clearFrame();

signals:
    void backendIdChanged();
    void frameChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void ensureRenderer();

    std::unique_ptr<IPreviewRenderer> renderer_;
};

} // namespace subcue
