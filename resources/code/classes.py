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

        self.pixels = pixels.astype(np.uint8)

    def __str__(self):
        """String representation for debugging."""
        return f"Frame with shape {self.pixels.shape}"

    def apply_filter(self, filter):
        self.pixels = filter.apply(self.pixels.astype(np.uint16)).astype(np.uint8)

    def get_pixels(self):
        return self.pixels.astype(np.uint16)

    def modify(self, func):
        """
        Apply a user-defined function to the frame's pixels.
        :param func: A function that takes x, y, and the current pixel (as a numpy array) and returns a new pixel.
        """
        height, width, _ = self.pixels.shape

        # Generate x and y coordinate grids
        x_coords, y_coords = np.meshgrid(np.arange(width), np.arange(height))

        # Prepare a flattened view for efficient mapping
        flat_pixels = self.pixels.reshape(-1, 3)
        flat_x_coords = x_coords.flatten()
        flat_y_coords = y_coords.flatten()

        # Apply the function vectorized
        new_flat_pixels = np.array([
            func(x, y, pixel)
            for x, y, pixel in zip(flat_x_coords, flat_y_coords, flat_pixels)
        ])

        # Reshape back to the original frame shape
        self.pixels = new_flat_pixels.reshape(height, width, 3).astype(np.uint8)


    def preview(self, wait_for_exit: bool = False, title: str = "Frame Preview"):
        # Show the frame (optional)
        window_name = title
        cv2.imshow(window_name, self.pixels)
        if (wait_for_exit):
            # Keep checking if the window is closed
            while cv2.getWindowProperty(window_name, cv2.WND_PROP_VISIBLE) >= 1:
                if cv2.waitKey(100) & 0xFF == ord('q'):  # Allow 'q' to close the window as well
                    break
        else:
            cv2.waitKey(1)


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

    def get_frame(self, frame_index: int):
        """Retrieve a specific frame by index."""
        if frame_index < 0 or frame_index >= self.__frame_duration:
            raise ValueError(f"Frame index {frame_index} is out of bounds (0 to {self._frame_duration - 1}).")

        # Set the frame position
        self.__video.set(cv2.CAP_PROP_POS_FRAMES, frame_index)

        # Read the frame
        ret, frame = self.__video.read()
        if ret:
            return Frame(frame)  # Return the raw frame as a NumPy array
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

    def frame_audio(self, index: int):
        return self.audio.frame_audio(index)



    def apply_filter(self, start: int, end: int):
        # get 4D array of frames
        # get 4D array of filters

        pass

class Audio:
    def __init__(self, video_path):
        """
        Initialize the Audio object with the path to the video file.
        """
        self.video_path = video_path
        self.fps = self._get_fps()
        self.sample_rate = 44100  # Standard audio sample rate
        self._audio_data = None
        self._loaded = False

    def _get_fps(self):
        """
        Use FFmpeg to extract the frames per second (FPS) of the video.
        """
        command = [
            "ffprobe", "-v", "error",
            "-select_streams", "v:0",
            "-show_entries", "stream=r_frame_rate",
            "-of", "csv=p=0",
            self.video_path
        ]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        fps_data = result.stdout.strip()
        num, denom = map(int, fps_data.split('/'))
        return num / denom

    def _extract_full_audio(self):
        """
        Extract the entire audio track from the video using FFmpeg.
        """
        output_audio = "full_audio.wav"
        command = [
            "ffmpeg", "-y",  # Overwrite existing files
            "-i", self.video_path,  # Input video
            "-vn",  # Exclude video
            "-ac", "1",  # Mono audio
            "-ar", str(self.sample_rate),  # Set sampling rate
            output_audio
        ]
        subprocess.run(command, check=True)

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

        # Clean up temporary file
        os.remove(audio_path)

        # Normalize audio if in 16-bit PCM format
        if audio_data.dtype == np.int16:
            audio_data = audio_data / 32768.0

        # Ensure audio data is not empty
        if len(audio_data) == 0:
            raise ValueError("Audio data is empty. Check the input video for a valid audio track.")

        return audio_data

    def preload_data(self, frame_duration: int, reload: bool = False):
        """
        Preload audio data for all frames, mapping audio segments to frames.
        Results are cached in an .npy file for faster reloading.

        Parameters:
        - frame_duration (int): Total number of frames in the video.
        - reload (bool): Force reloading and recalculation of audio data.
        """
        cache_file = f"{self.video_path}.npy"

        if os.path.isfile(cache_file) and not reload:
            # Load preprocessed data from cache
            self._audio_data = np.load(cache_file, allow_pickle=True)
        else:
            # Extract and preprocess audio data
            audio_data = self._load_audio_data()
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

            # Save preprocessed data to cache
            np.save(cache_file, self._audio_data)

        self._loaded = True

    def frame_audio(self, frame_index):
        """
        Get preloaded audio data for a specific frame.

        Parameters:
        - frame_index (int): The index of the frame.

        Returns:
        - dict: A dictionary containing 'volume', 'frequencies', and 'magnitude'.
        """
        if not self._loaded:
            raise ValueError("Audio data not preloaded. Call `preload_data()` first.")
        if (frame_index < len(self._audio_data)):
            return Frame_Audio(self._audio_data[frame_index])
        else:
            return Frame_Audio(self._audio_data[len(self._audio_data)-1])

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

        if new_frame.pixels.shape != (self.__height, self.__width, 3):
            raise ValueError("Frame dimensions do not match the initialized video resolution. "+
                             "Frame dimensions are "+str(len(new_frame.pixels[0]))+"x"+str(len(new_frame.pixels))+
                             " but renderer dimensions are "+str(self.__width)+"x"+str(self.height()))

        self.__frame_indices.append(frame_index)
        self.__unordered_writer.write(new_frame.pixels)
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
            self.__attach_audio("silent_render.mp4", self.__audio.video_path, "render.mp4")
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