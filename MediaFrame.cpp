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

    // set up layout to hold video widget
    layout = new QVBoxLayout(this);
    layout->addWidget(&videoWidget);

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
    isPaused = false;
    mediaPlayer.play();
    emit pauseStateChanged(false);
}

void MediaFrame::pauseVideo() {
    isPaused = true;
    mediaPlayer.pause();
    emit pauseStateChanged(true);
}