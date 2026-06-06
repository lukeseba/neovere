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
#include <QLabel>

class MediaFrame: public MaintainFrame {
    Q_OBJECT

public:
    enum class Mode { VideoFile, FrameBuffer, ImageFile };

    explicit MediaFrame(QWidget *parent = nullptr);
    ~MediaFrame() override;

    // === Existing video-file API (unchanged behavior) ===
    void setVideo(const QString &filePath);
    void reloadVideo(qint64 seekToMs = 0, bool resumePlaying = false);
    void playVideo();
    QMediaPlayer* getPlayer();
    void pauseVideo();
    // Drops the QMediaPlayer's hold on the current source so an external writer (the
    // Python renderer) can overwrite the file on Windows. The URL is stashed so
    // reloadVideo()/switchToVideoFile() can restore it once the render finishes.
    void releaseFile();

    // === New frame-buffer API ===
    void setFrameBuffer(FrameBufferReader* reader);   // doesn't take ownership
    void setFrameBufferAudio(const QString& path);     // audio file used while in FrameBuffer mode
    void reloadFrameBufferAudio();                     // re-read the audio file from disk
    void switchToVideoFile();                          // ensure we're in video-file mode
    void switchToFrameBuffer();                        // ensure we're in frame-buffer mode
    void switchToImageFile();                          // switch to static image viewer
    Mode currentMode() const { return mode; }
    PlaybackController* fbController() { return controller; }

    // === 'standard' template (on-demand single-frame) playback ===
    // Enter on-demand mode: the panel shows the frame buffer's slot 0 and the
    // controller drives the timeline from durationFrames/fps, requesting frames
    // on demand. freshEntry resets the playhead to 0 (first entry); otherwise it
    // is preserved (a render-only edit keeps the current position).
    void beginStandardMode(int durationFrames, float fps, bool freshEntry);
    void endStandardMode();                            // leave on-demand mode (back to legacy buffer playback)

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
    QUrl stashedSource;             // last source URL before releaseFile() cleared it
    qint64 pendingSeekMs = -1;     // -1 = no pending seek
    bool pendingPlay = false;       // whether to start playing after the next LoadedMedia
    bool fbMirroring = false;       // guard for controller↔mediaPlayer position sync in FrameBuffer mode

    // Frame-buffer path (new)
    PreviewWidget* fbWidget = nullptr;
    PlaybackController* controller = nullptr;
    
    // Static Image path (new)
    QLabel* imageWidget = nullptr;
};

#endif //MEDIAFRAME_H