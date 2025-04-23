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

class Color_Frame(Frame):
    def __init__(self, width: int, height: int, color: tuple = (0, 0, 0)):
        """
        Initialize a Color_Frame with a given width, height, and color.
        :param width: Width of the frame.
        :param height: Height of the frame.
        :param color: RGB color tuple (default is black: (0, 0, 0)).
        """
        if not (isinstance(color, tuple) and len(color) == 3 and
                all(isinstance(c, int) and 0 <= c <= 255 for c in color)):
            raise ValueError("Color must be a tuple of three integers (R, G, B) between 0 and 255.")

        # Create a NumPy array filled with the specified color
        pixels = np.full((height, width, 3), color, dtype=np.uint8)

        super().__init__(pixels)

    def change_color(self, color: tuple):
        """
        Change the entire frame's color.
        :param color: New RGB color tuple.
        """
        if not (isinstance(color, tuple) and len(color) == 3 and
                all(isinstance(c, int) and 0 <= c <= 255 for c in color)):
            raise ValueError("Color must be a tuple of three integers (R, G, B) between 0 and 255.")

        self._pixels[:] = color


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

        # Check if the video has an audio stream
        self.audio = None
        if self._has_audio():
            self.audio = Audio(self.__video_path)

    def _has_audio(self):
        """
        Check if the video file has an audio stream using ffprobe.
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

    def open(self):
        """Open the video file."""
        self.__video = cv2.VideoCapture(self.__video_path)

    def close(self):
        """Release the video file."""
        self.__video.release()

    def get_frame(self, frame_index: int, w = 1.0, h = None):
        """Retrieve a specific frame by index."""
        if frame_index < 0 or frame_index >= self.__frame_duration:
            raise ValueError(f"Frame index {frame_index} is out of bounds (0 to {self.__frame_duration - 1}).")

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


import cv2
import subprocess
import os
import shutil
from typing import List

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
        self.__audios = []  # List to store multiple audio files

    def set_frame(self, frame_index: int, new_frame):
        if new_frame.get_pixels(True).shape != (self.__height, self.__width, 3):
            raise ValueError("Frame dimensions do not match the initialized video resolution. "+
                             "Frame dimensions are "+str(len(new_frame.get_pixels()[0]))+"x"+str(len(new_frame.get_pixels()))+
                             " but renderer dimensions are "+str(self.__width)+"x"+str(self.height()))

        self.__frame_indices.append(frame_index)
        self.__unordered_writer.write(new_frame.get_pixels(True))
        self.__max_frame_index = max(self.__max_frame_index, frame_index)

    def attach_audio(self, audio: 'Audio', volume: float = 1.0):
        """
        Attach an Audio object to the renderer with a specified volume.

        Parameters:
        - audio (Audio): The Audio object containing audio data to be added.
        - volume (float): The volume level (0.0 to 1.0, default = 1.0).
        """
        if not 0.0 <= volume <= 1.0:
            raise ValueError("Volume must be between 0.0 and 1.0.")
        self.__audios.append((audio, volume))

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

    def sec_to_frame(self, seconds):
        if isinstance(seconds, list) and all(isinstance(item, float) for item in seconds):
            return [int(value * self.fps()) for value in seconds]
        elif isinstance(seconds, float) or isinstance(seconds, int):
            return int(seconds * self.fps())
        else:
            raise TypeError("Expected an int, float, or list of floats")

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
        if self.__audios:
            self.__attach_audios("silent_render.mp4", self.__audios, "render.mp4")
            print("Video compiled with audio as render.mp4")
        else:
            shutil.copy("silent_render.mp4", "render.mp4")
            print("Video compiled without audio as render.mp4")

        # Release OpenCV resources
        unordered_render.release()
        ordered_writer.release()

        # Attach audio if available
        if self.__audios:
            self.__attach_audios("silent_render.mp4", self.__audios, "render.mp4")
            print("Video compiled with audio as render.mp4")
        else:
            shutil.copy("silent_render.mp4", "render.mp4")
            print("Video compiled without audio as render.mp4")

    def __attach_audios(self, rendered_video: str, audios: List[tuple['Audio', float]], output_video: str):
        # Extract audio from each audio file
        audio_files = []
        for i, (audio, volume) in enumerate(audios):
            audio_file = f"temp_audio_{i}.aac"
            extract_audio_command = [
                "ffmpeg",
                "-y",  # Overwrite existing files
                "-i", audio.file_path,  # Input audio file
                "-af", f"volume={volume}",  # Apply volume adjustment
                "-vn",  # No video
                "-acodec", "aac",  # Save as AAC format
                audio_file
            ]
            subprocess.run(extract_audio_command, check=True)
            audio_files.append(audio_file)

        # Get the duration of the video
        get_video_duration_command = [
            "ffprobe",
            "-i", rendered_video,
            "-show_entries", "format=duration",
            "-v", "quiet",
            "-of", "csv=p=0"
        ]
        video_duration = float(subprocess.run(get_video_duration_command, stdout=subprocess.PIPE, text=True).stdout.strip())

        # Create a filter complex string to mix all audio files
        filter_complex = ""
        input_args = ["-i", rendered_video]
        audio_inputs = []

        for i in range(len(audio_files)):
            input_args.extend(["-i", audio_files[i]])
            filter_complex += f"[{len(audio_inputs)+1}:a]"
            audio_inputs.append(f"[{len(audio_inputs)+1}:a]")

        if not audio_inputs:
            raise ValueError("No audio streams found to mix.")

        filter_complex += f"amix=inputs={len(audio_inputs)}:duration=shortest[aout]"

        # Combine audio with the rendered video
        combine_audio_command = [
            "ffmpeg",
            "-y",  # Overwrite existing files
        ]
        combine_audio_command.extend(input_args)
        combine_audio_command.extend([
            "-filter_complex", filter_complex,
            "-map", "0:v",  # Map video stream from the rendered video
            "-map", "[aout]",  # Map trimmed/mixed audio
            "-c:v", "copy",  # Copy video codec without re-encoding
            "-c:a", "aac",  # Ensure audio is AAC
            output_video
        ])

        try:
            print("Executing FFmpeg command:", " ".join(combine_audio_command))
            subprocess.run(combine_audio_command, check=True)
        except subprocess.CalledProcessError as e:
            print("FFmpeg Error:", e.stderr)
            raise ValueError("FFmpeg failed to combine audio tracks. Check the input files and filter graph.")
        finally:
            # Clean up temporary audio files
            for audio_file in audio_files:
                if os.path.exists(audio_file):
                    os.remove(audio_file)

    def __get_unordered_frame_idx(self, target):
        for i, value in enumerate(reversed(self.__frame_indices)):
            if value == target:
                return len(self.__frame_indices) - 1 - i
        return -1

class Bot:
    def __init__(self, personality: str = "You are a helpful chatbot.", unique_key: str = None, voice: str = "onyx"):
        self._personality = personality
        self._voice = voice
        if unique_key == None:
            self._api_key = api_key
        else:
            self._api_key = unique_key

        self._client = OpenAI(api_key=self._api_key)

    def set_personality(self, personality: str):
        self._personality = personality

    def prompt(self, input: str) -> str:
        response = self._client.chat.completions.create(
            model="gpt-4o",
            messages=[
                {
                    "role": "system",
                    "content": self._personality
                },
                {
                    "role": "user",
                    "content": input
                }
            ]
        )
        # Extract the generated caption text
        return response.choices[0].message.content

        def _count_sounds(self, word: str) -> int:
            special_combinations = [
                "ch", "tch", "sh", "oo", "ld", "ss", "qu", "th", "ph", "ng",
                "gh", "wh", "kn", "wr", "gn", "sc", "sk", "st", "sp", "spl",
                "spr", "shr", "scr", "str", "dr", "tr", "bl", "cl", "fl", "gl",
                "pl", "sl", "br", "cr", "fr", "gr", "pr", "tr", "ou", "ght"
            ]
        for combo in special_combinations:
            word = word.replace(combo, "$")
        return len(word)

    def _calculate_word_timestamps(self, text, total_duration: float, first_timestamp: float):
        total_sounds = sum(self._count_sounds(re.sub(r"[.,!?]", "", token)) + 2 + (5 if re.search(r"[.,!?]", token) else 0) for token in text)
        seconds_per_sound = total_duration / total_sounds
        timestamps = []
        current_time = first_timestamp
        for token in text:
            sounds = self._count_sounds(re.sub(r"[.,!?]", "", token)) + 2 + (5 if re.search(r"[.,!?]", token) else 0)
            word_duration = sounds * seconds_per_sound
            timestamps.append((current_time, current_time + word_duration))
            current_time += word_duration
        return timestamps

    def _fix_timestamps(self, words, timestamps):
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
                corrected_timestamps = self._calculate_word_timestamps(selected_words, total_duration, starting_time)
                timestamps[start_idx:end_idx] = corrected_timestamps
                i = j
            else:
                i += 1
        return timestamps

    def transcribe(self, audio: Audio):
        with open(audio.file_path, "rb") as audio_file:
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


    def speak(self, text: str, speed: int = 1) -> Audio:
        global audio_counter
        filename = generate_random_filename(seed=audio_counter)
        audio_counter += 1
        # MAKE AUDIO
        audio_path = "AudioCache/"+filename+".wav"
        speech_file_path = Path(audio_path)
        response = self._client.audio.speech.create(
            model="tts-1",
            voice=self._voice,
            input=text,
        )
        response.stream_to_file(speech_file_path)

        if not speed == 1:
            # Step 1: Load the original audio
            y, sr = librosa.load(audio_path, sr=None)

            # Step 2: Adjust the sampling rate for speed-up
            new_sr = int(sr * speed)  # Increase the sampling rate by 1.5x for speed-up

            # Step 3: Save the resampled audio
            sf.write(audio_path, y, new_sr)

            # Step 4: Load the resampled audio at the original sampling rate
            # This ensures playback is correctly synchronized with the video
            faster_audio, _ = librosa.load(audio_path, sr=sr)

            # Save again to ensure compatibility
            sf.write(audio_path, faster_audio, sr)


        # Load the faster audio into your renderer or video synchronization system
        return Audio(audio_path)
