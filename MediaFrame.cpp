//
// Created by lukebalfanz on 11/6/24.
//

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

MediaFrame::MediaFrame(QWidget *parent)
    : MaintainFrame(16, 9, parent),
mediaPlayer(new QMediaPlayer(this)),
videoWidget(new QVideoWidget(this)) {

    this->isPaused = false;

    // Stack lets us flip between the existing video-file path (QVideoWidget)
    // and the new frame-buffer path (PreviewWidget) without changing layouts.
    layout = new QVBoxLayout(this);
    stack = new QStackedWidget(this);
    stack->addWidget(&videoWidget);          // index 0: VideoFile mode
    fbWidget = new PreviewWidget(this);
    stack->addWidget(fbWidget);              // index 1: FrameBuffer mode
    stack->setCurrentIndex(0);
    layout->addWidget(stack);

    controller = new PlaybackController(this);
    controller->setPreviewWidget(fbWidget);

    // While in FrameBuffer mode, mirror the controller's position into the (paused)
    // QMediaPlayer so the existing VideoSlider — which is wired to mediaPlayer — keeps
    // showing the right timeline position. fbMirroring guards against the reverse
    // direction triggering a feedback loop.
    QObject::connect(controller, &PlaybackController::positionChanged, this, [this](qint64 ms) {
        if (mode != Mode::FrameBuffer) return;
        if (fbMirroring) return;
        if (mediaPlayer.position() == ms) return;
        fbMirroring = true;
        mediaPlayer.setPosition(ms);
        fbMirroring = false;
    });

    // When the user scrubs the slider, it calls mediaPlayer.setPosition() which fires
    // positionChanged on the player. In FrameBuffer mode, route that back into the
    // controller so the displayed frame seeks too.
    QObject::connect(&mediaPlayer, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        if (mode != Mode::FrameBuffer) return;
        if (fbMirroring) return;
        if (controller && controller->positionMs() != ms) {
            fbMirroring = true;
            controller->setPositionMs(ms);
            fbMirroring = false;
        }
    });

    // configure media player to the video widget
    QAudioOutput *audioOut = new QAudioOutput();
    audioOut->setVolume(50);
    mediaPlayer.setVideoOutput(&videoWidget);
    mediaPlayer.setAudioOutput(audioOut);

    // get media resolution when media is changed
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

    // ensure video stays isPaused if unisPaused by anything else
    QObject::connect(&mediaPlayer, &QMediaPlayer::playbackStateChanged, [&](QMediaPlayer::PlaybackState state) {
        if (state == QMediaPlayer::PlayingState && isPaused) {
            mediaPlayer.pause();
        }
    });

    // set layout for the media frame
    this->setLayout(layout);
}

MediaFrame::~MediaFrame() {

}

QMediaPlayer* MediaFrame::getPlayer() {
    return &mediaPlayer;
}

void MediaFrame::reloadVideo(qint64 seekToMs, bool resumePlaying) {
    const QUrl source = mediaPlayer.source();
    setVideo("");
    mediaPlayer.setSource(source);
    // Defer the seek + play kick to give Qt time to actually load the new source.
    // Calling setPosition() immediately after setSource() is silently dropped because
    // the file isn't ready yet. 150ms is plenty for a small preview.mp4 on Apple Silicon.
    QTimer::singleShot(150, this, [this, seekToMs, resumePlaying]() {
        mediaPlayer.setPosition(seekToMs);
        if (resumePlaying) {
            playVideo();
        } else {
            // Briefly play to force the decoder to present the seeked frame.
            // The playbackStateChanged handler in the constructor auto-pauses
            // because isPaused stays true.
            isPaused = true;
            mediaPlayer.play();
        }
    });
}

void MediaFrame::setVideo(const QString &filePath) {
    if (filePath.isEmpty()) {
        mediaPlayer.setSource(QUrl());
    } else {
        mediaPlayer.setSource(QUrl::fromLocalFile(QFileInfo(filePath).absoluteFilePath()));
        mediaPlayer.setPosition(0);
        pauseVideo();
    }
}
void MediaFrame::playVideo() {
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
    if (mode == Mode::FrameBuffer && controller) {
        controller->pause();
        emit pauseStateChanged(true);
        return;
    }
    isPaused = true;
    mediaPlayer.pause();
    emit pauseStateChanged(true);
}

// === Frame-buffer mode ===

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
}

void MediaFrame::switchToFrameBuffer() {
    if (mode == Mode::FrameBuffer) return;
    mode = Mode::FrameBuffer;
    // Pause the existing QMediaPlayer so its audio doesn't keep playing under us
    isPaused = true;
    mediaPlayer.pause();
    stack->setCurrentIndex(1);
}