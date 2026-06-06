class Custom_Filter(Filter):
    """A custom filter masked through a vector Field."""

    def __init__(self, field: Optional[Field] = None) -> None:
        """Initialize a custom filter.

        Parameters:
            field (Optional[Field]): Field object providing the mask.
                If None, a default FOverlay() field will be used.
        """
        super().__init__(field)

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """Apply the filter to pixel data, masked by the field.

        ``self._map`` is an (H, W, 1) float array in [0, 1] that has already
        been rasterized to match ``pixels``. Use it to blend your effect in only
        where the field is present.

        Parameters:
            pixels (np.ndarray): The pixel data to apply the filter to.

        Returns:
            np.ndarray: The modified pixel data.
        """
        modified = pixels  # modify the pixels to create your effect

        # Blend the effect through the field mask, then clamp to the valid range.
        return np.clip(pixels * (1 - self._map) + modified * self._map, 0, 255)
