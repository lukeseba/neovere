class FDepth_Slice(Field):
        def __init__(self, depth_frame: 'Frame', feather: float, lower_bound: float, upper_bound: float) -> None:
            """Initialize a field from a depth map using a feathered slice between two thresholds.
            
            Parameters:
                depth_frame: The frame containing the depth map.
                feather: The width of the smooth transition zone on both edges.
                lower_bound: The minimum luminance to keep (0.0 to 1.0).
                upper_bound: The maximum luminance to keep (0.0 to 1.0).
            """
            super().__init__()
            
            # Extract normalized pixels
            pixels = depth_frame.get_pixels(standard_size=True).astype(np.float32) / 255.0
            
            # Compute luminance as a grayscale mask
            luminance = 0.299 * pixels[:, :, 2] + 0.587 * pixels[:, :, 1] + 0.114 * pixels[:, :, 0]
            
            # Feathering math
            if feather > 0.0:
                # 1. Fade-in mask (ramp from 0.0 to 1.0 around the lower bound)
                lower_fade = np.clip((luminance - (lower_bound - feather)) / (2 * feather), 0.0, 1.0)
                
                # 2. Fade-out mask (ramp from 1.0 to 0.0 around the upper bound)
                upper_fade = 1.0 - np.clip((luminance - (upper_bound - feather)) / (2 * feather), 0.0, 1.0)
                
                # 3. Multiply them to extract the perfectly feathered slice
                mask = lower_fade * upper_fade
            else:
                # Hard cut if no feather is applied
                mask = ((luminance > lower_bound) & (luminance < upper_bound)).astype(np.float32)
                
            self._map = (mask * 255).astype(np.uint8)