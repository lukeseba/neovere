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


class Color:
    """Convenience wrapper for choosing colors.

    Color exists so the editor can offer its inline color picker wherever a color
    is expected. It is not a real object to hold onto: calling Color((r, g, b))
    simply hands back the plain (r, g, b) tuple you gave it, so
    Solid_Color(Color((255, 0, 0))) behaves exactly like Solid_Color((255, 0, 0)).
    """

    def __new__(cls, color: tuple) -> tuple:
        # Return the plain tuple, so Color(...) evaluates to a tuple rather than a
        # Color instance. Returning a non-instance also means __init__ is never
        # run — it exists only to document the argument for the editor's picker.
        return tuple(color)

    def __init__(self, color: tuple) -> None:
        """Pick an (r, g, b) color with the inline color picker.

        Clicking this field in the editor opens a saturation/value square and a
        hue slider; the color you choose is written back here as an (r, g, b)
        tuple. Color(...) returns that tuple unchanged, so it can be dropped
        anywhere a color is expected.

        Parameters:
            color (tuple) @color: The color as an (r, g, b) tuple, each 0–255.

        Returns:
            tuple: The same (r, g, b) tuple, unchanged.
        """


class Position:
    """Convenience wrapper for choosing on-screen positions.

    Position exists so the editor can offer its inline position picker wherever a
    point on the frame is expected. It is not a real object to hold onto: calling
    Position((x, y)) simply hands back the plain (x, y) tuple you gave it, so it
    can be dropped anywhere a position is expected.
    """

    def __new__(cls, position: tuple) -> tuple:
        # Return the plain tuple, so Position(...) evaluates to a tuple rather than
        # a Position instance. Returning a non-instance also means __init__ is
        # never run — it exists only to document the argument for the editor.
        return tuple(position)

    def __init__(self, position: tuple) -> None:
        """Pick an (x, y) position with the inline frame picker.

        Clicking this field in the editor shows the current rendered frame with a
        draggable point (and an absolute/relative toggle); the point you choose is
        written back here as an (x, y) tuple. Position(...) returns that tuple
        unchanged, so it can be dropped anywhere a position is expected.

        Parameters:
            position (tuple) @position: The position as an (x, y) pixel tuple.

        Returns:
            tuple: The same (x, y) tuple, unchanged.
        """