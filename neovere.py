from noise import pnoise2
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


try:
    import cv2
except ImportError:
    print("Error: OpenCV is not installed. Please install it using `pip install opencv-python`.")
    exit(1)

fourcc = cv2.VideoWriter_fourcc(*'mp4v')

_paths = ["render.mp4", "/home/lukebalfanz/Downloads/parkour.mp4"]
arial = "/tmp/arial-bold.ttf"
class Pixel:
    def __init__(self, r: int, g: int, b: int):
        self.r = r
        self.g = g
        self.b = b

    def __str__(self):
        return f"({self.r}, {self.g}, {self.b})"

    def __add__(self, other):
        if isinstance(other, Pixel):
            return Pixel(max(255, self.r + other.r), max(255, self.g + other.g), max(255, self.b + other.b))
        if isinstance(other, (int, float)):
            return self.__add__(Pixel(other, other, other))
    def __sub__(self, other):
        if isinstance(other, Pixel):
            return Pixel(min(0, self.r - other.r), min(0, self.g - other.g), min(0, self.b - other.b))
        if isinstance(other, (int, float)):
            return self.__sub__(Pixel(other, other, other))
    def __mul__(self, other):
        if isinstance(other, Pixel):
            return Pixel(max(255, self.r * other.r), max(255, self.g * other.g), max(255, self.b * other.b))
        if isinstance(other, (int, float)):
            return self.__mul__(Pixel(other, other, other))
    def __truediv__(self, other):
        if isinstance(other, Pixel):
            return Pixel(min(0, self.r / other.r), min(0, self.g / other.g), min(0, self.b / other.b))
        if isinstance(other, (int, float)):
            return self.__truediv__(Pixel(other, other, other))



class Frame:
    def __init__(self, pixels: np.ndarray):
        """
        Initialize the Frame class with a NumPy array representing pixels.
        :param pixels: A 2D or 3D NumPy array representing the frame.
        """
        if not isinstance(pixels, np.ndarray):
            raise ValueError("Frame must be initialized with a NumPy array.")

        # Ensure it's a 2D or 3D array (e.g., grayscale or color image)
        if pixels.ndim not in (2, 3):
            raise ValueError("Frame must be a 2D (grayscale) or 3D (color) array.")

        self._pixels = pixels.astype(np.uint8)
        self._original_pixels = pixels.astype(np.uint8)
        self._height, self._width = self._pixels.shape[:2]

    def __str__(self):
        """String representation for debugging."""
        return f"Frame with shape {self.get_pixels().shape}"

    def apply_filter(self, filter):
        self._pixels = filter.apply(self.get_pixels().astype(np.uint16)).astype(np.uint8)

    def get_pixels(self, standard_size = False):
        if standard_size:
            return self._pixels.astype(np.uint8)
        else:
            return self._pixels.astype(np.uint16)

    def modify(self, func):
        """
        Apply a user-defined function to the frame's pixels.
        :param func: A function that takes x, y, and the current pixel (as a numpy array) and returns a new pixel.
        """
        height, width, _ = self.get_pixels().shape

        # Generate x and y coordinate grids
        x_coords, y_coords = np.meshgrid(np.arange(width), np.arange(height))

        # Prepare a flattened view for efficient mapping
        flat_pixels = self.get_pixels().reshape(-1, 3)
        flat_x_coords = x_coords.flatten()
        flat_y_coords = y_coords.flatten()

        # Apply the function vectorized
        new_flat_pixels = np.array([
            func(x, y, pixel)
            for x, y, pixel in zip(flat_x_coords, flat_y_coords, flat_pixels)
        ])

        # Reshape back to the original frame shape
        self._pixels = new_flat_pixels.reshape(height, width, 3).astype(np.uint8)

    def resize(self, w: int, h: int):
        self._pixels = cv2.resize(self._original_pixels, (w, h))
        self._width = w
        self._height = h

    def set_width(self, w: int):
        self.resize(w, self.height())

    def set_height(self, h: int):
        self.resize(self.width(), h)

    def width(self):
        return self._width

    def height(self):
        return self._height

    def preview(self, wait_for_exit: bool = False, title: str = "Frame Preview"):
        # Show the frame (optional)
        window_name = title
        cv2.imshow(window_name, self._pixels)
        if (wait_for_exit):
            # Keep checking if the window is closed
            while cv2.getWindowProperty(window_name, cv2.WND_PROP_VISIBLE) >= 1:
                if cv2.waitKey(100) & 0xFF == ord('q'):  # Allow 'q' to close the window as well
                    break
        else:
            cv2.waitKey(1)

    def crop(self, x1: int, y1: int, x2: int, y2: int):
        # Ensure the coordinates are within bounds
        x1 = max(0, min(x1, self._width))
        y1 = max(0, min(y1, self._height))
        x2 = max(0, min(x2, self._width))
        y2 = max(0, min(y2, self._height))

        # Crop the pixels
        self._pixels = self._pixels[y1:y2, x1:x2]

        # Update width and height
        self._height, self._width = self._pixels.shape[:2]


