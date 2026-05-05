#!/usr/bin/env python3
"""
Writes a test FrameBufferReader-compatible file to /tmp/neovere_frames.bin
with a small handful of test frames so the C++ side has something to display.

Run it before launching the app, then look for the test PreviewWidget popup.
"""

import os
import struct
import numpy as np

PATH = "/tmp/neovere_frames.bin"
MAGIC = 0x4E454F56  # 'NEOV'
HEADER_SIZE = 64

WIDTH = 480
HEIGHT = 270
FPS = 24.0
FRAME_COUNT = 6


def write_header(f, generation: int):
    # Layout matches the C++ Header struct in FrameBufferReader.h
    f.seek(0)
    f.write(struct.pack(
        "<IIIIIfII",
        MAGIC,
        generation,
        FRAME_COUNT,
        WIDTH,
        HEIGHT,
        FPS,
        3,  # channels
        0,  # dtype = uint8
    ))
    # Pad to 64 bytes
    f.write(b"\x00" * (HEADER_SIZE - 32))


def main():
    total_size = HEADER_SIZE + FRAME_COUNT * HEIGHT * WIDTH * 3
    with open(PATH, "wb") as f:
        f.truncate(total_size)
    # Now mmap-ish it via numpy to write frames
    frames = np.memmap(
        PATH,
        dtype=np.uint8,
        mode="r+",
        offset=HEADER_SIZE,
        shape=(FRAME_COUNT, HEIGHT, WIDTH, 3),
    )

    # Frame 0: solid red (RGB)
    frames[0, :, :] = (255, 0, 0)
    # Frame 1: solid green
    frames[1, :, :] = (0, 255, 0)
    # Frame 2: solid blue
    frames[2, :, :] = (0, 0, 255)
    # Frame 3: horizontal gradient red→blue
    grad = np.linspace(0, 255, WIDTH, dtype=np.uint8)
    frames[3, :, :, 0] = 255 - grad
    frames[3, :, :, 2] = grad
    # Frame 4: vertical gradient white→black
    vgrad = np.linspace(255, 0, HEIGHT, dtype=np.uint8)
    for c in range(3):
        frames[4, :, :, c] = vgrad[:, None]
    # Frame 5: checkerboard
    cell = 30
    cb = np.zeros((HEIGHT, WIDTH), dtype=np.uint8)
    for y in range(0, HEIGHT, cell):
        for x in range(0, WIDTH, cell):
            if ((y // cell) + (x // cell)) % 2 == 0:
                cb[y:y+cell, x:x+cell] = 255
    for c in range(3):
        frames[5, :, :, c] = cb

    frames.flush()
    del frames

    # Write header last so a reader that catches us mid-write can detect partial state
    with open(PATH, "r+b") as f:
        write_header(f, generation=1)

    print(f"Wrote {FRAME_COUNT} test frames ({WIDTH}x{HEIGHT}) to {PATH}")
    print(f"File size: {os.path.getsize(PATH)} bytes")


if __name__ == "__main__":
    main()
