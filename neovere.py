import numpy as np
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


try:
    import cv2
except ImportError:
    print("Error: OpenCV is not installed. Please install it using `pip install opencv-python`.")
    exit(1)

fourcc = cv2.VideoWriter_fourcc(*'mp4v')

_paths = ["render.mp4"]
arial = "C:/Users/luke/AppData/Local/Temp/arial-bold.ttf"

api_key = ""

audio_counter = 0

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
        self._pixels = filter.apply(self.get_pixels().astype(np.uint16)).astype(np.uint8)

    def get_pixels(self, standard_size: bool = False) -> np.ndarray:
        """Return the frame's pixel data as a NumPy array.

        Parameters:
            standard_size (bool): If True, returns pixels in uint8 format; otherwise, returns uint16.

        Returns:
            np.ndarray: The pixel data of the frame.
        """
        if standard_size:
            return self._pixels.astype(np.uint8)
        else:
            return self._pixels.astype(np.uint16)

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

    def resize(self, w: int, h: int) -> None:
        """Resize the frame to a new width and height.

        Parameters:
            w (int): The target width.
            h (int): The target height.
        """
        self._pixels = cv2.resize(self._original_pixels, (w, h))
        self._width = w
        self._height = h

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
        """Display the frame in a window for previewing.

        Parameters:
            wait_for_exit (bool): If True, waits until the user closes the window or presses 'q' key.
            title (str): The window title for the preview.
        """
        window_name = title
        cv2.imshow(window_name, self._pixels)
        if wait_for_exit:
            while cv2.getWindowProperty(window_name, cv2.WND_PROP_VISIBLE) >= 1:
                if cv2.waitKey(100) & 0xFF == ord('q'):
                    break
        else:
            cv2.waitKey(1)

    def crop(self, x1: int, y1: int, x2: int, y2: int) -> None:
        """Crop the frame to a rectangle defined by two corners.

        Parameters:
            x1 (int): Left (starting) x-coordinate.
            y1 (int): Top (starting) y-coordinate.
            x2 (int): Right (ending) x-coordinate.
            y2 (int): Bottom (ending) y-coordinate.
        """
        x1 = max(0, min(x1, self._width))
        y1 = max(0, min(y1, self._height))
        x2 = max(0, min(x2, self._width))
        y2 = max(0, min(y2, self._height))

        self._pixels = self._pixels[y1:y2, x1:x2]
        self._height, self._width = self._pixels.shape[:2]


