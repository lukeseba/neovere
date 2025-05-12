// VideoSlider.cpp
//
// Created by lukebalfanz on 11/7/24.
//

#include "VideoSlider.h"

#include <iostream>
#include <ostream>
#include <QPainter>
#include <QStyleOptionSlider>

VideoSlider::VideoSlider(QMediaPlayer *player, int sliderSize, QWidget *parent) : QSlider(Qt::Horizontal, parent) {
    this->player = player;
    this->sliderSize = sliderSize;
    this->setRange(0, sliderSize);
    this->videoUpdateTimer = new QTimer();
    this->sliderUpdateTimer = new QTimer();
    this->vidUpdate = true;
    this->manualSliderUpdate = false;

    videoUpdateTimer->setInterval(100);
    sliderUpdateTimer->setInterval(250);
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
                updateTimeStamp(int(position), player->duration());
            }
        }
    });

    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, [=](QMediaPlayer::MediaStatus status) {
        updateTimeStamp(player->position(), player->duration());
    });

    setMouseTracking(true); // Enable hover detection
}

// Add the new method implementation
void VideoSlider::setColor(const QColor &filledColor, const QColor &backgroundBaseColor) {
    this->filledColor = filledColor;
    this->backgroundBaseColor = backgroundBaseColor;
    update(); // Trigger repaint with new colors
}

// Modify the paintEvent to use the color variables
void VideoSlider::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    QStyleOptionSlider opt;
    initStyleOption(&opt);

    // Background: rounded rectangle with customizable color
    QRect trackRect = rect().adjusted(5, 0, -5, 0);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor background = underMouse() ? backgroundBaseColor.lighter(110) : backgroundBaseColor;
    int playheadWidth = underMouse() ? 8 : 4;
    painter.setBrush(background);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(trackRect, 2, 2);

    // Filled area to the left of the playhead with customizable color
    QRect filledRect = QRect(trackRect.left(), trackRect.top(),
                            (value() - minimum()) * trackRect.width() / (maximum() - minimum()),
                            trackRect.height());
    painter.setBrush(filledColor);
    painter.drawRoundedRect(filledRect, 2, 2);

    // Playhead: white line with a hint of blue
    int playheadX = trackRect.left() +
                    (value() - minimum()) * trackRect.width() / (maximum() - minimum());
    QRect playheadRect(playheadX - 2 - playheadWidth/2, trackRect.top(), playheadWidth, trackRect.height());
    painter.setPen(Qt::gray);
    painter.setBrush(QColor(250, 250, 255));
    painter.drawRect(playheadRect);
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
    update(); // Trigger repaint to apply hover highlight
    QSlider::enterEvent(event);
}
void VideoSlider::leaveEvent(QEvent *event) {
    update(); // Trigger repaint to remove hover highlight
    QSlider::leaveEvent(event);
}

void VideoSlider::sliderChange(SliderChange change) {
    if (change == SliderValueChange && !vidUpdate && !sliderUpdateTimer->isActive()) {
        if (player->playbackState() == QMediaPlayer::PlayingState) {
            player->pause();
        }
        manualSliderUpdate = true;
        setVidPosition();
        sliderUpdateTimer->start();
    }
    QSlider::sliderChange(change);
}

void VideoSlider::setVidPosition() const {
    player->setPosition((static_cast<qint64>(this->value()) * player->duration())/ sliderSize);
}
void VideoSlider::updateTimeStamp(int position, int duration) const {
    if (button != nullptr) {
        button->setText(convertToTimestamp(position)+" / "+convertToTimestamp(duration));
    }
}

void VideoSlider::assignButton(QPushButton *button) {
    this->button = button;
}

QString VideoSlider::convertToTimestamp(int seconds) {
    return QVariant(seconds/60000).toString()+":" + (seconds/1000%60<10 ? "0" : "") + QVariant(seconds/1000%60).toString();
}
