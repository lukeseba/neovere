//
// Created by lukebalfanz on 11/7/24.
//

#ifndef VIDEOSLIDER_H
#define VIDEOSLIDER_H
#include <QMediaPlayer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>


class VideoSlider: public QSlider {
    Q_OBJECT
public:
    explicit VideoSlider(QMediaPlayer *player, int sliderSize, QWidget *parent = nullptr);
    void assignButton(QPushButton *button);
    void updateTimeStamp(int position, int duration) const;
    void updateTimeStamp();


protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent (QEnterEvent *event) override;
    void leaveEvent (QEvent *event) override;
    void sliderChange(SliderChange change) override;
    void setVidPosition() const;


    QMediaPlayer *player;
    int sliderSize;
    QTimer* sliderUpdateTimer;
    QTimer* videoUpdateTimer;
    bool vidUpdate;
    bool manualSliderUpdate;
    QPushButton *button{};

protected:
    static QString convertToTimestamp(int seconds);

    void paintEvent(QPaintEvent *event) override;
};



#endif //VIDEOSLIDER_H
