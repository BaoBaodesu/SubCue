#pragma once

#include "preview/preview_renderer.h"

#include <memory>

namespace subcue {

class D3d11PreviewAdapter final : public IPreviewRenderer {
public:
    D3d11PreviewAdapter();
    ~D3d11PreviewAdapter() override;

    [[nodiscard]] QString backendId() const override;
    void present(const QImage &image, MediaTime pts, quint64 generation) override;
    QSGNode *syncNode(QQuickItem *item, QSGNode *oldNode) override;
    [[nodiscard]] QImage lastImage() const override;
    [[nodiscard]] MediaTime lastPts() const override;
    [[nodiscard]] quint64 lastGeneration() const override;
    [[nodiscard]] bool hasFrame() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace subcue
