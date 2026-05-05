#include "PlaybackController.h"
#include <QUrl>
#include <QFileInfo>
#include <cmath>

PlaybackController::PlaybackController(QObject* parent) : QObject(parent) {
    audioPlayer = new QMediaPlayer(this);
    audioOutput = new QAudioOutput(this);
    audioPlayer->setAudioOutput(audioOutput);

    // Tick at ~60Hz so frame display and scrubber updates feel smooth even
    // when video fps is lower (e.g., 12 fps preview). The actual frame index
    // is computed from the position, not driven by a per-frame timer.
    tickTimer.setInterval(16);
    connect(&tickTimer, &QTimer::timeout, this, &PlaybackController::tick);
}

PlaybackController::~PlaybackController() = default;

void PlaybackController::setPreviewWidget(PreviewWidget* w) {
    widget = w;
    applyFrameForPosition();
}

void PlaybackController::setFrameBuffer(FrameBufferReader* reader) {
    bool isNewBuffer = (reader != buffer);
    buffer = reader;
    if (widget) widget->setBuffer(reader);
    emit durationChanged(durationMs());
    if (isNewBuffer) {
        setPositionMs(0);
    } else {
        // Same reader, just refreshed contents — clamp position and repaint.
        refresh();
    }
}

void PlaybackController::setAudioFile(const QString& path) {
    if (path.isEmpty() || !QFile::exists(path)) {
        audioPlayer->setSource(QUrl());
        return;
    }
    QUrl newUrl = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath());
    if (audioPlayer->source() == newUrl) return;  // already loaded; don't reload
    audioPlayer->setSource(newUrl);
}

void PlaybackController::reloadAudioFile() {
    if (!audioPlayer->source().isValid()) return;
    QUrl src = audioPlayer->source();
    qint64 savedPos = positionMsValue;
    bool wasPlaying = (audioPlayer->playbackState() == QMediaPlayer::PlayingState);

    // Force a fresh load: clear, then re-set. Qt won't reload an identical URL
    // even if the file content changed on disk.
    audioPlayer->setSource(QUrl());
    audioPlayer->setSource(src);

    // The new audio loads asynchronously; defer position+resume until it's ready.
    QTimer::singleShot(120, this, [this, savedPos, wasPlaying]() {
        audioPlayer->setPosition(savedPos);
        if (wasPlaying) {
            audioPlayer->play();
        }
    });
}

void PlaybackController::refresh() {
    if (buffer) buffer->refreshHeader();
    qint64 dur = durationMs();
    if (positionMsValue > dur) positionMsValue = dur;
    emit durationChanged(dur);
    applyFrameForPosition();
    emit positionChanged(positionMsValue);
}

qint64 PlaybackController::durationMs() const {
    if (!buffer || !buffer->isOpen()) return 0;
    const auto& h = buffer->header();
    if (h.fps <= 0 || h.frame_count == 0) return 0;
    return (qint64)((double)h.frame_count / (double)h.fps * 1000.0);
}

int PlaybackController::frameCount() const {
    if (!buffer || !buffer->isOpen()) return 0;
    return (int)buffer->header().frame_count;
}

float PlaybackController::fps() const {
    if (!buffer || !buffer->isOpen()) return 0.0f;
    return buffer->header().fps;
}

void PlaybackController::play() {
    if (playing) return;
    if (durationMs() <= 0) return;
    if (positionMsValue >= durationMs()) {
        // At end: rewind to start so the user gets something to play.
        setPositionMs(0);
    }
    playing = true;
    if (audioPlayer->source().isValid()) {
        audioPlayer->setPosition(positionMsValue);
        audioPlayer->play();
    }
    tickTimer.start();
    emit playingChanged(true);
}

void PlaybackController::pause() {
    if (!playing) return;
    playing = false;
    audioPlayer->pause();
    tickTimer.stop();
    emit playingChanged(false);
}

void PlaybackController::togglePlayPause() {
    if (playing) pause(); else play();
}

void PlaybackController::setPositionMs(qint64 ms) {
    qint64 dur = durationMs();
    if (ms < 0) ms = 0;
    if (dur > 0 && ms > dur) ms = dur;
    positionMsValue = ms;
    if (audioPlayer->source().isValid()) audioPlayer->setPosition(ms);
    applyFrameForPosition();
    emit positionChanged(ms);
}

void PlaybackController::applyFrameForPosition() {
    if (!widget || !buffer || !buffer->isOpen()) return;
    const auto& h = buffer->header();
    if (h.fps <= 0 || h.frame_count == 0) {
        widget->setFrameIndex(-1);
        return;
    }
    int idx = (int)std::floor((double)positionMsValue * (double)h.fps / 1000.0);
    if (idx < 0) idx = 0;
    if (idx >= (int)h.frame_count) idx = (int)h.frame_count - 1;
    widget->setFrameIndex(idx);
}

void PlaybackController::tick() {
    if (!playing) return;

    // Advance position by tick interval. Anchor to audio's actual position
    // every ~10 ticks (~160ms) to bound drift.
    qint64 step = tickTimer.interval();
    positionMsValue += step;

    qint64 dur = durationMs();
    if (dur > 0 && positionMsValue >= dur) {
        if (loopAtEnd) {
            positionMsValue = 0;
            if (audioPlayer->source().isValid()) {
                // Audio player stops at end of media. Seek to 0 AND restart playback.
                audioPlayer->setPosition(0);
                audioPlayer->play();
            }
        } else {
            positionMsValue = dur;
            pause();
            applyFrameForPosition();
            emit positionChanged(positionMsValue);
            return;
        }
    }

    if (++driftCheckCounter >= 10) {
        driftCheckCounter = 0;
        resyncFromAudio();
    }

    applyFrameForPosition();
    emit positionChanged(positionMsValue);
}

void PlaybackController::resyncFromAudio() {
    if (!audioPlayer->source().isValid()) return;
    if (audioPlayer->playbackState() != QMediaPlayer::PlayingState) return;
    qint64 audioPos = audioPlayer->position();
    // If the audio player just (re)loaded its source, its position is briefly 0 even
    // while it's "Playing". Don't yank our position back to 0 in that case.
    if (audioPos == 0 && positionMsValue > 200) return;
    qint64 diff = audioPos - positionMsValue;
    // Wider tolerance to absorb load jitter; only snap on real desync.
    if (std::abs(diff) > 500) {
        positionMsValue = audioPos;
    } else {
        positionMsValue += diff / 4;
    }
}
