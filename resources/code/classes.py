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
        if gpu_enabled:
            cv2.imshow(window_name, np.asnumpy(self._pixels))
        else:
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
        self.__width = max(1, int(self.__source_width * dx))
        self.__height = max(1, int(self.__source_height * dx))

        # If preview-scaled, transcode source to a smaller cached copy with
        # nearest-neighbor (pixel decimation) — fast encode, fast decode.
        if dx != 1.0:
            cache_dir = "VideoCache"
            os.makedirs(cache_dir, exist_ok=True)
            safe_name = "".join(c if c.isalnum() else f"_{ord(c)}_" for c in video_path)
            cache_path = f"{cache_dir}/{safe_name}_dx{dx}.mp4"
            if not os.path.isfile(cache_path):
                print(f"[preview] Downscaling {video_path} -> {self.__width}x{self.__height} (one-time, cached)...")
                result = subprocess.run([
                    "ffmpeg", "-y", "-i", video_path,
                    "-vf", f"scale={self.__width}:{self.__height}:flags=neighbor",
                    "-c:v", "libx264", "-preset", "ultrafast", "-crf", "23",
                    "-an",
                    cache_path
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if result.returncode != 0:
                    print("[preview] ffmpeg downscale failed; falling back to in-process resize.")
                    if os.path.isfile(cache_path):
                        os.remove(cache_path)
            if os.path.isfile(cache_path):
                self.__video_path = cache_path
                self.__pre_scaled = True

        self.open()

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

        # Map scaled (preview) index to source index
        if dt != 1.0:
            source_idx = min(self.__source_frame_count - 1, int(round(frame_index / dt)))
        else:
            source_idx = frame_index

        self.__video.set(cv2.CAP_PROP_POS_FRAMES, source_idx)

        ret, frame = self.__video.read()
        if ret:
            if gpu_enabled:
                frame = np.asarray(frame)
            # Apply preview scale only if source isn't pre-scaled by ffmpeg
            if dx != 1.0 and not self.__pre_scaled:
                frame = cv2.resize(frame, (self.__width, self.__height))
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

    def preload_data(self, reload: bool = False) -> None:
        """Preload and process audio into per-frame features (volume, spectrum) for faster access.

        Parameters:
            reload (bool): Whether to force reloading even if cached data exists.
        """
        cache_file = f"AudioCache/{self._encode_cache_name(self._file_path)}.npy"

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
                yf = fft(frame_audio)
                xf = fftfreq(len(frame_audio), 1 / self._sample_rate)

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
        if not self._audio_data.all():
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
        write_pixels = new_frame.get_pixels(True)
        if gpu_enabled:
            write_pixels = np.asnumpy(write_pixels)
        self.__unordered_writer.write(write_pixels)
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
                    ordered_writer.write(frame)
                    if preview:
                        if gpu_enabled:
                            frame = np.asarray(frame)
                        Frame(frame).preview()
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