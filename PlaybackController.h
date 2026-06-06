#ifndef PLAYBACKCONTROLLER_H
#define PLAYBACKCONTROLLER_H

#include <QObject>
#include <QTimer>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QString>
#include "PreviewWidget.h"
#include "FrameBufferReader.h"

// Drives a PreviewWidget in time-sync with a separate audio player.
// In frame-buffer mode the controller is the single source of truth for
// playback position; the audio player follows it but is also used to keep
// drift bounded (we re-anchor against audio position periodically).
class PlaybackController : public QObject {
    Q_OBJECT

public:
    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    void setPreviewWidget(PreviewWidget* w);
    void setFrameBuffer(FrameBufferReader* reader);  // controller does NOT own
    void setAudioFile(const QString& path);          // empty = no audio
    void reloadAudioFile();                          // force-reload current audio (e.g. file content changed)
    void setLoopAtEnd(bool loop) { loopAtEnd = loop; }

    // Re-read header / duration after the writer updated frames.
    // Preserves position (clamped to new duration).
    void refresh();

    // === 'standard' template (on-demand single-frame) playback ===
    // In standard mode the shared buffer holds only the latest frame (slot 0);
    // the timeline length comes from durationFrames/fps (set_duration in the
    // script), NOT the buffer's frame_count. While playing, the controller emits
    // frameNeeded() for whatever frame matches the master clock (audio if present,
    // else wall-clock) and the host renders it on demand, dropping frames it can't
    // keep up with. freshEntry resets the position to 0 (first entry); otherwise
    // the position is preserved (a render-only edit keeps you where you were).
    void setStandardMode(int durationFrames, float fps, bool freshEntry);
    void clearStandardMode();                        // revert to legacy buffer-driven playback
    bool isStandardMode() const { return onDemand; }
    void onFrameRendered(int frameIndex);            // host: a requested frame is now in slot 0
    int currentFrame() const { return frameForPosition(); }

    bool isPlaying() const { return playing; }
    qint64 positionMs() const { return positionMsValue; }
    qint64 durationMs() const;
    int frameCount() const;
    float fps() const;

public slots:
    void play();
    void pause();
    void togglePlayPause();
    void setPositionMs(qint64 ms);

signals:
    void positionChanged(qint64 ms);
    void playingChanged(bool playing);
    void durationChanged(qint64 ms);
    // Standard mode: the host should render this frame on demand and call
    // onFrameRendered() when it lands in slot 0. Coalescing/back-pressure is the
    // host's job (it owns the single Python worker), so this may fire every tick.
    void frameNeeded(int frameIndex);

private slots:
    void tick();

private:
    PreviewWidget* widget = nullptr;
    FrameBufferReader* buffer = nullptr;
    QMediaPlayer* audioPlayer = nullptr;
    QAudioOutput* audioOutput = nullptr;
    QTimer tickTimer;

    bool playing = false;
    qint64 positionMsValue = 0;
    bool loopAtEnd = true;
    int driftCheckCounter = 0;

    // 'standard' (on-demand) mode state
    bool onDemand = false;          // true between setStandardMode()/clearStandardMode()
    int stdDurationFrames = 0;      // timeline length in frames (from set_duration)
    float stdFps = 30.0f;           // timeline fps (from set_fps)

    void applyFrameForPosition();
    void resyncFromAudio();
    int frameForPosition() const;   // frame index matching the current position (both modes)
};

#endif  // PLAYBACKCONTROLLER_H
