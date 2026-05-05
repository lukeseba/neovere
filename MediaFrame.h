//
// Created by lukebalfanz on 11/6/24.
//

#ifndef MEDIAFRAME_H
#define MEDIAFRAME_H

#include "MaintainFrame.h"
#include "PreviewWidget.h"
#include "PlaybackController.h"
#include "FrameBufferReader.h"
#include <QMediaPlayer>
#include <QVideoWidget>
#include <QVBoxLayout>
#include <QStackedWidget>

class MediaFrame: public MaintainFrame {
    Q_OBJECT

public:
    enum class Mode { VideoFile, FrameBuffer };

    explicit MediaFrame(QWidget *parent = nullptr);
    ~MediaFrame() override;

    // === Existing video-file API (unchanged behavior) ===
    void setVideo(const QString &filePath);
    void reloadVideo(qint64 seekToMs = 0, bool resumePlaying = false);
    void playVideo();
    QMediaPlayer* getPlayer();
    void pauseVideo();

    // === New frame-buffer API ===
    void setFrameBuffer(FrameBufferReader* reader);   // doesn't take ownership
    void setFrameBufferAudio(const QString& path);     // audio file used while in FrameBuffer mode
    void reloadFrameBufferAudio();                     // re-read the audio file from disk
    void switchToVideoFile();                          // ensure we're in video-file mode
    void switchToFrameBuffer();                        // ensure we're in frame-buffer mode
    Mode currentMode() const { return mode; }
    PlaybackController* fbController() { return controller; }

signals:
    void pauseStateChanged(bool paused);

private:
    Mode mode = Mode::VideoFile;
    QStackedWidget* stack = nullptr;

    // Video-file path (existing)
    QMediaPlayer mediaPlayer;
    QVideoWidget videoWidget;
    QVBoxLayout *layout;
    bool isPaused;
    qint64 pendingSeekMs = -1;     // -1 = no pending seek
    bool pendingPlay = false;       // whether to start playing after the next LoadedMedia
    bool fbMirroring = false;       // guard for controller↔mediaPlayer position sync in FrameBuffer mode

    // Frame-buffer path (new)
    PreviewWidget* fbWidget = nullptr;
    PlaybackController* controller = nullptr;
};

#endif //MEDIAFRAME_H
