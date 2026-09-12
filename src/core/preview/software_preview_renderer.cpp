#include "preview/preview_renderer.h"

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGSimpleTextureNode>
#include <QtQuick/QSGTexture>

namespace subcue {

QString SoftwarePreviewRenderer::backendId() const
{
    return QStringLiteral("software");
}

void SoftwarePreviewRenderer::present(const QImage &image, MediaTime pts, quint64 generation)
{
    image_ = image;
    pts_ = pts;
    generation_ = generation;
}

QSGNode *SoftwarePreviewRenderer::syncNode(QQuickItem *item, QSGNode *oldNode)
{
    if (item == nullptr || image_.isNull() || item->window() == nullptr) {
        delete oldNode;
        return nullptr;
    }

    auto *node = dynamic_cast<QSGSimpleTextureNode *>(oldNode);
    if (node == nullptr) {
        delete oldNode;
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
    }

    QSGTexture *texture = item->window()->createTextureFromImage(image_);
    node->setTexture(texture);
    const QRectF bounds = item->boundingRect();
    if (image_.width() <= 0 || image_.height() <= 0 || bounds.isEmpty()) {
        node->setRect(bounds);
        return node;
    }

    const double imageAspect = static_cast<double>(image_.width()) / static_cast<double>(image_.height());
    const double viewAspect = bounds.width() / bounds.height();
    QRectF fitted = bounds;
    if (imageAspect > viewAspect) {
        const double height = bounds.width() / imageAspect;
        fitted.setY(bounds.y() + (bounds.height() - height) * 0.5);
        fitted.setHeight(height);
    } else {
        const double width = bounds.height() * imageAspect;
        fitted.setX(bounds.x() + (bounds.width() - width) * 0.5);
        fitted.setWidth(width);
    }
    node->setRect(fitted);
    return node;
}

QImage SoftwarePreviewRenderer::lastImage() const
{
    return image_;
}

MediaTime SoftwarePreviewRenderer::lastPts() const
{
    return pts_;
}

quint64 SoftwarePreviewRenderer::lastGeneration() const
{
    return generation_;
}

bool SoftwarePreviewRenderer::hasFrame() const
{
    return !image_.isNull();
}

} // namespace subcue
