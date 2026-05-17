#ifndef VIDEOSLIDER_H
#define VIDEOSLIDER_H

#include <QMediaPlayer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QColor>
#include "MediaFrame.h"

class VideoSlider: public QSlider {
    Q_OBJECT
public:
    explicit VideoSlider(MediaFrame *mediaPanel, int sliderSize, QWidget *parent = nullptr);
    void assignButton(QPushButton *button);
    void updateTimeStamp(int position, int duration) const;
    void updateTimeStamp();
    void setColor(const QColor &filledColor, const QColor &backgroundBaseColor = QColor(120, 130, 150));

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void sliderChange(SliderChange change) override;
    void setVidPosition() const;
    void paintEvent(QPaintEvent *event) override;

private:
    MediaFrame *mediaPanel;
    int sliderSize;
    QTimer* sliderUpdateTimer;
    QTimer* videoUpdateTimer;
    bool vidUpdate;
    bool manualSliderUpdate;
    bool wasPlayingBeforeInteraction; // <--- Tracks state before click/drag
    QPushButton *button{};
    QColor filledColor = QColor(185, 205, 230);
    QColor backgroundBaseColor = QColor(120, 130, 150);

    static QString convertToTimestamp(int seconds);
};

#endif // VIDEOSLIDER_H