class Video:
    def __init__(self, video_path: str):
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

        self.audio = Audio(self.__video_path)

    def open(self):
        """Open the video file."""
        self.__video = cv2.VideoCapture(self.__video_path)

    def close(self):
        """Release the video file."""
        self.__video.release()

    def get_frame(self, frame_index: int, w = 1.0, h = None):
        """Retrieve a specific frame by index."""
        if frame_index < 0 or frame_index >= self.__frame_duration:
            raise ValueError(f"Frame index {frame_index} is out of bounds (0 to {self._frame_duration - 1}).")

        # Set the frame position
        self.__video.set(cv2.CAP_PROP_POS_FRAMES, frame_index)

        # Read the frame
        ret, frame = self.__video.read()
        if ret:
            if h == None and w != 1.0:
                frame = cv2.resize(frame, (0, 0), fx=w, fy=w)
            elif h != None:
                frame = cv2.resize(frame, (w, h))
            return Frame(frame)
        else:
            raise ValueError(f"Frame {frame_index} could not be read. The video may be closed.")

    def frame_duration(self):
        """Get the total number of frames."""
        return self.__frame_duration

    def fps(self):
        """Get the frames per second (FPS) of the video."""
        return self.__fps

    def width(self):
        return self.__width

    def height(self):
        return self.__height

    def resize(self, w: int, h: int):
        self._pixels = cv2.resize(self._original_pixels, (w, h))
        self.__width = w
        self.__height = h

    def set_width(self, w: int):
        self.resize(w, self.height())

    def set_height(self, h: int):
        self.resize(self.width(), h)


    def frame_audio(self, index: int):
        return self.audio.frame_audio(index)



    def apply_filter(self, start: int, end: int):
        # get 4D array of frames
        # get 4D array of filters

        pass

