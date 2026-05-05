//
// Created by lukebalfanz on 11/6/24.
//

#ifndef MEDIAFRAME_H
#define MEDIAFRAME_H

#include "MaintainFrame.h"
#include <QMediaPlayer>
#include <QVideoWidget>
#include <QVBoxLayout>

class MediaFrame: public MaintainFrame {
    Q_OBJECT

public:
    explicit MediaFrame(QWidget *parent = nullptr);
    ~MediaFrame() override;
    void setVideo(const QString &filePath);
    void reloadVideo(qint64 seekToMs = 0);
    void playVideo();
    QMediaPlayer* getPlayer();
    void pauseVideo();
    signals:
    void pauseStateChanged(bool paused);

private:
    QMediaPlayer mediaPlayer;
    QVideoWidget videoWidget;
    QVBoxLayout *layout;
    bool isPaused;
};

#endif //MEDIAFRAME_H
