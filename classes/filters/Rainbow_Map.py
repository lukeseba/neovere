class Rainbow_Map(Filter):
    """A filter that maps grayscale pixel intensities to a rainbow gradient repeated multiple times."""

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

        # Create a rainbow colormap from red to purple
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