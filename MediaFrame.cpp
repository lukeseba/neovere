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
            // get resolution from metadata
            QSize resolution = mediaPlayer.metaData().value(QMediaMetaData::Resolution).toSize();
            if (resolution.isValid()) {
                aspectWidth = resolution.width();
                aspectHeight = resolution.height();

                // dont ask
                resize(width()+1, height());
                resize(width()-1, height());
            } else {
                qDebug() << "Resolution is invalid";
            }
        }
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

void MediaFrame::reloadVideo() {
    const QUrl source = mediaPlayer.source();
    setVideo("");
    mediaPlayer.setSource(source);
    mediaPlayer.setPosition(0);
    pauseVideo();
}

void MediaFrame::setVideo(const QString &filePath) {
    if (filePath.isEmpty()) {
        mediaPlayer.setSource(QUrl());
    } else {
        mediaPlayer.setSource(QUrl::fromLocalFile(filePath));
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