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

    void applyFrameForPosition();
    void resyncFromAudio();
};

#endif  // PLAYBACKCONTROLLER_H
