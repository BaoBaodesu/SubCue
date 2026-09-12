#include "platform/windows/d3d11_preview_adapter.h"

#include "preview/preview_renderer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d11.h>

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtQuick/QSGSimpleTextureNode>
#include <QtQuick/QSGTexture>
#include <QtQuick/qsgtexture_platform.h>

#include <utility>

namespace subcue {
namespace {

struct DxReleaser {
    template<typename T>
    void operator()(T *value) const noexcept
    {
        if (value) {
            value->Release();
        }
    }
};

} // namespace

struct D3d11PreviewAdapter::Impl {
    SoftwarePreviewRenderer software;
    QString backend = QStringLiteral("d3d11-fallback");
    std::unique_ptr<ID3D11Texture2D, DxReleaser> texture;
    QSize textureSize;
    bool uploaded = false;

    void resetGpu()
    {
        texture.reset();
        textureSize = {};
        uploaded = false;
        backend = QStringLiteral("d3d11-fallback");
    }
};

D3d11PreviewAdapter::D3d11PreviewAdapter()
    : impl_(std::make_unique<Impl>())
{
}

D3d11PreviewAdapter::~D3d11PreviewAdapter() = default;

QString D3d11PreviewAdapter::backendId() const
{
    return impl_->backend;
}

void D3d11PreviewAdapter::present(const QImage &image, MediaTime pts, quint64 generation)
{
    impl_->software.present(image, pts, generation);
    impl_->uploaded = false;
}

QSGNode *D3d11PreviewAdapter::syncNode(QQuickItem *item, QSGNode *oldNode)
{
    QQuickWindow *window = item ? item->window() : nullptr;
    QSGRendererInterface *rif = window ? window->rendererInterface() : nullptr;
    const QImage image = impl_->software.lastImage();
    if (window == nullptr || rif == nullptr || image.isNull()
        || rif->graphicsApi() != QSGRendererInterface::Direct3D11) {
        impl_->resetGpu();
        return impl_->software.syncNode(item, oldNode);
    }

    auto *device = static_cast<ID3D11Device *>(
        rif->getResource(window, QSGRendererInterface::DeviceResource));
    if (device == nullptr) {
        impl_->resetGpu();
        return impl_->software.syncNode(item, oldNode);
    }

    const QImage converted = image.convertToFormat(QImage::Format_RGBA8888);
    const QSize size = converted.size();
    if (impl_->texture && impl_->textureSize != size) {
        impl_->texture.reset();
    }
    if (!impl_->texture) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(size.width());
        description.Height = static_cast<UINT>(size.height());
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D *textureRaw = nullptr;
        if (FAILED(device->CreateTexture2D(&description, nullptr, &textureRaw)) || textureRaw == nullptr) {
            impl_->resetGpu();
            return impl_->software.syncNode(item, oldNode);
        }
        impl_->texture.reset(textureRaw);
        impl_->textureSize = size;
    }

    ID3D11DeviceContext *context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr) {
        impl_->resetGpu();
        return impl_->software.syncNode(item, oldNode);
    }
    context->UpdateSubresource(
        impl_->texture.get(),
        0,
        nullptr,
        converted.constBits(),
        static_cast<UINT>(converted.bytesPerLine()),
        0);
    context->Release();

    QSGTexture *sgTexture = QNativeInterface::QSGD3D11Texture::fromNative(
        impl_->texture.get(), window, size, QQuickWindow::TextureHasAlphaChannel);
    if (sgTexture == nullptr) {
        impl_->resetGpu();
        return impl_->software.syncNode(item, oldNode);
    }

    auto *node = dynamic_cast<QSGSimpleTextureNode *>(oldNode);
    if (node == nullptr) {
        delete oldNode;
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
    }
    node->setTexture(sgTexture);

    const QRectF bounds = item->boundingRect();
    QRectF fitted = bounds;
    if (size.width() > 0 && size.height() > 0 && !bounds.isEmpty()) {
        const double imageAspect = static_cast<double>(size.width()) / static_cast<double>(size.height());
        const double viewAspect = bounds.width() / bounds.height();
        if (imageAspect > viewAspect) {
            const double height = bounds.width() / imageAspect;
            fitted.setY(bounds.y() + (bounds.height() - height) * 0.5);
            fitted.setHeight(height);
        } else {
            const double width = bounds.height() * imageAspect;
            fitted.setX(bounds.x() + (bounds.width() - width) * 0.5);
            fitted.setWidth(width);
        }
    }
    node->setRect(fitted);
    impl_->backend = QStringLiteral("d3d11");
    impl_->uploaded = true;
    return node;
}

QImage D3d11PreviewAdapter::lastImage() const
{
    return impl_->software.lastImage();
}

MediaTime D3d11PreviewAdapter::lastPts() const
{
    return impl_->software.lastPts();
}

quint64 D3d11PreviewAdapter::lastGeneration() const
{
    return impl_->software.lastGeneration();
}

bool D3d11PreviewAdapter::hasFrame() const
{
    return impl_->software.hasFrame();
}

} // namespace subcue
