#include "FrameBufferReader.h"
#include <cstring>

FrameBufferReader::FrameBufferReader(QString p) : path(std::move(p)) {}

FrameBufferReader::~FrameBufferReader() {
    close();
}

bool FrameBufferReader::open() {
    file.setFileName(path);
    if (!file.exists()) return false;
    if (!file.open(QIODevice::ReadOnly)) return false;
    mappedSize = file.size();
    if (mappedSize < (qint64)sizeof(Header)) {
        file.close();
        return false;
    }
    mappedData = file.map(0, mappedSize);
    if (!mappedData) {
        file.close();
        return false;
    }
    refreshHeader();
    if (cachedHeader.magic != MAGIC) {
        close();
        return false;
    }
    return true;
}

void FrameBufferReader::close() {
    if (mappedData) {
        file.unmap(mappedData);
        mappedData = nullptr;
    }
    if (file.isOpen()) file.close();
    mappedSize = 0;
    cachedHeader = Header{};
}

void FrameBufferReader::refreshHeader() {
    if (!mappedData) return;
    std::memcpy(&cachedHeader, mappedData, sizeof(Header));
}

const uint8_t* FrameBufferReader::frameData(uint32_t frameIndex) const {
    if (!mappedData) return nullptr;
    if (frameIndex >= cachedHeader.frame_count) return nullptr;
    qint64 offset = (qint64)sizeof(Header) + (qint64)frameIndex * frameSizeBytes();
    if (offset + frameSizeBytes() > mappedSize) return nullptr;
    return mappedData + offset;
}

qint64 FrameBufferReader::frameSizeBytes() const {
    return (qint64)cachedHeader.width * cachedHeader.height * cachedHeader.channels;
}
