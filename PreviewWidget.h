#ifndef PREVIEWWIDGET_H
#define PREVIEWWIDGET_H

#include <QWidget>
#include <QImage>
#include "FrameBufferReader.h"

// Paints a single frame from a shared-memory FrameBufferReader.
// Aspect-fit, black letterboxing, no cv2/QMediaPlayer involved.
class PreviewWidget : public QWidget {
    Q_OBJECT

public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    // Bind to a frame buffer reader. Does NOT take ownership.
    // Pass nullptr to clear.
    void setBuffer(FrameBufferReader* reader);

    // -1 means "no frame; paint black".
    void setFrameIndex(int idx);
    int frameIndex() const { return currentFrameIndex; }

    // Re-read the buffer header (generation may have changed) and repaint.
    // Clamps current frame index to the new frame_count.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    FrameBufferReader* reader = nullptr;
    int currentFrameIndex = -1;
    QImage cachedImage;

    void buildCachedImage();
};

#endif  // PREVIEWWIDGET_H
