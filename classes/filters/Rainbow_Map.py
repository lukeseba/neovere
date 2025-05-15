class Rainbow_Map(Filter):
    """A filter that maps each pixel to a full rainbow gradient based on its lightness."""

    def __init__(self, field: Optional[Field] = None) -> None:
        """Initialize the Rainbow_Map with an optional Field mask."""
        super().__init__(field)

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """Convert input to grayscale then map to a full rainbow colormap."""
        # Work in float for accurate blending
        img = pixels.astype(np.float32)

        # Compute luminance (Rec. 601): Y = 0.299 R + 0.587 G + 0.114 B (note BGR input)
        gray = 0.299 * img[:, :, 2] + 0.587 * img[:, :, 1] + 0.114 * img[:, :, 0]
        norm = np.clip(gray / 255.0, 0.0, 1.0)

        # Map normalized brightness to hue (0°=red through ~150°=magenta)
        # Hue range in OpenCV is [0,180]; we'll use up to 150 for full rainbow
        hue = (norm * 150).astype(np.uint8)

        # Full saturation and value
        sat = np.full_like(hue, 255, dtype=np.uint8)
        val = np.full_like(hue, 255, dtype=np.uint8)

        # Merge into HSV image and convert to BGR
        hsv = cv2.merge([hue, sat, val])
        rainbow_bgr = cv2.cvtColor(hsv, cv2.COLOR_HSV2BGR).astype(np.float32)

        # Blend original and rainbow based on the field mask (self._map has shape HxWx1)
        blended = (1.0 - self._map) * img + self._map * rainbow_bgr

        return np.clip(blended, 0, 255).astype(np.uint8)