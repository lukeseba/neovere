#include "PreviewWidget.h"
#include <QPainter>

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 90);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void PreviewWidget::setBuffer(FrameBufferReader* r) {
    reader = r;
    currentFrameIndex = -1;
    cachedImage = QImage();
    update();
}

void PreviewWidget::setFrameIndex(int idx) {
    if (idx == currentFrameIndex) return;
    currentFrameIndex = idx;
    buildCachedImage();
    update();
}

void PreviewWidget::refresh() {
    if (!reader || !reader->isOpen()) {
        cachedImage = QImage();
        update();
        return;
    }
    reader->refreshHeader();
    int frameCount = (int)reader->header().frame_count;
    if (frameCount == 0) {
        currentFrameIndex = -1;
        cachedImage = QImage();
    } else {
        if (currentFrameIndex < 0) currentFrameIndex = 0;
        if (currentFrameIndex >= frameCount) currentFrameIndex = frameCount - 1;
        buildCachedImage();
    }
    update();
}

void PreviewWidget::buildCachedImage() {
    cachedImage = QImage();
    if (!reader || !reader->isOpen()) return;
    if (currentFrameIndex < 0) return;
    const auto& h = reader->header();
    if (h.frame_count == 0) return;
    if ((uint32_t)currentFrameIndex >= h.frame_count) return;

    const uint8_t* data = reader->frameData((uint32_t)currentFrameIndex);
    if (!data) return;

    // Wrap the mmap'd bytes directly as a QImage. The underlying buffer must
    // outlive any rendering using this image — ok here because the QImage is
    // rebuilt before each paint and the reader is kept alive by the caller.
    cachedImage = QImage(
        data,
        (int)h.width,
        (int)h.height,
        (int)(h.width * h.channels),
        QImage::Format_RGB888
    );
}

void PreviewWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (cachedImage.isNull()) return;

    QSize widgetSize = size();
    QSize imgSize = cachedImage.size();
    QSize fit = imgSize.scaled(widgetSize, Qt::KeepAspectRatio);
    int x = (widgetSize.width() - fit.width()) / 2;
    int y = (widgetSize.height() - fit.height()) / 2;
    p.drawImage(QRect(QPoint(x, y), fit), cachedImage);
}
