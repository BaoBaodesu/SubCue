#include "video_preview_item.h"

namespace subcue {

VideoPreviewItem::VideoPreviewItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    ensureRenderer();
}

QString VideoPreviewItem::backendId() const
{
    return renderer_ ? renderer_->backendId() : QStringLiteral("none");
}

bool VideoPreviewItem::hasFrame() const
{
    return renderer_ && renderer_->hasFrame();
}

qint64 VideoPreviewItem::framePtsUs() const
{
    return renderer_ ? renderer_->lastPts().microseconds() : -1;
}

QImage VideoPreviewItem::lastImage() const
{
    return renderer_ ? renderer_->lastImage() : QImage();
}

void VideoPreviewItem::presentImage(const QImage &image, qint64 ptsUs, quint64 generation)
{
    present(image, MediaTime::fromMicroseconds(ptsUs), generation);
}

void VideoPreviewItem::present(const QImage &image, MediaTime pts, quint64 generation)
{
    ensureRenderer();
    const QString previousBackend = backendId();
    renderer_->present(image, pts, generation);
    if (backendId() != previousBackend) {
        emit backendIdChanged();
    }
    emit frameChanged();
    update();
}

void VideoPreviewItem::clearFrame()
{
    ensureRenderer();
    renderer_->present(QImage(), MediaTime::fromMicroseconds(-1), 0);
    emit frameChanged();
    update();
}

QSGNode *VideoPreviewItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    ensureRenderer();
    QSGNode *node = renderer_->syncNode(this, oldNode);
    return node;
}

void VideoPreviewItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange) {
        const QImage image = lastImage();
        const MediaTime pts = renderer_ ? renderer_->lastPts() : MediaTime::fromMicroseconds(-1);
        const quint64 generation = renderer_ ? renderer_->lastGeneration() : 0;
        renderer_.reset();
        ensureRenderer();
        if (!image.isNull()) {
            renderer_->present(image, pts, generation);
        }
        emit backendIdChanged();
        update();
    }
}

void VideoPreviewItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        update();
    }
}

void VideoPreviewItem::ensureRenderer()
{
    if (!renderer_) {
        renderer_ = PreviewRendererFactory::create(window());
    }
}

} // namespace subcue