class Audio:
    def __init__(self, file_path):
        """
        Initialize the Audio object with the path to the video, audio, or WAV file.
        """
        self.file_path = file_path
        self.file_type = self._determine_file_type()
        self.fps = self._get_fps() if self.file_type == "mp4" else None
        self.sample_rate = 44100  # Standard audio sample rate
        self._audio_data = None
        self._loaded = False

    def _determine_file_type(self):
        """
        Determine the file type based on the extension.
        """
        if self.file_path.endswith(".mp4"):
            return "mp4"
        elif self.file_path.endswith(".mp3"):
            return "mp3"
        elif self.file_path.endswith(".wav"):
            return "wav"
        else:
            raise ValueError("Unsupported file type. Supported types are: mp4, mp3, wav.")

    def _get_fps(self):
        """
        Use FFmpeg to extract the frames per second (FPS) of the video.
        """
        command = [
            "ffprobe", "-v", "error",
            "-select_streams", "v:0",
            "-show_entries", "stream=r_frame_rate",
            "-of", "csv=p=0",
            self.file_path
        ]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        fps_data = result.stdout.strip()
        num, denom = map(int, fps_data.split('/'))
        return num / denom

    def _extract_full_audio(self):
        """
        Extract the entire audio track from the file using FFmpeg.
        For WAV files, return the path directly.
        """
        if self.file_type == "wav":
            return self.file_path

        output_audio = "full_audio.wav"
        command = [
            "ffmpeg", "-y",  # Overwrite existing files
            "-i", self.file_path,  # Input file
            "-vn" if self.file_type == "mp4" else "",  # Exclude video for MP4 files
            "-f", "wav",  # Force WAV format for output
            "-ac", "1",  # Mono audio
            "-ar", str(self.sample_rate),  # Set sampling rate
            output_audio
        ]
        # Filter out empty strings in the command list
        command = [arg for arg in command if arg]

        # Run FFmpeg command and capture output
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        # Log FFmpeg's output for debugging
        if result.returncode != 0:
            print("FFmpeg Error:", result.stderr)
            raise ValueError("FFmpeg failed to convert the file. Check the input format and file path.")

        # Ensure the audio file is created and has data
        if not os.path.isfile(output_audio) or os.path.getsize(output_audio) == 0:
            raise ValueError("Failed to extract audio. The file is empty or not created.")

        return output_audio



    def _load_audio_data(self):
        """
        Load the full audio data into memory from the extracted audio file.
        """
        audio_path = self._extract_full_audio()
        sample_rate, audio_data = wavfile.read(audio_path)

        # Clean up temporary file if it was created
        if self.file_type != "wav":
            os.remove(audio_path)

        # Normalize audio if in 16-bit PCM format
        if audio_data.dtype == np.int16:
            audio_data = audio_data / 32768.0

        # Ensure audio data is not empty
        if len(audio_data) == 0:
            raise ValueError("Audio data is empty. Check the input file for a valid audio track.")

        return audio_data

    def preload_data(self, frame_duration: int = None, reload: bool = False):
        """
        Preload audio data for all frames or the entire audio duration (if it's an MP3 or WAV).

        Parameters:
        - frame_duration (int): Total number of frames in the video (ignored for MP3 and WAV files).
        - reload (bool): Force reloading and recalculation of audio data.
        """
        cache_file = f"AudioCache/{self._encode_cache_name(self.file_path)}.npy"

        if os.path.isfile(cache_file) and not reload:
            # Load preprocessed data from cache
            self._audio_data = np.load(cache_file, allow_pickle=True)
        else:
            # Extract and preprocess audio data
            audio_data = self._load_audio_data()

            if self.fps:
                # Process audio for MP4
                samples_per_frame = int(self.sample_rate / self.fps)
                self._audio_data = []

                for i in range(frame_duration):
                    start_idx = i * samples_per_frame
                    end_idx = start_idx + samples_per_frame

                    # Adjust indices to ensure they stay within bounds
                    if start_idx >= len(audio_data):
                        break  # Stop if the start index exceeds the audio data length
                    if end_idx > len(audio_data):
                        end_idx = len(audio_data)  # Clamp end index to the audio length

                    frame_audio = audio_data[start_idx:end_idx]

                    # Handle cases where frame_audio is empty (e.g., last frame)
                    if len(frame_audio) == 0:
                        break

                    # Compute volume and frequency spectrum
                    volume = np.sqrt(np.mean(frame_audio ** 2))
                    yf = fft(frame_audio)
                    xf = fftfreq(len(frame_audio), 1 / self.sample_rate)

                    positive_frequencies = xf[:len(yf) // 2]
                    magnitude = np.abs(yf[:len(yf) // 2])

                    self._audio_data.append({
                        "volume": volume,
                        "frequencies": positive_frequencies,
                        "magnitude": magnitude
                    })
            else:
                # Process audio for MP3 and WAV
                volume = np.sqrt(np.mean(audio_data ** 2))
                yf = fft(audio_data)
                xf = fftfreq(len(audio_data), 1 / self.sample_rate)

                positive_frequencies = xf[:len(yf) // 2]
                magnitude = np.abs(yf[:len(yf) // 2])

                self._audio_data = [{
                    "volume": volume,
                    "frequencies": positive_frequencies,
                    "magnitude": magnitude
                }]

            # Save preprocessed data to cache
            np.save(cache_file, self._audio_data)

        self._loaded = True

    def _encode_cache_name(self, path: str) -> str:
        """
        Encodes a file path into an alphanumeric string.
        Non-alphanumeric characters are replaced with _ASCII_CODE_ format.
        """
        encoded = ""
        for char in path:
            if char.isalnum():
                encoded += char
            else:
                encoded += f"_{ord(char)}_"
        return encoded

    def frame_audio(self, frame_index):
        """
        Get preloaded audio data for a specific frame (or the entire audio for MP3 and WAV).

        Parameters:
        - frame_index (int): The index of the frame (ignored for MP3 and WAV).

        Returns:
        - dict: A dictionary containing 'volume', 'frequencies', and 'magnitude'.
        """
        if not self._loaded:
            raise ValueError("Audio data not preloaded. Call `preload_data()` first.")

        if self.fps:
            # Handle MP4 frame-specific data
            if frame_index < len(self._audio_data):
                return self._audio_data[frame_index]
            else:
                return self._audio_data[-1]  # Return last frame data for out-of-bound indices
        else:
            # Handle MP3 and WAV single audio data
            return self._audio_data[0]

    def length(self):
        """
        Get the length of the audio file in seconds.

        Returns:
        - float: The length of the audio file in seconds.
        """
        if not self._audio_data:
            self._load_audio_data()

        audio_path = self._extract_full_audio()
        sample_rate, audio_data = wavfile.read(audio_path)

        # Calculate the length of the audio
        audio_length = len(audio_data) / sample_rate

        # Clean up temporary file if it was created
        if self.file_type != "wav":
            os.remove(audio_path)

        return audio_length

class Frame_Audio:
    def __init__(self, audio_data):
        self._audio_data = audio_data
        self.__freqs = self._audio_data["frequencies"]

    def get_volume(self):
        return self._audio_data["volume"]

    def get_frequency(self, freq: int):
        return self._audio_data["magnitude"][int(freq/(self.__freqs[1]-self.__freqs[0]))]

    def list_frequencies(self):
        return self.__freqs

    def list_magnitudes(self):
        return self._audio_data["magnitude"]

import cv2
import subprocess
import os


class NonlinearRenderer:
    def __init__(self, width: int, height: int, fps: int):
        self.__width = width
        self.__height = height
        self.__fps = fps
        self.__frame_indices = []
        self.__unordered_writer = cv2.VideoWriter(
            "unordered_render.mp4",
            cv2.VideoWriter_fourcc(*"mp4v"),
            self.__fps,
            (self.__width, self.__height)
        )
        self.__max_frame_index = -1  # To track the highest frame index
        self.__audio = None  # Placeholder for Audio object

    def set_frame(self, frame_index: int, new_frame):

        if new_frame.get_pixels(True).shape != (self.__height, self.__width, 3):
            raise ValueError("Frame dimensions do not match the initialized video resolution. "+
                             "Frame dimensions are "+str(len(new_frame.get_pixels()[0]))+"x"+str(len(new_frame.get_pixels()))+
                             " but renderer dimensions are "+str(self.__width)+"x"+str(self.height()))

        self.__frame_indices.append(frame_index)
        self.__unordered_writer.write(new_frame.get_pixels(True))
        self.__max_frame_index = max(self.__max_frame_index, frame_index)

    def attach_audio(self, audio: Audio):
        """
        Attach an Audio object to the renderer.

        Parameters:
        - audio (Audio): The Audio object containing audio data to be added.
        """
        self.__audio = audio

    def set_resolution(self, width: int, height: int):
        """
        Set the resolution of the renderer.

        Parameters:
        - width (int): The new width of the video.
        - height (int): The new height of the video.
        """
        self.__width = width
        self.__height = height

        self.__unordered_writer = cv2.VideoWriter(
            "unordered_render.mp4",
            cv2.VideoWriter_fourcc(*"mp4v"),
            self.__fps,
            (self.__width, self.__height)
        )

    def set_fps(self, fps: int):
        """
        Set the frames per second (fps) of the renderer.

        Parameters:
        - fps (int): The new fps value.
        """
        self.__fps = fps

        self.__unordered_writer = cv2.VideoWriter(
            "unordered_render.mp4",
            cv2.VideoWriter_fourcc(*"mp4v"),
            self.__fps,
            (self.__width, self.__height)
        )

    def fps(self):
        """
        Get the current fps of the renderer.

        Returns:
        - int: The current fps value.
        """
        return self.__fps

    def width(self):
        """
        Get the current width of the renderer.

        Returns:
        - int: The current width value.
        """
        return self.__width

    def height(self):
        """
        Get the current height of the renderer.

        Returns:
        - int: The current height value.
        """
        return self.__height

    def render(self, preview: bool = False):
        # Release the unordered writer and initialize ordered writer
        self.__unordered_writer.release()
        ordered_writer = cv2.VideoWriter(
            "silent_render.mp4",
            cv2.VideoWriter_fourcc(*"mp4v"),
            self.__fps,
            (self.__width, self.__height)
        )
        unordered_render = cv2.VideoCapture("unordered_render.mp4")

        # Render frames
        for frame_index in range(self.__max_frame_index + 1):
            unordered_frame_idx = self.__get_unordered_frame_idx(frame_index)
            if unordered_frame_idx == -1:
                # Write a blank frame if no replacement frame is available
                blank_frame = cv2.UMat(self.__height, self.__width, cv2.CV_8UC3, [0, 0, 0])
                ordered_writer.write(blank_frame)
            else:
                unordered_render.set(cv2.CAP_PROP_POS_FRAMES, unordered_frame_idx)
                ret, frame2 = unordered_render.read()
                if ret:
                    if preview:
                        Frame(frame2).preview()
                    ordered_writer.write(frame2)
                else:
                    raise ValueError(f"Error: Could not read frame {frame_index}.")

        # Release OpenCV resources
        unordered_render.release()
        ordered_writer.release()

        # Attach audio if available
        if self.__audio:
            self.__attach_audio("silent_render.mp4", self.__audio.file_path, "render.mp4")
            print("Video compiled with audio as render.mp4")
        else:
            shutil.copy("silent_render.mp4", "render.mp4")
            print("Video compiled without audio as render.mp4")

    def __attach_audio(self, rendered_video: str, audio_video_path: str, output_video: str):
        audio_file = "temp_audio.aac"

        # Extract audio from the audio's video path
        extract_audio_command = [
            "ffmpeg",
            "-y",  # Overwrite existing files
            "-i", audio_video_path,  # Input original video
            "-vn",  # No video
            "-acodec", "aac",  # Save as AAC format
            audio_file
        ]
        subprocess.run(extract_audio_command, check=True)

        # Combine audio with the rendered video
        combine_audio_command = [
            "ffmpeg",
            "-y",  # Overwrite existing files
            "-i", rendered_video,  # Input rendered video
            "-i", audio_file,  # Input extracted audio
            "-c:v", "copy",  # Copy video codec without re-encoding
            "-c:a", "aac",  # Ensure audio is AAC
            "-shortest",  # Stop when the shortest stream ends
            output_video
        ]
        subprocess.run(combine_audio_command, check=True)

        # Clean up temporary audio file
        os.remove(audio_file)

    def __get_unordered_frame_idx(self, target):
        for i, value in enumerate(reversed(self.__frame_indices)):
            if value == target:
                return len(self.__frame_indices) - 1 - i
        return -1
def read_font_from_qt_resource(resource_path):
    file = QFile(resource_path)
    if not file.open(QFile.ReadOnly):
        raise FileNotFoundError(f"Cannot open resource {resource_path}")

    # Write to a temporary file if needed
    temp_path = "/tmp/arial.ttf"  # Adjust for your OS
    with open(temp_path, "wb") as temp_file:
        temp_file.write(file.readAll())

    return temp_path
videos = {}
renderer = NonlinearRenderer(640, 480, 24)

#replace 'video' with renderer when getting video.height and such

if _paths:  # Ensure _paths is not empty
    # Create a dictionary to store Video objects with their names as keys

    for path in _paths:
        if path:  # Ensure the path is not empty
            # Extract the video name (file name without extension)
            video_name = os.path.splitext(os.path.basename(path))[0]
            videos[video_name] = Video(path)

    for video_name in videos:
        video = videos[video_name]
        video.audio.preload_data(video.frame_duration())
if len(_paths) != 0:
    class Field:
        def __init__(self):
            self._map = np.zeros((renderer.height(), renderer.width()), dtype = np.uint8)
            self.inverted = False

        def get(self, x: int, y: int):
            return (self._map[y][x] / 255).astype(np.float16)

        def set(self, value: float, x: int, y: int):
            self._map[y][x] = int(value * 255)

        def add(self, other):
            if isinstance(other, Field):
                return self.add(other.get_map())
            else:
                self._map = np.clip(self._map.astype(np.int16) + other*255, 0, 255).astype(np.uint8)
                return self
        def sub(self, other):
            if isinstance(other, Field):
                return self.sub(other.get_map())
            else:
                self._map = self.add(other * -1)
                return self
        def mult(self, other):
            if isinstance(other, Field):
                return self.mult(other.get_map())
            else:
                self._map = np.clip(self._map.astype(np.int16) * other, 0, 255).astype(np.uint8)
                return self
        def div(self, other):
            if isinstance(other, Field):
                return self.div(other.get_map())
            else:
                self._map = np.clip(self._map.astype(np.int16) / other, 0, 255).astype(np.uint8)
                return self

        def __add__(self, other):
            if isinstance(other, Field):
                return self.__add__(other.get_map())
            else:
                clone = copy.deepcopy(self)
                clone.add(other)
                return clone

        def __sub__(self, other):
            if isinstance(other, Field):
                return self.__sub__(other.get_map())
            else:
                return self.__add__(other * -1)

        def __mul__(self, other):
            if isinstance(other, Field):
                return self.__mul__(other.get_map())
            else:
                clone = copy.deepcopy(self)
                clone.mult(other)
                return clone

        def __truediv__(self, other):
            if isinstance(other, Field):
                return self.__truediv__(other.get_map())
            else:
                clone = copy.deepcopy(self)
                clone.set_map(np.clip(self._map.astype(np.float16) / other, 0, 255).astype(np.uint8))
                return clone

        def invert(self):
            self._map = 255 - self._map
            self.inverted = not self.inverted
            return self


        def move(self, x: int, y: int):
            """
            Efficiently shift the _map by (x, y) using OpenCV.
            Pixels shifted outside the bounds are filled with 0s or 1s based on `self.inverted`.

            :param x: Amount to shift in the x-direction (positive is right, negative is left).
            :param y: Amount to shift in the y-direction (positive is down, negative is up).
            """
            # Determine the fill value based on inversion
            fill_value = 255 if self.inverted else 0

            # Create the transformation matrix for shifting
            translation_matrix = np.float32([[1, 0, x], [0, 1, y]])

            # Apply the translation
            self._map = cv2.warpAffine(
                self._map, translation_matrix,
                (self._map.shape[1], self._map.shape[0]),  # Output size
                borderMode=cv2.BORDER_CONSTANT,
                borderValue=fill_value
            )
            return self


        def resize(self, width: int, height: int):
            """
            Resize the _map to the specified width and height using OpenCV.
            - If the new size is larger, fill new areas with 0s or 1s depending on `self.inverted`.
            - If the new size is smaller, crop the current map.

            :param width: New width of the _map.
            :param height: New height of the _map.
            """
            # Determine the fill value based on inversion
            fill_value = 255 if self.inverted else 0

            # If expanding, create a new map filled with the fill value
            if width > self._map.shape[1] or height > self._map.shape[0]:
                # Create a larger map filled with the fill value
                new_map = np.full((height, width), fill_value, dtype=np.uint8)
                # Determine the overlap region
                overlap_x_end = min(self._map.shape[1], width)
                overlap_y_end = min(self._map.shape[0], height)
                # Copy the old map into the new map
                new_map[:overlap_y_end, :overlap_x_end] = self._map[:overlap_y_end, :overlap_x_end]
                self._map = new_map
            else:
                # Use OpenCV to resize the map directly if shrinking or reshaping
                self._map = cv2.resize(self._map, (width, height), interpolation=cv2.INTER_AREA)

            return self

        def fit(self):
            """
            Stretches the region of interest (ROI) containing all the `255`s (or `0`s if inverted) to the edges of the canvas.
            """
            # Determine the target value based on whether the field is inverted
            target_value = 0 if self.inverted else 255

            # Find the bounding box of the region containing the target value
            coords = cv2.findNonZero((self._map == target_value).astype(np.uint8))
            if coords is None:
                return self  # If no target value exists, do nothing

            # Get the bounding box (x, y, width, height) of the region
            x, y, w, h = cv2.boundingRect(coords)

            # Extract the region of interest (ROI)
            roi = self._map[y:y+h, x:x+w]

            # Resize the ROI to fit the full canvas size
            resized_roi = cv2.resize(roi, (self._map.shape[1], self._map.shape[0]), interpolation=cv2.INTER_NEAREST)

            # Fill the map with the resized ROI
            self._map = resized_roi

            # Ensure binary values remain consistent after stretching
            if not self.inverted:
                self._map[self._map != 255] = 0  # Enforce binary values: 255 for target, 0 otherwise
            else:
                self._map[self._map != 0] = 255  # Enforce binary values: 0 for target, 255 otherwise

            return self


        def get_map(self):
            return self._map.astype(np.float16) / 255

        def set_map(self, map: np.ndarray):
            self._map = map

        def blur(self, param: tuple = (5, 5)):
            self._map = cv2.blur(self._map, param)
            return self

        def mirror_x(self):
            """
            Mirror the field along the x-axis (horizontal flip).
            """
            self._map = cv2.flip(self._map, 1)  # Flip around the vertical axis
            return self

        def mirror_y(self):
            """
            Mirror the field along the y-axis (vertical flip).
            """
            self._map = cv2.flip(self._map, 0)  # Flip around the horizontal axis
            return self

    class FOverlay(Field):
        def __init__(self, opacity: int = 1.0):
            super().__init__()
            self._map = np.full((renderer.height(), renderer.width()), opacity*255, dtype=np.uint8)

    class FPerlin(Field):
        def __init__(self, seed: int = 0, scale: int = 100, octaves: int = 4, persistence: int = 0.2, lacunarity: int = 2.0, contrast: int = 0.0, midpoint=0.5):
            super().__init__()

            # Parameters
            self.width, self.height = renderer.width(), renderer.height()
            self.scale = scale
            self.octaves = octaves
            self.persistence = persistence
            self.lacunarity = lacunarity
            self.seed = seed
            self.contrast = contrast
            self.midpoint = midpoint


            # Generate grid of coordinates
            self.update()

        def update(self):
            x = np.linspace(0, self.width / self.scale, self.width)
            y = np.linspace(0, self.height / self.scale, self.height)
            x_coords, y_coords = np.meshgrid(x, y)


            # Vectorized noise function
            vectorized_pnoise2 = np.vectorize(lambda x, y: pnoise2(x, y, octaves=self.octaves, persistence=self.persistence, lacunarity=self.lacunarity, base=self.seed))

            # Apply Perlin noise
            self._map = (vectorized_pnoise2(x_coords, y_coords) + 1)/2
            if (self.contrast != 0):
                self._map = 1 / (1 + np.exp(-self.contrast * (self._map - self.midpoint)))
            self._map *= 255

    class FLine(Field):
        def __init__(self, x1, y1, x2, y2, thickness):
            super().__init__()
            cv2.line(self._map,(int(x1), int(y1)), (int(x2), int(y2)), 255, int(thickness))

    class FRect(Field):
        def __init__(self, x1, y1, x2, y2, thickness = -1):
            super().__init__()
            cv2.rectangle(self._map,(int(x1), int(y1)), (int(x2), int(y2)), 255, int(thickness))

    class FEllipse(Field):
        def __init__(self, center, ellipse_width, ellipse_height, angle=0, thickness=-1):
            """
            Initialize an FEllipse object, automatically drawing the ellipse onto the map.

            Parameters:
            - width (int): Width of the field (canvas).
            - height (int): Height of the field (canvas).
            - center (tuple): The (x, y) center of the ellipse.
            - ellipse_width (int): The total width of the ellipse (bounding box width).
            - ellipse_height (int): The total height of the ellipse (bounding box height).
            - angle (float): The rotation angle of the ellipse in degrees (default 0).
            - thickness (int): Thickness of the ellipse border (-1 for filled ellipse, default).
            """
            super().__init__()

            # Convert width and height to axes (semi-width and semi-height)
            axes = (ellipse_width // 2, ellipse_height // 2)

            # Draw the ellipse directly on the map
            cv2.ellipse(
                self._map,
                (int(center[0]), int(center[1])),
                axes,
                angle,
                0, 360,  # Full ellipse
                255,  # White ellipse
                thickness
            )

    class FPoly(Field):
        def __init__(self, points: np.ndarray):
            super().__init__()

            # Reshape the points for OpenCV (required shape: number_of_points x 1 x 2)
            points = points.reshape((-1, 1, 2)).astype(np.int32)

            # Draw the polygon outline
            cv2.fillPoly(self._map, [points], color=255)

    class FText(Field):
        def __init__(self, text: str, position: tuple, font_scale: float, thickness: int = 1, custom_font=None):
            """
            Initialize an FText object, automatically drawing the text onto the map.

            Parameters:
            - text (str): The text to render.
            - position (tuple): The (x, y) position for the center of the text.
            - font_scale (float): Scale of the text.
            - thickness (int): Thickness of the text strokes for OpenCV font.
            - custom_font (str or None): Path to a custom TTF font file. If None, uses OpenCV's default font.
            """
            super().__init__()

            if custom_font:
                # Use Pillow for custom fonts
                self._draw_with_pillow(text, position, font_scale, custom_font)
            else:
                # Fall back to OpenCV's putText
                self._draw_with_opencv(text, position, font_scale, thickness)

        def _draw_with_pillow(self, text, position, font_scale, custom_font):
            """Draw the text using Pillow and a custom font."""
            # Convert the Field map to a Pillow image
            pil_image = Image.fromarray(self._map)

            # Create a drawing context
            draw = ImageDraw.Draw(pil_image)

            # Load the custom font
            font_size = int(font_scale * 20)  # Scale font size appropriately
            font = ImageFont.truetype(custom_font, font_size)

            # Calculate text size and alignment using font.getbbox()
            text_bbox = font.getbbox(text)  # (left, top, right, bottom)
            text_width = text_bbox[2] - text_bbox[0]
            text_height = text_bbox[3] - text_bbox[1]

            # Calculate the top-left corner position for center alignment
            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] - text_height / 2)

            # Draw the text
            draw.text((bottom_left_x, bottom_left_y), text, font=font, fill=255)

            # Convert the Pillow image back to a NumPy array
            self._map = np.array(pil_image)

        def _draw_with_opencv(self, text, position, font_scale, thickness):
            """Draw the text using OpenCV's putText."""
            # Calculate the text size
            (text_width, text_height), baseline = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, font_scale, thickness)

            # Calculate the bottom-left corner position for center alignment
            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] + text_height / 2)

            # Draw the text centered on the map
            cv2.putText(
                self._map,
                text,
                (bottom_left_x, bottom_left_y),
                cv2.FONT_HERSHEY_SIMPLEX,
                font_scale,
                255,  # White text
                thickness,
                lineType=cv2.LINE_AA
            )



    class FAudio(Field):
        def __init__(self, aud: Frame_Audio, start: int = 0, end: int = None):
            super().__init__()
            try:
                freqs = aud.list_frequencies()
                mags = aud.list_magnitudes()

                # Handle start and end indices
                if end is None:
                    end = len(freqs)
                else:
                    end = int(end / (aud.list_frequencies()[1]-aud.list_frequencies()[0]))
                start = int(start / (aud.list_frequencies()[1]-aud.list_frequencies()[0]))

                if end > len(freqs):
                    end = len(freqs)
                if start < 0 or start >= len(freqs):
                    print(f"Invalid range: start={start}, end={end}")
                    return

                norm = max(mags) / renderer.height()
                if norm == 0 or np.isnan(norm) or np.isinf(norm):
                    print(f"Normalization error, mags={mags}")
                    return

                # Create visualization
                total_bars = end - start
                bar_width = renderer.width() / total_bars
                points = []

                for i in range(start, end):
                    x = i * bar_width + bar_width / 2
                    y = renderer.height() - mags[i] / norm
                    points.extend([x, y])

                points.extend([
                    renderer.width() - bar_width, renderer.height(),
                    0, renderer.height()
                ])

                self.add(FPoly(np.array(points, dtype=np.float32)))

                # Add volume indicator
                self.add(FRect(
                    renderer.width() - bar_width,
                    renderer.height() - aud.get_volume() * renderer.height(),
                    renderer.width(),
                    renderer.height()
                ))

            except Exception as e:
                print(f"Error initializing FAudio: {e}")

if len(_paths) != 0:
    class Filter:
        def __init__(self, field: Field = FOverlay()):
            self.field = field
            self._map = self.field.get_map()[:, :, np.newaxis]

        def apply(self, pixels):
            pass

    class Solid_Color(Filter):
        def __init__(self, r: int, g: int, b: int, field: Field = FOverlay()):
            super().__init__(field)
            self.__r = r
            self.__g = g
            self.__b = b

        def invert(self):
            self.__r = 255 - self.__r
            self.__g = 255 - self.__g
            self.__b = 255 - self.__b

        def apply(self, pixels):
            pixels = np.clip((pixels * (1 - self._map) + [self.__b, self.__g, self.__r] * self._map), 0, 255)
            return pixels

    class Draw_Frame(Filter):
        def __init__(self, frame: Frame, field: Field = FOverlay()):
            super().__init__(field)
            self.frame = frame

        def apply(self, pixels):
            return np.clip(self._map * self.frame.get_pixels() + (1 - self._map) * pixels, 0, 255)

        def invert(self):
            self.frame = Frame(255-self.frame.get_pixels())
            return self

        def mirror_x(self):
            """
            Mirror the frame along the x-axis (horizontal flip).
            """
            self.frame = Frame(cv2.flip(self.frame.get_pixels(), 1))  # Flip around the vertical axis
            return self

        def mirror_y(self):
            """
            Mirror the frame along the y-axis (vertical flip).
            """
            self.frame = Frame(cv2.flip(self.frame.get_pixels(), 0))  # Flip around the horizontal axis
            return self

