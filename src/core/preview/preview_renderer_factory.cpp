#include "preview/preview_renderer.h"

#ifdef Q_OS_WIN
#include "platform/windows/d3d11_preview_adapter.h"
#endif

namespace subcue {

std::unique_ptr<IPreviewRenderer> PreviewRendererFactory::create(QQuickWindow *window)
{
#ifdef Q_OS_WIN
    Q_UNUSED(window);
    return std::make_unique<D3d11PreviewAdapter>();
#else
    Q_UNUSED(window);
    return std::make_unique<SoftwarePreviewRenderer>();
#endif
}

} // namespace subcue
