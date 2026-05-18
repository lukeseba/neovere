#include "MediaFrame.h"

#include <iostream>
#include <ostream>
#include <QVBoxLayout>
#include <QAudioOutput>
#include <QCoreApplication>
#include <QMediaPlayer>
#include <QMediaMetaData>
#include <QDebug>
#include <QFileInfo>
#include <QDateTime>
#include <QTimer>
#include <QPixmap>

MediaFrame::MediaFrame(QWidget *parent)
    : MaintainFrame(16, 9, parent),
mediaPlayer(this),
videoWidget(this) {

    this->isPaused = false;

    // Stack lets us flip between the existing video-file path (QVideoWidget)
    // and the new frame-buffer path (PreviewWidget) without changing layouts.
    layout = new QVBoxLayout(this);
    stack = new QStackedWidget(this);
    stack->addWidget(&videoWidget);          // index 0: VideoFile mode
    fbWidget = new PreviewWidget(this);
    stack->addWidget(fbWidget);              // index 1: FrameBuffer mode

    // Add image widget
    imageWidget = new QLabel(this);
    imageWidget->setAlignment(Qt::AlignCenter);
    imageWidget->setScaledContents(true);
    imageWidget->setStyleSheet("background-color: black;");
    stack->addWidget(imageWidget);           // index 2: ImageFile mode

    stack->setCurrentIndex(0);
    layout->addWidget(stack);

    controller = new PlaybackController(this);
    controller->setPreviewWidget(fbWidget);

    QObject::connect(controller, &PlaybackController::positionChanged, this, [this](qint64 ms) {
        if (mode != Mode::FrameBuffer) return;
        if (fbMirroring) return;
        if (mediaPlayer.position() == ms) return;
        fbMirroring = true;
        mediaPlayer.setPosition(ms);
        fbMirroring = false;
    });

    QObject::connect(&mediaPlayer, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        if (mode != Mode::FrameBuffer) return;
        if (fbMirroring) return;
        if (controller && controller->positionMs() != ms) {
            fbMirroring = true;
            controller->setPositionMs(ms);
            fbMirroring = false;
        }
    });

    QAudioOutput *audioOut = new QAudioOutput();
    audioOut->setVolume(50);
    mediaPlayer.setVideoOutput(&videoWidget);
    mediaPlayer.setAudioOutput(audioOut);

    QObject::connect(&mediaPlayer, &QMediaPlayer::mediaStatusChanged, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia) {
            QSize resolution = mediaPlayer.metaData().value(QMediaMetaData::Resolution).toSize();
            if (resolution.isValid()) {
                aspectWidth = resolution.width();
                aspectHeight = resolution.height();
                resize(width()+1, height());
                resize(width()-1, height());
            } else {
                qDebug() << "Resolution is invalid";
            }
        } else if (status == QMediaPlayer::InvalidMedia) {
            qDebug() << "Invalid media:" << mediaPlayer.source() << "-" << mediaPlayer.errorString();
        }
    });


    QObject::connect(&mediaPlayer, &QMediaPlayer::errorOccurred, [&](QMediaPlayer::Error error, const QString &errorString) {
        qDebug() << "Media player error" << error << ":" << errorString << "| source:" << mediaPlayer.source();
    });

    QObject::connect(&mediaPlayer, &QMediaPlayer::playbackStateChanged, [&](QMediaPlayer::PlaybackState state) {
        if (state == QMediaPlayer::PlayingState && isPaused) {
            mediaPlayer.pause();
        }
    });

    this->setLayout(layout);
}

MediaFrame::~MediaFrame() {

}

QMediaPlayer* MediaFrame::getPlayer() {
    return &mediaPlayer;
}

void MediaFrame::releaseFile() {
    QUrl src = mediaPlayer.source();
    if (!src.isEmpty()) stashedSource = src;
    isPaused = true;
    mediaPlayer.pause();
    mediaPlayer.setSource(QUrl());
}

void MediaFrame::reloadVideo(qint64 seekToMs, bool resumePlaying) {
    if (mode == Mode::ImageFile) return; // Static images don't need reloading from renders

    QUrl source = mediaPlayer.source();
    if (source.isEmpty() && !stashedSource.isEmpty()) source = stashedSource;
    setVideo("");
    if (!source.isEmpty()) mediaPlayer.setSource(source);
    stashedSource = QUrl();

    QTimer::singleShot(150, this, [this, seekToMs, resumePlaying]() {
        mediaPlayer.setPosition(seekToMs);
        if (resumePlaying) {
            playVideo();
        } else {
            isPaused = true;
            mediaPlayer.play();
        }
    });
}

void MediaFrame::setVideo(const QString &filePath) {
    if (filePath.isEmpty()) {
        mediaPlayer.setSource(QUrl());
        imageWidget->clear();
    } else {
        QString lowerPath = filePath.toLower();

        // Intercept images before giving them to the video player
        if (lowerPath.endsWith(".png") || lowerPath.endsWith(".jpg") || lowerPath.endsWith(".jpeg")) {
            switchToImageFile();
            QPixmap pixmap(filePath);
            if (!pixmap.isNull()) {
                imageWidget->setPixmap(pixmap);
                aspectWidth = pixmap.width();
                aspectHeight = pixmap.height();

                // Kick layout reflow to snap aspect ratio
                resize(width()+1, height());
                resize(width()-1, height());
            }
        } else {
            switchToVideoFile();
            mediaPlayer.setSource(QUrl::fromLocalFile(QFileInfo(filePath).absoluteFilePath()));
            mediaPlayer.setPosition(0);
            pauseVideo();
        }
    }
}
void MediaFrame::playVideo() {
    if (mode == Mode::ImageFile) return;
    if (mode == Mode::FrameBuffer && controller) {
        controller->play();
        emit pauseStateChanged(false);
        return;
    }
    isPaused = false;
    mediaPlayer.play();
    emit pauseStateChanged(false);
}

void MediaFrame::pauseVideo() {
    if (mode == Mode::ImageFile) return;
    if (mode == Mode::FrameBuffer && controller) {
        controller->pause();
        emit pauseStateChanged(true);
        return;
    }
    isPaused = true;
    mediaPlayer.pause();
    emit pauseStateChanged(true);
}

void MediaFrame::setFrameBuffer(FrameBufferReader* reader) {
    if (controller) controller->setFrameBuffer(reader);
}

void MediaFrame::setFrameBufferAudio(const QString& path) {
    if (controller) controller->setAudioFile(path);
}

void MediaFrame::reloadFrameBufferAudio() {
    if (controller) controller->reloadAudioFile();
}

void MediaFrame::switchToVideoFile() {
    if (mode == Mode::VideoFile) return;
    mode = Mode::VideoFile;
    if (controller) controller->pause();
    stack->setCurrentIndex(0);
    if (mediaPlayer.source().isEmpty() && !stashedSource.isEmpty()) {
        mediaPlayer.setSource(stashedSource);
    }
}

void MediaFrame::switchToFrameBuffer() {
    if (mode == Mode::FrameBuffer) return;
    mode = Mode::FrameBuffer;
    isPaused = true;
    mediaPlayer.pause();
#ifdef Q_OS_WIN
    QUrl src = mediaPlayer.source();
    if (!src.isEmpty()) stashedSource = src;
    mediaPlayer.setSource(QUrl());
#endif
    stack->setCurrentIndex(1);
}

void MediaFrame::switchToImageFile() {
    if (mode == Mode::ImageFile) return;
    mode = Mode::ImageFile;
    isPaused = true;
    mediaPlayer.pause();
    if (controller) controller->pause();
    stack->setCurrentIndex(2);
}