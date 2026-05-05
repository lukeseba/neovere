#ifndef FRAMEBUFFERREADER_H
#define FRAMEBUFFERREADER_H

#include <QFile>
#include <QString>
#include <cstdint>

// Reads frames from a memory-mapped shared buffer written by the Python worker.
// File layout:
//   [64-byte header]
//     uint32 magic       (0x4E454F56 = 'NEOV')
//     uint32 generation  (incremented by writer each new render)
//     uint32 frame_count
//     uint32 width
//     uint32 height
//     float  fps
//     uint32 channels    (3 for RGB888)
//     uint32 dtype       (0 = uint8)
//     [32 bytes reserved/padding]
//   [frame_count * width * height * channels bytes of pixel data, RGB]
class FrameBufferReader {
public:
    static constexpr uint32_t MAGIC = 0x4E454F56;  // 'NEOV'
    static constexpr int HEADER_SIZE = 64;

    struct Header {
        uint32_t magic;
        uint32_t generation;
        uint32_t frame_count;
        uint32_t width;
        uint32_t height;
        float fps;
        uint32_t channels;
        uint32_t dtype;
        uint8_t reserved[32];
    };
    static_assert(sizeof(Header) == HEADER_SIZE, "Header must be exactly 64 bytes");

    explicit FrameBufferReader(QString path);
    ~FrameBufferReader();

    bool open();
    void close();
    bool isOpen() const { return mappedData != nullptr; }

    // Re-read the cached header from the mmap region. Call when the writer's
    // generation counter may have changed.
    void refreshHeader();
    const Header& header() const { return cachedHeader; }

    // Returns nullptr if frameIndex is out of bounds.
    const uint8_t* frameData(uint32_t frameIndex) const;

    qint64 frameSizeBytes() const;

private:
    QFile file;
    uchar* mappedData = nullptr;
    qint64 mappedSize = 0;
    Header cachedHeader{};
    QString path;
};

#endif  // FRAMEBUFFERREADER_H
