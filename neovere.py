import subprocess
import os
import sys
import copy
import shutil
from scipy.io import wavfile
from scipy.fft import fft, fftfreq
from PIL import Image, ImageDraw, ImageFont
from PyQt5.QtCore import QFile
import librosa
import soundfile as sf
from openai import OpenAI
from pathlib import Path
import string
import random
from typing import List, Tuple
import re
import pickle



try:
    import cv2
except ImportError:
    print("Error: OpenCV is not installed. Please install it using `pip install opencv-python`.")
    exit(1)

fourcc = cv2.VideoWriter_fourcc(*'mp4v')

_paths = ["render.mp4"]
arial = "C:/Users/luke/AppData/Local/Temp/arial-bold.ttf"

api_key = "" #[%$# #$%]
gpu_enabled = True
dx = 0.25
dt = 1.0

audio_counter = 0

if gpu_enabled:
    try:
        import cupy as np
    except Exception as e:
        print(f"[Warning] GPU acceleration unavailable ({e}). Falling back to CPU (numpy).")
        import numpy as np
        np.asnumpy = lambda x: np.asarray(x)  # Teaches NumPy to safely ignore CuPy commands
        gpu_enabled = False
else:
    import numpy as np
    np.asnumpy = lambda x: np.asarray(x)

import numpy as rnp


# ---------- Phase profiler ----------
import time as _time
from collections import defaultdict as _defaultdict
from contextlib import contextmanager as _contextmanager

_phase_totals = _defaultdict(float)
_phase_counts = _defaultdict(int)

@_contextmanager
def _profile(name):
    t = _time.perf_counter()
    try:
        yield
    finally:
        _phase_totals[name] += _time.perf_counter() - t
        _phase_counts[name] += 1

def _print_profile_summary():
    if not _phase_totals:
        return
    total = sum(_phase_totals.values())
    print("\n[profile] phase breakdown:")
    print(f"  {'phase':<24} {'total(s)':>10} {'calls':>8} {'avg(ms)':>10} {'%':>6}")
    for name in sorted(_phase_totals, key=lambda k: -_phase_totals[k]):
        t = _phase_totals[name]
        n = _phase_counts[name]
        avg_ms = (t / n) * 1000 if n else 0
        pct = (t / total) * 100 if total > 0 else 0
        print(f"  {name:<24} {t:>10.3f} {n:>8} {avg_ms:>10.3f} {pct:>5.1f}%")
    _phase_totals.clear()
    _phase_counts.clear()


from typing import Union
from typing import Optional

class FrameAudio:
    def __init__(self, audio_data: dict) -> None:
        """Initialize a FrameAudio object with per-frame audio data.

        Parameters:
            audio_data (dict): A dictionary containing audio features for the frame.
                Expected keys:
                    - 'volume' (float): Root-mean-square volume of the frame.
                    - 'frequencies' (np.ndarray): Array of frequency bins.
                    - 'magnitude' (np.ndarray): Array of magnitudes corresponding to the frequencies.
        """
        self._audio_data = audio_data
        self.__freqs = self._audio_data["frequencies"]

    def get_volume(self) -> float:
        """Retrieve the root-mean-square volume of the frame.

        Returns:
            float: The volume level for the frame.
        """
        return self._audio_data["volume"]

    def get_frequency(self, freq: int) -> float:
        """Retrieve the magnitude of a specific frequency within the frame.

        Parameters:
            freq (int): The frequency (in Hz) to retrieve the magnitude for.

        Returns:
            float: Magnitude corresponding to the requested frequency.

        Raises:
            ValueError: If the frequency is outside the valid range of the available data.
        """
        bin_width = self.__freqs[1] - self.__freqs[0]
        index = int(freq / bin_width)

        if index < 0 or index >= len(self.__freqs):
            raise ValueError(f"Requested frequency {freq} Hz is out of bounds for available frequency range.")

        return self._audio_data["magnitude"][index]

    def list_frequencies(self) -> np.ndarray:
        """List all frequency bins available in the frame.

        Returns:
            np.ndarray: Array of frequencies (in Hz).
        """
        return self.__freqs

    def list_magnitudes(self) -> np.ndarray:
        """List all magnitude values corresponding to the frequency bins.

        Returns:
            np.ndarray: Array of magnitudes.
        """
        return self._audio_data["magnitude"]

class Frame:
    """A class to represent and manipulate a single frame of pixel data."""

    def __init__(self, pixels: np.ndarray) -> None:
        """Initialize the Frame with a 2D or 3D NumPy array of pixel data.

        Parameters:
            pixels (np.ndarray): A 2D (grayscale) or 3D (color) NumPy array representing the frame's pixels.
        """
        if not isinstance(pixels, np.ndarray):
            raise ValueError("Frame must be initialized with a NumPy array.")

        if pixels.ndim not in (2, 3):
            raise ValueError("Frame must be a 2D (grayscale) or 3D (color) array.")

        # Auto-promote 2D grayscale to 3D RGB
        if pixels.ndim == 2:
            # Duplicate the single channel 3 times across the last axis
            pixels = np.stack([pixels, pixels, pixels], axis=-1)

        self._pixels = pixels.astype(np.uint8)
        self._original_pixels = pixels.astype(np.uint8)
        self._height, self._width = self._pixels.shape[:2]

    def __str__(self) -> str:
        """Return a string describing the shape of the frame.

        Returns:
            str: String representation of the frame.
        """
        return f"Frame with shape {self.get_pixels().shape}"

    def apply_filter(self, filter) -> None:
        """Apply a filter object to the frame's pixels.

        Parameters:
            filter: A filter to apply to the frame
        """
        with _profile(f"filter:{filter.__class__.__name__}"):
            # Prefer apply_to (resolves the field mask at the frame's exact
            # resolution); fall back to apply for legacy/custom filters.
            runner = getattr(filter, "apply_to", None)
            if runner is None:
                runner = filter.apply
            self._pixels = runner(self.get_pixels().astype(np.uint16)).astype(np.uint8)

        return self

    def get_pixels(self, standard_size: bool = False) -> np.ndarray:
        """Return the frame's pixel data as a NumPy array.

        Parameters:
            standard_size (bool): If True, returns pixels in uint8 format; otherwise, returns uint16.

        Returns:
            np.ndarray: The pixel data of the frame.
        """
        if standard_size:
            # astype with copy=False returns the same array when dtype already matches
            return self._pixels.astype(np.uint8, copy=False)
        else:
            return self._pixels.astype(np.uint16, copy=False)

    def modify(self, func) -> None:
        """Apply a user-defined function to each pixel in the frame.

        Parameters:
            func (callable): A function that takes (x, y, pixel) and returns a modified pixel array.
        """
        height, width, _ = self.get_pixels().shape

        x_coords, y_coords = np.meshgrid(np.arange(width), np.arange(height))
        flat_pixels = self.get_pixels().reshape(-1, 3)
        flat_x_coords = x_coords.flatten()
        flat_y_coords = y_coords.flatten()

        new_flat_pixels = np.array([
            func(x, y, pixel)
            for x, y, pixel in zip(flat_x_coords, flat_y_coords, flat_pixels)
        ])

        self._pixels = new_flat_pixels.reshape(height, width, 3).astype(np.uint8)

        return self

    def resize(self, w: int, h: int) -> None:
        """Resize the frame to a new width and height.

        Parameters:
            w (int): The target width.
            h (int): The target height.
        """
        # 1. BRIDGE TO CPU
        pixels_cpu = self._original_pixels.get() if hasattr(self._original_pixels, 'get') else self._original_pixels

        # 2. PERFORM ON CPU
        resized = cv2.resize(pixels_cpu, (w, h))

        # 3. BRIDGE TO GPU (if enabled)
        self._pixels = np.asarray(resized)

        self._width = w
        self._height = h

        return self

    def set_width(self, w: int) -> None:
        """Set a new width for the frame while keeping the current height.

        Parameters:
            w (int): The new width in pixels.
        """
        self.resize(w, self.height())

    def set_height(self, h: int) -> None:
        """Set a new height for the frame while keeping the current width.

        Parameters:
            h (int): The new height in pixels.
        """
        self.resize(self.width(), h)

    def width(self) -> int:
        """Return the current width of the frame.

        Returns:
            int: Width in pixels.
        """
        return self._width

    def height(self) -> int:
        """Return the current height of the frame.

        Returns:
            int: Height in pixels.
        """
        return self._height

    def preview(self, wait_for_exit: bool = False, title: str = "Frame Preview") -> None:
        """Display the frame in a window for previewing."""
        window_name = title

        # Safely extract to CPU without relying on np.asnumpy
        pixels_cpu = self._pixels.get() if hasattr(self._pixels, 'get') else self._pixels

        cv2.imshow(window_name, pixels_cpu)

        if wait_for_exit:
            while cv2.getWindowProperty(window_name, cv2.WND_PROP_VISIBLE) >= 1:
                if cv2.waitKey(100) & 0xFF == ord('q'):
                    break
        else:
            cv2.waitKey(1)

    def crop(self, corner1: tuple, corner2: tuple) -> None:
        """Crop the frame to a rectangle defined by two corners.

        Parameters:
            corner1 (tuple) @position: (x, y) pixels of the top-left corner.
            corner2 (tuple) @position: (x, y) pixels of the bottom-right corner.
        """
        x1, y1 = int(corner1[0]), int(corner1[1])
        x2, y2 = int(corner2[0]), int(corner2[1])
        x1 = max(0, min(x1, self._width))
        y1 = max(0, min(y1, self._height))
        x2 = max(0, min(x2, self._width))
        y2 = max(0, min(y2, self._height))

        self._pixels = self._pixels[y1:y2, x1:x2]
        self._height, self._width = self._pixels.shape[:2]

        return self


class Color_Frame(Frame):
    """A class to represent and manipulate a frame composed of a single RGB color"""

    def __init__(self, width: int, height: int, color: tuple = (0, 0, 0)):
        """Create a frame filled with a single solid RGB color.

        Parameters:
            width (int): Frame width in pixels.
            height (int): Frame height in pixels.
            color (tuple) @color: The fill color as an (r, g, b) tuple, each 0–255.
        """
        if not (isinstance(color, tuple) and len(color) == 3 and
                all(isinstance(c, int) and 0 <= c <= 255 for c in color)):
            raise ValueError("Color must be a tuple of three integers (R, G, B) between 0 and 255.")

        # Create an empty array and fill it using a GPU/CPU safe array cast
        pixels = np.zeros((height, width, 3), dtype=np.uint8)
        pixels[:] = np.array(color, dtype=np.uint8)

        super().__init__(pixels)

    def change_color(self, color: tuple) -> None:
        """Replace the frame's fill color.

        Parameters:
            color (tuple) @color: The new color as an (r, g, b) tuple, each 0–255.
        """
        if not (isinstance(color, tuple) and len(color) == 3 and
                all(isinstance(c, int) and 0 <= c <= 255 for c in color)):
            raise ValueError("Color must be a tuple of three integers (R, G, B) between 0 and 255.")

        # Safely wrap the tuple for GPU compatibility here as well
        self._pixels[:] = np.array(color, dtype=np.uint8)


