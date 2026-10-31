#pragma once

#include "GridItem.h"
#include "ImageItem.h"

#include <atomic>

namespace dot::test {

class CountingGrid : public GridItem {
public:
    std::atomic<int> backgrounds = 0;
    std::atomic<int> foregrounds = 0;

protected:
    QSGNode *updateBackgroundNode(QSGNode *oldNode, BackgroundLayer layer) override {
        backgrounds.fetch_add(1, std::memory_order_relaxed);
        return GridItem::updateBackgroundNode(oldNode, layer);
    }

    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override {
        foregrounds.fetch_add(1, std::memory_order_relaxed);
        return GridItem::updatePaintNode(oldNode, data);
    }
};

class CountingImages : public ImageItem {
public:
    std::atomic<bool> failTextures = false;
    std::atomic<int> textureAttempts = 0;

protected:
    QSGTexture *createTexture(QQuickWindow *window, const QImage &image) override {
        textureAttempts.fetch_add(1, std::memory_order_relaxed);
        if(failTextures.load(std::memory_order_relaxed)) return nullptr;
        return ImageItem::createTexture(window, image);
    }
};

} // namespace dot::test
