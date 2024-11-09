//
// Created by lukebalfanz on 11/7/24.
//

#include "VideoSlider.h"
#include <iostream>
#include <qevent.h>

VideoSlider::VideoSlider(QMediaPlayer *player, int sliderSize, QWidget *parent) : QSlider(Qt::Horizontal, parent) {
    this->player = player;
    this->sliderSize = sliderSize;
    this->setRange(0, sliderSize);
    this->videoUpdateTimer = new QTimer();
    this->sliderUpdateTimer = new QTimer();
    this->vidUpdate = true;
    this->manualSliderUpdate = false;

    videoUpdateTimer->setInterval(100);
    sliderUpdateTimer->setInterval(500);
    videoUpdateTimer->setSingleShot(true);
    sliderUpdateTimer->setSingleShot(true);

    // Connect media player position change to update slider position
    QObject::connect(player, &QMediaPlayer::positionChanged, [=](qint64 position) {
        if (!videoUpdateTimer->isActive() && !manualSliderUpdate) {
            if (player->duration() > 0) {
                vidUpdate = true;
                this->setValue(int((position * sliderSize) / player->duration())); // Update slider position
                vidUpdate = false;
                videoUpdateTimer->start();
            }
        }
    });
}

void VideoSlider::mousePressEvent(QMouseEvent *event) {
    QSlider::mousePressEvent(event);
}
void VideoSlider::mouseReleaseEvent(QMouseEvent *event) {
    if (manualSliderUpdate) {
        manualSliderUpdate = false;
        setVidPosition();
        player->play();
        sliderUpdateTimer->stop();
    }
    QSlider::mouseReleaseEvent(event);
}
void VideoSlider::enterEvent(QEnterEvent *event) {
    QSlider::enterEvent(event);
}
void VideoSlider::leaveEvent(QEvent *event) {
    QSlider::leaveEvent(event);
}

void VideoSlider::sliderChange(SliderChange change) {
    if (change == SliderValueChange && !vidUpdate && !sliderUpdateTimer->isActive()) {
        if (player->isPlaying()) {
            player->pause();
        }
        manualSliderUpdate = true;
        setVidPosition();
        sliderUpdateTimer->start();
    }
    QSlider::sliderChange(change);
}

void VideoSlider::setVidPosition() {
    player->setPosition((static_cast<qint64>(this->value()) * player->duration())/ sliderSize);
}