class Video:
    """A class to represent and manipulate a video file, including reading frames and extracting audio."""

    def __init__(self, video_path: str) -> None:
        """Initialize the Video object by opening the video file, extracting basic properties,
        and checking if it contains audio.

        Parameters:
            video_path (str): Path to the video file to be loaded.

        Raises:
            SystemExit: If the video file cannot be opened, exits the program.
        """
        self.__original_path = video_path
        self.__video_path = video_path
        self.__pre_scaled = False

        # Probe source metadata (always from the original file)
        probe = cv2.VideoCapture(video_path)
        if not probe.isOpened():
            print("Error: Could not open video file.")
            print(video_path)
            exit()
        self.__source_fps = probe.get(cv2.CAP_PROP_FPS)
        self.__source_frame_count = int(probe.get(cv2.CAP_PROP_FRAME_COUNT))
        self.__source_width = int(probe.get(cv2.CAP_PROP_FRAME_WIDTH))
        self.__source_height = int(probe.get(cv2.CAP_PROP_FRAME_HEIGHT))
        probe.release()

        self.__fps = self.__source_fps * dt
        self.__frame_duration = max(1, int(self.__source_frame_count * dt))
        # Round to even dimensions so they match what h264/ffmpeg produces
        self.__width = max(2, (int(self.__source_width * dx) // 2) * 2)
        self.__height = max(2, (int(self.__source_height * dx) // 2) * 2)

        # If preview-scaled, transcode source to a smaller cached copy with
        # nearest-neighbor (pixel decimation) — fast encode, fast decode.
        if dx != 1.0:
            cache_dir = "VideoCache"
            os.makedirs(cache_dir, exist_ok=True)
            safe_name = "".join(c if c.isalnum() else f"_{ord(c)}_" for c in video_path)
            cache_path = f"{cache_dir}/{safe_name}_dx{dx}.mp4"

            # Verify cache dimensions. If the source file changed, delete the stale cache.
            if os.path.isfile(cache_path):
                probe_cache = cv2.VideoCapture(cache_path)
                cw = int(probe_cache.get(cv2.CAP_PROP_FRAME_WIDTH))
                ch = int(probe_cache.get(cv2.CAP_PROP_FRAME_HEIGHT))
                probe_cache.release()
                if cw != self.__width or ch != self.__height:
                    os.remove(cache_path)

            if not os.path.isfile(cache_path):
                print(f"[preview] Downscaling {video_path} -> {self.__width}x{self.__height} (one-time, cached)...")
                # Try Apple's hardware H.264 encoder first; fall back to libx264 on any other platform.
                encoders_to_try = [
                    ["-c:v", "h264_videotoolbox", "-b:v", "8M"],
                    ["-c:v", "libx264", "-preset", "ultrafast", "-crf", "23"],
                ]
                result = None
                for enc_args in encoders_to_try:
                    result = subprocess.run([
                        "ffmpeg", "-y", "-i", video_path,
                        "-vf", f"scale={self.__width}:{self.__height}:flags=neighbor",
                        *enc_args,
                        "-an",
                        cache_path
                    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    if result.returncode == 0:
                        break
                    if os.path.isfile(cache_path):
                        os.remove(cache_path)
                if result is None or result.returncode != 0:
                    print("[preview] ffmpeg downscale failed; falling back to in-process resize.")
            if os.path.isfile(cache_path):
                self.__video_path = cache_path
                self.__pre_scaled = True

        self.open()
        # Tracks the next source frame index that cv2.read() will return.
        # Used to decide between sequential read-forward vs. seek+decode.
        self.__current_source_pos = 0

        # In-memory frame cache for fast preview iteration. Only enabled
        # when the user is in preview mode (dx < 1 or dt < 1). The RAM-aware
        # format picker inside _prebuild_frame_cache decides between raw / jpeg
        # / skip based on actual memory available.
        self.__frame_cache = None
        self.__cache_format = None
        if dx < 1 or dt < 1:
            self._prebuild_frame_cache()

        # Check if the video has an audio stream (always check original)
        self.audio = None
        if self._has_audio():
            self.audio = Audio(self.__original_path)

    def _has_audio(self):
        """Check if the video file has an audio stream using ffprobe.

        Returns:
            bool: True if the video contains audio, False otherwise.
        """
        check_audio_command = [
            "ffprobe",
            "-i", self.__original_path,
            "-show_streams",
            "-select_streams", "a",
            "-loglevel", "error"
        ]
        result = subprocess.run(check_audio_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return str(result.stdout) != ''

    def _prebuild_frame_cache(self) -> None:
        """Read every (scaled) frame into memory once for instant subsequent access.

        Chooses raw numpy or JPEG-encoded storage based on available RAM.
        """
        import time
        n = self.__frame_duration
        raw_per_frame = self.__width * self.__height * 3
        raw_total = n * raw_per_frame

        # Detect available RAM (psutil is best-effort; fall back to a conservative guess)
        try:
            import psutil
            free_ram = psutil.virtual_memory().available
        except Exception:
            try:
                free_ram = os.sysconf('SC_PHYS_PAGES') * os.sysconf('SC_PAGE_SIZE') // 2
            except Exception:
                free_ram = 4 * 1024 ** 3  # 4 GB last-resort assumption

        budget = int(free_ram * 0.3)  # leave headroom for Python interpreter + per-frame buffers

        # Pick storage format
        if raw_total <= budget:
            fmt = "raw"
        elif raw_total // 8 <= budget:  # JPEG ~5-15× smaller; assume 8× to be safe
            fmt = "jpeg"
        else:
            print(f"[preview] Cache would exceed RAM budget ({raw_total/1e9:.1f} GB raw, {budget/1e9:.1f} GB available). Skipping pre-cache.")
            self.__cache_format = None
            return

        size_mb = raw_total / (1024*1024) if fmt == "raw" else (raw_total // 8) / (1024*1024)
        print(f"[preview] Caching {n} frames of {self.__original_path} as {fmt} (~{size_mb:.0f} MB, free RAM {free_ram/1e9:.1f} GB)...")
        start = time.time()

        cache = []
        last_read = -1
        for scaled_f in range(n):
            target_source = int(round(scaled_f / dt)) if dt != 1.0 else scaled_f
            while last_read < target_source - 1:
                self.__video.read()
                last_read += 1
            ret, frame = self.__video.read()
            last_read += 1
            if ret:
                # Always ensure the loaded frame strictly matches the expected dimensions
                if frame.shape[1] != self.__width or frame.shape[0] != self.__height:
                    frame = cv2.resize(frame, (self.__width, self.__height))
                if fmt == "jpeg":
                    ok, encoded = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                    cache.append(encoded if ok else None)
                else:
                    cache.append(frame)
            else:
                cache.append(None)

        self.__frame_cache = cache
        self.__cache_format = fmt
        self.__current_source_pos = last_read + 1
        print(f"[preview] Cached {len(cache)} frames in {time.time() - start:.1f}s")

    def open(self) -> None:
        """Open the video file using OpenCV's VideoCapture.

        This method is called during initialization to ensure the video is ready to be processed.
        """
        self.__video = cv2.VideoCapture(self.__video_path)

    def close(self) -> None:
        """Release the video file and free any associated resources.
        """
        self.__video.release()

    def get_frame(self, frame_index: int, w=1.0, h=None) -> Frame:
        """Retrieve a specific frame by its index and optionally resize it.

        Parameters:
            frame_index (int): The index of the frame to retrieve.
            w (float, optional): A scaling factor for the width. Defaults to 1.0 (no scaling).
            h (int, optional): The target height for resizing. If None, the height is scaled proportionally.

        Returns:
            Frame: A Frame object containing the requested video frame.

        Raises:
            ValueError: If the frame index is out of bounds or if the frame cannot be read.
        """
        if frame_index < 0 or frame_index >= self.__frame_duration:
            raise ValueError(f"Frame index {frame_index} is out of bounds (0 to {self.__frame_duration - 1}).")

        with _profile("video.get_frame"):
            return self._get_frame_impl(frame_index, w, h)

    def _get_frame_impl(self, frame_index, w, h):
        # Fast path: in-memory cached frame.
        if self.__frame_cache is not None:
            cached = self.__frame_cache[frame_index]
            if cached is None:
                frame = rnp.zeros((self.__height, self.__width, 3), dtype=rnp.uint8)
            elif self.__cache_format == "jpeg":
                # imdecode returns a fresh array; no copy needed
                frame = cv2.imdecode(cached, cv2.IMREAD_COLOR)
            else:
                # Raw cache holds shared arrays; copy to protect from in-place filter mutation
                frame = cached.copy()

            # 1. ALWAYS perform OpenCV resizing on the CPU first
            if h is None and w != 1.0:
                frame = cv2.resize(frame, (0, 0), fx=w, fy=w)
            elif h is not None:
                frame = cv2.resize(frame, (w, h))

            # 2. THEN push the final sized frame to the GPU
            if gpu_enabled:
                frame = np.asarray(frame)

            return Frame(frame)

        # Map scaled (preview) index to source index
        if dt != 1.0:
            source_idx = min(self.__source_frame_count - 1, int(round(frame_index / dt)))
        else:
            source_idx = frame_index

        # For small forward jumps, sequential read+discard is faster than seek+decode
        SEEK_THRESHOLD = 60
        delta = source_idx - self.__current_source_pos
        if 0 <= delta <= SEEK_THRESHOLD:
            for _ in range(delta):
                self.__video.read()
            ret, frame = self.__video.read()
        else:
            self.__video.set(cv2.CAP_PROP_POS_FRAMES, source_idx)
            ret, frame = self.__video.read()
        self.__current_source_pos = source_idx + 1

        if ret:
            # Always ensure the loaded frame strictly matches the expected dimensions
            if frame.shape[1] != self.__width or frame.shape[0] != self.__height:
                frame = cv2.resize(frame, (self.__width, self.__height))

            # 1. ALWAYS perform OpenCV resizing on the CPU first
            if h is None and w != 1.0:
                frame = cv2.resize(frame, (0, 0), fx=w, fy=w)
            elif h is not None:
                frame = cv2.resize(frame, (w, h))

            # 2. THEN push the final sized frame to the GPU
            if gpu_enabled:
                frame = np.asarray(frame)

            return Frame(frame)
        else:
            raise ValueError(f"Frame {frame_index} could not be read. The video may be closed.")

    def frame_duration(self) -> int:
        """Get the total number of frames in the video.

        Returns:
            int: The total frame count.
        """
        return self.__frame_duration

    def fps(self) -> int:
        """Get the frames per second (FPS) of the video.

        Returns:
            float: The FPS value of the video.
        """
        return self.__fps

    def width(self) -> int:
        """Get the width of the video.

        Returns:
            int: The width of the video in pixels.
        """
        return self.__width

    def height(self) -> int:
        """Get the height of the video.

        Returns:
            int: The height of the video in pixels.
        """
        return self.__height

    def resize(self, w: int, h: int) -> None:
        """Resize the video to the specified width and height.

        Parameters:
            w (int): The target width in pixels.
            h (int): The target height in pixels.
        """
        self._pixels = cv2.resize(self._original_pixels, (w, h))
        self.__width = w
        self.__height = h

    def set_width(self, w: int) -> None:
        """Set a new width for the video while keeping the current height.

        Parameters:
            w (int): The new width in pixels.
        """
        self.resize(w, self.height())

    def set_height(self, h: int) -> None:
        """Set a new height for the video while keeping the current width.

        Parameters:
            h (int): The new height in pixels.
        """
        self.resize(self.width(), h)

    def frame_audio(self, index: int) -> FrameAudio:
        """Get the audio frame corresponding to the given video frame index.

        Parameters:
            index (int): The index of the video frame.

        Returns:
            FrameAudio: The corresponding audio frame for the specified video frame.
        """
        with _profile("video.frame_audio"):
            if dt != 1.0:
                source_idx = min(self.__source_frame_count - 1, int(round(index / dt)))
            else:
                source_idx = index
            return self.audio.frame_audio(source_idx)

class Audio:
    def __init__(self, file_path: str) -> None:
        """Initialize an Audio object with a given file path.

        Parameters:
            file_path (str): Path to the MP4, MP3, or WAV file.

        Raises:
            ValueError: If the provided file extension is unsupported.
        """
        self._file_path = file_path
        self._file_type = self._determine_file_type()
        self._fps = self._get_fps() if self._file_type == "mp4" else 60
        self._sample_rate = 44100
        self._audio_data = None
        self._loaded = False
        self._duration = None

    def _determine_file_type(self) -> str:
        """Determine the file type based on the file extension.

        Returns:
            str: 'mp4', 'mp3', or 'wav'.

        Raises:
            ValueError: If the file type is not one of the supported formats.
        """
        if self._file_path.endswith(".mp4"):
            return "mp4"
        elif self._file_path.endswith(".mp3"):
            return "mp3"
        elif self._file_path.endswith(".wav"):
            return "wav"
        else:
            raise ValueError("Unsupported file type. Supported types are: mp4, mp3, wav.")

    def _get_fps(self) -> float:
        command = [
            "ffprobe", "-v", "error",
            "-select_streams", "v:0",
            "-show_entries", "stream=r_frame_rate",
            "-of", "csv=p=0",
            self._file_path
        ]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8', errors='replace')
        fps_data = result.stdout.strip()

        try:
            num, denom = map(int, fps_data.split('/'))
            fps = num / denom
            if fps <= 0:
                raise ValueError
            return fps
        except Exception:
            raise ValueError(f"[Error] Could not extract FPS from video file: {fps_data}")


    def _extract_full_audio(self) -> str:
        """Extract the full audio content into a WAV file using FFmpeg.

        Returns:
            str: The path to the extracted WAV file.

        Raises:
            ValueError: If FFmpeg fails to extract or produce a valid audio file.
        """
        if self._file_type == "wav":
            return self._file_path

        output_audio = "full_audio.wav"

        # REMOVED the forced "-ar" downsampling flag so the WAV
        # inherits the exact native sample rate of the source file
        command = [
            "ffmpeg", "-y",
            "-i", self._file_path,
            "-vn" if self._file_type == "mp4" else "",
            "-f", "wav",
            "-ac", "1",
            output_audio
        ]

        command = [arg for arg in command if arg]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8', errors='replace')

        if result.returncode != 0:
            print("FFmpeg Error:", result.stderr)
            raise ValueError("FFmpeg failed to convert the file. Check the input format and file path.")

        if not os.path.isfile(output_audio) or os.path.getsize(output_audio) == 0:
            raise ValueError("Failed to extract audio. The file is empty or was not created.")

        return output_audio

    def _load_audio_data(self) -> np.ndarray:
        """Load and normalize the full audio data into memory.

        Returns:
            np.ndarray: The loaded and normalized audio waveform.

        Raises:
            ValueError: If the audio data is empty or invalid.
        """
        audio_path = self._extract_full_audio()

        if not os.path.exists(audio_path):
            raise FileNotFoundError(f"Extracted audio file not found: {audio_path}")

        try:
            sample_rate, audio_data = wavfile.read(audio_path)
        except Exception as e:
            raise RuntimeError(f"[Error] Failed to read WAV file '{audio_path}': {e}")

        if self._file_type != "wav":
            try:
                os.remove(audio_path)
            except Exception as e:
                print(f"[Warning] Failed to remove temporary audio file: {e}")

        if audio_data.size == 0:
            raise ValueError("[Error] Audio data is empty. The video may not have a valid audio track.")

        # Convert stereo to mono
        if audio_data.ndim > 1:
            audio_data = audio_data.mean(axis=1)

        # Normalize audio depending on its dtype
        if audio_data.dtype == np.int16:
            audio_data = audio_data.astype(np.float32) / 32768.0
        elif audio_data.dtype == np.int32:
            audio_data = audio_data.astype(np.float32) / 2147483648.0
        elif np.issubdtype(audio_data.dtype, np.floating):
            audio_data = np.clip(audio_data, -1.0, 1.0)
        else:
            raise TypeError(f"[Error] Unsupported audio dtype: {audio_data.dtype}")

        self._sample_rate = sample_rate
        self._duration = len(audio_data) / sample_rate

        print(f"[Info] Loaded audio: sample_rate={sample_rate}, duration={self._duration:.2f}s, dtype={audio_data.dtype}")
        return audio_data



    def _get_duration(self) -> float:
        """Calculate the duration of the audio file.

        Returns:
            float: Duration of the audio in seconds.
        """
        audio_path = self._extract_full_audio()
        sample_rate, audio_data = wavfile.read(audio_path)
        duration = len(audio_data) / sample_rate

        if self._file_type != "wav":
            os.remove(audio_path)

        return duration

    def _encode_cache_name(self, path: str) -> str:
        """Encode the given file path into a filesystem-safe cache name.

        Parameters:
            path (str): The original file path.

        Returns:
            str: The encoded cache name.
        """
        encoded = ""
        for char in path:
            if char.isalnum():
                encoded += char
            else:
                encoded += f"_{ord(char)}_"
        return encoded

    def preload_data(self, reload: bool = False, padding_factor: int = 1) -> None:
        """Preload and process audio into per-frame features (volume, spectrum) for faster access.

        Parameters:
            reload (bool): Whether to force reloading even if cached data exists.
            padding_factor (int): Multiplier for FFT zero-padding to increase frequency resolution. Default is 1 (no padding).
        """
        os.makedirs("AudioCache", exist_ok=True)

        cache_file = f"AudioCache/{self._encode_cache_name(self._file_path)}_pad{padding_factor}.npy"

        if os.path.isfile(cache_file) and not reload:
            full_audio_data = rnp.load(cache_file, allow_pickle=True)

            self._audio_data = full_audio_data[:-1]
            self._sample_rate = full_audio_data[-1]["sample rate"]
            self._duration = full_audio_data[-1]["duration"]
        else:
            audio_data = self._load_audio_data()
            samples_per_frame = int(self._sample_rate / self._fps)

            self._audio_data = []
            frame_duration = self._duration * self._fps

            for i in range(int(frame_duration)):
                start_idx = i * samples_per_frame
                end_idx = start_idx + samples_per_frame

                if start_idx >= len(audio_data):
                    break
                if end_idx > len(audio_data):
                    end_idx = len(audio_data)

                frame_audio = audio_data[start_idx:end_idx]

                if len(frame_audio) == 0:
                    break

                volume = np.sqrt(np.mean(frame_audio ** 2))

                # 💥 NEW: Apply the dynamic padding factor to the math
                fft_size = len(frame_audio) * padding_factor

                yf = fft(frame_audio, n=fft_size)
                xf = fftfreq(fft_size, 1 / self._sample_rate)

                positive_frequencies = xf[:len(yf) // 2]
                magnitude = rnp.abs(yf[:len(yf) // 2])

                self._audio_data.append({
                    "volume": volume,
                    "frequencies": positive_frequencies,
                    "magnitude": magnitude
                })

            self._audio_data.append({
                "duration": self._duration,
                "sample rate": self._sample_rate
            })
            rnp.save(cache_file, self._audio_data)
            self._audio_data = self._audio_data[:-1]

        self._loaded = True


    def frame_audio(self, frame_index: int) -> FrameAudio:
        """Retrieve audio data corresponding to a specific frame.

        Parameters:
            frame_index (int): The frame index to retrieve.

        Returns:
            FrameAudio: Audio features (volume, frequencies, magnitude) for the requested frame.

        Raises:
            ValueError: If preload_data() has not been called before accessing frame data.
        """
        if not self._loaded:
            raise ValueError("Audio data not preloaded. Call `preload_data()` first.")

        if self._file_type == "mp4":
            if frame_index < len(self._audio_data):
                return FrameAudio(self._audio_data[frame_index])
            else:
                return FrameAudio(self._audio_data[-1])
        else:
            # FIX: Multiply by the exact float ratio BEFORE casting to int
            # This perfectly maps any video framerate to the 60 FPS audio cache!
            target_idx = int(frame_index * (self._fps / renderer.fps()))

            if target_idx < len(self._audio_data):
                return FrameAudio(self._audio_data[target_idx])
            else:
                return FrameAudio(self._audio_data[-1])

    def length(self) -> float:
        """Return the length of the audio in seconds.

        Returns:
            float: Total duration of the audio file.
        """
        # If we already calculated the duration during preloading, return it instantly
        if hasattr(self, '_duration') and self._duration > 0:
            return self._duration

        # Safely check if the audio data list is empty without using .all()
        if not getattr(self, '_audio_data', []):
            # This automatically sets self._duration
            self._load_audio_data()

        return getattr(self, '_duration', 0.0)

    def fps(self) -> float:
        """Get the frames per second associated with the video or assumed for audio.

        Returns:
            float: Frames per second value.
        """
        return self._fps

    def file_type(self) -> str:
        """Return the file type for this Audio object.

        Returns:
            str: File type ('mp4', 'mp3', or 'wav').
        """
        return self._file_type

    def file_path(self) -> str:
        """Return the original file path provided during initialization.

        Returns:
            str: File path.
        """
        return self._file_path


class ImageFile:
    """A class to represent and manipulate static images as media assets."""

    def __init__(self, file_path: str) -> None:
        self.__file_path = file_path

        # Load with unchanged to preserve alpha channel if it exists
        raw_image = cv2.imread(file_path, cv2.IMREAD_UNCHANGED)
        if raw_image is None:
            raise ValueError(f"Could not load image file: {file_path}")

        # 💥 THE FIX: Safely scale 16-bit Blender exports down to 8-bit BEFORE any math!
        if raw_image.dtype == np.uint16:
            raw_image = (raw_image / 256).astype(np.uint8)

        # Now, the image is guaranteed to be 8-bit. The alpha math will work perfectly.
        if raw_image.shape[2] == 4:
            alpha = raw_image[:, :, 3] / 255.0
            bgr = raw_image[:, :, :3]

            # Use standard numpy to avoid GPU setup lag during initialization
            bg = rnp.zeros_like(bgr, dtype=rnp.float32)

            for c in range(3):
                bg[:, :, c] = (alpha * bgr[:, :, c])
            self.__image = bg.astype(rnp.uint8)
        else:
            self.__image = raw_image

        self.__height, self.__width = self.__image.shape[:2]

    def get_frame(self, frame_index: int = 0, w=1.0, h=None) -> Frame:
        """Returns the image as a Frame. The frame_index is ignored since it's static."""
        with _profile("image.get_frame"):
            frame = self.__image.copy()

            if gpu_enabled:
                frame = np.asarray(frame)

            if h is None and w != 1.0:
                if gpu_enabled:
                    frame_cpu = np.asnumpy(frame).astype(np.uint8)
                    frame_cpu = cv2.resize(frame_cpu, (0, 0), fx=w, fy=w)
                    frame = np.asarray(frame_cpu)
                else:
                    frame = cv2.resize(frame, (0, 0), fx=w, fy=w)
            elif h is not None:
                if gpu_enabled:
                    frame_cpu = np.asnumpy(frame).astype(np.uint8)
                    frame_cpu = cv2.resize(frame_cpu, (w, h))
                    frame = np.asarray(frame_cpu)
                else:
                    frame = cv2.resize(frame, (w, h))

            return Frame(frame)

    def frame_duration(self) -> int:
        return 999999

    def fps(self) -> float:
        return renderer.fps()

    def width(self) -> int:
        return self.__width

    def height(self) -> int:
        return self.__height

    def file_path(self) -> str:
        return self.__file_path

import cv2
import subprocess
import os


import cv2
import subprocess
import os
import shutil
from typing import List

class NonlinearRenderer:
    def __init__(self, width: int, height: int, fps: int) -> None:
        """Initialize a NonlinearRenderer for rendering video frames non-sequentially.

        Parameters:
            width (int): Width of the output video in pixels.
            height (int): Height of the output video in pixels.
            fps (int): Frames per second for the output video.
        """
        self.__width = width
        self.__height = height
        self.__fps = fps
        self.__frame_indices: List[int] = []
        self.__unordered_writer = cv2.VideoWriter(
            "unordered_render.mp4",
            cv2.VideoWriter_fourcc(*"mp4v"),
            self.__fps,
            (self.__width, self.__height)
        )
        self.__max_frame_index = -1
        self.__audios: List[tuple['Audio', float]] = []
        self.__in_order = True
        self.__expected_next = 0
        self.__preview_mode = False
        self.__show_previews = False
        # Shared-memory frame buffer state — only used when the wrapper enables it.
        # The path must match what the C++ side opens in FrameBufferReader; we use the
        # platform's temp dir (Windows: %TEMP%, POSIX: /tmp) so the two sides agree.
        import tempfile as _tempfile
        self.__use_frame_buffer = False
        self.__fb_path = os.path.join(_tempfile.gettempdir(), "neovere_frames.bin")
        self.__fb_mm = None              # numpy memmap of frame data (post-header)
        self.__fb_max_frames = 0         # capacity of allocated buffer
        self.__fb_generation = 0
        self.__fb_failed = False         # if buffer alloc failed, fall back to video file
        self.__fb_actual_frames = 0      # number of frames the user actually wrote

        # -- 'standard' template state (setup/render projects driven by run()) --
        # A standard project precomputes state in setup() and renders individual
        # frames on demand via render(f). The engine keeps the registered render
        # callback alive (its __globals__ holds the user's namespace) so frames
        # can be produced one at a time without rebuilding the whole video.
        self.__duration = 0              # timeline length in frames (set_duration)
        self.__std_setup = None          # the user's setup() callable
        self.__std_render = None         # the user's render(f) callable
        self.__std_setup_key = None      # source hash of the last setup() we ran
        self.__std_registered = False    # True once run() registered this dispatch
        self.__std_request_frame = 0     # frame to render for the initial publish
        self.__std_has_audio = False     # True if setup() attached audio this session

    def set_use_frame_buffer(self, enabled: bool) -> None:
        """When enabled, set_frame() also writes frames to a shared-memory buffer
        for the C++ side to display directly (skipping the video file decode path).
        Falls back to disk-only rendering if the buffer can't be allocated."""
        self.__use_frame_buffer = enabled

    def set_preview_mode(self, enabled: bool) -> None:
        """Enable preview mode: skip audio mux, output to preview.mp4 instead of render.mp4."""
        self.__preview_mode = enabled

    def set_show_previews(self, enabled: bool) -> None:
        """When enabled, set_frame() also calls new_frame.preview() so the user sees each
        frame as it's added to the render. Used by the Run button for live progress display."""
        self.__show_previews = enabled

    def reset(self) -> None:
        """Clear all accumulated frame state and start fresh."""
        self.__frame_indices = []
        self.__max_frame_index = -1
        self.__in_order = True
        self.__expected_next = 0
        self.__audios = []
        self.__unordered_writer = cv2.VideoWriter(
            "unordered_render.mp4",
            cv2.VideoWriter_fourcc(*"mp4v"),
            self.__fps,
            (self.__width, self.__height)
        )
        # Reset frame buffer state too so each render starts with a fresh allocation.
        self.__fb_mm = None
        self.__fb_max_frames = 0
        self.__fb_failed = False
        self.__fb_actual_frames = 0

    # -- shared-memory frame buffer helpers --

    def _fb_header_size(self) -> int:
        return 64

    def _fb_allocate(self) -> bool:
        """Allocate the shared-memory frame buffer based on available RAM.
        Returns True on success, False if we should fall back to video-file mode.

        IMPORTANT: this NEVER shrinks the file. Shrinking would invalidate the
        C++ side's mmap and crash with SIGBUS when it reads an unmapped page.
        """
        try:
            try:
                import psutil
                free_ram = psutil.virtual_memory().available
            except Exception:
                free_ram = 4 * 1024 ** 3

            # Use up to 30% of free RAM for the preview frame buffer.
            budget = int(free_ram * 0.3)
            bytes_per_frame = self.__width * self.__height * 3
            if bytes_per_frame <= 0:
                return False

            max_frames = max(1, budget // bytes_per_frame)
            max_frames = min(max_frames, 20000)
            needed_size = self._fb_header_size() + max_frames * bytes_per_frame

            # Grow-only: only call truncate if the file is missing or smaller than needed.
            if not os.path.exists(self.__fb_path):
                with open(self.__fb_path, "wb") as f:
                    f.truncate(needed_size)
            else:
                current_size = os.path.getsize(self.__fb_path)
                if current_size < needed_size:
                    # Open in append-binary which does NOT truncate to 0 first.
                    with open(self.__fb_path, "ab") as f:
                        f.truncate(needed_size)
                # If the file is already big enough we leave it alone — C++ mmap is
                # still valid, and the unused tail bytes are harmless.

            self.__fb_mm = rnp.memmap(
                self.__fb_path,
                dtype=rnp.uint8,
                mode="r+",
                offset=self._fb_header_size(),
                shape=(max_frames, self.__height, self.__width, 3),
            )
            self.__fb_max_frames = max_frames
            self.__fb_actual_frames = 0
            self.__fb_generation += 1
            self._fb_write_header(0)
            return True
        except Exception as e:
            print(f"[fb] allocation failed: {e}")
            return False

    def _fb_write_header(self, frame_count: int) -> None:
        """Write the 64-byte header. Layout matches FrameBufferReader::Header in C++."""
        import struct
        try:
            with open(self.__fb_path, "r+b") as f:
                f.seek(0)
                f.write(struct.pack(
                    "<IIIIIfII",
                    0x4E454F56,             # magic 'NEOV'
                    self.__fb_generation,
                    frame_count,
                    self.__width,
                    self.__height,
                    float(self.__fps),
                    3,                      # channels (RGB)
                    0,                      # dtype (uint8)
                ))
                f.write(b"\x00" * (self._fb_header_size() - 32))
                f.flush()
        except Exception as e:
            print(f"[fb] header write failed: {e}")

    def _fb_publish_single(self, frame_index: int, write_pixels) -> None:
        """Publish ONE frame to the shared buffer for on-demand (standard) playback.

        Unlike set_frame()'s accumulation, this always writes to slot 0 and
        reports frame_count=1: the C++ side displays "the latest frame" and uses
        the timeline duration (set_duration) for the scrubber, not the buffer's
        frame_count. Bumps the generation and prints a per-frame sentinel so the
        viewport knows which logical frame just became available.

        write_pixels must be HxWx3 uint8 in cv2's BGR order (as get_pixels(True)
        returns); the buffer stores RGB, so we swap channels on write.
        """
        if self.__fb_mm is None:
            if not self._fb_allocate():
                self.__fb_failed = True
                return
        if self.__fb_mm is None:
            return
        # cv2 gives BGR, the buffer holds RGB. Convert with a fast slice swap.
        self.__fb_mm[0] = write_pixels[:, :, ::-1]
        try:
            self.__fb_mm.flush()
        except Exception:
            pass
        self.__fb_actual_frames = 1
        self.__fb_generation += 1
        self._fb_write_header(1)
        print(f"<<<NEO_FRAME {int(frame_index)} {self.__fb_generation}>>>", flush=True)

    def set_frame(self, frame_index: int, new_frame: 'Frame') -> None:
        """Set a frame at a specific index for the output video.

        Parameters:
            frame_index (int): The index at which the frame will be placed.
            new_frame (Frame): Frame object containing pixel data.

        Raises:
            ValueError: If the frame dimensions do not match the initialized resolution.
        """
        # Compute the uint8 view once and reuse for both the shape check and the write.
        write_pixels = new_frame.get_pixels(True)
        if write_pixels.shape != (self.__height, self.__width, 3):
            raise ValueError(
                f"Frame dimensions do not match the initialized video resolution. "
                f"Frame dimensions are {write_pixels.shape[1]}x{write_pixels.shape[0]}, "
                f"but renderer dimensions are {self.__width}x{self.__height}."
            )

        self.__frame_indices.append(frame_index)
        if gpu_enabled:
            write_pixels = np.asnumpy(write_pixels)
        with _profile("renderer.set_frame.write"):
            self.__unordered_writer.write(write_pixels)

        # Mirror the frame into the shared-memory buffer if enabled.
        if self.__use_frame_buffer and not self.__fb_failed:
            if self.__fb_mm is None:
                if not self._fb_allocate():
                    self.__fb_failed = True
            if self.__fb_mm is not None and frame_index < self.__fb_max_frames:
                # cv2 gives BGR, the buffer holds RGB. Convert with a fast slice swap.
                self.__fb_mm[frame_index] = write_pixels[:, :, ::-1]
                if frame_index + 1 > self.__fb_actual_frames:
                    self.__fb_actual_frames = frame_index + 1
            elif self.__fb_mm is not None:
                # Out of capacity — disable buffer for the rest of this render
                self.__fb_failed = True
                print(f"[fb] frame {frame_index} exceeds buffer capacity {self.__fb_max_frames}; falling back to video file")

        if self.__show_previews:
            new_frame.preview()
        self.__max_frame_index = max(self.__max_frame_index, frame_index)

        # Track whether frames were appended sequentially (0, 1, 2, ...) with no gaps
        if frame_index != self.__expected_next:
            self.__in_order = False
        self.__expected_next = max(self.__expected_next, frame_index + 1)

    def attach_audio(self, audio: 'Audio', volume: float = 1.0) -> None:
        """Attach an Audio object to the renderer with a specified volume adjustment.

        Parameters:
            audio (Audio): The Audio object containing the audio data.
            volume (float, optional): Volume multiplier between 0.0 and 1.0. Defaults to 1.0.

        Raises:
            ValueError: If the volume is not between 0.0 and 1.0.

        Notes:
            Passing audio=None is a no-op. This lets standard projects call
            renderer.attach_audio(video.audio) in setup() without checking whether
            the source clip actually has an audio track.
        """
        if audio is None:
            return
        if not 0.0 <= volume <= 1.0:
            raise ValueError("Volume must be between 0.0 and 1.0.")
        self.__audios.append((audio, volume))

    def set_resolution(self, width: int, height: int) -> None:
        """Change the resolution of the output video.

        Parameters:
            width (int): New width in pixels.
            height (int): New height in pixels.
        """
        self.__width = width
        self.__height = height
        self.__unordered_writer = cv2.VideoWriter(
            "unordered_render.mp4",
            cv2.VideoWriter_fourcc(*"mp4v"),
            self.__fps,
            (self.__width, self.__height)
        )

    def set_fps(self, fps: int) -> None:
        """Change the frames per second (fps) of the output video.

        Parameters:
            fps (int): New frames per second value.
        """
        self.__fps = fps
        self.__unordered_writer = cv2.VideoWriter(
            "unordered_render.mp4",
            cv2.VideoWriter_fourcc(*"mp4v"),
            self.__fps,
            (self.__width, self.__height)
        )

    def fps(self) -> int:
        """Get the current frames per second (fps).

        Returns:
            int: Current fps value.
        """
        return self.__fps

    def set_duration(self, frames: int) -> None:
        """Set the timeline length, in frames, for a 'standard' project.

        Standard projects render frames on demand (one at a time) rather than
        building the whole video up front, so the engine needs to be told how
        many frames the timeline spans. Used to clamp render(f) requests and to
        drive the playback scrubber / export range.

        Parameters:
            frames (int): Total number of frames in the timeline.
        """
        self.__duration = max(0, int(frames))

    def duration(self) -> int:
        """Get the timeline length in frames (see set_duration)."""
        return self.__duration

    def width(self) -> int:
        """Get the current video width.

        Returns:
            int: Current width in pixels.
        """
        return self.__width

    def height(self) -> int:
        """Get the current video height.

        Returns:
            int: Current height in pixels.
        """
        return self.__height

    def sec_to_frame(self, seconds: Union[float, int, List[float]]) -> Union[int, List[int]]:
        """Convert seconds to frame indices based on the fps.

        Parameters:
            seconds (float | int | List[float]): Time(s) in seconds.

        Returns:
            int | List[int]: Corresponding frame index or list of frame indices.

        Raises:
            TypeError: If input type is invalid.
        """
        if isinstance(seconds, list) and all(isinstance(item, float) for item in seconds):
            return [int(value * self.fps()) for value in seconds]
        elif isinstance(seconds, (float, int)):
            return int(seconds * self.fps())
        else:
            raise TypeError("Expected an int, float, or list of floats.")

    # ------------------------------------------------------------------ #
    #  'standard' template support (setup/render projects via run())      #
    # ------------------------------------------------------------------ #

    def _begin_dispatch(self, fresh_namespace: bool = False) -> None:
        """Called by the worker before (re-)executing a user script.

        Clears the standard-mode registration so we can detect whether the NEW
        script calls run(). When the worker had to start a fresh namespace
        (e.g. the previous project was legacy), it passes fresh_namespace=True
        and we also forget the last setup() hash, forcing setup() to run again
        so its precomputed globals are repopulated in the new namespace.
        """
        self.__std_registered = False
        self.__std_render = None
        self.__std_setup = None
        if fresh_namespace:
            self.__std_setup_key = None

    def _set_request_frame(self, frame_index: int) -> None:
        """Set which frame run() should render for its initial publish."""
        self.__std_request_frame = max(0, int(frame_index))

    def is_standard(self) -> bool:
        """True if the most recently executed script registered a render via run()."""
        return self.__std_registered

    def _run_standard(self, setup, render) -> None:
        """Engine entry point for 'standard' projects (called by the module-level
        run(setup, render)). Runs setup() once — and again only when its source
        changes — then publishes the initial frame and signals readiness. Returns
        immediately; subsequent frames are produced on demand by _render_one().
        """
        if not callable(render):
            raise TypeError("run(setup, render): render must be a callable taking one argument (the frame index).")
        self.__std_render = render
        self.__std_setup = setup
        self.__std_registered = True

        # Run setup() only when its source changed since the last run, so that
        # render-only edits keep the precomputed globals alive (the worker reuses
        # the namespace). If we can't read the source, run it every time.
        run_setup = True
        if setup is not None:
            try:
                import inspect, hashlib
                src = inspect.getsource(setup)
                key = hashlib.sha1(src.encode("utf-8")).hexdigest()
                run_setup = (key != self.__std_setup_key)
                self.__std_setup_key = key
            except (OSError, TypeError):
                run_setup = True
        else:
            run_setup = False

        if run_setup and setup is not None:
            setup()

        # Publish the initial frame so the viewport shows something right away.
        self._render_one(self.__std_request_frame)

        print("<<<NEO_STANDARD_READY {} {} {} {} {}>>>".format(
            int(self.__width), int(self.__height), float(self.__fps),
            int(self.__duration), int(self.__fb_generation)), flush=True)

        # If setup() ran this dispatch it may have called attach_audio(); (re)export
        # the preview audio and tell C++ whether to load or clear it. We skip this on
        # render-only edits (setup didn't run, __audios is empty, and the previously
        # exported cache is still the right one) so audio keeps playing seamlessly.
        if run_setup:
            self._refresh_standard_audio()
            print("<<<NEO_STANDARD_AUDIO {}>>>".format(
                "ready" if self.__std_has_audio else "none"), flush=True)

    def _render_one(self, frame_index: int) -> None:
        """Render a single frame on demand (standard projects) and publish it.

        Clamps the request to the timeline, calls the user's render(f), validates
        the returned frame's resolution, and mirrors it into the shared buffer.
        """
        if self.__std_render is None:
            return
        f = int(frame_index)
        if self.__duration > 0:
            if f < 0:
                f = 0
            elif f >= self.__duration:
                f = self.__duration - 1

        result = self.__std_render(f)
        if hasattr(result, "get_pixels"):
            write_pixels = result.get_pixels(True)
        else:
            write_pixels = np.asarray(result)

        if write_pixels.shape != (self.__height, self.__width, 3):
            raise ValueError(
                f"render({f}) returned a frame of "
                f"{write_pixels.shape[1] if write_pixels.ndim >= 2 else '?'}x"
                f"{write_pixels.shape[0] if write_pixels.ndim >= 1 else '?'}, "
                f"but the renderer resolution is {self.__width}x{self.__height}. "
                f"Call renderer.set_resolution(...) in setup() so they match."
            )

        if gpu_enabled:
            write_pixels = np.asnumpy(write_pixels)
        self._fb_publish_single(f, write_pixels)

    def _refresh_standard_audio(self) -> None:
        """(Re)build the preview audio cache from whatever setup() attached.

        Standard projects never call render() (the full-video path that normally
        exports audio), so we export the attached tracks here instead — once per
        dispatch in which setup() ran. Writes cached_preview_audio.aac (the same
        file legacy previews use) and sets __std_has_audio so _run_standard can tell
        the C++ side whether to load or clear the preview audio track.
        """
        cache = "cached_preview_audio.aac"
        audios = list(self.__audios)
        self.__std_has_audio = False
        if audios:
            try:
                self._export_preview_audio_only(audios, cache)
                self.__std_has_audio = True
                return
            except Exception:
                # Export failed (bad file, ffmpeg error, ...). Fall through and treat
                # this as "no audio" so the preview stays silent rather than playing a
                # stale track. Surface the cause for the user.
                import traceback as _tb
                _tb.print_exc()
        # No audio attached (or export failed): drop any stale cache so the preview is
        # silent and a later legacy preview won't pick it up as "previous" audio.
        if os.path.exists(cache):
            import time
            for _attempt in range(30):
                try:
                    os.remove(cache)
                    break
                except PermissionError:
                    # Qt may still hold the handle; give it a moment to release.
                    time.sleep(0.05)
                except OSError:
                    break

    def _export_preview_audio_only(self, audios: List[tuple['Audio', float]], out_path: str) -> None:
        """Mux the attached audio tracks into a single AAC file (no video).

        Mirrors __attach_audios' per-track volume + amix, but emits audio only so
        standard projects (which never produce silent_render.mp4) can still preview
        their sound. Raises on ffmpeg failure.
        """
        if not audios:
            raise ValueError("No audio streams to export.")

        if len(audios) == 1:
            audio, volume = audios[0]
            subprocess.run([
                "ffmpeg", "-y",
                "-i", audio.file_path(),
                "-af", f"volume={volume}",
                "-vn", "-acodec", "aac",
                out_path
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            return

        audio_files = []
        try:
            for i, (audio, volume) in enumerate(audios):
                tmp = f"temp_preview_audio_{i}.aac"
                subprocess.run([
                    "ffmpeg", "-y",
                    "-i", audio.file_path(),
                    "-af", f"volume={volume}",
                    "-vn", "-acodec", "aac",
                    tmp
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
                audio_files.append(tmp)

            input_args = []
            filter_complex = ""
            for i, tmp in enumerate(audio_files):
                input_args.extend(["-i", tmp])
                filter_complex += f"[{i}:a]"
            filter_complex += f"amix=inputs={len(audio_files)}:duration=longest[aout]"

            subprocess.run([
                "ffmpeg", "-y", *input_args,
                "-filter_complex", filter_complex,
                "-map", "[aout]",
                "-c:a", "aac",
                out_path
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        finally:
            for tmp in audio_files:
                if os.path.exists(tmp):
                    try:
                        os.remove(tmp)
                    except OSError:
                        pass

    def render(self, preview: bool = False) -> None:
        """Render the final ordered video and optionally attach audio tracks.

        Parameters:
            preview (bool, optional): Whether to preview each frame during rendering. Defaults to False.

        Raises:
            ValueError: If frames cannot be read during rendering.
        """
        self.__unordered_writer.release()

        # Fast path: frames were already appended sequentially with no gaps.
        # No need to re-encode — just rename. If preview was requested, read
        # the renamed file sequentially and call .preview() per frame (no write).
        no_gaps = self.__expected_next == self.__max_frame_index + 1
        if self.__in_order and no_gaps:
            if os.path.exists("silent_render.mp4"):
                os.remove("silent_render.mp4")
            os.rename("unordered_render.mp4", "silent_render.mp4")
            if preview:
                cap = cv2.VideoCapture("silent_render.mp4")
                while True:
                    ret, frame = cap.read()
                    if not ret:
                        break
                    if gpu_enabled:
                        frame = np.asarray(frame)
                    Frame(frame).preview()
                cap.release()
        else:
            ordered_writer = cv2.VideoWriter(
                "silent_render.mp4",
                cv2.VideoWriter_fourcc(*"mp4v"),
                self.__fps,
                (self.__width, self.__height)
            )
            frame_map = {logical_idx: write_idx for write_idx, logical_idx in enumerate(self.__frame_indices)}

            for frame_index in range(self.__max_frame_index + 1):
                unordered_idx = frame_map.get(frame_index, -1)
                if unordered_idx == -1:
                    blank_frame = cv2.UMat(self.__height, self.__width, cv2.CV_8UC3, [0, 0, 0])
                    ordered_writer.write(blank_frame)
                else:
                    unordered_render.set(cv2.CAP_PROP_POS_FRAMES, unordered_idx)
                    ret, frame = unordered_render.read()
                    if ret:
                        ordered_writer.write(frame)
                        if preview:
                            if gpu_enabled:
                                frame = np.asarray(frame)
                            Frame(frame).preview()
                    else:
                        raise ValueError(f"Could not read frame {frame_index}.")

            unordered_render.release()
            ordered_writer.release()

        # If frame-buffer mode succeeded, finalize the header with the actual frame count
        # and emit a marker so the C++ side can switch the preview tab to FrameBuffer mode
        # right away — even before the audio mux below finishes.
        if self.__use_frame_buffer and not self.__fb_failed and self.__fb_mm is not None:
            try:
                self.__fb_mm.flush()
            except Exception:
                pass
            self._fb_write_header(self.__fb_actual_frames)
            print("<<<NEO_FRAMES_READY>>>", flush=True)

        if self.__preview_mode:
            # Phase 1: produce a Qt-loadable preview.mp4 fast by muxing in cached audio
            # from the previous render if available, otherwise the silent placeholder.
            # This means the user hears the previous render's audio immediately on the
            # visual update (overwritten with the new audio in phase 2 below).
            if os.path.exists("preview.mp4"):
                # Windows: the C++ side releases preview.mp4 inside switchToFrameBuffer()
                # right after we print NEO_FRAMES_READY, but that hop runs through Qt's
                # event loop in another process so there's a small race window. Retry on
                # PermissionError for up to ~600ms while QMediaPlayer drops its handle.
                for _attempt in range(30):
                    try:
                        os.remove("preview.mp4")
                        break
                    except PermissionError:
                        _time.sleep(0.02)
                else:
                    os.remove("preview.mp4")  # final attempt; re-raise if still locked
            audio_for_phase1 = "cached_preview_audio.aac" if os.path.exists("cached_preview_audio.aac") else "silent_audio.aac"
            if os.path.exists(audio_for_phase1):
                result = subprocess.run([
                    "ffmpeg", "-y",
                    "-i", "silent_render.mp4",
                    "-i", audio_for_phase1,
                    "-shortest",
                    "-c:v", "copy",
                    "-c:a", "copy",
                    "preview.mp4"
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if result.returncode != 0:
                    shutil.copy("silent_render.mp4", "preview.mp4")
            else:
                shutil.copy("silent_render.mp4", "preview.mp4")
            # Tell C++ to reload — user sees the rendered video with previous audio.
            print("<<<NEO_VIDEO_READY>>>", flush=True)

            # Phase 2: if the user attached real audio, overwrite preview.mp4 with it,
            # then cache the resulting audio track for next render's phase 1.
            if self.__audios:
                self.__attach_audios("silent_render.mp4", self.__audios, "preview.mp4")
                # Extract the muxed audio into the cache (fast remux, no re-encode).
                subprocess.run([
                    "ffmpeg", "-y",
                    "-i", "preview.mp4",
                    "-vn", "-c:a", "copy",
                    "cached_preview_audio.aac"
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                # Tell C++ to reload the audio so frame-buffer playback uses THIS render's
                # audio (otherwise Qt's player keeps the previously-loaded version).
                print("<<<NEO_AUDIO_READY>>>", flush=True)
                print("Preview compiled as preview.mp4 (with audio)")
            else:
                import time # Ensure time is available
                # No audio attached this render — invalidate the cache so phase 1 falls
                # back to silent next time.
                if os.path.exists("cached_preview_audio.aac"):
                    # Give the Qt App up to 1.5 seconds to drop the file handle
                    for _attempt in range(30):
                        try:
                            os.remove("cached_preview_audio.aac")
                            break # Success!
                        except PermissionError:
                            time.sleep(0.05)
                    else:
                        # Soft-fail: If Qt stubbornly holds it, don't crash the whole script!
                        print("[Warning] Qt is still holding the audio cache. Will overwrite next time.")

                print("Preview compiled as preview.mp4 (silent)")
        elif self.__audios:
            with _profile("renderer.attach_audios"):
                self.__attach_audios("silent_render.mp4", self.__audios, "render.mp4")
            print("Video compiled with audio as render.mp4")
        else:
            shutil.copy("silent_render.mp4", "render.mp4")
            print("Video compiled without audio as render.mp4")

        _print_profile_summary()

        # Reset state so subsequent renders (e.g., under the persistent worker)
        # start from a clean slate.
        self.__frame_indices = []
        self.__max_frame_index = -1
        self.__in_order = True
        self.__expected_next = 0
        self.__audios = []
        self.__unordered_writer = cv2.VideoWriter(
            "unordered_render.mp4",
            cv2.VideoWriter_fourcc(*"mp4v"),
            self.__fps,
            (self.__width, self.__height)
        )

    def __attach_audios(self, rendered_video: str, audios: List[tuple['Audio', float]], output_video: str) -> None:
        """Attach multiple audio tracks to the final rendered video using ffmpeg.

        Parameters:
            rendered_video (str): Path to the silent video file.
            audios (List[Tuple[Audio, float]]): List of Audio objects with associated volume adjustments.
            output_video (str): Path to save the final video with audio.

        Raises:
            ValueError: If FFmpeg fails to combine the audio tracks.
        """
        audio_files = []
        for i, (audio, volume) in enumerate(audios):
            audio_file = f"temp_audio_{i}.aac"
            extract_command = [
                "ffmpeg", "-y",
                "-i", audio.file_path(),
                "-af", f"volume={volume}",
                "-vn",
                "-acodec", "aac",
                audio_file
            ]
            subprocess.run(extract_command, check=True)
            audio_files.append(audio_file)

        input_args = ["-i", rendered_video]
        filter_complex = ""
        for i, audio_file in enumerate(audio_files):
            input_args.extend(["-i", audio_file])
            filter_complex += f"[{i+1}:a]"

        if not audio_files:
            raise ValueError("No audio streams found to mix.")

        filter_complex += f"amix=inputs={len(audio_files)}:duration=shortest[aout]"

        combine_command = [
            "ffmpeg", "-y", *input_args,
            "-filter_complex", filter_complex,
            "-map", "0:v",
            "-map", "[aout]",
            "-c:v", "copy",
            "-c:a", "aac",
            output_video
        ]

        try:
            print("Executing FFmpeg command:", " ".join(combine_command))
            subprocess.run(combine_command, check=True)
        except subprocess.CalledProcessError as e:
            raise ValueError("FFmpeg failed to combine audio tracks.") from e
        finally:
            for audio_file in audio_files:
                if os.path.exists(audio_file):
                    os.remove(audio_file)

    def __get_unordered_frame_idx(self, target: int) -> int:
        """Find the corresponding unordered frame index for a given logical frame index.

        Parameters:
            target (int): Logical frame index.

        Returns:
            int: Index in the unordered video file or -1 if not found.
        """
        for i, value in enumerate(reversed(self.__frame_indices)):
            if value == target:
                return len(self.__frame_indices) - 1 - i
        return -1

class Bot:
    """Chatbot that can generate text responses, synthesize speech, and transcribe audio."""

    def __init__(self, personality: str = "You are a helpful chatbot.", unique_key: Optional[str] = None, voice: str = "onyx") -> None:
        """Initialize a Bot instance.

        Parameters:
            personality (str): The system prompt defining the chatbot's behavior.
            unique_key (Optional[str]): An optional API key to override the default.
            voice (str): Voice model name used for text-to-speech synthesis.
        """
        self._personality = personality
        self._voice = voice
        self._api_key = unique_key if unique_key else api_key
        self._client = OpenAI(api_key=self._api_key)

    def set_personality(self, personality: str) -> None:
        """Update the chatbot's system prompt.

        Parameters:
            personality (str): The new system prompt to use.
        """
        self._personality = personality

    def prompt(self, input_text: str) -> str:
        """Generate a chatbot response for a given user input.

        Parameters:
            input_text (str): The user's input message.

        Returns:
            str: The chatbot's generated response.
        """
        response = self._client.chat.completions.create(
            model="gpt-4o",
            messages=[
                {"role": "system", "content": self._personality},
                {"role": "user", "content": input_text}
            ]
        )
        return response.choices[0].message.content

    def transcribe(self, audio: 'Audio') -> Tuple[List[str], List[Tuple[float, float]]]:
        """Transcribe spoken audio into text with word-level timestamps.

        Parameters:
            audio (Audio): The audio object to transcribe.

        Returns:
            Tuple[List[str], List[Tuple[float, float]]]: A list of words and their corresponding (start, end) timestamps.
        """
        with open(audio.file_path(), "rb") as audio_file:
            transcription = self._client.audio.transcriptions.create(
                file=audio_file,
                model="whisper-1",
                response_format="verbose_json",
                timestamp_granularities=["word"]
            )
        words = [word_obj.word for word_obj in transcription.words]
        timestamps = [(word_obj.start, word_obj.end) for word_obj in transcription.words]
        timestamps = self._fix_timestamps(words, timestamps)
        return words, timestamps

    def speak(self, text: str, speed: float = 1.0) -> 'Audio':
        """Synthesize speech from text and optionally adjust its playback speed.

        Parameters:
            text (str): The text to convert into speech.
            speed (float): A multiplier to adjust the playback speed (default is 1.0).

        Returns:
            Audio: An Audio object containing the synthesized speech.
        """
        global audio_counter
        filename = generate_random_filename(seed=audio_counter)
        audio_counter += 1

        os.makedirs("AudioCache", exist_ok=True)
        audio_path = f"AudioCache/{filename}.wav"
        speech_file_path = Path(audio_path)

        response = self._client.audio.speech.create(
            model="tts-1",
            voice=self._voice,
            input=text,
        )
        response.stream_to_file(speech_file_path)

        if speed != 1.0:
            y, sr = librosa.load(audio_path, sr=None)
            new_sr = int(sr * speed)
            sf.write(audio_path, y, new_sr)
            faster_audio, _ = librosa.load(audio_path, sr=sr)
            sf.write(audio_path, faster_audio, sr)

        return Audio(audio_path)

    def _count_sounds(self, word: str) -> int:
        """Estimate the number of sound units in a word.

        Parameters:
            word (str): The word to analyze.

        Returns:
            int: The estimated number of sound units.
        """
        special_combinations = [
            "ch", "tch", "sh", "oo", "ld", "ss", "qu", "th", "ph", "ng",
            "gh", "wh", "kn", "wr", "gn", "sc", "sk", "st", "sp", "spl",
            "spr", "shr", "scr", "str", "dr", "tr", "bl", "cl", "fl", "gl",
            "pl", "sl", "br", "cr", "fr", "gr", "pr", "tr", "ou", "ght"
        ]
        for combo in special_combinations:
            word = word.replace(combo, "$")
        return len(word)

    def _calculate_word_timestamps(self, words: List[str], total_duration: float, first_timestamp: float) -> List[Tuple[float, float]]:
        """Calculate estimated timestamps for each word based on duration.

        Parameters:
            words (List[str]): The words to timestamp.
            total_duration (float): The total audio duration.
            first_timestamp (float): The timestamp for the first word.

        Returns:
            List[Tuple[float, float]]: List of (start, end) timestamps per word.
        """
        total_sounds = sum(
            self._count_sounds(re.sub(r"[.,!?]", "", token)) + 2 + (5 if re.search(r"[.,!?]", token) else 0)
            for token in words
        )
        seconds_per_sound = total_duration / total_sounds
        timestamps = []
        current_time = first_timestamp

        for token in words:
            sounds = self._count_sounds(re.sub(r"[.,!?]", "", token)) + 2 + (5 if re.search(r"[.,!?]", token) else 0)
            word_duration = sounds * seconds_per_sound
            timestamps.append((current_time, current_time + word_duration))
            current_time += word_duration

        return timestamps

    def _fix_timestamps(self, words: List[str], timestamps: List[Tuple[float, float]]) -> List[Tuple[float, float]]:
        """Resolve overlapping word timestamps by recalculating them locally.

        Parameters:
            words (List[str]): The list of words.
            timestamps (List[Tuple[float, float]]): The original timestamps.

        Returns:
            List[Tuple[float, float]]: The corrected timestamps.
        """
        i = 0
        while i < len(timestamps):
            j = i + 1
            while j < len(timestamps) and timestamps[j][0] == timestamps[i][0]:
                j += 1
            if j > i + 1:
                start_idx = max(0, i - 1)
                end_idx = j
                selected_words = words[start_idx:end_idx]
                starting_time = timestamps[start_idx][0]
                total_duration = timestamps[end_idx - 1][1] - starting_time if end_idx < len(timestamps) else 0
                corrected = self._calculate_word_timestamps(selected_words, total_duration, starting_time)
                timestamps[start_idx:end_idx] = corrected
                i = j
            else:
                i += 1
        return timestamps

class Audio_Analyzer:
    """A suite of tools for advanced audio frequency and transient analysis."""

    @staticmethod
    def get_range_mag(freqs: rnp.ndarray, mags: rnp.ndarray, start_hz: float, end_hz: float) -> float:
        """
        Calculates the energy of a specific frequency band relative to the 
        total energy of the entire audio frame.
        Returns a value from 0.0 (silent) to 1.0 (all audio energy is in this band).
        """
        # Guard against completely silent frames to prevent divide-by-zero
        total_mag = float(rnp.sum(mags))
        if total_mag <= 0.0001:
            return 0.0

        bin_width = freqs[1] - freqs[0]
        
        # Convert Hz boundaries to array indices safely
        start_idx = max(0, int(start_hz / bin_width))
        end_idx = min(int(end_hz / bin_width), len(freqs))
        
        # Guard against reversed or invalid ranges
        if start_idx >= end_idx:
            return 0.0
            
        segment_mags = mags[start_idx:end_idx]
        
        # Sum the energy in our target band
        segment_mag_sum = float(rnp.sum(segment_mags))
        
        # Return the ratio (0.0 to 1.0)
        return min(1.0, segment_mag_sum / total_mag)
        
    @staticmethod
    def calculate_bloom(freqs: rnp.ndarray, mags: rnp.ndarray, peak_global_idx: int, window_hz: float = 40.0) -> float:
        """
        Calculates the transient splash relative to the peak magnitude.
        Returns a value from 0.0 (pure tone) to 1.0 (maximum noisy splash).
        """
        peak_freq = freqs[peak_global_idx]
        peak_mag = mags[peak_global_idx]
        
        # Silence guard: If the main note is basically dead, there is no bloom.
        if peak_mag <= 0.0001:
            return 0.0

        bin_width = freqs[1] - freqs[0]
        bin_range = int(window_hz / bin_width)
        
        start_i = max(0, peak_global_idx - bin_range)
        end_i = min(len(freqs), peak_global_idx + bin_range + 1)
        
        neighborhood_freqs = freqs[start_i:end_i]
        neighborhood_mags = mags[start_i:end_i]
        
        weights = 1.0 - (rnp.abs(neighborhood_freqs - peak_freq) / window_hz)
        weights = rnp.clip(weights, 0.0, 1.0)
        
        local_peak_idx = peak_global_idx - start_i
        weights[local_peak_idx] = 0.0
        
        # 1. Calculate actual splash
        raw_bloom = float(rnp.sum(neighborhood_mags * weights))
        
        # 2. Calculate maximum possible splash (if all neighbors were as loud as the peak)
        max_possible_bloom = float(peak_mag * rnp.sum(weights))
        
        if max_possible_bloom <= 0:
            return 0.0
            
        # 3. Return the ratio
        return min(1.0, raw_bloom / max_possible_bloom)
        
    @staticmethod
    def get_peak_data(freqs: rnp.ndarray, mags: rnp.ndarray, start_hz: float, end_hz: float) -> tuple[float, int]:
        """
        Finds the loudest frequency within a range.
        Returns a tuple containing:
        1. peak_norm: The normalized position of the peak (0.0 to 1.0) between start_hz and end_hz.
        2. global_peak_idx: The exact array index of the peak (useful for the bloom calculator).
        """
        bin_width = freqs[1] - freqs[0]
        
        # Convert Hz to array indices
        start_idx = max(0, int(start_hz / bin_width))
        end_idx = min(int(end_hz / bin_width), len(freqs))

        segment_mags = mags[start_idx:end_idx]
        
        # Guard against silent or empty segments
        if len(segment_mags) == 0 or rnp.max(segment_mags) <= 0.0001:
            return 0.0, 0
            
        # Find the peak within the slice, then calculate its global position
        max_idx = int(segment_mags.argmax())
        global_peak_idx = start_idx + max_idx
        peak_freq = freqs[global_peak_idx]
        
        # Normalize the peak's position (0.0 is start_hz, 1.0 is end_hz)
        # Note: Your original code used 350 here. This uses the true range (end_hz - start_hz).
        normalization_range = end_hz - start_hz
        peak_norm = float(rnp.clip((peak_freq - start_hz) / normalization_range, 0.0, 1.0))
        
        return peak_norm, global_peak_idx
    
    @staticmethod
    def get_mean_frequency(freqs: rnp.ndarray, mags: rnp.ndarray, start_hz: float, end_hz: float) -> tuple[float, float]:
        """
        Calculates the mean frequency (spectral centroid) within a specific range.
        Returns a tuple containing:
        1. mean_norm: The normalized position of the mean frequency (0.0 to 1.0) between start_hz and end_hz.
        2. exact_freq: The exact calculated mean frequency in Hz.
        """
        bin_width = freqs[1] - freqs[0]
        
        # Convert Hz to array indices safely
        start_idx = max(0, int(start_hz / bin_width))
        end_idx = min(int(end_hz / bin_width), len(freqs))

        segment_freqs = freqs[start_idx:end_idx]
        segment_mags = mags[start_idx:end_idx]
        
        # Guard against silent or empty segments to prevent division by zero
        total_mag = float(rnp.sum(segment_mags))
        if len(segment_mags) == 0 or total_mag <= 0.0001:
            return 0.0, float(start_hz)
            
        # Calculate the spectral centroid (weighted average of frequencies)
        # Formula: Sum(Frequency * Magnitude) / Sum(Magnitude)
        mean_freq = float(rnp.sum(segment_freqs * segment_mags) / total_mag)
        
        # Normalize the mean frequency's position (0.0 is start_hz, 1.0 is end_hz)
        normalization_range = end_hz - start_hz
        if normalization_range <= 0:
             return 0.0, float(start_hz)
             
        mean_norm = float(rnp.clip((mean_freq - start_hz) / normalization_range, 0.0, 1.0))
        
        return mean_norm, mean_freq
        
    @staticmethod
    def draw_visualizer(frame, frame_audio, x_range: tuple = (0.0, 0.5), y_range: tuple = (0.5, 1.0), freq_range: tuple = None, color: tuple = (255, 0, 0)):
        """
        Draws an audio visualizer graph directly onto the provided frame.
        x_range and y_range map the placement via normalized coordinates (0.0 to 1.0).
        """
        # Safely extract the dimensions of the current frame (works for both CPU and GPU arrays)
        pixels = frame.get_pixels(standard_size=True)
        h, w = pixels.shape[:2]

        # Calculate exact pixel sizes and positions based on the float ranges
        vis_width = w * (x_range[1] - x_range[0])
        vis_height = h * (y_range[1] - y_range[0])
        vis_x = w * x_range[0]
        vis_y = h * y_range[0]

        # Initialize the visualizer field with or without specific frequency bounds
        if freq_range is not None:
            audio_vis_field = FAudio(frame_audio, start=freq_range[0], end=freq_range[1])
        else:
            audio_vis_field = FAudio(frame_audio)

        # 1. Scale the graph down to the target box size
        audio_vis_field.scale(vis_width / w, vis_height / h)
        
        # 2. Expand its virtual bounding box back to full screen so it doesn't clip
        audio_vis_field.resize(w, h)
        
        # 3. Move it into the exact calculated screen coordinates
        audio_vis_field.move(int(vis_x), int(vis_y))

        # Unpack the color tuple into the filter and apply it to the frame
        frame.apply_filter(Solid_Color(color[0], color[1], color[2]).set_field(audio_vis_field))

        return frame

class Color_Picker:
    @staticmethod
    def average_color(frame: Frame, points: list[tuple[int, int]]) -> tuple[int, int, int]:
        """Calculate the average color within specified (x, y) points in the frame.

        Parameters:
            frame (Frame): The Frame object to sample colors from.
            points (List[Tuple[int, int]]): A list of (x, y) tuples representing pixel coordinates.

        Returns:
            Tuple[int, int, int]: The average color as an (R, G, B) tuple.

        Raises:
            ValueError: If points list is empty or contains out-of-bounds coordinates.
        """
        if not points:
            raise ValueError("Points list cannot be empty.")

        pixels = frame.get_pixels(standard_size=True)
        height, width = pixels.shape[:2]

        sum_r = 0
        sum_g = 0
        sum_b = 0
        count = 0

        for x, y in points:
            if 0 <= x < width and 0 <= y < height:
                b, g, r = pixels[y, x]
                sum_r += int(r)
                sum_g += int(g)
                sum_b += int(b)
                count += 1
            else:
                raise ValueError(f"Point {(x, y)} is out of frame bounds ({width}, {height}).")

        if count == 0:
            raise ValueError("No valid points found within frame bounds.")

        avg_r = sum_r // count
        avg_g = sum_g // count
        avg_b = sum_b // count

        return (avg_r, avg_g, avg_b)

class Media_Optimizer:
    @staticmethod
    def downscale_image(input_path: str, output_path: str, scale: float) -> bool:
        """
        Reads an image, downscales it by a float multiplier, and saves it.
        
        :param input_path: Path to the original image.
        :param output_path: Path to save the downscaled image.
        :param scale: Float multiplier (e.g., 0.5 for half size, 0.25 for quarter size).
        :return: True if successful, False if the image couldn't be read.
        """
        # 1. Load the original image
        img = cv2.imread(input_path)
        
        if img is None:
            print(f"Error: Could not read image at {input_path}")
            return False
            
        # 2. Calculate the exact new pixel dimensions
        new_width = int(img.shape[1] * scale)
        new_height = int(img.shape[0] * scale)
        
        # 3. Resize using INTER_AREA (best for crushing pixels down)
        resized_img = cv2.resize(img, (new_width, new_height), interpolation=cv2.INTER_AREA)
        
        # 4. Save to the new path
        cv2.imwrite(output_path, resized_img)
        return True

def read_font_from_qt_resource(resource_path):
    file = QFile(resource_path)
    if not file.open(QFile.ReadOnly):
        raise FileNotFoundError(f"Cannot open resource {resource_path}")

    # Write to a temporary file if needed
    temp_path = "/tmp/arial.ttf"  # Adjust for your OS
    with open(temp_path, "wb") as temp_file:
        temp_file.write(file.readAll())

    return temp_path

def generate_random_filename(length: int = 10, seed: int = None) -> str:
    """
    Generate a random string that can be safely used as a filename.
    :param length: Length of the random string (default is 10).
    :param seed: Seed value for reproducibility (default is None).
    :return: A randomly generated filename-safe string.
    """
    if seed is not None:
        random.seed(seed)

    characters = string.ascii_letters + string.digits
    return ''.join(random.choices(characters, k=length))




media = {}
renderer = NonlinearRenderer(640, 480, 24)

#replace 'video' with renderer when getting video.height and such

if _paths:
    for path in _paths:
        if path:
            media_full_name = os.path.splitext(os.path.basename(path))
            media_name = media_full_name[0]
            media_type = media_full_name[1].lower()

            if media_type == ".mp4":
                media[media_name] = Video(path)
            elif media_type == ".mp3":
                media[media_name] = Audio(path)
            elif media_type in [".jpg", ".jpeg", ".png"]:
                media[media_name] = ImageFile(path)

    for media_name in media:
        current_media = media[media_name]
        if isinstance(current_media, Video) and current_media.audio != None:
            try:
                current_media.audio.preload_data()
            except:
                print("Failed to preload audio data for " + media_name)
        elif isinstance(current_media, Audio):
            try:
                current_media.preload_data()
            except:
                print("Failed to preload audio data for " + media_name)

if len(_paths) != 0:
    class Field:
        """A vector-based field.

        A Field tracks only the *shape(s)* that define it (polygons / contours)
        rather than a full-canvas bitmap. There is no background: the geometry is
        stored resolution-independently and rasterized on demand by ``get_map``.

        Each field also carries reversible, render-time effects:
            * ``blur_amount``    - softens the whole mask (Gaussian).
            * ``feather_amount`` - fades the shape edges inward.
            * ``inverted``       - flips the mask (everything except the shape).
        These are applied only while rasterizing, so the underlying geometry is
        never destroyed and the effects can be undone (set the amount back to 0).
        """

        def __init_subclass__(cls, **kwargs):
            super().__init_subclass__(**kwargs)
            original_init = cls.__init__
            def profiled_init(self, *args, **kw):
                with _profile(f"construct_field:{cls.__name__}"):
                    original_init(self, *args, **kw)
            cls.__init__ = profiled_init

        def __init__(self) -> None:
            """Initialize an empty Field (no shapes, no background)."""
            self.shapes = []                       # list of shape dicts (vector or raster)
            self.blur_amount = 0.0                 # reversible Gaussian blur (px @ ref res)
            self.feather_amount = 0.0              # reversible edge feather (px @ ref res)
            self.inverted = False                  # invert the rasterized mask
            self._ref_w = int(renderer.width())    # resolution shapes were authored at
            self._ref_h = int(renderer.height())
            self._cache = {}                       # (h, w) -> cached CPU float32 mask
            self._device_cache = {}                # (h, w) -> cached backend (GPU/CPU) map

        # ------------------------------------------------------------------ #
        # Shape construction helpers (used by subclasses)
        # ------------------------------------------------------------------ #
        def _add_poly(self, contours, holes=None, value: float = 1.0, additive: bool = True) -> 'Field':
            """Append a filled polygon shape (with optional holes) to this field."""
            c = [rnp.asarray(x, dtype=rnp.float32).reshape(-1, 2) for x in contours]
            hl = [rnp.asarray(x, dtype=rnp.float32).reshape(-1, 2) for x in (holes or [])]
            self.shapes.append({
                'type': 'poly', 'contours': c, 'holes': hl,
                'value': float(value), 'additive': bool(additive),
            })
            return self

        def _add_contours_from_mask(self, mask, value: float = 1.0, additive: bool = True) -> 'Field':
            """Vectorize a binary/grayscale bitmap into polygon contours (with holes).

            Used for shapes that are easiest to draw first (e.g. text) and then
            convert to a resolution-independent vector form.
            """
            m = mask.get() if hasattr(mask, 'get') else rnp.asarray(mask)
            if m.dtype == rnp.uint8:
                binary = (m > 127).astype(rnp.uint8)
            else:
                binary = (m > 0.5).astype(rnp.uint8)
            found = cv2.findContours(binary, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_SIMPLE)
            contours = found[-2]
            hierarchy = found[-1]
            outers, holes = [], []
            if hierarchy is not None and len(contours):
                hierarchy = hierarchy[0]
                for i, cnt in enumerate(contours):
                    pts = cnt.reshape(-1, 2).astype(rnp.float32)
                    if len(pts) < 3:
                        continue
                    if hierarchy[i][3] == -1:
                        outers.append(pts)
                    else:
                        holes.append(pts)
            if outers or holes:
                self.shapes.append({
                    'type': 'poly', 'contours': outers, 'holes': holes,
                    'value': float(value), 'additive': bool(additive),
                })
            return self

        def _fullcanvas_shape(self, value: float, additive: bool = True) -> dict:
            """A shape covering the whole reference canvas (used by FOverlay / scalars)."""
            w, h = self._ref_w, self._ref_h
            rect = rnp.array([[0, 0], [w, 0], [w, h], [0, h]], dtype=rnp.float32)
            return {'type': 'poly', 'contours': [rect], 'holes': [],
                    'value': float(value), 'additive': bool(additive)}

        def _raster_shape(self, mask, additive: bool = True) -> dict:
            """Wrap a precomputed float mask as a (resolution-locked) raster shape."""
            m = mask.get() if hasattr(mask, 'get') else rnp.asarray(mask)
            m = m.astype(rnp.float32)
            if m.shape[0] != self._ref_h or m.shape[1] != self._ref_w:
                m = cv2.resize(m, (self._ref_w, self._ref_h), interpolation=cv2.INTER_LINEAR)
            return {'type': 'raster', 'mask': m, 'value': 1.0, 'additive': bool(additive)}

        # ------------------------------------------------------------------ #
        # Cloning
        # ------------------------------------------------------------------ #
        @staticmethod
        def _copy_shape(s: dict) -> dict:
            ns = dict(s)
            if s.get('type') == 'raster':
                ns['mask'] = s['mask'].copy()
            else:
                ns['contours'] = [c.copy() for c in s.get('contours', [])]
                ns['holes'] = [c.copy() for c in s.get('holes', [])]
            return ns

        def _clone(self) -> 'Field':
            """Fast, independent copy (deepcopy is far slower for our small state)."""
            clone = self.__class__.__new__(self.__class__)
            clone.shapes = [self._copy_shape(s) for s in self.shapes]
            clone.blur_amount = self.blur_amount
            clone.feather_amount = self.feather_amount
            clone.inverted = self.inverted
            clone._ref_w = self._ref_w
            clone._ref_h = self._ref_h
            clone._cache = {}
            clone._device_cache = {}
            return clone

        def _bump(self) -> None:
            """Invalidate cached rasterizations after any mutation to shape or effects."""
            if self._cache:
                self._cache.clear()
            if self._device_cache:
                self._device_cache.clear()

        @staticmethod
        def _scalar_value(other):
            """Return a float if ``other`` is a scalar, else None."""
            if isinstance(other, bool):
                return None
            if isinstance(other, (int, float)):
                return float(other)
            if hasattr(other, 'ndim') and getattr(other, 'ndim', None) == 0:
                try:
                    return float(other)
                except Exception:
                    return None
            if rnp.isscalar(other):
                try:
                    return float(other)
                except Exception:
                    return None
            return None

        def _is_pure(self) -> bool:
            """True if no render-time effects are baked in (safe to union by reference)."""
            return (not self.inverted) and (not self.blur_amount) and (not self.feather_amount)

        # ------------------------------------------------------------------ #
        # Rasterization (the only place a bitmap is produced)
        # ------------------------------------------------------------------ #
        @staticmethod
        def _scaled_int(contour, sx, sy):
            pts = rnp.asarray(contour, dtype=rnp.float32).reshape(-1, 2)
            pts = pts * rnp.array([sx, sy], dtype=rnp.float32)
            return rnp.round(pts).astype(rnp.int32)

        @staticmethod
        def _feather_mask(mask, amount):
            a = float(amount)
            if a <= 0:
                return mask
            binary = (mask > 0.004).astype(rnp.uint8)
            if int(binary.max()) == 0:
                return mask
            dist = cv2.distanceTransform(binary, cv2.DIST_L2, 3)
            ramp = rnp.clip(dist / a, 0.0, 1.0).astype(rnp.float32)
            return (mask * ramp).astype(rnp.float32)

        @staticmethod
        def _blur_mask(mask, amount):
            k = int(round(float(amount)))
            if k <= 1:
                return mask
            if k % 2 == 0:
                k += 1
            return cv2.GaussianBlur(mask.astype(rnp.float32), (k, k), 0)

        def _rasterize(self, height: int, width: int):
            """Render the field's shapes to a CPU float32 mask in [0, 1]."""
            height = int(height)
            width = int(width)

            # Legacy fallback: support custom fields that still assign a raw self._map.
            if not self.shapes:
                legacy = getattr(self, '_map', None)
                if legacy is not None:
                    m = legacy.get() if hasattr(legacy, 'get') else rnp.asarray(legacy)
                    m = m.astype(rnp.float32)
                    if m.ndim == 3:
                        m = m[..., 0]
                    if float(m.max() if m.size else 0) > 1.0001:
                        m = m / 255.0
                    if m.shape[0] != height or m.shape[1] != width:
                        m = cv2.resize(m, (width, height), interpolation=cv2.INTER_LINEAR)
                    if self.inverted:
                        m = 1.0 - m
                    return rnp.clip(m, 0.0, 1.0).astype(rnp.float32)

            key = (height, width)
            cached = self._cache.get(key)
            if cached is not None:
                return cached

            ref_w = self._ref_w if self._ref_w else width
            ref_h = self._ref_h if self._ref_h else height
            sx = width / float(ref_w)
            sy = height / float(ref_h)

            acc = rnp.zeros((height, width), dtype=rnp.float32)
            for s in self.shapes:
                if s.get('type') == 'raster':
                    m = s['mask']
                    m = m.get() if hasattr(m, 'get') else rnp.asarray(m)
                    if m.shape[0] != height or m.shape[1] != width:
                        m = cv2.resize(m.astype(rnp.float32), (width, height), interpolation=cv2.INTER_LINEAR)
                    layer = m.astype(rnp.float32)
                else:
                    layer8 = rnp.zeros((height, width), dtype=rnp.uint8)
                    cnts = [self._scaled_int(c, sx, sy) for c in s.get('contours', [])]
                    cnts = [c for c in cnts if len(c) >= 3]
                    if cnts:
                        cv2.fillPoly(layer8, cnts, 255)
                    holes = [self._scaled_int(c, sx, sy) for c in s.get('holes', [])]
                    holes = [c for c in holes if len(c) >= 3]
                    if holes:
                        cv2.fillPoly(layer8, holes, 0)
                    layer = layer8.astype(rnp.float32) / 255.0

                v = float(s.get('value', 1.0))
                if v != 1.0:
                    layer = layer * v
                if s.get('additive', True):
                    acc += layer
                else:
                    acc -= layer

            rnp.clip(acc, 0.0, 1.0, out=acc)

            if self.feather_amount and self.feather_amount > 0:
                acc = self._feather_mask(acc, self.feather_amount * sx)
            if self.blur_amount and self.blur_amount > 0:
                acc = self._blur_mask(acc, self.blur_amount * sx)
            if self.inverted:
                acc = 1.0 - acc

            if len(self._cache) > 8:
                self._cache.clear()
            self._cache[key] = acc
            return acc

        # ------------------------------------------------------------------ #
        # Combination operations
        # ------------------------------------------------------------------ #
        def _bake_self(self) -> 'Field':
            """Flatten this field's shapes + effects into a single raster shape.

            After this the field's geometry and its (formerly reversible) effects
            are fused into one mask, so anything composited on top is unaffected
            by those effects.
            """
            base = self._rasterize(self._ref_h, self._ref_w)
            self.shapes = [{'type': 'raster', 'mask': base.astype(rnp.float32),
                            'value': 1.0, 'additive': True}]
            self.blur_amount = 0.0
            self.feather_amount = 0.0
            self.inverted = False
            self._bump()
            return self

        def _absorb(self, other, additive: bool) -> 'Field':
            """Combine ``other`` into this field (union if additive, else difference).

            Fields carry their own (reversible) blur/feather/invert effects, so two
            fields with different looks can't simply share raw geometry. When this
            field already has effects baked in, it is flattened first and the
            other's *rendered* mask is composited on top — matching the old bitmap
            behaviour where each mask kept its own appearance.
            """
            sv = self._scalar_value(other)
            if sv is not None:
                if not self._is_pure():
                    self._bake_self()
                self.shapes.append(self._fullcanvas_shape(sv, additive))
                return self

            if isinstance(other, Field):
                if additive and self._is_pure() and other._is_pure():
                    # Pure geometry on both sides: union losslessly by copying shapes.
                    for s in other.shapes:
                        ns = self._copy_shape(s)
                        self._rescale_shape(ns, other._ref_w, other._ref_h, self._ref_w, self._ref_h)
                        self.shapes.append(ns)
                else:
                    # Effects present (or subtraction): composite rendered masks so
                    # each field keeps its own look.
                    if not self._is_pure():
                        self._bake_self()
                    m = other.get_map(self._ref_h, self._ref_w)
                    self.shapes.append(self._raster_shape(m, additive))
                return self

            # Raw array map (e.g. another field's get_map()).
            if hasattr(other, 'shape'):
                if not self._is_pure():
                    self._bake_self()
                m = other.get() if hasattr(other, 'get') else rnp.asarray(other)
                m = m.astype(rnp.float32)
                if m.ndim == 3:
                    m = m[..., 0]
                if float(m.max() if m.size else 0) > 1.0001:
                    m = m / 255.0
                self.shapes.append(self._raster_shape(m, additive))
                return self

            raise TypeError(f"Cannot combine Field with {type(other)!r}.")

        @staticmethod
        def _rescale_shape(s: dict, from_w, from_h, to_w, to_h) -> dict:
            if from_w == to_w and from_h == to_h:
                return s
            if s.get('type') == 'raster':
                s['mask'] = cv2.resize(s['mask'], (int(to_w), int(to_h)), interpolation=cv2.INTER_LINEAR)
            else:
                f = rnp.array([to_w / float(from_w), to_h / float(from_h)], dtype=rnp.float32)
                s['contours'] = [c * f for c in s['contours']]
                s['holes'] = [c * f for c in s['holes']]
            return s

        def _bake_combine(self, other, divide: bool) -> 'Field':
            """Multiply/divide by another field or map by baking to a raster shape."""
            base = self._rasterize(self._ref_h, self._ref_w)
            if isinstance(other, Field):
                om = other.get_map(self._ref_h, self._ref_w)
            else:
                om = other
            om = om.get() if hasattr(om, 'get') else rnp.asarray(om)
            om = om.astype(rnp.float32)
            if om.ndim == 3:
                om = om[..., 0]
            if float(om.max() if om.size else 0) > 1.0001:
                om = om / 255.0
            if om.shape[0] != self._ref_h or om.shape[1] != self._ref_w:
                om = cv2.resize(om, (self._ref_w, self._ref_h), interpolation=cv2.INTER_LINEAR)
            if divide:
                om = rnp.where(om <= 1e-6, 1e-6, om)
                res = rnp.clip(base / om, 0.0, 1.0)
            else:
                res = rnp.clip(base * om, 0.0, 1.0)
            # Effects are now baked into the raster; reset them.
            self.shapes = [{'type': 'raster', 'mask': res.astype(rnp.float32),
                            'value': 1.0, 'additive': True}]
            self.blur_amount = 0.0
            self.feather_amount = 0.0
            self.inverted = False
            self._bump()
            return self

        def add(self, other) -> 'Field':
            """Add another Field, a raw map, or a scalar to this Field (in place)."""
            self._absorb(other, True)
            self._bump()
            return self

        def sub(self, other) -> 'Field':
            """Subtract another Field, a raw map, or a scalar from this Field (in place)."""
            self._absorb(other, False)
            self._bump()
            return self

        def mult(self, other) -> 'Field':
            """Multiply this Field by a scalar (scales intensity) or another Field (in place)."""
            sv = self._scalar_value(other)
            if sv is not None:
                for s in self.shapes:
                    s['value'] = s.get('value', 1.0) * sv
                self._bump()
                return self
            return self._bake_combine(other, divide=False)

        def div(self, other) -> 'Field':
            """Divide this Field by a scalar or another Field (in place)."""
            sv = self._scalar_value(other)
            if sv is not None:
                if sv == 0:
                    raise ValueError("Cannot divide a Field by zero.")
                for s in self.shapes:
                    s['value'] = s.get('value', 1.0) / sv
                self._bump()
                return self
            return self._bake_combine(other, divide=True)

        def __add__(self, other) -> 'Field':
            with _profile("field.__add__"):
                return self._clone().add(other)

        def __sub__(self, other) -> 'Field':
            return self._clone().sub(other)

        def __mul__(self, other) -> 'Field':
            return self._clone().mult(other)

        def __truediv__(self, other) -> 'Field':
            return self._clone().div(other)

        # ------------------------------------------------------------------ #
        # Geometry transforms (lossless – operate on the vector shapes)
        # ------------------------------------------------------------------ #
        def _content_points(self):
            pts = []
            for s in self.shapes:
                if s.get('type') == 'poly':
                    for c in s['contours']:
                        pts.append(rnp.asarray(c, dtype=rnp.float32).reshape(-1, 2))
                    for c in s['holes']:
                        pts.append(rnp.asarray(c, dtype=rnp.float32).reshape(-1, 2))
            if pts:
                return rnp.concatenate(pts, axis=0)
            return None

        def _content_bbox(self):
            pts = self._content_points()
            if pts is not None and len(pts):
                x0, y0 = pts.min(axis=0)
                x1, y1 = pts.max(axis=0)
                return float(x0), float(y0), float(x1), float(y1)
            for s in self.shapes:
                if s.get('type') == 'raster':
                    ys, xs = rnp.where(s['mask'] > 0.004)
                    if len(xs):
                        return float(xs.min()), float(ys.min()), float(xs.max()), float(ys.max())
            return None

        def _content_center(self):
            bbox = self._content_bbox()
            if bbox is None:
                return self._ref_w / 2.0, self._ref_h / 2.0
            x0, y0, x1, y1 = bbox
            return (x0 + x1) / 2.0, (y0 + y1) / 2.0

        def set_position(self, position: tuple) -> 'Field':
            """Move the field so its center sits at the given point.

            Parameters:
                position (tuple) @position: (x, y) pixel coordinates for the field's center.
            """
            cx, cy = self._content_center()
            x, y = float(position[0]), float(position[1])
            dx, dy = x - cx, y - cy
            off = rnp.array([dx, dy], dtype=rnp.float32)
            for s in self.shapes:
                if s.get('type') == 'raster':
                    M = rnp.float32([[1, 0, dx], [0, 1, dy]])
                    s['mask'] = cv2.warpAffine(s['mask'], M, (self._ref_w, self._ref_h),
                                               borderMode=cv2.BORDER_CONSTANT, borderValue=0)
                else:
                    s['contours'] = [c + off for c in s['contours']]
                    s['holes'] = [c + off for c in s['holes']]
            self._bump()
            return self

        def resize(self, scale: float) -> 'Field':
            """Scale the field by ``scale``, anchored at its own center.

            Parameters:
                scale (float): Scale factor (>1 grows, <1 shrinks). The shape's
                    center stays put while it grows/shrinks around it.
            """
            scale = float(scale)
            if scale <= 0:
                raise ValueError("Resize scale must be positive.")
            cx, cy = self._content_center()
            center = rnp.array([cx, cy], dtype=rnp.float32)
            for s in self.shapes:
                if s.get('type') == 'raster':
                    M = cv2.getRotationMatrix2D((float(cx), float(cy)), 0.0, scale)
                    s['mask'] = cv2.warpAffine(s['mask'], M, (self._ref_w, self._ref_h),
                                               borderMode=cv2.BORDER_CONSTANT, borderValue=0)
                else:
                    s['contours'] = [((c - center) * scale + center) for c in s['contours']]
                    s['holes'] = [((c - center) * scale + center) for c in s['holes']]
            self._bump()
            return self

        def fit(self) -> 'Field':
            """Stretch the field's bounding box to fill the whole canvas."""
            bbox = self._content_bbox()
            if bbox is None:
                return self
            x0, y0, x1, y1 = bbox
            bw = max(x1 - x0, 1e-6)
            bh = max(y1 - y0, 1e-6)
            sx = self._ref_w / bw
            sy = self._ref_h / bh
            origin = rnp.array([x0, y0], dtype=rnp.float32)
            factor = rnp.array([sx, sy], dtype=rnp.float32)
            for s in self.shapes:
                if s.get('type') == 'raster':
                    M = rnp.float32([[sx, 0, -x0 * sx], [0, sy, -y0 * sy]])
                    s['mask'] = cv2.warpAffine(s['mask'], M, (self._ref_w, self._ref_h))
                else:
                    s['contours'] = [((c - origin) * factor) for c in s['contours']]
                    s['holes'] = [((c - origin) * factor) for c in s['holes']]
            self._bump()
            return self

        def mirror_x(self) -> 'Field':
            """Mirror the field along the vertical (X) axis."""
            w = self._ref_w
            for s in self.shapes:
                if s.get('type') == 'raster':
                    s['mask'] = cv2.flip(s['mask'], 1)
                else:
                    for c in s['contours']:
                        c[:, 0] = w - c[:, 0]
                    for c in s['holes']:
                        c[:, 0] = w - c[:, 0]
            self._bump()
            return self

        def mirror_y(self) -> 'Field':
            """Mirror the field along the horizontal (Y) axis."""
            h = self._ref_h
            for s in self.shapes:
                if s.get('type') == 'raster':
                    s['mask'] = cv2.flip(s['mask'], 0)
                else:
                    for c in s['contours']:
                        c[:, 1] = h - c[:, 1]
                    for c in s['holes']:
                        c[:, 1] = h - c[:, 1]
            self._bump()
            return self

        def invert(self) -> 'Field':
            """Invert the field (everything except the shape). Reversible."""
            self.inverted = not self.inverted
            self._bump()
            return self

        # ------------------------------------------------------------------ #
        # Reversible render-time effects
        # ------------------------------------------------------------------ #
        def blur(self, amount=5) -> 'Field':
            """Set the (reversible) blur amount in pixels. ``amount`` may also be a
            kernel tuple like ``(5, 5)`` for backwards compatibility. Use
            ``unblur()`` or ``blur(0)`` to remove it without touching the shape."""
            if isinstance(amount, (tuple, list)):
                amount = max(amount) if len(amount) else 0
            self.blur_amount = float(amount)
            self._bump()
            return self

        def feather(self, amount=5) -> 'Field':
            """Set the (reversible) feather amount in pixels (fades edges inward).
            Use ``unfeather()`` or ``feather(0)`` to remove it."""
            if isinstance(amount, (tuple, list)):
                amount = max(amount) if len(amount) else 0
            self.feather_amount = float(amount)
            self._bump()
            return self

        def unblur(self) -> 'Field':
            """Remove blur, restoring the crisp underlying shape."""
            self.blur_amount = 0.0
            self._bump()
            return self

        def unfeather(self) -> 'Field':
            """Remove feathering, restoring hard edges."""
            self.feather_amount = 0.0
            self._bump()
            return self

        # ------------------------------------------------------------------ #
        # Sampling / output
        # ------------------------------------------------------------------ #
        def get(self, x: int, y: int) -> float:
            """Sample the (rasterized) normalized value at a coordinate, in [0, 1]."""
            m = self._rasterize(self._ref_h, self._ref_w)
            yy, xx = int(y), int(x)
            if 0 <= yy < m.shape[0] and 0 <= xx < m.shape[1]:
                return float(m[yy, xx])
            return 0.0

        def preview(self, wait_for_exit: bool = False, title: str = "Field Preview") -> None:
            """Display the rasterized field using OpenCV."""
            m = self._rasterize(self._ref_h, self._ref_w)
            img = (rnp.clip(m, 0.0, 1.0) * 255).astype(rnp.uint8)
            cv2.imshow(title, img)
            if wait_for_exit:
                while cv2.getWindowProperty(title, cv2.WND_PROP_VISIBLE) >= 1:
                    if cv2.waitKey(100) & 0xFF == ord('q'):
                        break
            else:
                cv2.waitKey(1)

        def get_map(self, height: int = None, width: int = None) -> np.ndarray:
            """Rasterize the field to a normalized [0, 1] bitmap.

            Parameters:
                height (int, optional): Target height. Defaults to the renderer's.
                width (int, optional): Target width. Defaults to the renderer's.

            Returns:
                np.ndarray: A (height, width) float32 map in [0, 1]. Because the
                field is vector-based, it is rasterized at whatever resolution is
                requested, so it always matches the pixels it will be applied to.
            """
            if height is None:
                height = renderer.height()
            if width is None:
                width = renderer.width()
            height = int(height)
            width = int(width)
            key = (height, width)
            cached = self._device_cache.get(key)
            if cached is not None:
                return cached
            dm = np.asarray(self._rasterize(height, width))
            if len(self._device_cache) > 8:
                self._device_cache.clear()
            self._device_cache[key] = dm
            return dm


    class FOverlay(Field):
        def __init__(self, opacity: float = 1.0) -> None:
            """A uniform field covering the entire canvas at a constant opacity.

            Handy as a solid base layer or a tint: feed it to a color filter to
            flood the frame, or lower its opacity to dim whatever sits behind it.

            Parameters:
                opacity (float): Fill strength from 0.0 (fully transparent) to
                    1.0 (fully opaque). Defaults to 1.0. Values outside 0..1
                    raise a ValueError.
            """
            if not (0.0 <= opacity <= 1.0):
                raise ValueError(f"Opacity must be between 0 and 1, but got {opacity}.")
            super().__init__()
            self.shapes.append(self._fullcanvas_shape(float(opacity), True))


    class FLine(Field):
        def __init__(self, start: tuple, end: tuple, thickness: float) -> None:
            """A straight line segment of fixed thickness, stored as a vector rectangle.

            The line runs from start to end; its thickness is added evenly to both
            sides of that center line.

            Parameters:
                start (tuple) @position: (x, y) pixel coordinates of the start point.
                end (tuple) @position: (x, y) pixel coordinates of the end point.
                thickness (float): Line width in pixels, centered on the segment.
                    Must be greater than 0.
            """
            if thickness <= 0:
                raise ValueError(f"Thickness must be a positive number, but got {thickness}.")
            super().__init__()
            x1, y1 = float(start[0]), float(start[1])
            x2, y2 = float(end[0]), float(end[1])
            half = float(thickness) / 2.0
            dx, dy = x2 - x1, y2 - y1
            length = (dx * dx + dy * dy) ** 0.5
            if length < 1e-6:
                contour = rnp.array([
                    [x1 - half, y1 - half], [x1 + half, y1 - half],
                    [x1 + half, y1 + half], [x1 - half, y1 + half],
                ], dtype=rnp.float32)
            else:
                ux, uy = dx / length, dy / length
                nx, ny = -uy * half, ux * half
                contour = rnp.array([
                    [x1 + nx, y1 + ny], [x2 + nx, y2 + ny],
                    [x2 - nx, y2 - ny], [x1 - nx, y1 - ny],
                ], dtype=rnp.float32)
            self._add_poly([contour])


    class FRect(Field):
        def __init__(self, corner1: tuple, corner2: tuple, thickness: int = -1) -> None:
            """A rectangle, either filled solid or drawn as a hollow border ring.

            The two corners may be given in any order; the rectangle spans the
            bounding box between them.

            Parameters:
                corner1 (tuple) @position: (x, y) pixel coordinates of one corner.
                corner2 (tuple) @position: (x, y) pixel coordinates of the opposite corner.
                thickness (int): Border width in pixels, centered on the edges.
                    Use -1 (the default) to fill the rectangle solid; any other
                    value must be positive.
            """
            if thickness != -1 and thickness <= 0:
                raise ValueError(f"Thickness must be a positive number or -1 to fill, but got {thickness}.")
            super().__init__()
            x1, y1 = float(corner1[0]), float(corner1[1])
            x2, y2 = float(corner2[0]), float(corner2[1])
            ax, bx = min(x1, x2), max(x1, x2)
            ay, by = min(y1, y2), max(y1, y2)
            if thickness == -1:
                self._add_poly([self._rect_pts(ax, ay, bx, by)])
            else:
                h = float(thickness) / 2.0
                outer = self._rect_pts(ax - h, ay - h, bx + h, by + h)
                ix1, iy1, ix2, iy2 = ax + h, ay + h, bx - h, by - h
                holes = [self._rect_pts(ix1, iy1, ix2, iy2)] if (ix2 > ix1 and iy2 > iy1) else None
                self._add_poly([outer], holes=holes)

        @staticmethod
        def _rect_pts(ax, ay, bx, by):
            return rnp.array([[ax, ay], [bx, ay], [bx, by], [ax, by]], dtype=rnp.float32)


    class FEllipse(Field):
        def __init__(
                self,
                center: tuple,
                ellipse_width: float,
                ellipse_height: float,
                angle: float = 0,
                thickness: int = -1
        ) -> None:
            """An ellipse, approximated as a vector polygon, filled or as a border ring.

            Parameters:
                center (tuple) @position: (x, y) pixel coordinates of the ellipse's center.
                ellipse_width (float): Full width (horizontal diameter) in pixels,
                    measured before any rotation.
                ellipse_height (float): Full height (vertical diameter) in pixels,
                    measured before any rotation.
                angle (float): Clockwise rotation of the ellipse, in degrees.
                    Defaults to 0.
                thickness (int): Border width in pixels. Use -1 (the default) to
                    fill the ellipse solid; any other value must be positive.
            """
            if thickness != -1 and thickness <= 0:
                raise ValueError(f"Thickness must be a positive number or -1 to fill, but got {thickness}.")
            super().__init__()
            cx, cy = int(center[0]), int(center[1])
            ax = max(int(ellipse_width // 2), 1)
            ay = max(int(ellipse_height // 2), 1)
            ang = int(angle)
            if thickness == -1:
                pts = cv2.ellipse2Poly((cx, cy), (ax, ay), ang, 0, 360, 5)
                self._add_poly([rnp.asarray(pts, dtype=rnp.float32)])
            else:
                h = int(thickness // 2)
                outer = cv2.ellipse2Poly((cx, cy), (ax + h, ay + h), ang, 0, 360, 5)
                inner = cv2.ellipse2Poly((cx, cy), (max(ax - h, 1), max(ay - h, 1)), ang, 0, 360, 5)
                self._add_poly([rnp.asarray(outer, dtype=rnp.float32)],
                               holes=[rnp.asarray(inner, dtype=rnp.float32)])


    class FPoly(Field):
        def __init__(self, points: np.ndarray) -> None:
            """A filled polygon defined by an ordered list of (x, y) vertices.

            Parameters:
                points (np.ndarray): Vertex coordinates as an (N, 2) array or a
                    flat sequence of x, y pairs, in pixels. At least 3 vertices
                    are required; the outline closes automatically from the last
                    vertex back to the first.
            """
            pts = points.get() if hasattr(points, 'get') else rnp.asarray(points)
            pts = pts.reshape(-1, 2)
            if pts.shape[0] < 3:
                raise ValueError(f"A polygon requires at least 3 points, but received {pts.shape[0]}.")
            super().__init__()
            self._add_poly([pts.astype(rnp.float32)])


    class FText(Field):
        def __init__(
                self,
                text: str,
                position: tuple,
                font_scale: float,
                thickness: int = 1,
                custom_font: str = None
        ) -> None:
            """Text rendered once to a bitmap, then traced into vector contours.

            Because the glyphs become contours, the result behaves like any other
            vector field and can be moved, scaled, cropped, and so on.

            Parameters:
                text (str): The string to render.
                position (tuple) @position: (x, y) pixel coordinates of the text's center.
                font_scale (float): Glyph size multiplier. With a custom font this
                    maps to a pixel height; with the built-in font it is OpenCV's
                    font scale.
                thickness (int): Stroke width in pixels for the built-in font.
                    Defaults to 1. Ignored when custom_font is supplied.
                custom_font (str): Path to a .ttf/.otf font file, rendered via
                    Pillow. Defaults to None, which uses the built-in OpenCV font.
            """
            super().__init__()
            mask = rnp.zeros((self._ref_h, self._ref_w), dtype=rnp.uint8)
            if custom_font:
                mask = self._draw_with_pillow(mask, text, position, font_scale, custom_font)
            else:
                mask = self._draw_with_opencv(mask, text, position, font_scale, thickness)
            self._add_contours_from_mask(mask)

        def _draw_with_pillow(self, mask, text, position, font_scale, custom_font):
            pil_image = Image.fromarray(mask)
            draw = ImageDraw.Draw(pil_image)
            try:
                font_size = int(font_scale * 20)
                font = ImageFont.truetype(custom_font, font_size)
            except IOError:
                raise FileNotFoundError(f"Custom font file '{custom_font}' not found or could not be opened.")
            text_bbox = font.getbbox(text)
            text_width = text_bbox[2] - text_bbox[0]
            text_height = text_bbox[3] - text_bbox[1]
            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] - text_height / 2)
            draw.text((bottom_left_x, bottom_left_y), text, font=font, fill=255)
            return rnp.array(pil_image)

        def _draw_with_opencv(self, mask, text, position, font_scale, thickness):
            (text_width, text_height), baseline = cv2.getTextSize(
                text, cv2.FONT_HERSHEY_SIMPLEX, font_scale, thickness
            )
            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] + text_height / 2)
            cv2.putText(
                mask, text, (bottom_left_x, bottom_left_y),
                cv2.FONT_HERSHEY_SIMPLEX, font_scale, 255, thickness, lineType=cv2.LINE_AA
            )
            return mask


    class FAudio(Field):
        def __init__(self, aud: FrameAudio, start: int = 0, end: int = None) -> None:
            """Visualize an audio frame's frequency spectrum as a bar graph.

            Builds a full-canvas bar graph from the frame's frequency magnitudes,
            with a separate bar on the right showing the overall volume.

            Parameters:
                aud (FrameAudio): The audio frame to visualize; supplies the
                    per-frequency magnitudes and the overall volume.
                start (int): Lowest frequency to include, in hertz. Defaults to 0.
                end (int): Highest frequency to include, in hertz. Defaults to
                    None, which extends to the highest available frequency.
            """
            super().__init__()

            try:
                freqs = aud.list_frequencies()
                mags = aud.list_magnitudes()

                # Handle start and end indices based on the frequency bin width
                bin_width_hz = freqs[1] - freqs[0]

                if end is None:
                    end_idx = len(freqs)
                else:
                    end_idx = int(end / bin_width_hz)

                start_idx = int(start / bin_width_hz)

                if end_idx > len(freqs):
                    end_idx = len(freqs)
                if start_idx < 0 or start_idx >= len(freqs):
                    raise ValueError(f"Invalid range: start={start_idx}, end={end_idx}")

                # Normalize the magnitudes for visualization
                norm = max(mags) / renderer.height()
                if norm == 0 or np.isnan(norm) or np.isinf(norm):
                    return  # Silent frame, leave the field empty

                # Create the points for frequency bars
                total_bars = end_idx - start_idx
                if total_bars <= 0:
                    return

                bar_width = renderer.width() / total_bars
                points = []

                for i in range(start_idx, end_idx):
                    # Subtract start_idx so the first point is always drawn at x=0
                    x = (i - start_idx) * bar_width + bar_width / 2
                    y = renderer.height() - (mags[i] / norm)
                    points.extend([x, y])

                # Add the base of the visualization (polygon to close the bars)
                points.extend([renderer.width(), renderer.height(), 0, renderer.height()])
                self.add(FPoly(np.array(points, dtype=np.float32)))

                # Add the volume indicator as a rectangle
                self.add(FRect(
                    (renderer.width() - bar_width,
                     renderer.height() - aud.get_volume() * renderer.height()),
                    (renderer.width(), renderer.height())
                ))

            except ValueError as ve:
                print(f"Error in FAudio initialization: {ve}")
            except Exception as e:
                print(f"Unexpected error initializing FAudio: {e}")


class FDepth_Slice(Field):
        def __init__(self, depth_frame: 'Frame', feather: float, lower_bound: float, upper_bound: float) -> None:
            """Initialize a field from a depth map using a feathered slice between two thresholds.
            
            Parameters:
                depth_frame: The frame containing the depth map.
                feather: The width of the smooth transition zone on both edges.
                lower_bound: The minimum luminance to keep (0.0 to 1.0).
                upper_bound: The maximum luminance to keep (0.0 to 1.0).
            """
            super().__init__()
            
            # Extract normalized pixels
            pixels = depth_frame.get_pixels(standard_size=True).astype(np.float32) / 255.0
            
            # Compute luminance as a grayscale mask
            luminance = 0.299 * pixels[:, :, 2] + 0.587 * pixels[:, :, 1] + 0.114 * pixels[:, :, 0]
            
            # Feathering math
            if feather > 0.0:
                # 1. Fade-in mask (ramp from 0.0 to 1.0 around the lower bound)
                lower_fade = np.clip((luminance - (lower_bound - feather)) / (2 * feather), 0.0, 1.0)
                
                # 2. Fade-out mask (ramp from 1.0 to 0.0 around the upper bound)
                upper_fade = 1.0 - np.clip((luminance - (upper_bound - feather)) / (2 * feather), 0.0, 1.0)
                
                # 3. Multiply them to extract the perfectly feathered slice
                mask = lower_fade * upper_fade
            else:
                # Hard cut if no feather is applied
                mask = ((luminance > lower_bound) & (luminance < upper_bound)).astype(np.float32)
                
            self._map = (mask * 255).astype(np.uint8)

if len(_paths) != 0:
    class Filter:
        """A filter that masks an effect through a vector-based Field.

        Filters no longer pre-bake a fixed-resolution mask. Instead they hold a
        reference to a :class:`Field` and rasterize it on demand at the *exact*
        resolution of the pixels being processed (via ``apply_to``). This keeps
        the mask aligned with the frame no matter what ``dx`` / preview scaling
        the renderer is using, so the old broadcasting mismatches can't happen.
        """

        def __init_subclass__(cls, **kwargs):
            super().__init_subclass__(**kwargs)
            original_init = cls.__init__
            def profiled_init(self, *args, **kw):
                with _profile(f"construct_filter:{cls.__name__}"):
                    original_init(self, *args, **kw)
            cls.__init__ = profiled_init

        def __init__(self, field: Optional[Field] = None) -> None:
            """Initialize a Filter instance.

            Parameters:
                field (Optional[Field]): A Field object to apply the filter on.
                    If None, a default FOverlay() field will be used.
            """
            if field is None:
                field = FOverlay()
            self.set_field(field)

        def set_field(self, field: Field) -> 'Filter':
            """Set the Field that masks this filter.

            The field is stored by reference and is rasterized lazily, so no map
            is computed here. The last resolved mask is invalidated.

            Parameters:
                field (Field): The Field object providing the mask.

            Returns:
                Filter: The Filter instance itself (for method chaining).
            """
            self.field = field
            self._map = None
            return self

        def _resolved_map(self, pixels: np.ndarray) -> np.ndarray:
            """Rasterize the field to match the given pixels, as an (H, W, 1) map."""
            height = int(pixels.shape[0])
            width = int(pixels.shape[1])
            m = self.field.get_map(height, width)
            if hasattr(m, 'ndim') and m.ndim == 2:
                m = m[:, :, np.newaxis]
            return m

        def _mask_for(self, pixels: np.ndarray) -> np.ndarray:
            """Return a mask guaranteed to match ``pixels`` (resolving it if needed)."""
            m = self._map
            if (m is None
                    or m.shape[0] != pixels.shape[0]
                    or m.shape[1] != pixels.shape[1]):
                m = self._resolved_map(pixels)
            return m

        def apply_to(self, pixels: np.ndarray) -> np.ndarray:
            """Resolve the mask at the pixel resolution, then apply the filter.

            This is the entry point used by ``Frame.apply_filter``. It sets
            ``self._map`` (an (H, W, 1) float mask in [0, 1]) so that even custom
            ``apply`` implementations that reference ``self._map`` directly work
            without worrying about resolution.
            """
            self._map = self._resolved_map(pixels)
            return self.apply(pixels)

        def apply(self, pixels: np.ndarray) -> np.ndarray:
            """Apply the filter to a set of pixels (overridden by subclasses)."""
            return pixels

        def _apply(self, pixels: np.ndarray) -> None:
            """Deprecated placeholder kept for backwards compatibility."""
            pass

    class Solid_Color(Filter):
        """A filter that overlays a solid color onto a Field-based mask."""

        def __init__(self, color: tuple, field: Optional[Field] = None) -> None:
            """Initialize a Solid_Color filter.

            Parameters:
                color (tuple) @color: The overlay color as an (r, g, b) tuple, each 0–255.
                field (Optional[Field]): Field object providing the overlay mask.
                    If None, a default FOverlay() field will be used.
            """
            super().__init__(field)
            self.__r, self.__g, self.__b = int(color[0]), int(color[1]), int(color[2])

        def invert(self) -> 'Solid_Color':
            """Invert the solid color (i.e., subtract each RGB component from 255).

            Returns:
                Solid_Color: The Solid_Color instance itself (for method chaining).
            """
            self.__r = 255 - self.__r
            self.__g = 255 - self.__g
            self.__b = 255 - self.__b
            return self

        def apply(self, pixels: np.ndarray) -> np.ndarray:
            """Apply the solid color filter to pixel data using the field mask."""
            m = self._mask_for(pixels)

            # Explicitly convert the Python list to an array so CuPy can process it
            color_array = np.array([self.__b, self.__g, self.__r])

            return np.clip(pixels * (1 - m) + color_array * m, 0, 255)

    class Invert(Filter):
        """A filter that inverts the colors of a Field-based mask."""

        def __init__(self, field: Optional[Field] = None) -> None:
            """Initialize an Invert filter.

            Parameters:
                field (Optional[Field]): Field object providing the overlay mask.
                    If None, a default FOverlay() field will be used.
            """
            super().__init__(field)

        def apply(self, pixels: np.ndarray) -> np.ndarray:
            """Apply the inversion filter to pixel data using the field mask.

            This method inverts the pixel colors and blends them based on the map values.

            Parameters:
                pixels (np.ndarray): The pixel data to apply the inversion filter to.

            Returns:
                np.ndarray: The color-inverted and blended pixel data, clipped to the valid range [0, 255].
            """
            m = self._mask_for(pixels)

            # Invert the pixels
            inverted_pixels = 255 - pixels

            # Blend based on the map values
            filtered_pixels = (1 - m) * pixels + m * inverted_pixels

            return np.clip(filtered_pixels, 0, 255)

    class Draw_Frame(Filter):
        """A filter that draws a frame onto an image at a specified position."""

        def __init__(self, frame: Frame, position: tuple = None, field: Optional[Field] = None) -> None:
            """Initialize a Draw_Frame filter.

            Parameters:
                frame (Frame): The Frame object that will be drawn onto the image.
                position (tuple) @position: (x, y) top-left pixel for the frame. If None, the frame is centered.
                field (Optional[Field]): Field object providing the overlay mask. Defaults to FOverlay() if None.
            """
            if field is None:
                field = FOverlay()
            super().__init__(field)
            self.frame = frame
            if position is None:
                self.x = None
                self.y = None
            else:
                self.x, self.y = int(position[0]), int(position[1])

        def apply(self, pixels: np.ndarray) -> np.ndarray:
            """Apply the frame to the given pixels at the specified (x, y) position.

            If `x` and `y` are None, the frame is centered. If the frame extends beyond the image bounds, it is cropped.
            Any uncovered space is filled with the original pixels.

            Parameters:
                pixels (np.ndarray): The pixel data onto which the frame will be applied.

            Returns:
                np.ndarray: The image with the frame applied at the specified position, blended with the field map.
            """
            m = self._mask_for(pixels)

            frame_pixels = self.frame.get_pixels()
            frame_h, frame_w = frame_pixels.shape[:2]
            pixels_h, pixels_w = pixels.shape[:2]

            # Determine x and y position (centered if None)
            if self.x is None:
                x_offset = (pixels_w - frame_w) // 2
            else:
                x_offset = self.x

            if self.y is None:
                y_offset = (pixels_h - frame_h) // 2
            else:
                y_offset = self.y

            # Ensure the offsets are within bounds
            x_start = max(x_offset, 0)
            y_start = max(y_offset, 0)
            x_end = min(x_offset + frame_w, pixels_w)
            y_end = min(y_offset + frame_h, pixels_h)

            # Compute the region of the frame that fits within the image bounds
            frame_x_start = max(0, -x_offset)
            frame_y_start = max(0, -y_offset)
            frame_x_end = frame_x_start + (x_end - x_start)
            frame_y_end = frame_y_start + (y_end - y_start)

            # Prevent assignment if the cropped dimensions are invalid
            if x_end <= x_start or y_end <= y_start or frame_x_end <= frame_x_start or frame_y_end <= frame_y_start:
                return pixels  # Return unchanged pixels if there's nothing to draw

            # Create a copy of the original pixels
            new_frame = pixels.copy()

            # Apply the frame onto the new canvas only within valid bounds
            new_frame[y_start:y_end, x_start:x_end] = frame_pixels[frame_y_start:frame_y_end, frame_x_start:frame_x_end]

            # Blend with the field map
            return np.clip(m * new_frame + (1 - m) * pixels, 0, 255)

        def set_position(self, position: tuple) -> 'Draw_Frame':
            """Updates the frame's position.

            Parameters:
                position (tuple) @position: (x, y) top-left pixel for the frame.

            Returns:
                Draw_Frame: The current instance with updated position.
            """
            self.x, self.y = int(position[0]), int(position[1])
            return self

        def invert(self) -> 'Draw_Frame':
            """Invert the frame colors.

            Returns:
                Draw_Frame: The current instance with the inverted frame.
            """
            self.frame = Frame(255 - self.frame.get_pixels())
            return self

        def mirror_x(self) -> 'Draw_Frame':
            """Mirror the frame along the x-axis (horizontal flip).

            Returns:
                Draw_Frame: The current instance with the frame mirrored along the x-axis.
            """
            self.frame = Frame(cv2.flip(self.frame.get_pixels(), 1))
            return self

        def mirror_y(self) -> 'Draw_Frame':
            """Mirror the frame along the y-axis (vertical flip).

            Returns:
                Draw_Frame: The current instance with the frame mirrored along the y-axis.
            """
            self.frame = Frame(cv2.flip(self.frame.get_pixels(), 0))
            return self


class Blur(Filter):
    """A filter that applies a blur effect to the image using the field mask."""

    def __init__(self, blur_kernel: int = 5, field: Optional[Field] = None) -> None:
        """Initialize a blur filter.

        Parameters:
            blur_kernel (int): Size of the blur kernel; must be positive and odd.
            field (Optional[Field]): Field object providing the overlay mask.
                If None, a default FOverlay() field will be used.
        """
        super().__init__(field)
        if blur_kernel < 1 or blur_kernel % 2 == 0:
            raise ValueError("blur_kernel size must be a positive odd integer.")
        self.blur_kernel = blur_kernel

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """Apply the blur filter to pixel data using the field mask.

        Parameters:
            pixels (np.ndarray): The pixel data to apply the filter to.

        Returns:
            np.ndarray: The blurred pixel data masked by self._map.
        """
        # Ensure pixels are a CPU numpy array for OpenCV compatibility
        if gpu_enabled:
            pixels_cpu = np.asnumpy(pixels).astype(np.uint8)
            blurred_cpu = cv2.GaussianBlur(pixels_cpu, (self.blur_kernel, self.blur_kernel), 0)
            blurred = np.asarray(blurred_cpu)
            # Convert back to GPU array if needed
            if isinstance(pixels, np.ndarray) and pixels.__module__ == 'cupy':
                blurred = np.asarray(blurred)
        else:
            blurred = cv2.GaussianBlur(pixels, (self.blur_kernel, self.blur_kernel), 0)

        # Convert field map to match pixel channels if needed
        mask = self._map
        if mask.ndim == 2:
            mask = mask[:, :, np.newaxis]

        alpha = mask.astype(np.float32)
        blended = alpha * blurred + (1.0 - alpha) * pixels

        return np.clip(blended, 0, 255).astype(np.uint8)

class Color_Map(Filter):
    """A filter that maps grayscale pixel intensities to a custom color gradient repeated multiple times."""

    def __init__(self, colors: List[Tuple[int, int, int]] = None, repeat: int = 1, inverse_transition: bool = False, offset: float = 0.0, field: Optional[Field] = None) -> None:
        """Initialize the Color_Map filter.

        Parameters:
            colors (List[Tuple[int, int, int]], optional): List of RGB tuples defining the color gradient.
                Defaults to [(255, 255, 255), (0, 0, 0)] which is white to black.
            repeat (int): Number of times to repeat the color map across the intensity range.
            inverse_transition (bool): If True, every other repetition of the gradient is inverted
                to create smooth transitions between repetitions.
            offset (float): Offset to apply to the repetitions cycle, in the range [0, 1).
            field (Optional[Field]): Field object providing the overlay mask.
                If None, a default FOverlay() field will be used.
        """
        super().__init__(field)
        if colors is None:
            colors = [(255, 255, 255), (0, 0, 0)]
        if repeat < 1:
            raise ValueError("Repeat count must be at least 1.")
        self.repeat = repeat
        self.inverse_transition = inverse_transition
        # Normalize offset into [0,1)
        self.offset = offset % 1.0
        
        # Normalize colors to floats 0-1 for interpolation
        self.color_stops = np.array(colors, dtype=np.float32) / 255.0

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """Apply the repeated custom color mapping filter to the pixel data.

        Parameters:
            pixels (np.ndarray): The pixel data to apply the filter to.

        Returns:
            np.ndarray: The color-mapped pixel data masked by self._map.
        """
        # Convert pixels to grayscale by luminance formula
        gray = (0.299 * pixels[:, :, 2] + 0.587 * pixels[:, :, 1] + 0.114 * pixels[:, :, 0]).astype(np.uint8)
        # Normalize grayscale to range [0, 1]
        norm_gray = gray / 255.0

        num_bins = 256
        color_map = np.zeros((num_bins, 3), dtype=np.float32)

        segments = len(self.color_stops) - 1
        segment_length = num_bins // segments if segments > 0 else num_bins

        for i in range(segments):
            start_color = self.color_stops[i]
            end_color = self.color_stops[i + 1]
            for j in range(segment_length):
                t = j / segment_length
                color = (1 - t) * start_color + t * end_color
                idx = i * segment_length + j
                if idx < num_bins:
                    color_map[idx] = color

        # Fill any leftover bins with the last color stop
        for idx in range(segments * segment_length, num_bins):
            color_map[idx] = self.color_stops[-1] if len(self.color_stops) > 0 else np.array([1.0, 1.0, 1.0], dtype=np.float32)

        # Prepare output array
        new_pixels = np.zeros_like(pixels, dtype=np.float32)

        # Repeat the normalized grayscale value scaled by repeat and add offset, then wrap by modulo 1
        repeated_value = (norm_gray * self.repeat + self.offset) % 1.0

        if self.inverse_transition:
            # For repeated_value, determine for each pixel if floor of repetition is even or odd
            repetition_indices = np.floor((norm_gray * self.repeat + self.offset)) .astype(int)
            fractional_part = repeated_value

            # For even repetitions keep fractional_part as is, for odd repetitions invert fractional_part to get smooth transition
            fractional_part = np.where(repetition_indices % 2 == 1, 1.0 - fractional_part, fractional_part)

            indices = (fractional_part * (num_bins - 1)).astype(np.int32)
        else:
            # Without inverse transition just use fractional part normally
            indices = (repeated_value * (num_bins - 1)).astype(np.int32)

        # Map pixels to custom colors (OpenCV uses BGR ordering)
        new_pixels[:, :, 2] = color_map[indices, 0] * 255  # R channel
        new_pixels[:, :, 1] = color_map[indices, 1] * 255  # G channel
        new_pixels[:, :, 0] = color_map[indices, 2] * 255  # B channel

        # Apply the filter mask (self._map) to blend with original pixels
        map_3c = self._map
        if map_3c.ndim == 4:
            map_3c = np.squeeze(map_3c, axis=(2, 3))
        if map_3c.ndim == 2:
            map_3c = map_3c[:, :, np.newaxis]

        blended = map_3c * new_pixels + (1 - map_3c) * pixels
        return np.clip(blended, 0, 255).astype(np.uint8)

class Rainbow_Map(Filter):
    """A filter that maps grayscale pixel intensities to a rainbow gradient repeated multiple times."""

    _cached_rainbow_map = None

    def __init__(self, repeat: int = 1, field: Optional[Field] = None) -> None:
        """Initialize the Rainbow_Map filter.

        Parameters:
            repeat (int): Number of times to repeat the rainbow map across the intensity range.
            field (Optional[Field]): Field object providing the overlay mask.
                If None, a default FOverlay() field will be used.
        """
        super().__init__(field)
        if repeat < 1:
            raise ValueError("Repeat count must be at least 1.")
        self.repeat = repeat

    @classmethod
    def _get_rainbow_map(cls):
        if cls._cached_rainbow_map is not None:
            return cls._cached_rainbow_map
        color_stops = np.array([
            [1.0, 0.0, 0.0],   # Red
            [1.0, 0.5, 0.0],   # Orange
            [1.0, 1.0, 0.0],   # Yellow
            [0.0, 1.0, 0.0],   # Green
            [0.0, 0.0, 1.0],   # Blue
            [0.29, 0.0, 0.51], # Indigo (approx)
            [0.58, 0.0, 0.83]  # Purple
        ], dtype=np.float32)
        num_bins = 256
        rainbow_map = np.zeros((num_bins, 3), dtype=np.float32)
        segments = len(color_stops) - 1
        segment_length = num_bins // segments
        for i in range(segments):
            start_color = color_stops[i]
            end_color = color_stops[i + 1]
            for j in range(segment_length):
                t = j / segment_length
                color = (1 - t) * start_color + t * end_color
                idx = i * segment_length + j
                if idx < num_bins:
                    rainbow_map[idx] = color
        for idx in range(segments * segment_length, num_bins):
            rainbow_map[idx] = color_stops[-1]
        cls._cached_rainbow_map = rainbow_map
        return rainbow_map

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """Apply the repeated rainbow mapping filter to the pixel data.

        Parameters:
            pixels (np.ndarray): The pixel data to apply the filter to.

        Returns:
            np.ndarray: The color-mapped pixel data masked by self._map.
        """
        # Convert pixels to grayscale by luminance formula
        gray = (0.299 * pixels[:, :, 2] + 0.587 * pixels[:, :, 1] + 0.114 * pixels[:, :, 0]).astype(np.uint8)
        # Normalize grayscale to range [0, 1]
        norm_gray = gray / 255.0

        rainbow_map = self._get_rainbow_map()
        num_bins = 256

        # Repeat the normalized grayscale value to range [0, 1]*repeat then mod 1 so it wraps around
        repeated_value = (norm_gray * self.repeat) % 1.0
        indices = (repeated_value * (num_bins - 1)).astype(np.int32)

        # Prepare output array
        new_pixels = np.zeros_like(pixels, dtype=np.float32)

        # Map pixels to rainbow colors (OpenCV uses BGR ordering)
        new_pixels[:, :, 2] = rainbow_map[indices, 0] * 255  # R channel
        new_pixels[:, :, 1] = rainbow_map[indices, 1] * 255  # G channel
        new_pixels[:, :, 0] = rainbow_map[indices, 2] * 255  # B channel

        # Apply the filter mask (self._map) to blend with original pixels
        map_3c = self._map
        if map_3c.ndim == 4:
            map_3c = np.squeeze(map_3c, axis=(2, 3))
        if map_3c.ndim == 2:
            map_3c = map_3c[:, :, np.newaxis]

        blended = map_3c * new_pixels + (1 - map_3c) * pixels
        return np.clip(blended, 0, 255).astype(np.uint8)

class Scanlines(Filter):
    """
    A filter that applies a CRT scanline effect"""

    def __init__(self, intensity: float = 0.4, line_thickness: int = 1, line_spacing: int = 2, invert_lines: bool = False, field: Optional[Field] = None ) -> None:
        """
        Initialize the CRT_Scanline_Only_Filter.

        Parameters:
            intensity (float): The darkness factor for scanlines (0 = no darkening, 1 = full black lines).
            line_thickness (int): Thickness in pixels of each scanline.
            line_spacing (int): Number of pixels between start of one scanline and next (including thickness).
            invert_lines (bool): If True, darken odd lines instead of even lines.
            field (Optional[Field]): Overlay mask for applying the effect; defaults to FOverlay() if None.
        """
        if field is None:
            field = FOverlay()
        super().__init__(field)

        if not (0.0 <= intensity <= 1.0):
            raise ValueError("intensity must be between 0 and 1.")
        if line_thickness < 1:
            raise ValueError("line_thickness must be at least 1.")
        if line_spacing < line_thickness:
            raise ValueError("line_spacing must be >= line_thickness.")

        self.intensity = intensity
        self.line_thickness = line_thickness
        self.line_spacing = line_spacing
        self.invert_lines = invert_lines

        # Precompute scanline mask map of shape (height, width)
        height = renderer.height()
        width = renderer.width()
        mask = np.ones((height, width), dtype=np.float32)
        for y in range(0, height, line_spacing):
            # Decide if this line should be darkened depending on line parity and invert_lines
            line_index = y // line_spacing
            apply_dark = (line_index % 2 == 0) != invert_lines
            if apply_dark:
                end_y = min(y + line_thickness, height)
                mask[y:end_y, :] = 1.0 - intensity  # darken line

        self.scanline_map = mask  # float32 mask with values near 1 or reduced by intensity

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """
        Apply the CRT scanline effect to the pixel data.

        Parameters:
            pixels (np.ndarray): Input image pixels as (H,W,3) uint8 array.

        Returns:
            np.ndarray: The pixel data with CRT scanline effect applied.
        """
        # Convert pixels to float for calculation
        pixels_f = pixels.astype(np.float32)

        # Expand scanline mask to 3 channels
        scanline_mask_3c = self.scanline_map[:, :, np.newaxis]

        # Apply the scanline intensity mask
        filtered_pixels = pixels_f * scanline_mask_3c

        # Blend with original pixels using the field mask self._map
        # Normalize self._map to range [0, 1]
        if self._map.ndim == 2:
            alpha = self._map.astype(np.float32)
            max_alpha = np.max(alpha)
            if max_alpha > 0:
                alpha = alpha / max_alpha
            alpha = alpha[:, :, np.newaxis]
        elif self._map.ndim == 3:
            alpha = self._map.astype(np.float32)
            max_alpha = np.max(alpha)
            if max_alpha > 0:
                alpha = alpha / max_alpha
        else:
            alpha = np.ones_like(scanline_mask_3c, dtype=np.float32)

        blended = alpha * filtered_pixels + (1 - alpha) * pixels_f

        return np.clip(blended, 0, 255).astype(np.uint8)

class Screen_Tear(Filter):
    """A filter that simulates capturing a CRT screen partially through drawing the frame,
    showing partial frame reveal, blending with a previous frame for open areas."""

    def __init__(self, previous_frame: Optional[Frame] = None, reveal_fraction: float = 0.5, field: Optional[Field] = None) -> None:
        """
        Initialize the CRT_Scanline_Filter without internal scanlines.

        Parameters:
            previous_frame (Optional[Frame]): The previous Frame to show in the uncovered region.
                If None, black will be used for empty areas.
            reveal_fraction (float): Fraction of the frame height revealed (0.0 to 1.0).
            field (Optional[Field]): Field overlay mask, default to FOverlay() if None.
        """
        super().__init__(field)
        if not (0.0 <= reveal_fraction <= 1.0):
            raise ValueError("reveal_fraction must be between 0.0 and 1.0")
        self.previous_frame = previous_frame
        self.reveal_fraction = reveal_fraction

        # Precompute a mask of all ones since scanlines are handled by separate Scanlines filter
        height = renderer.height()
        width = renderer.width()
        self.scanline_map = np.ones((height, width), dtype=np.float32)  # No darkening here

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """
        Apply the partial frame reveal effect only.

        Parameters:
            pixels (np.ndarray): The current frame pixel data.

        Returns:
            np.ndarray: The pixel data with partial reveal effect applied.
        """
        height, width = pixels.shape[:2]
        reveal_rows = int(height * self.reveal_fraction)
        # Clamp reveal_rows to height
        reveal_rows = max(0, min(reveal_rows, height))

        # Create an output canvas initially with previous frame or black if none
        if self.previous_frame:
            prev_pixels = self.previous_frame.get_pixels(standard_size=True)
            if prev_pixels.shape[:2] != (height, width):
                # Resize previous frame pixels to match current frame size
                prev_pixels = cv2.resize(prev_pixels, (width, height), interpolation=cv2.INTER_LINEAR)
        else:
            prev_pixels = np.zeros((height, width, 3), dtype=np.uint8)

        # Compose the partial frame: top reveal_rows from pixels, rest from prev_pixels
        output_pixels = np.zeros_like(pixels, dtype=np.float32)
        output_pixels[:reveal_rows, :, :] = pixels[:reveal_rows, :, :]
        output_pixels[reveal_rows:, :, :] = prev_pixels[reveal_rows:, :, :]

        # Apply the mask (all ones, so no change here)
        scanline_mask_3c = self.scanline_map[:, :, np.newaxis]
        output_pixels = output_pixels.astype(np.float32) * scanline_mask_3c

        # Blend with field mask - use self._map as alpha mask for blending original pixels and partial reveal output
        if self._map.ndim == 2:
            alpha = self._map.astype(np.float32)
            alpha = alpha[:, :, np.newaxis]  # shape (h, w, 1)
        elif self._map.ndim == 3:
            alpha = self._map.astype(np.float32)
        else:
            alpha = np.ones((height, width, 1), dtype=np.float32)

        # Normalize alpha to 0 to 1
        alpha = alpha / np.max(alpha) if np.max(alpha) > 0 else alpha

        # Blend original pixels with partial reveal output by alpha mask
        blended = alpha * output_pixels + (1 - alpha) * pixels
        blended = np.clip(blended, 0, 255)

        return blended.astype(np.uint8)

class SideOffsetFilter(Filter):
    """A filter that offsets lines of pixels based on an angle (0-360 degrees),
    wrapping pixels that are pushed off-screen around to the opposite side.

    The offset values array length will be resampled via interpolation to match frame height (for mostly vertical shifts)
    or frame width (for mostly horizontal shifts) depending on the angle.

    Angles:
        - 0 or 360 degrees: offset horizontally to the right (like side=2 Right).
        - 90 degrees: offset vertically downward (like side=3 Bottom).
        - 180 degrees: offset horizontally to the left (like side=0 Left).
        - 270 degrees: offset vertically upward (like side=1 Top).
        - Intermediate angles interpolate offsets in both directions proportionally.
          The offset is applied along a vector line rotated by the angle, with values along that line.
    """

    def __init__(self, values: np.ndarray, angle: float, blend_width: int = 10, field: Optional[Field] = None) -> None:
        """
        Initialize the SideOffsetFilter with angle-based offset.

        Args:
            values (np.ndarray): 1D array of floats in [0,1] representing offset magnitude.
                These represent offset values along a line (like a waveform) that will be rotated by the angle.
            angle (float): Offset angle in degrees [0-360).
            blend_width (int): Width in pixels over which to blend edges.
            field (Optional[Field]): Field for masking. Defaults to FOverlay() if None.
        """
        super().__init__(field)
        if not (0 <= angle < 360):
            angle = angle % 360
        if not isinstance(values, np.ndarray):
            raise TypeError("Values must be a numpy ndarray.")
        if values.ndim != 1:
            raise ValueError("Values array must be 1D.")
        if blend_width < 0:
            raise ValueError("blend_width must be non-negative.")

        self.values = values.astype(np.float32)
        self.angle = angle
        self.blend_width = blend_width

    def _resample_values(self, target_length: int) -> np.ndarray:
        """Resample self.values to target_length using linear interpolation."""
        if len(self.values) == target_length:
            return self.values
        if len(self.values) == 1:
            return np.full(target_length, self.values[0], dtype=np.float32)

        orig_x = np.linspace(0, 1, len(self.values))
        target_x = np.linspace(0, 1, target_length)
        resampled = np.interp(target_x, orig_x, self.values)
        return resampled.astype(np.float32)

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """
        Apply the offset effect by treating the values as a line along the rotated axis,
        offsetting pixels in the direction of the angle accordingly with wrapping and blending.

        Args:
            pixels (np.ndarray): Input image pixels as (H, W, 3) uint8 array.

        Returns:
            np.ndarray: Offset pixels blended with original using self._map mask.
        """
        height, width = pixels.shape[:2]
        bw = self.blend_width
        pixels_f = pixels.astype(np.float32)

        angle_int = int(round(self.angle)) % 360

        # Optimize for cardinal angles: 0, 90, 180, 270, 360
        if angle_int == 0 or angle_int == 360:
            # Horizontal offset to right
            resampled_values = self._resample_values(width)
            max_offset = width
            offsets = resampled_values * max_offset  # length width
            # Create output array
            output = np.empty_like(pixels_f)
            for y in range(height):
                row = pixels_f[y]
                row_offset = int(offsets[y if y < len(offsets) else -1]) if len(offsets) == height else int(offsets[y % width])
                # Use vectorized shift with wrap
                offset_vals = offsets
                # Since offsets length == width, shift each row by offsets[x]? No, offsets along x axis, needed to treat properly
                # offsets is 1D along horizontal dimension (width)
                # Actually, offsets length == width, so each pixel column has offset value
                # But for horizontal shift, offsets should be along vertical dimension (height)? Original logic uses resample to length line_length, which is width here for horizontal line, so resampled_values length == width

                # For horizontal shift of each row by per-column offset does not make sense, so must resample per row for vertical offsets - here horizontal shift implies offsets per row

                # For horizontal shift, offsets length should be equal to height, one value per row

            # Correction: For 0 degrees, horizontal offset, offsets length should be number of rows (height), each row shifted by offset

            resampled_values = self._resample_values(height)
            offsets = (resampled_values * width).astype(int)  # one offset per row
            output = np.empty_like(pixels_f)

            for y in range(height):
                offset = offsets[y]
                # Shift row y pixels to right by offset, wrap-around
                row = pixels_f[y]
                output[y] = np.roll(row, offset, axis=0)
            
            # Calculate blend mask
            blend_mask = np.ones((height, width), dtype=np.float32)
            if bw > 0:
                norm_offset = offsets[:, None]  # shape (height,1)
                blend_dist_start = np.clip(norm_offset / bw, 0, 1)
                blend_dist_end = np.clip((width - norm_offset) / bw, 0, 1)
                blend_mask = np.minimum(blend_dist_start, blend_dist_end)
                blend_mask = np.repeat(blend_mask, width, axis=1)

            blend_mask_3c = blend_mask[:, :, None]

            # Normalize field map alpha
            if self._map.ndim == 2:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
                mask_alpha = mask_alpha[:, :, None]
            elif self._map.ndim == 3:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
            else:
                mask_alpha = np.ones((height, width, 1), dtype=np.float32)

            final_alpha = mask_alpha * blend_mask_3c
            output = final_alpha * output + (1 - final_alpha) * pixels_f
            return np.clip(output, 0, 255).astype(np.uint8)

        elif angle_int == 180:
            # Horizontal offset to left (negative horizontal offset)
            resampled_values = self._resample_values(height)
            offsets = (resampled_values * width).astype(int)
            output = np.empty_like(pixels_f)
            for y in range(height):
                offset = offsets[y]
                row = pixels_f[y]
                output[y] = np.roll(row, -offset, axis=0)

            blend_mask = np.ones((height, width), dtype=np.float32)
            if bw > 0:
                norm_offset = offsets[:, None]
                blend_dist_start = np.clip(norm_offset / bw, 0, 1)
                blend_dist_end = np.clip((width - norm_offset) / bw, 0, 1)
                blend_mask = np.minimum(blend_dist_start, blend_dist_end)
                blend_mask = np.repeat(blend_mask, width, axis=1)

            blend_mask_3c = blend_mask[:, :, None]

            if self._map.ndim == 2:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
                mask_alpha = mask_alpha[:, :, None]
            elif self._map.ndim == 3:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
            else:
                mask_alpha = np.ones((height, width, 1), dtype=np.float32)

            final_alpha = mask_alpha * blend_mask_3c
            output = final_alpha * output + (1 - final_alpha) * pixels_f
            return np.clip(output, 0, 255).astype(np.uint8)

        elif angle_int == 90:
            # Vertical offset downward
            resampled_values = self._resample_values(width)
            offsets = (resampled_values * height).astype(int)
            output = np.empty_like(pixels_f)
            for x in range(width):
                column = pixels_f[:, x, :]
                offset = offsets[x]
                output[:, x, :] = np.roll(column, offset, axis=0)

            blend_mask = np.ones((height, width), dtype=np.float32)
            if bw > 0:
                norm_offset = offsets[None, :]
                blend_dist_start = np.clip(norm_offset / bw, 0, 1)
                blend_dist_end = np.clip((height - norm_offset) / bw, 0, 1)
                blend_mask = np.minimum(blend_dist_start, blend_dist_end)
                blend_mask = np.repeat(blend_mask, height, axis=0)

            blend_mask_3c = blend_mask[:, :, None]

            if self._map.ndim == 2:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
                mask_alpha = mask_alpha[:, :, None]
            elif self._map.ndim == 3:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
            else:
                mask_alpha = np.ones((height, width, 1), dtype=np.float32)

            final_alpha = mask_alpha * blend_mask_3c
            output = final_alpha * output + (1 - final_alpha) * pixels_f
            return np.clip(output, 0, 255).astype(np.uint8)

        elif angle_int == 270:
            # Vertical offset upward
            resampled_values = self._resample_values(width)
            offsets = (resampled_values * height).astype(int)
            output = np.empty_like(pixels_f)
            for x in range(width):
                column = pixels_f[:, x, :]
                offset = offsets[x]
                output[:, x, :] = np.roll(column, -offset, axis=0)

            blend_mask = np.ones((height, width), dtype=np.float32)
            if bw > 0:
                norm_offset = offsets[None, :]
                blend_dist_start = np.clip(norm_offset / bw, 0, 1)
                blend_dist_end = np.clip((height - norm_offset) / bw, 0, 1)
                blend_mask = np.minimum(blend_dist_start, blend_dist_end)
                blend_mask = np.repeat(blend_mask, height, axis=0)

            blend_mask_3c = blend_mask[:, :, None]

            if self._map.ndim == 2:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
                mask_alpha = mask_alpha[:, :, None]
            elif self._map.ndim == 3:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
            else:
                mask_alpha = np.ones((height, width, 1), dtype=np.float32)

            final_alpha = mask_alpha * blend_mask_3c
            output = final_alpha * output + (1 - final_alpha) * pixels_f
            return np.clip(output, 0, 255).astype(np.uint8)

        # Else for other angles execute original full calculation path
        output = np.empty_like(pixels_f)
        angle_rad = np.deg2rad(self.angle)
        sin_a = np.sin(angle_rad)
        cos_a = np.cos(angle_rad)

        dir_vec = np.array([cos_a, sin_a], dtype=np.float32)
        offset_vec = np.array([-sin_a, cos_a], dtype=np.float32)

        corners = np.array([[0,0], [width-1, 0], [0, height-1], [width-1, height-1]], dtype=np.float32)
        proj = np.dot(corners, dir_vec)
        min_proj = proj.min()
        max_proj = proj.max()
        line_length = max_proj - min_proj
        length_int = max(1, int(np.ceil(line_length)))
        resampled_values = self._resample_values(length_int)
        max_offset = max(height, width)

        xs, ys = np.meshgrid(np.arange(width), np.arange(height))
        coords = np.stack((xs, ys), axis=-1).astype(np.float32)

        proj_coords = np.dot(coords, dir_vec)
        proj_normalized = (proj_coords - min_proj) / line_length * (length_int - 1)
        proj_normalized_clipped = np.clip(proj_normalized, 0, length_int - 1)

        idx_low = np.floor(proj_normalized_clipped).astype(int)
        idx_high = np.clip(idx_low + 1, 0, length_int - 1)
        weight_high = proj_normalized_clipped - idx_low
        weight_low = 1.0 - weight_high

        try:
            offset_vals = weight_low * resampled_values[idx_low] + weight_high * resampled_values[idx_high]
        except IndexError:
            offset_vals = np.zeros_like(proj_normalized_clipped, dtype=np.float32)

        offsets = offset_vals * max_offset
        offset_dx = offset_vec[0] * offsets
        offset_dy = offset_vec[1] * offsets

        new_x = (xs + offset_dx).astype(np.float32)
        new_y = (ys + offset_dy).astype(np.float32)

        new_x_wrapped = np.mod(new_x, width)
        new_y_wrapped = np.mod(new_y, height)

        x0 = np.floor(new_x_wrapped).astype(int)
        x1 = (x0 + 1) % width
        y0 = np.floor(new_y_wrapped).astype(int)
        y1 = (y0 + 1) % height

        x_frac = new_x_wrapped - x0
        y_frac = new_y_wrapped - y0

        try:
            p00 = pixels_f[y0, x0]
            p10 = pixels_f[y0, x1]
            p01 = pixels_f[y1, x0]
            p11 = pixels_f[y1, x1]
        except IndexError:
            return pixels

        top = p00 * (1 - x_frac[..., None]) + p10 * x_frac[..., None]
        bottom = p01 * (1 - x_frac[..., None]) + p11 * x_frac[..., None]
        p = top * (1 - y_frac[..., None]) + bottom * y_frac[..., None]

        blend_mask = np.ones((height, width), dtype=np.float32)
        if bw > 0:
            norm_offset = offsets
            blend_dist_start = np.clip(norm_offset / bw, 0, 1)
            blend_dist_end = np.clip((max_offset - norm_offset) / bw, 0, 1)
            blend_mask = np.minimum(blend_dist_start, blend_dist_end)

        blend_mask_3c = blend_mask[:, :, None]

        if self._map.ndim == 2:
            mask_alpha = self._map.astype(np.float32)
            max_alpha = np.max(mask_alpha)
            if max_alpha > 0:
                mask_alpha /= max_alpha
            mask_alpha = mask_alpha[:, :, None]
        elif self._map.ndim == 3:
            mask_alpha = self._map.astype(np.float32)
            max_alpha = np.max(mask_alpha)
            if max_alpha > 0:
                mask_alpha /= max_alpha
        else:
            mask_alpha = np.ones((height, width, 1), dtype=np.float32)

        final_alpha = mask_alpha * blend_mask_3c
        output = final_alpha * p + (1 - final_alpha) * pixels_f

        return np.clip(output, 0, 255).astype(np.uint8)

def run(setup, render):
    """Hand control to the engine for a 'standard' project.

    A standard project is built from two functions instead of a frame loop:

        def setup():
            # Runs once. Re-runs only when you edit this function.
            # Initialise variables / precompute expensive things here,
            # and declare them `global` so render() can read them.
            ...

        def render(f):
            # Renders ONE frame (index f), independently of every other
            # frame. Return a Frame (or raw HxWx3 uint8 pixels).
            return ...

        run(setup, render)   # <- hand control to the engine

    The engine keeps your namespace alive between edits, so editing only
    render() will NOT re-run setup() (your precomputed state is preserved).
    While playing, the engine renders whatever frame matches the current
    audio timestamp and skips frames it can't produce in time, so the
    effective frame rate adapts to how expensive render() is.
    """
    renderer._run_standard(setup, render)


