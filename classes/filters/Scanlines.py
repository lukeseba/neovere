class Scanlines(Filter):
    """
    A filter that applies a CRT scanline effect"""

    def __init__(self, intensity: float = 0.4, line_thickness: int = 1, line_spacing: int = 2, invert_lines: bool = False, field: Optional[Field] = None ) -> None:
        """
        Initialize the CRT_Scanline_Only_Filter.

        Parameters:
            intensity (float): The darkness factor for scanlines (0 = no darkening, 1 = full black lines).
            line_thickness (int): Thickness in pixels of each scanline.
            line_spacing (int): Number of pixels between start of one scanline and next (including thickness).
            invert_lines (bool): If True, darken odd lines instead of even lines.
            field (Optional[Field]): Overlay mask for applying the effect; defaults to FOverlay() if None.
        """
        if field is None:
            field = FOverlay()
        super().__init__(field)

        if not (0.0 <= intensity <= 1.0):
            raise ValueError("intensity must be between 0 and 1.")
        if line_thickness < 1:
            raise ValueError("line_thickness must be at least 1.")
        if line_spacing < line_thickness:
            raise ValueError("line_spacing must be >= line_thickness.")

        self.intensity = intensity
        self.line_thickness = line_thickness
        self.line_spacing = line_spacing
        self.invert_lines = invert_lines

        # Precompute scanline mask map of shape (height, width)
        height = renderer.height()
        width = renderer.width()
        mask = np.ones((height, width), dtype=np.float32)
        for y in range(0, height, line_spacing):
            # Decide if this line should be darkened depending on line parity and invert_lines
            line_index = y // line_spacing
            apply_dark = (line_index % 2 == 0) != invert_lines
            if apply_dark:
                end_y = min(y + line_thickness, height)
                mask[y:end_y, :] = 1.0 - intensity  # darken line

        self.scanline_map = mask  # float32 mask with values near 1 or reduced by intensity

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """
        Apply the CRT scanline effect to the pixel data.

        Parameters:
            pixels (np.ndarray): Input image pixels as (H,W,3) uint8 array.

        Returns:
            np.ndarray: The pixel data with CRT scanline effect applied.
        """
        # Convert pixels to float for calculation
        pixels_f = pixels.astype(np.float32)

        # Expand scanline mask to 3 channels
        scanline_mask_3c = self.scanline_map[:, :, np.newaxis]

        # Apply the scanline intensity mask
        filtered_pixels = pixels_f * scanline_mask_3c

        # Blend with original pixels using the field mask self._map
        # Normalize self._map to range [0, 1]
        if self._map.ndim == 2:
            alpha = self._map.astype(np.float32)
            max_alpha = np.max(alpha)
            if max_alpha > 0:
                alpha = alpha / max_alpha
            alpha = alpha[:, :, np.newaxis]
        elif self._map.ndim == 3:
            alpha = self._map.astype(np.float32)
            max_alpha = np.max(alpha)
            if max_alpha > 0:
                alpha = alpha / max_alpha
        else:
            alpha = np.ones_like(scanline_mask_3c, dtype=np.float32)

        blended = alpha * filtered_pixels + (1 - alpha) * pixels_f

        return np.clip(blended, 0, 255).astype(np.uint8)