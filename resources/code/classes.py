from typing import List
import cv2
import numpy as np

try:
    import cv2
except ImportError:
    print("Error: OpenCV is not installed. Please install it using `pip install opencv-python`.")
    exit(1)



class Pixel:
    def __init__(self, r: int, g: int, b: int):
        self.r = r
        self.g = g
        self.b = b

    def __str__(self):
        return f"({self.r}, {self.g}, {self.b})"



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

        self.__pixels = pixels

    def __str__(self):
        """String representation for debugging."""
        return f"Frame with shape {self.__pixels.shape}"

    def preview(self):
        # Show the frame (optional)
        window_name = "First Frame"
        cv2.imshow(window_name, self.__pixels)

        # Keep checking if the window is closed
        while cv2.getWindowProperty(window_name, cv2.WND_PROP_VISIBLE) >= 1:
            if cv2.waitKey(100) & 0xFF == ord('q'):  # Allow 'q' to close the window as well
                break


class ReadOnlyVideo:
    def __init__(self, video_path: str):
        self._video_path = video_path
        self._open()

        if not self._video.isOpened():
            print("Error: Could not open video file.")
            print(self._video_path)
            exit()

        self._fps = self._video.get(cv2.CAP_PROP_FPS)
        self._frame_duration = int(self._video.get(cv2.CAP_PROP_FRAME_COUNT))  # Total number of frames

    def _open(self):
        """Open the video file."""
        self._video = cv2.VideoCapture(self._video_path)

    def close(self):
        """Release the video file."""
        self._video.release()

    def get_frame(self, frame_index: int):
        """Retrieve a specific frame by index."""
        if frame_index < 0 or frame_index >= self._frame_duration:
            raise ValueError(f"Frame index {frame_index} is out of bounds (0 to {self._frame_duration - 1}).")

        # Set the frame position
        self._video.set(cv2.CAP_PROP_POS_FRAMES, frame_index)

        # Read the frame
        ret, frame = self._video.read()
        if ret:
            return Frame(frame)  # Return the raw frame as a NumPy array
        else:
            raise ValueError(f"Frame {frame_index} could not be read. The video may be closed.")

    def frame_duration(self):
        """Get the total number of frames."""
        return self._frame_duration

    def fps(self):
        """Get the frames per second (FPS) of the video."""
        return self._fps

class Video(ReadOnlyVideo):
    def __init__(self, video_path: str):
        super().__init__(video_path)

    def overwrite_frame(self, frame_index: int, new_frame):
        """
        Overwrite a specific frame with a new frame.
        Note: This requires saving to a new video file, as OpenCV does not support in-place modification of video files.
        """
        raise NotImplementedError(
            "Direct frame overwriting is not supported. You can implement this by saving to a new video file."
        )

    def save_as_new_video(self, output_path: str):
        """
        Save the video as a new file, optionally with modifications.
        """
        # Reopen the video for reading
        self._open()

        # Get video properties
        frame_width = int(self._video.get(cv2.CAP_PROP_FRAME_WIDTH))
        frame_height = int(self._video.get(cv2.CAP_PROP_FRAME_HEIGHT))
        fourcc = cv2.VideoWriter_fourcc(*"mp4v")  # Codec
        out = cv2.VideoWriter(output_path, fourcc, self._fps, (frame_width, frame_height))

        # Write frames to the new video file
        for frame_index in range(self._frame_duration):
            ret, frame = self._video.read()
            if not ret:
                break
            out.write(frame)

        # Release resources
        out.release()
        self._video.release()
