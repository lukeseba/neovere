class Custom_Filter(Filter):
    """A filter that overlays a solid color onto a Field-based mask."""

    def __init__(self, field: Optional[Field] = None) -> None:
        """Initialize a Solid_Color filter.

        Parameters:

            field (Optional[Field]): Field object providing the overlay mask.
                If None, a default FOverlay() field will be used.
        """
        super().__init__(field)

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """Apply the solid color filter to pixel data using the field mask.

        Parameters:
            pixels (np.ndarray): The pixel data to apply the filter to.

        Returns:
            np.ndarray: The modified pixel data.
        """
        pixels = # modify given pixels to apply filter.
        return pixels * self._map # apply field (self._map) to mask the filter