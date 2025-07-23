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