class Blur(Filter):
    """A filter that applies a blur effect to the image using the field mask."""

    def __init__(self, blur_kernel: int = 5, field: Optional[Field] = None) -> None:
        """Initialize a blur filter.

        Parameters:
            blur_kernel (int): Size of the blur kernel; must be positive and odd.
            field (Optional[Field]): Field object providing the overlay mask.
                If None, a default FOverlay() field will be used.
        """
        super().__init__(field)
        if blur_kernel < 1 or blur_kernel % 2 == 0:
            raise ValueError("blur_kernel size must be a positive odd integer.")
        self.blur_kernel = blur_kernel

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """Apply the blur filter to pixel data using the field mask.

        Parameters:
            pixels (np.ndarray): The pixel data to apply the filter to.

        Returns:
            np.ndarray: The blurred pixel data masked by self._map.
        """
        # Ensure pixels are a CPU numpy array for OpenCV compatibility
        if gpu_enabled:
            pixels_cpu = np.asnumpy(pixels).astype(np.uint8)
            blurred_cpu = cv2.GaussianBlur(pixels_cpu, (self.blur_kernel, self.blur_kernel), 0)
            blurred = np.asarray(blurred_cpu)
            # Convert back to GPU array if needed
            if isinstance(pixels, np.ndarray) and pixels.__module__ == 'cupy':
                blurred = np.asarray(blurred)
        else:
            blurred = cv2.GaussianBlur(pixels, (self.blur_kernel, self.blur_kernel), 0)

        # Convert field map to match pixel channels if needed
        mask = self._map
        if mask.ndim == 2:
            mask = mask[:, :, np.newaxis]

        alpha = mask.astype(np.float32)
        blended = alpha * blurred + (1.0 - alpha) * pixels

        return np.clip(blended, 0, 255).astype(np.uint8)