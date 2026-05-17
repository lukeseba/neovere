#include "VideoSlider.h"
#include <iostream>
#include <QPainter>
#include <QStyleOptionSlider>
#include <QMouseEvent>

VideoSlider::VideoSlider(MediaFrame *panel, int size, QWidget *parent) : QSlider(Qt::Horizontal, parent) {
    this->mediaPanel = panel;
    this->sliderSize = size;
    this->setRange(0, sliderSize);
    this->videoUpdateTimer = new QTimer(this);
    this->sliderUpdateTimer = new QTimer(this);
    this->vidUpdate = true;
    this->manualSliderUpdate = false;

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
}

void VideoSlider::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (mediaPanel->currentMode() == MediaFrame::Mode::VideoFile) {
            if (mediaPanel->getPlayer()->playbackState() == QMediaPlayer::PlayingState) {
                mediaPanel->getPlayer()->pause();
            }
        } else {
            if (mediaPanel->fbController()->isPlaying()) {
                mediaPanel->fbController()->pause();
            }
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
        if (mediaPanel->currentMode() == MediaFrame::Mode::VideoFile) {
            mediaPanel->getPlayer()->play();
        } else {
            mediaPanel->fbController()->play();
        }
        sliderUpdateTimer->stop();
    }
    QSlider::mouseReleaseEvent(event);
}

void VideoSlider::enterEvent(QEnterEvent *event) {
    update();
    QSlider::enterEvent(event);
}

void VideoSlider::leaveEvent(QEvent *event) {
    update();
    QSlider::leaveEvent(event);
}

void VideoSlider::sliderChange(SliderChange change) {
    if (change == SliderValueChange && !vidUpdate && !sliderUpdateTimer->isActive()) {
        if (mediaPanel->currentMode() == MediaFrame::Mode::VideoFile) {
            if (mediaPanel->getPlayer()->playbackState() == QMediaPlayer::PlayingState) {
                mediaPanel->getPlayer()->pause();
            }
        } else {
            if (mediaPanel->fbController()->isPlaying()) {
                mediaPanel->fbController()->pause();
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
}

QString VideoSlider::convertToTimestamp(int position) const {
    int totalSeconds = position / 1000;
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    return QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

void VideoSlider::setColor(const QColor &filled, const QColor &background) {
    filledColor = filled;
    backgroundBaseColor = background;
    update();
}

void VideoSlider::paintEvent(QPaintEvent *event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    
    QRect r = rect();
    int trackHeight = 4;
    QRect trackRect(r.left(), r.center().y() - trackHeight/2, r.width(), trackHeight);
    
    p.setPen(Qt::NoPen);
    p.setBrush(backgroundBaseColor);
    p.drawRoundedRect(trackRect, 2, 2);
    
    int fillWidth = 0;
    if (maximum() > 0) {
        fillWidth = (value() * r.width()) / maximum();
    }
    QRect fillRect(trackRect.left(), trackRect.top(), fillWidth, trackHeight);
    p.setBrush(filledColor);
    p.drawRoundedRect(fillRect, 2, 2);
}