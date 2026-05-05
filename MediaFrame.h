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
    void reloadVideo(qint64 seekToMs = 0, bool resumePlaying = false);
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
    qint64 pendingSeekMs = -1;     // -1 = no pending seek
    bool pendingPlay = false;       // whether to start playing after the next LoadedMedia
};

#endif //MEDIAFRAME_H
