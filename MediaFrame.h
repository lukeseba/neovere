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
    void playVideo();
    QMediaPlayer* getPlayer();
    void pauseVideo();

private:
    QMediaPlayer mediaPlayer;
    QVideoWidget videoWidget;
    QVBoxLayout *layout;
    bool paused;
};

#endif //MEDIAFRAME_H
