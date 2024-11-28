from typing import List
import numpy as np
import subprocess
import os
import shutil

try:
    import cv2
except ImportError:
    print("Error: OpenCV is not installed. Please install it using `pip install opencv-python`.")
    exit(1)

fourcc = cv2.VideoWriter_fourcc(*'mp4v')

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

        self.pixels = pixels

    def __str__(self):
        """String representation for debugging."""
        return f"Frame with shape {self.pixels.shape}"

    def get_pixel(self, x: int, y: int):
        return Pixel(self.pixels[y][x][2], self.pixels[y][x][1], self.pixels[y][x][0])

    def set_pixel(self, x: int, y: int, pixel: Pixel):
        self.pixels[y][x] = [round(pixel.b), round(pixel.g), round(pixel.r)]

    def preview(self, wait_for_exit: bool = False, title: str = "Frame Preview"):
        # Show the frame (optional)
        window_name = "frame"
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

class NonlinearRenderer:
    def __init__(self, vid: Video):
        self.__video = vid
        self.__frame_indices = []
        self.__unordered_writer = cv2.VideoWriter(
            "unordered_render.mp4",
            fourcc,
            self.__video.fps(),
            (self.__video.width(), self.__video.height())
        )

    def set_frame(self, frame_index: int, new_frame: Frame):
        self.__frame_indices.append(frame_index)
        self.__unordered_writer.write(new_frame.pixels)

    def render(self, preview: bool = False):
        # Release the unordered writer and initialize ordered writer
        self.__unordered_writer.release()
        ordered_writer = cv2.VideoWriter(
            "render.mp4",
            fourcc,
            self.__video.fps(),
            (self.__video.width(), self.__video.height())
        )
        unordered_render = cv2.VideoCapture("unordered_render.mp4")

        # Render frames
        for frame_index in range(self.__video.frame_duration()):
            frame1 = self.__video.get_frame(frame_index).pixels

            unordered_frame_idx = self.__get_unordered_frame_idx(frame_index)
            if unordered_frame_idx == -1:
                # Write the original video frame if no replacement frame
                ordered_writer.write(frame1)
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

        # Attach audio from the original video
        self.__attach_audio("render.mp4", self.__video._Video__video_path, "final_render.mp4")
        print("Video compiled with audio as final_render.mp4")

    def __attach_audio(self, rendered_video: str, original_video: str, output_video: str):
        """Attach the original video's audio to the rendered video using FFmpeg."""
        audio_file = "temp_audio.aac"

        # Extract audio from the original video
        extract_audio_command = [
            "ffmpeg",
            "-y",  # Overwrite existing files
            "-i", original_video,  # Input original video
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