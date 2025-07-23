class Screen_Tear(Filter):
    """A filter that simulates capturing a CRT screen partially through drawing the frame,
    showing partial frame reveal, blending with a previous frame for open areas."""

    def __init__(self, previous_frame: Optional[Frame] = None, reveal_fraction: float = 0.5, field: Optional[Field] = None) -> None:
        """
        Initialize the CRT_Scanline_Filter without internal scanlines.

        Parameters:
            previous_frame (Optional[Frame]): The previous Frame to show in the uncovered region.
                If None, black will be used for empty areas.
            reveal_fraction (float): Fraction of the frame height revealed (0.0 to 1.0).
            field (Optional[Field]): Field overlay mask, default to FOverlay() if None.
        """
        super().__init__(field)
        if not (0.0 <= reveal_fraction <= 1.0):
            raise ValueError("reveal_fraction must be between 0.0 and 1.0")
        self.previous_frame = previous_frame
        self.reveal_fraction = reveal_fraction

        # Precompute a mask of all ones since scanlines are handled by separate Scanlines filter
        height = renderer.height()
        width = renderer.width()
        self.scanline_map = np.ones((height, width), dtype=np.float32)  # No darkening here

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """
        Apply the partial frame reveal effect only.

        Parameters:
            pixels (np.ndarray): The current frame pixel data.

        Returns:
            np.ndarray: The pixel data with partial reveal effect applied.
        """
        height, width = pixels.shape[:2]
        reveal_rows = int(height * self.reveal_fraction)
        # Clamp reveal_rows to height
        reveal_rows = max(0, min(reveal_rows, height))

        # Create an output canvas initially with previous frame or black if none
        if self.previous_frame:
            prev_pixels = self.previous_frame.get_pixels(standard_size=True)
            if prev_pixels.shape[:2] != (height, width):
                # Resize previous frame pixels to match current frame size
                prev_pixels = cv2.resize(prev_pixels, (width, height), interpolation=cv2.INTER_LINEAR)
        else:
            prev_pixels = np.zeros((height, width, 3), dtype=np.uint8)

        # Compose the partial frame: top reveal_rows from pixels, rest from prev_pixels
        output_pixels = np.zeros_like(pixels, dtype=np.float32)
        output_pixels[:reveal_rows, :, :] = pixels[:reveal_rows, :, :]
        output_pixels[reveal_rows:, :, :] = prev_pixels[reveal_rows:, :, :]

        # Apply the mask (all ones, so no change here)
        scanline_mask_3c = self.scanline_map[:, :, np.newaxis]
        output_pixels = output_pixels.astype(np.float32) * scanline_mask_3c

        # Blend with field mask - use self._map as alpha mask for blending original pixels and partial reveal output
        if self._map.ndim == 2:
            alpha = self._map.astype(np.float32)
            alpha = alpha[:, :, np.newaxis]  # shape (h, w, 1)
        elif self._map.ndim == 3:
            alpha = self._map.astype(np.float32)
        else:
            alpha = np.ones((height, width, 1), dtype=np.float32)

        # Normalize alpha to 0 to 1
        alpha = alpha / np.max(alpha) if np.max(alpha) > 0 else alpha

        # Blend original pixels with partial reveal output by alpha mask
        blended = alpha * output_pixels + (1 - alpha) * pixels
        blended = np.clip(blended, 0, 255)

        return blended.astype(np.uint8)