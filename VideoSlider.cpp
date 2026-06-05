// VideoSlider.cpp
//
// Created by lukebalfanz on 11/7/24.
//

#include "VideoSlider.h"

#include <iostream>
#include <ostream>
#include <QPainter>
#include <QStyleOptionSlider>
#include <QMouseEvent>
#include <QGuiApplication>
#include <QClipboard>

VideoSlider::VideoSlider(MediaFrame *panel, int size, QWidget *parent) : QSlider(Qt::Horizontal, parent) {
    this->mediaPanel = panel;
    this->sliderSize = size;
    this->setRange(0, sliderSize);
    this->videoUpdateTimer = new QTimer(this);
    this->sliderUpdateTimer = new QTimer(this);
    this->vidUpdate = true;
    this->manualSliderUpdate = false;
    this->wasPlayingBeforeInteraction = false;

    videoUpdateTimer->setInterval(100);
    sliderUpdateTimer->setInterval(250);
    videoUpdateTimer->setSingleShot(true);
    sliderUpdateTimer->setSingleShot(true);

    auto updateSlider = [=](qint64 position, qint64 duration) {
        if (!videoUpdateTimer->isActive() && !manualSliderUpdate) {
            if (duration > 0) {
                vidUpdate = true;
                this->setValue(int((position * sliderSize) / duration));
                vidUpdate = false;
                videoUpdateTimer->start();
                updateTimeStamp(int(position), int(duration));
            } else {
                updateTimeStamp(0, 0);
            }
        }
    };

    QObject::connect(mediaPanel->getPlayer(), &QMediaPlayer::positionChanged, [=](qint64 pos) {
        if (mediaPanel->currentMode() == MediaFrame::Mode::VideoFile) {
            updateSlider(pos, mediaPanel->getPlayer()->duration());
        }
    });

    QObject::connect(mediaPanel->fbController(), &PlaybackController::positionChanged, [=](qint64 pos) {
        if (mediaPanel->currentMode() == MediaFrame::Mode::FrameBuffer) {
            updateSlider(pos, mediaPanel->fbController()->durationMs());
        }
    });

    setMouseTracking(true); // Enable hover detection
}

void VideoSlider::setColor(const QColor &filledColor, const QColor &backgroundBaseColor) {
    this->filledColor = filledColor;
    this->backgroundBaseColor = backgroundBaseColor;
    update(); // Trigger repaint with new colors
}

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
    int fillWidth = 0;
    if (maximum() > minimum()) {
        fillWidth = (value() - minimum()) * trackRect.width() / (maximum() - minimum());
    }
    QRect filledRect = QRect(trackRect.left(), trackRect.top(), fillWidth, trackRect.height());
    painter.setBrush(filledColor);
    painter.drawRoundedRect(filledRect, 2, 2);

    // Playhead: white line with a hint of blue
    int playheadX = trackRect.left() + fillWidth;
    QRect playheadRect(playheadX - 2 - playheadWidth/2, trackRect.top(), playheadWidth, trackRect.height());
    painter.setPen(Qt::gray);
    painter.setBrush(QColor(250, 250, 255));
    painter.drawRect(playheadRect);
}

void VideoSlider::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        // Record current playback state and pause before moving the slider
        if (mediaPanel->currentMode() == MediaFrame::Mode::VideoFile) {
            wasPlayingBeforeInteraction = (mediaPanel->getPlayer()->playbackState() == QMediaPlayer::PlayingState);
            if (wasPlayingBeforeInteraction) mediaPanel->getPlayer()->pause();
        } else {
            wasPlayingBeforeInteraction = mediaPanel->fbController()->isPlaying();
            if (wasPlayingBeforeInteraction) mediaPanel->fbController()->pause();
        }

        manualSliderUpdate = true;

        double pos = event->pos().x() / (double)width();
        setValue(pos * maximum());
        setVidPosition();
    }
    QSlider::mousePressEvent(event);
}

void VideoSlider::mouseReleaseEvent(QMouseEvent *event) {
    if (manualSliderUpdate) {
        manualSliderUpdate = false;
        setVidPosition();

        // Only resume playback if it was playing BEFORE you clicked
        if (wasPlayingBeforeInteraction) {
            if (mediaPanel->currentMode() == MediaFrame::Mode::VideoFile) {
                mediaPanel->getPlayer()->play();
            } else {
                mediaPanel->fbController()->play();
            }
        }
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

        // If dragged via keyboard or otherwise (not caught by mousePress)
        if (!manualSliderUpdate) {
            if (mediaPanel->currentMode() == MediaFrame::Mode::VideoFile) {
                wasPlayingBeforeInteraction = (mediaPanel->getPlayer()->playbackState() == QMediaPlayer::PlayingState);
                if (wasPlayingBeforeInteraction) mediaPanel->getPlayer()->pause();
            } else {
                wasPlayingBeforeInteraction = mediaPanel->fbController()->isPlaying();
                if (wasPlayingBeforeInteraction) mediaPanel->fbController()->pause();
            }
        }

        manualSliderUpdate = true;
        setVidPosition();
        sliderUpdateTimer->start();
    }
    QSlider::sliderChange(change);
}

void VideoSlider::setVidPosition() const {
    if (mediaPanel->currentMode() == MediaFrame::Mode::VideoFile) {
        if (mediaPanel->getPlayer()->duration() > 0) {
            mediaPanel->getPlayer()->setPosition((static_cast<qint64>(this->value()) * mediaPanel->getPlayer()->duration())/ sliderSize);
        }
    } else {
        if (mediaPanel->fbController()->durationMs() > 0) {
            mediaPanel->fbController()->setPositionMs((static_cast<qint64>(this->value()) * mediaPanel->fbController()->durationMs())/ sliderSize);
        }
    }
}

void VideoSlider::updateTimeStamp(int position, int duration) const {
    if (button != nullptr) {
        button->setText(convertToTimestamp(position) + " / " + convertToTimestamp(duration));
    }
}

void VideoSlider::updateTimeStamp() {
    if (mediaPanel->currentMode() == MediaFrame::Mode::VideoFile) {
        updateTimeStamp(mediaPanel->getPlayer()->position(), mediaPanel->getPlayer()->duration());
    } else {
        updateTimeStamp(mediaPanel->fbController()->positionMs(), mediaPanel->fbController()->durationMs());
    }
}

void VideoSlider::assignButton(QPushButton *btn) {
    this->button = btn;

    // Connect the button click to our clipboard logic
    QObject::connect(button, &QPushButton::clicked, [this]() {
        qint64 currentPosMs = 0;

        // 1. Get the current position in milliseconds based on the mode
        if (mediaPanel->currentMode() == MediaFrame::Mode::VideoFile) {
            currentPosMs = mediaPanel->getPlayer()->position();
        } else {
            currentPosMs = mediaPanel->fbController()->positionMs();
        }

        // 2. Define the framerate.
        // (If your engine tracks this dynamically, replace 24.0 with mediaPanel->fps() or similar)
        double fps = 24.0;

        // 3. Convert milliseconds to the exact frame index
        qint64 currentFrame = qRound((currentPosMs / 1000.0) * fps);

        // 4. Copy the frame number to the system clipboard
        QClipboard *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QString::number(currentFrame));

        // Optional visual feedback: briefly show it was copied (it will be overwritten on the next tick)
        button->setText("Copied: " + QString::number(currentFrame));
    });
}

QString VideoSlider::convertToTimestamp(int seconds) {
    return QVariant(seconds/60000).toString()+":" + (seconds/1000%60<10 ? "0" : "") + QVariant(seconds/1000%60).toString();
}