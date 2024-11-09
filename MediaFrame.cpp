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

    this->paused = false;

    // set up layout to hold video widget
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(&videoWidget);

    // configure media player to the video widget
    QAudioOutput *audioOut = new QAudioOutput();
    audioOut->setVolume(50);
    mediaPlayer.setVideoOutput(&videoWidget);
    mediaPlayer.setAudioOutput(audioOut);

    // get media resolution when media is changed
    QObject::connect(&mediaPlayer, &QMediaPlayer::mediaStatusChanged, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
            // get resolution from metadata
            QSize resolution = mediaPlayer.metaData().value(QMediaMetaData::Resolution).toSize();
            if (resolution.isValid()) {
                aspectWidth = resolution.width();
                aspectHeight = resolution.height();
                resize(aspectWidth, aspectHeight);
            } else {
                qDebug() << "Resolution is invalid";
            }
        }
    });

    // ensure video stays paused if unpaused by anything else
    QObject::connect(&mediaPlayer, &QMediaPlayer::playbackStateChanged, [&](QMediaPlayer::PlaybackState state) {
        if (state == QMediaPlayer::PlayingState && paused) {
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

void MediaFrame::setVideo(const QString &filePath) {
    mediaPlayer.setSource(QUrl::fromLocalFile(filePath));
}
void MediaFrame::playVideo() {
    paused = false;
    mediaPlayer.play();
}

void MediaFrame::pauseVideo() {
    paused = true;
    mediaPlayer.pause();
}