class Color_Frame(Frame):
    """A class to represent and manipulate a frame composed of a single RGB color"""

    def __init__(self, width: int, height: int, color: tuple = (0, 0, 0)):
        """Initialize a Color_Frame with a specified width, height, and an optional RGB color.

        Parameters:
            width (int): The width of the frame in pixels.
            height (int): The height of the frame in pixels.
            color (tuple, optional): A tuple representing the RGB color to fill the frame.
                                      Defaults to (0, 0, 0), which is black.

        Raises:
            ValueError: If the provided color is not a valid RGB tuple with integers between 0 and 255.
        """
        if not (isinstance(color, tuple) and len(color) == 3 and
                all(isinstance(c, int) and 0 <= c <= 255 for c in color)):
            raise ValueError("Color must be a tuple of three integers (R, G, B) between 0 and 255.")

        # Create a NumPy array filled with the specified color
        pixels = np.full((height, width, 3), color, dtype=np.uint8)

        super().__init__(pixels)

    def change_color(self, color: tuple) -> None:
        """Change the entire frame's color to the specified RGB value.

        Parameters:
            color (tuple): A tuple representing the new RGB color to set the frame to.

        Raises:
            ValueError: If the provided color is not a valid RGB tuple with integers between 0 and 255.
        """
        if not (isinstance(color, tuple) and len(color) == 3 and
                all(isinstance(c, int) and 0 <= c <= 255 for c in color)):
            raise ValueError("Color must be a tuple of three integers (R, G, B) between 0 and 255.")

        # Update all pixel values in the frame to the new color
        self._pixels[:] = color



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
        self.__video_path = video_path
        self.open()

        if not self.__video.isOpened():
            print("Error: Could not open video file.")
            print(self.__video_path)
            exit()

        self.__fps = self.__video.get(cv2.CAP_PROP_FPS)
        self.__frame_duration = int(self.__video.get(cv2.CAP_PROP_FRAME_COUNT))  # Total number of frames
        self.__width = int(self.__video.get(cv2.CAP_PROP_FRAME_WIDTH))
        self.__height = int(self.__video.get(cv2.CAP_PROP_FRAME_HEIGHT))

        # Check if the video has an audio stream
        self.audio = None
        if self._has_audio():
            self.audio = Audio(self.__video_path)

    def _has_audio(self):
        """Check if the video file has an audio stream using ffprobe.

        Returns:
            bool: True if the video contains audio, False otherwise.
        """
        check_audio_command = [
            "ffprobe",
            "-i", self.__video_path,
            "-show_streams",
            "-select_streams", "a",
            "-loglevel", "error"
        ]
        result = subprocess.run(check_audio_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return str(result.stdout) != ''

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

        # Set the frame position
        self.__video.set(cv2.CAP_PROP_POS_FRAMES, frame_index)

        # Read the frame
        ret, frame = self.__video.read()
        if ret:
            if h is None and w != 1.0:
                frame = cv2.resize(frame, (0, 0), fx=w, fy=w)
            elif h is not None:
                frame = cv2.resize(frame, (w, h))
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
        return self.audio.frame_audio(index)


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
        """Extract the frames per second (FPS) from a video file using FFmpeg.

        Returns:
            float: The FPS value extracted from the video.

        Raises:
            ValueError: If FFmpeg fails to retrieve valid FPS information.
        """
        command = [
            "ffprobe", "-v", "error",
            "-select_streams", "v:0",
            "-show_entries", "stream=r_frame_rate",
            "-of", "csv=p=0",
            self._file_path
        ]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        fps_data = result.stdout.strip()
        num, denom = map(int, fps_data.split('/'))
        return num / denom

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
        command = [
            "ffmpeg", "-y",
            "-i", self._file_path,
            "-vn" if self._file_type == "mp4" else "",
            "-f", "wav",
            "-ac", "1",
            "-ar", str(self._sample_rate),
            output_audio
        ]
        command = [arg for arg in command if arg]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

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
        sample_rate, audio_data = wavfile.read(audio_path)

        self._sample_rate = sample_rate
        self._duration = len(audio_data) / sample_rate

        if self._file_type != "wav":
            os.remove(audio_path)

        if audio_data.dtype == np.int16:
            audio_data = audio_data / 32768.0

        if len(audio_data) == 0:
            raise ValueError("Audio data is empty. Check the input file for a valid audio track.")

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

    def preload_data(self, reload: bool = False) -> None:
        """Preload and process audio into per-frame features (volume, spectrum) for faster access.

        Parameters:
            reload (bool): Whether to force reloading even if cached data exists.
        """
        cache_file = f"AudioCache/{self._encode_cache_name(self._file_path)}.npy"

        if os.path.isfile(cache_file) and not reload:
            full_audio_data = np.load(cache_file, allow_pickle=True)
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
                yf = fft(frame_audio)
                xf = fftfreq(len(frame_audio), 1 / self._sample_rate)

                positive_frequencies = xf[:len(yf) // 2]
                magnitude = np.abs(yf[:len(yf) // 2])

                self._audio_data.append({
                    "volume": volume,
                    "frequencies": positive_frequencies,
                    "magnitude": magnitude
                })

            self._audio_data.append({
                "duration": self._duration,
                "sample rate": self._sample_rate
            })

            np.save(cache_file, self._audio_data)

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
            return FrameAudio(self._audio_data[frame_index * int(self._fps / renderer.fps())])

    def length(self) -> float:
        """Return the length of the audio in seconds.

        Returns:
            float: Total duration of the audio file.
        """
        if not self._audio_data:
            self._load_audio_data()

        audio_path = self._extract_full_audio()
        sample_rate, audio_data = wavfile.read(audio_path)
        audio_length = len(audio_data) / sample_rate

        if self._file_type != "wav":
            os.remove(audio_path)

        return audio_length

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

    def set_frame(self, frame_index: int, new_frame: 'Frame') -> None:
        """Set a frame at a specific index for the output video.

        Parameters:
            frame_index (int): The index at which the frame will be placed.
            new_frame (Frame): Frame object containing pixel data.

        Raises:
            ValueError: If the frame dimensions do not match the initialized resolution.
        """
        if new_frame.get_pixels(True).shape != (self.__height, self.__width, 3):
            raise ValueError(
                f"Frame dimensions do not match the initialized video resolution. "
                f"Frame dimensions are {new_frame.get_pixels().shape[1]}x{new_frame.get_pixels().shape[0]}, "
                f"but renderer dimensions are {self.__width}x{self.__height}."
            )

        self.__frame_indices.append(frame_index)
        self.__unordered_writer.write(new_frame.get_pixels(True))
        self.__max_frame_index = max(self.__max_frame_index, frame_index)

    def attach_audio(self, audio: 'Audio', volume: float = 1.0) -> None:
        """Attach an Audio object to the renderer with a specified volume adjustment.

        Parameters:
            audio (Audio): The Audio object containing the audio data.
            volume (float, optional): Volume multiplier between 0.0 and 1.0. Defaults to 1.0.

        Raises:
            ValueError: If the volume is not between 0.0 and 1.0.
        """
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

    def render(self, preview: bool = False) -> None:
        """Render the final ordered video and optionally attach audio tracks.

        Parameters:
            preview (bool, optional): Whether to preview each frame during rendering. Defaults to False.

        Raises:
            ValueError: If frames cannot be read during rendering.
        """
        self.__unordered_writer.release()
        ordered_writer = cv2.VideoWriter(
            "silent_render.mp4",
            cv2.VideoWriter_fourcc(*"mp4v"),
            self.__fps,
            (self.__width, self.__height)
        )
        unordered_render = cv2.VideoCapture("unordered_render.mp4")

        for frame_index in range(self.__max_frame_index + 1):
            unordered_idx = self.__get_unordered_frame_idx(frame_index)
            if unordered_idx == -1:
                blank_frame = cv2.UMat(self.__height, self.__width, cv2.CV_8UC3, [0, 0, 0])
                ordered_writer.write(blank_frame)
            else:
                unordered_render.set(cv2.CAP_PROP_POS_FRAMES, unordered_idx)
                ret, frame = unordered_render.read()
                if ret:
                    if preview:
                        Frame(frame).preview()
                    ordered_writer.write(frame)
                else:
                    raise ValueError(f"Could not read frame {frame_index}.")

        unordered_render.release()
        ordered_writer.release()

        if self.__audios:
            self.__attach_audios("silent_render.mp4", self.__audios, "render.mp4")
            print("Video compiled with audio as render.mp4")
        else:
            shutil.copy("silent_render.mp4", "render.mp4")
            print("Video compiled without audio as render.mp4")

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

def set_openai_key(key: str):
    global api_key
    api_key = key

media = {}
renderer = NonlinearRenderer(640, 480, 24)

#replace 'video' with renderer when getting video.height and such

if _paths:  # Ensure _paths is not empty
    # Create a dictionary to store Video objects with their names as keys

    for path in _paths:
        if path:  # Ensure the path is not empty
            # Extract the media name (file name without extension)
            media_full_name = os.path.splitext(os.path.basename(path))
            media_name = media_full_name[0]
            media_type = media_full_name[1]
            if media_type == ".mp4":
                media[media_name] = Video(path)
            elif media_type == ".mp3":
                media[media_name] = Audio(path)

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
        def __init__(self) -> None:
            """Initialize a Field object with a blank, zero-filled map.

            The map is initialized with dimensions based on the renderer and
            stores 8-bit unsigned integer values (0–255).
            """
            self._map = np.zeros((renderer.height(), renderer.width()), dtype=np.uint8)
            self.inverted = False

        def get(self, x: int, y: int) -> np.float16:
            """Retrieve the normalized value at a given coordinate.

            Parameters:
                x (int): X-coordinate (column index).
                y (int): Y-coordinate (row index).

            Returns:
                np.float16: The value at (x, y), normalized to [0, 1].
            """
            return (self._map[y][x] / 255).astype(np.float16)

        def set(self, value: float, x: int, y: int) -> None:
            """Set a normalized value at a given coordinate.

            Parameters:
                value (float): Value between 0 and 1.
                x (int): X-coordinate.
                y (int): Y-coordinate.
            """
            self._map[y][x] = int(value * 255)

        def add(self, other: 'Field' or float) -> 'Field':
            """Add another Field or a scalar to this Field.

            Parameters:
                other (Field or float): Field or scalar value to add.

            Returns:
                Field: The updated Field object.
            """
            if isinstance(other, Field):
                return self.add(other.get_map())
            self._map = np.clip(self._map.astype(np.int16) + other * 255, 0, 255).astype(np.uint8)
            return self

        def sub(self, other: 'Field' or float) -> 'Field':
            """Subtract another Field or a scalar from this Field.

            Parameters:
                other (Field or float): Field or scalar value to subtract.

            Returns:
                Field: The updated Field object.
            """
            if isinstance(other, Field):
                return self.sub(other.get_map())
            self._map = self.add(other * -1)
            return self

        def mult(self, other: 'Field' or float) -> 'Field':
            """Multiply this Field by another Field or scalar.

            Parameters:
                other (Field or float): Field or scalar multiplier.

            Returns:
                Field: The updated Field object.
            """
            if isinstance(other, Field):
                return self.mult(other.get_map())
            self._map = np.clip(self._map.astype(np.int16) * other, 0, 255).astype(np.uint8)
            return self

        def div(self, other: 'Field' or float) -> 'Field':
            """Divide this Field by another Field or scalar.

            Parameters:
                other (Field or float): Field or scalar divisor.

            Returns:
                Field: The updated Field object.
            """
            if isinstance(other, Field):
                return self.div(other.get_map())
            self._map = np.clip(self._map.astype(np.int16) / other, 0, 255).astype(np.uint8)
            return self

        def __add__(self, other: 'Field' or float) -> 'Field':
            """Return a new Field representing this + other.

            Parameters:
                other (Field or float): Field or scalar value to add.

            Returns:
                Field: A new Field with the result.
            """
            clone = copy.deepcopy(self)
            return clone.add(other)

        def __sub__(self, other: 'Field' or float) -> 'Field':
            """Return a new Field representing this - other.

            Parameters:
                other (Field or float): Field or scalar value to subtract.

            Returns:
                Field: A new Field with the result.
            """
            return self.__add__(other * -1)

        def __mul__(self, other: 'Field' or float) -> 'Field':
            """Return a new Field representing this * other.

            Parameters:
                other (Field or float): Field or scalar multiplier.

            Returns:
                Field: A new Field with the result.
            """
            clone = copy.deepcopy(self)
            return clone.mult(other)

        def __truediv__(self, other: 'Field' or float) -> 'Field':
            """Return a new Field representing this / other.

            Parameters:
                other (Field or float): Field or scalar divisor.

            Returns:
                Field: A new Field with the result.
            """
            clone = copy.deepcopy(self)
            clone.set_map(np.clip(self._map.astype(np.float16) / other, 0, 255).astype(np.uint8))
            return clone

        def invert(self) -> 'Field':
            """Invert the field, flipping 0s to 255s and vice versa.

            Returns:
                Field: The updated Field object.
            """
            self._map = 255 - self._map
            self.inverted = not self.inverted
            return self

        def move(self, dx: int, dy: int) -> 'Field':
            """Translate the field by (dx, dy) using an affine transform.

            Parameters:
                dx (int): Horizontal shift (positive is right).
                dy (int): Vertical shift (positive is down).

            Returns:
                Field: The updated Field object.

            Raises:
                ValueError: If the image exceeds OpenCV's size limits.
            """
            height, width = self._map.shape[:2]

            if width >= 32000 or height >= 32000:
                raise ValueError("Image too large for OpenCV warpAffine.")

            translation_matrix = np.float32([[1, 0, dx], [0, 1, dy]])
            self._map = cv2.warpAffine(
                self._map, translation_matrix, (width, height),
                borderMode=cv2.BORDER_CONSTANT,
                borderValue=255 if self.inverted else 0
            )
            return self

        def resize(self, width_or_scale: int or float, height: int = None) -> 'Field':
            """Resize the field to specific dimensions or by a scale factor.

            Parameters:
                width_or_scale (int or float): Target width or a scale factor.
                height (int, optional): Target height (only if resizing by explicit dimensions).

            Returns:
                Field: The resized Field object.
            """
            if isinstance(width_or_scale, (int, float)) and height is None:
                scale = width_or_scale
                width = int(self._map.shape[1] * scale)
                height = int(self._map.shape[0] * scale)
            else:
                width = int(width_or_scale)
                height = int(height)

            fill_value = 255 if self.inverted else 0

            if width > self._map.shape[1] or height > self._map.shape[0]:
                new_map = np.full((height, width), fill_value, dtype=np.uint8)
                overlap_x_end = min(self._map.shape[1], width)
                overlap_y_end = min(self._map.shape[0], height)
                new_map[:overlap_y_end, :overlap_x_end] = self._map[:overlap_y_end, :overlap_x_end]
                self._map = new_map
            else:
                self._map = cv2.resize(self._map, (width, height), interpolation=cv2.INTER_AREA)

            return self

        def scale(self, scale_x: float, scale_y: float = None) -> 'Field':
            """Scale the field by a factor along the x and y axes.

            Parameters:
                scale_x (float): Scaling factor for width.
                scale_y (float, optional): Scaling factor for height (defaults to scale_x if None).

            Returns:
                Field: The scaled Field object.

            Raises:
                ValueError: If any scale factor is non-positive.
            """
            if scale_x <= 0 or (scale_y is not None and scale_y <= 0):
                raise ValueError("Scale factors must be positive.")

            if scale_y is None:
                scale_y = scale_x

            new_width = int(self._map.shape[1] * scale_x)
            new_height = int(self._map.shape[0] * scale_y)
            self._map = cv2.resize(self._map, (new_width, new_height), interpolation=cv2.INTER_LINEAR)
            return self

        def fit(self) -> 'Field':
            """Stretch the region containing active values to fill the canvas.

            Returns:
                Field: The updated Field object.
            """
            target_value = 0 if self.inverted else 255
            coords = cv2.findNonZero((self._map == target_value).astype(np.uint8))

            if coords is None:
                return self

            x, y, w, h = cv2.boundingRect(coords)
            roi = self._map[y:y+h, x:x+w]
            resized_roi = cv2.resize(roi, (self._map.shape[1], self._map.shape[0]), interpolation=cv2.INTER_LINEAR)

            self._map = resized_roi
            if not self.inverted:
                self._map[self._map != 255] = 0
            else:
                self._map[self._map != 0] = 255

            return self

        def preview(self, wait_for_exit: bool = False, title: str = "Field Preview") -> None:
            """Display the field using OpenCV.

            Parameters:
                wait_for_exit (bool): Whether to wait for a user key press before exiting.
                title (str): Title of the display window.
            """
            window_name = title
            cv2.imshow(window_name, self._map)
            if wait_for_exit:
                while cv2.getWindowProperty(window_name, cv2.WND_PROP_VISIBLE) >= 1:
                    if cv2.waitKey(100) & 0xFF == ord('q'):
                        break
            else:
                cv2.waitKey(1)

        def get_map(self) -> np.ndarray:
            """Get the normalized field map.

            Returns:
                np.ndarray: The map normalized to [0, 1].
            """
            return self._map.astype(np.float16) / 255

        def set_map(self, map: np.ndarray) -> None:
            """Set the internal map.

            Parameters:
                map (np.ndarray): New map data (must match expected shape).
            """
            self._map = map

        def blur(self, param: tuple = (5, 5)) -> 'Field':
            """Apply a blur to the field.

            Parameters:
                param (tuple): Kernel size for blurring.

            Returns:
                Field: The blurred Field object.
            """
            self._map = cv2.blur(self._map, param)
            return self

        def mirror_x(self) -> 'Field':
            """Mirror the field along the vertical (X) axis.

            Returns:
                Field: The mirrored Field object.
            """
            self._map = cv2.flip(self._map, 1)
            return self

        def mirror_y(self) -> 'Field':
            """Mirror the field along the horizontal (Y) axis.

            Returns:
                Field: The mirrored Field object.
            """
            self._map = cv2.flip(self._map, 0)
            return self

        def crop(self, top_left: tuple, bottom_right: tuple) -> 'Field':
            """Crop the field to a rectangle defined by two points.

            Parameters:
                top_left (tuple): (x, y) coordinates for the top-left corner.
                bottom_right (tuple): (x, y) coordinates for the bottom-right corner.

            Returns:
                Field: The cropped Field object.
            """
            x1, y1 = map(int, top_left)
            x2, y2 = map(int, bottom_right)

            x1, x2 = max(0, min(x1, self._map.shape[1])), max(0, min(x2, self._map.shape[1]))
            y1, y2 = max(0, min(y1, self._map.shape[0])), max(0, min(y2, self._map.shape[0]))

            if x1 > x2:
                x1, x2 = x2, x1
            if y1 > y2:
                y1, y2 = y2, y1

            self._map = self._map[y1:y2, x1:x2]
            return self


    class FOverlay(Field):
        def __init__(self, opacity: float = 1.0) -> None:
            """Initialize an FOverlay object with a uniform opacity field.

            The overlay is initialized to a constant opacity across the entire canvas.
            Opacity should be a value between 0 (fully transparent) and 1 (fully opaque).

            Parameters:
                opacity (float, optional): Initial opacity value for the overlay.
                    Defaults to 1.0 (fully opaque).

            Raises:
                ValueError: If opacity is not within the range [0, 1].
            """
            if not (0.0 <= opacity <= 1.0):
                raise ValueError(f"Opacity must be between 0 and 1, but got {opacity}.")

            super().__init__()
            self._map = np.full(
                (renderer.height(), renderer.width()),
                int(opacity * 255),
                dtype=np.uint8
            )

    class FLine(Field):
        def __init__(self, x1: float, y1: float, x2: float, y2: float, thickness: float) -> None:
            """Initialize an FLine object that draws a straight line on the field.

            Creates a binary line between two points with a specified thickness.
            The line is drawn onto the field's internal map immediately upon initialization.

            Parameters:
                x1 (float): X-coordinate of the start point.
                y1 (float): Y-coordinate of the start point.
                x2 (float): X-coordinate of the end point.
                y2 (float): Y-coordinate of the end point.
                thickness (float): Thickness of the line in pixels.

            Raises:
                ValueError: If thickness is not a positive number.
            """
            if thickness <= 0:
                raise ValueError(f"Thickness must be a positive number, but got {thickness}.")

            super().__init__()
            cv2.line(
                self._map,
                (int(x1), int(y1)),
                (int(x2), int(y2)),
                color=255,
                thickness=int(thickness)
            )


    class FRect(Field):
        def __init__(self, x1: float, y1: float, x2: float, y2: float, thickness: int = -1) -> None:
            """Initialize an FRect object that draws a rectangle on the field.

            Creates a rectangle between two points, with customizable thickness.
            By default, the rectangle is filled if thickness is set to -1.

            Parameters:
                x1 (float): X-coordinate of the top-left corner.
                y1 (float): Y-coordinate of the top-left corner.
                x2 (float): X-coordinate of the bottom-right corner.
                y2 (float): Y-coordinate of the bottom-right corner.
                thickness (int, optional): Thickness of the rectangle border.
                    - Set to -1 to fill the rectangle. Defaults to -1.

            Raises:
                ValueError: If thickness is not -1 and is less than or equal to 0.
            """
            if thickness != -1 and thickness <= 0:
                raise ValueError(f"Thickness must be a positive number or -1 to fill, but got {thickness}.")

            super().__init__()
            cv2.rectangle(
                self._map,
                (int(x1), int(y1)),
                (int(x2), int(y2)),
                color=255,
                thickness=int(thickness)
            )


    class FEllipse(Field):
        def __init__(
                self,
                center: tuple[float, float],
                ellipse_width: float,
                ellipse_height: float,
                angle: float = 0,
                thickness: int = -1
        ) -> None:
            """Initialize an FEllipse object that draws an ellipse on the field.

            Creates an ellipse centered at a given point with specified width, height,
            rotation angle, and border thickness. By default, the ellipse is filled
            if thickness is set to -1.

            Parameters:
                center (tuple[float, float]): (x, y) coordinates for the center of the ellipse.
                ellipse_width (float): Total width of the ellipse's bounding box.
                ellipse_height (float): Total height of the ellipse's bounding box.
                angle (float, optional): Rotation angle of the ellipse in degrees.
                    Defaults to 0 (no rotation).
                thickness (int, optional): Thickness of the ellipse border.
                    - Set to -1 to fill the ellipse (default).

            Raises:
                ValueError: If thickness is not -1 and is less than or equal to 0.
            """
            if thickness != -1 and thickness <= 0:
                raise ValueError(f"Thickness must be a positive number or -1 to fill, but got {thickness}.")

            super().__init__()

            # Convert total width and height into semi-axes
            axes = (int(ellipse_width // 2), int(ellipse_height // 2))

            # Draw the ellipse onto the map
            cv2.ellipse(
                self._map,
                (int(center[0]), int(center[1])),
                axes,
                angle,
                0, 360,  # Cover the full 360 degrees
                255,     # White color
                thickness
            )


    class FPoly(Field):
        def __init__(self, points: np.ndarray) -> None:
            """Initialize an FPoly object that draws a filled polygon on the field.

            Takes a set of points and fills a polygon based on their coordinates.
            The polygon will be drawn in white (value 255) onto the field map.

            Parameters:
                points (np.ndarray): A NumPy array of shape (N, 2) containing (x, y) coordinates
                    for the vertices of the polygon.

                    - N must be at least 3 to form a valid polygon.
                    - Points are automatically reshaped as required by OpenCV.

            Raises:
                ValueError: If fewer than 3 points are provided.
            """
            if points.shape[0] < 3:
                raise ValueError(f"A polygon requires at least 3 points, but received {points.shape[0]}.")

            super().__init__()

            # Reshape points to (N, 1, 2) as expected by OpenCV
            points = points.reshape((-1, 1, 2)).astype(np.int32)

            # Fill the polygon on the map
            cv2.fillPoly(self._map, [points], color=255)


    class FText(Field):
        def __init__(
                self,
                text: str,
                position: tuple,
                font_scale: float,
                thickness: int = 1,
                custom_font: str = None
        ) -> None:
            """Initialize an FText object that renders text onto the field map.

            Allows rendering text either using a custom TrueType font (via Pillow) or using
            OpenCV's built-in fonts. The text is automatically center-aligned based on the
            provided position.

            Parameters:
                text (str): The text string to render.
                position (tuple): A tuple (x, y) representing the center position for the text.
                font_scale (float): Scale factor to size the text.
                thickness (int, optional): Thickness of the text stroke (default is 1).
                custom_font (str, optional): Path to a custom TTF font file.
                    If None, OpenCV's default font is used.

            Raises:
                FileNotFoundError: If a custom font path is provided but the file cannot be found.
            """
            super().__init__()

            if custom_font:
                self._draw_with_pillow(text, position, font_scale, custom_font)
            else:
                self._draw_with_opencv(text, position, font_scale, thickness)

        def _draw_with_pillow(
                self,
                text: str,
                position: tuple,
                font_scale: float,
                custom_font: str
        ) -> None:
            """Render text using Pillow with a custom TrueType font.

            Converts the internal field map to a Pillow image, draws the text,
            and converts it back to a NumPy array.

            Parameters:
                text (str): Text to render.
                position (tuple): Center position (x, y) for the text.
                font_scale (float): Scale factor for the font size.
                custom_font (str): Path to a .ttf font file.
            """
            pil_image = Image.fromarray(self._map)
            draw = ImageDraw.Draw(pil_image)

            try:
                font_size = int(font_scale * 20)
                font = ImageFont.truetype(custom_font, font_size)
            except IOError:
                raise FileNotFoundError(f"Custom font file '{custom_font}' not found or could not be opened.")

            text_bbox = font.getbbox(text)  # (left, top, right, bottom)
            text_width = text_bbox[2] - text_bbox[0]
            text_height = text_bbox[3] - text_bbox[1]

            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] - text_height / 2)

            draw.text((bottom_left_x, bottom_left_y), text, font=font, fill=255)
            self._map = np.array(pil_image)

        def _draw_with_opencv(
                self,
                text: str,
                position: tuple,
                font_scale: float,
                thickness: int
        ) -> None:
            """Render text using OpenCV's built-in font.

            Parameters:
                text (str): Text to render.
                position (tuple): Center position (x, y) for the text.
                font_scale (float): Scale factor for the font size.
                thickness (int): Stroke thickness for the text.
            """
            (text_width, text_height), baseline = cv2.getTextSize(
                text, cv2.FONT_HERSHEY_SIMPLEX, font_scale, thickness
            )

            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] + text_height / 2)

            cv2.putText(
                self._map,
                text,
                (bottom_left_x, bottom_left_y),
                cv2.FONT_HERSHEY_SIMPLEX,
                font_scale,
                255,
                thickness,
                lineType=cv2.LINE_AA
            )




    class FAudio(Field):
        def __init__(self, aud: FrameAudio, start: int = 0, end: int = None) -> None:
            """Initialize an FAudio object to visualize the audio frame data on the field map.

            This class generates a visualization of the audio frame's magnitudes within the
            specified frequency range. It creates bars corresponding to the frequencies and
            displays a volume indicator based on the RMS volume of the frame.

            Parameters:
                aud (FrameAudio): The FrameAudio object containing the audio data (frequencies and magnitudes).
                start (int, optional): The starting index of the frequency range to visualize (default is 0).
                end (int, optional): The ending index of the frequency range to visualize. If None, uses the full range.

            Raises:
                ValueError: If the start or end indices are invalid.
                Exception: If an error occurs during the visualization process.
            """
            super().__init__()

            try:
                freqs = aud.list_frequencies()
                mags = aud.list_magnitudes()

                # Handle start and end indices, adjust for the frequency bin width
                if end is None:
                    end = len(freqs)
                else:
                    end = int(end / (freqs[1] - freqs[0]))  # Convert to index

                start = int(start / (freqs[1] - freqs[0]))  # Convert to index

                if end > len(freqs):
                    end = len(freqs)
                if start < 0 or start >= len(freqs):
                    raise ValueError(f"Invalid range: start={start}, end={end}")

                # Normalize the magnitudes for visualization
                norm = max(mags) / renderer.height()
                if norm == 0 or np.isnan(norm) or np.isinf(norm):
                    print(f"Normalization error, mags={mags}")
                    return

                # Create the points for frequency bars
                total_bars = end - start
                bar_width = renderer.width() / total_bars
                points = []

                for i in range(start, end):
                    x = i * bar_width + bar_width / 2
                    y = renderer.height() - mags[i] / norm
                    points.extend([x, y])

                # Add the base of the visualization (polygon to close the bars)
                points.extend([renderer.width() - bar_width, renderer.height(), 0, renderer.height()])
                self.add(FPoly(np.array(points, dtype=np.float32)))

                # Add the volume indicator as a rectangle
                self.add(FRect(
                    renderer.width() - bar_width,
                    renderer.height() - aud.get_volume() * renderer.height(),
                    renderer.width(),
                    renderer.height()
                ))

            except ValueError as ve:
                print(f"Error in FAudio initialization: {ve}")
            except Exception as e:
                print(f"Unexpected error initializing FAudio: {e}")


if len(_paths) != 0:
    class Filter:
        """A filter that aligns a Field object with the current renderer dimensions."""

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
            """Set a new Field and crop its map to match the renderer dimensions.

            Parameters:
                field (Field): The Field object containing the source map.

            Returns:
                Filter: The Filter instance itself (for method chaining).
            """
            self.field = field
            full_map = self.field.get_map()[:, :, np.newaxis]

            render_height = renderer.height()
            render_width = renderer.width()

            orig_height, orig_width = full_map.shape[:2]

            start_y = max((orig_height - render_height) // 2, 0)
            start_x = max((orig_width - render_width) // 2, 0)
            end_y = start_y + min(render_height, orig_height)
            end_x = start_x + min(render_width, orig_width)

            self._map = full_map[start_y:end_y, start_x:end_x]

            return self

        def _apply(self, pixels: np.ndarray) -> None:
            """Apply the filter to a set of pixels.

            Parameters:
                pixels (np.ndarray): The pixel data to process.

            Note:
                This method is currently a placeholder and must be implemented.
            """
            pass

    class Solid_Color(Filter):
        """A filter that overlays a solid color onto a Field-based mask."""

        def __init__(self, r: int, g: int, b: int, field: Optional[Field] = None) -> None:
            """Initialize a Solid_Color filter.

            Parameters:
                r (int): Red component of the color (0–255).
                g (int): Green component of the color (0–255).
                b (int): Blue component of the color (0–255).
                field (Optional[Field]): Field object providing the overlay mask.
                    If None, a default FOverlay() field will be used.
            """
            super().__init__(field)
            self.__r = r
            self.__g = g
            self.__b = b

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
            """Apply the solid color filter to pixel data using the field mask.

            Parameters:
                pixels (np.ndarray): The pixel data to apply the color filter to.

            Returns:
                np.ndarray: The color-modified pixel data.
            """
            pixels = np.clip(
                (pixels * (1 - self._map) + [self.__b, self.__g, self.__r] * self._map),
                0, 255
            )
            return pixels

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
            # Invert the pixels
            inverted_pixels = 255 - pixels

            # Blend based on the map values
            filtered_pixels = (1 - self._map) * pixels + self._map * inverted_pixels

            return np.clip(filtered_pixels, 0, 255)

    class Draw_Frame(Filter):
        """A filter that draws a frame onto an image at a specified position."""

        def __init__(self, frame: Frame, x: Optional[int] = None, y: Optional[int] = None, field: Optional[Field] = None) -> None:
            """Initialize a Draw_Frame filter.

            Parameters:
                frame (Frame): The Frame object that will be drawn onto the image.
                x (Optional[int]): The x-coordinate for positioning the frame. If None, the frame is centered.
                y (Optional[int]): The y-coordinate for positioning the frame. If None, the frame is centered.
                field (Optional[Field]): Field object providing the overlay mask. Defaults to FOverlay() if None.
            """
            if field is None:
                field = FOverlay()
            super().__init__(field)
            self.frame = frame
            self.x = x
            self.y = y

        def apply(self, pixels: np.ndarray) -> np.ndarray:
            """Apply the frame to the given pixels at the specified (x, y) position.

            If `x` and `y` are None, the frame is centered. If the frame extends beyond the image bounds, it is cropped.
            Any uncovered space is filled with the original pixels.

            Parameters:
                pixels (np.ndarray): The pixel data onto which the frame will be applied.

            Returns:
                np.ndarray: The image with the frame applied at the specified position, blended with the field map.
            """
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
            return np.clip(self._map * new_frame + (1 - self._map) * pixels, 0, 255)

        def set_position(self, x: int, y: int) -> 'Draw_Frame':
            """Updates the frame's position.

            Parameters:
                x (int): The new x-coordinate for the frame.
                y (int): The new y-coordinate for the frame.

            Returns:
                Draw_Frame: The current instance with updated position.
            """
            self.x = x
            self.y = y
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



