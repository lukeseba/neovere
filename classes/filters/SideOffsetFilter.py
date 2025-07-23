class SideOffsetFilter(Filter):
    """A filter that offsets lines of pixels based on an angle (0-360 degrees),
    wrapping pixels that are pushed off-screen around to the opposite side.

    The offset values array length will be resampled via interpolation to match frame height (for mostly vertical shifts)
    or frame width (for mostly horizontal shifts) depending on the angle.

    Angles:
        - 0 or 360 degrees: offset horizontally to the right (like side=2 Right).
        - 90 degrees: offset vertically downward (like side=3 Bottom).
        - 180 degrees: offset horizontally to the left (like side=0 Left).
        - 270 degrees: offset vertically upward (like side=1 Top).
        - Intermediate angles interpolate offsets in both directions proportionally.
          The offset is applied along a vector line rotated by the angle, with values along that line.
    """

    def __init__(self, values: np.ndarray, angle: float, blend_width: int = 10, field: Optional[Field] = None) -> None:
        """
        Initialize the SideOffsetFilter with angle-based offset.

        Args:
            values (np.ndarray): 1D array of floats in [0,1] representing offset magnitude.
                These represent offset values along a line (like a waveform) that will be rotated by the angle.
            angle (float): Offset angle in degrees [0-360).
            blend_width (int): Width in pixels over which to blend edges.
            field (Optional[Field]): Field for masking. Defaults to FOverlay() if None.
        """
        super().__init__(field)
        if not (0 <= angle < 360):
            angle = angle % 360
        if not isinstance(values, np.ndarray):
            raise TypeError("Values must be a numpy ndarray.")
        if values.ndim != 1:
            raise ValueError("Values array must be 1D.")
        if blend_width < 0:
            raise ValueError("blend_width must be non-negative.")

        self.values = values.astype(np.float32)
        self.angle = angle
        self.blend_width = blend_width

    def _resample_values(self, target_length: int) -> np.ndarray:
        """Resample self.values to target_length using linear interpolation."""
        if len(self.values) == target_length:
            return self.values
        if len(self.values) == 1:
            return np.full(target_length, self.values[0], dtype=np.float32)

        orig_x = np.linspace(0, 1, len(self.values))
        target_x = np.linspace(0, 1, target_length)
        resampled = np.interp(target_x, orig_x, self.values)
        return resampled.astype(np.float32)

    def apply(self, pixels: np.ndarray) -> np.ndarray:
        """
        Apply the offset effect by treating the values as a line along the rotated axis,
        offsetting pixels in the direction of the angle accordingly with wrapping and blending.

        Args:
            pixels (np.ndarray): Input image pixels as (H, W, 3) uint8 array.

        Returns:
            np.ndarray: Offset pixels blended with original using self._map mask.
        """
        height, width = pixels.shape[:2]
        bw = self.blend_width
        pixels_f = pixels.astype(np.float32)

        angle_int = int(round(self.angle)) % 360

        # Optimize for cardinal angles: 0, 90, 180, 270, 360
        if angle_int == 0 or angle_int == 360:
            # Horizontal offset to right
            resampled_values = self._resample_values(width)
            max_offset = width
            offsets = resampled_values * max_offset  # length width
            # Create output array
            output = np.empty_like(pixels_f)
            for y in range(height):
                row = pixels_f[y]
                row_offset = int(offsets[y if y < len(offsets) else -1]) if len(offsets) == height else int(offsets[y % width])
                # Use vectorized shift with wrap
                offset_vals = offsets
                # Since offsets length == width, shift each row by offsets[x]? No, offsets along x axis, needed to treat properly
                # offsets is 1D along horizontal dimension (width)
                # Actually, offsets length == width, so each pixel column has offset value
                # But for horizontal shift, offsets should be along vertical dimension (height)? Original logic uses resample to length line_length, which is width here for horizontal line, so resampled_values length == width

                # For horizontal shift of each row by per-column offset does not make sense, so must resample per row for vertical offsets - here horizontal shift implies offsets per row

                # For horizontal shift, offsets length should be equal to height, one value per row

            # Correction: For 0 degrees, horizontal offset, offsets length should be number of rows (height), each row shifted by offset

            resampled_values = self._resample_values(height)
            offsets = (resampled_values * width).astype(int)  # one offset per row
            output = np.empty_like(pixels_f)

            for y in range(height):
                offset = offsets[y]
                # Shift row y pixels to right by offset, wrap-around
                row = pixels_f[y]
                output[y] = np.roll(row, offset, axis=0)
            
            # Calculate blend mask
            blend_mask = np.ones((height, width), dtype=np.float32)
            if bw > 0:
                norm_offset = offsets[:, None]  # shape (height,1)
                blend_dist_start = np.clip(norm_offset / bw, 0, 1)
                blend_dist_end = np.clip((width - norm_offset) / bw, 0, 1)
                blend_mask = np.minimum(blend_dist_start, blend_dist_end)
                blend_mask = np.repeat(blend_mask, width, axis=1)

            blend_mask_3c = blend_mask[:, :, None]

            # Normalize field map alpha
            if self._map.ndim == 2:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
                mask_alpha = mask_alpha[:, :, None]
            elif self._map.ndim == 3:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
            else:
                mask_alpha = np.ones((height, width, 1), dtype=np.float32)

            final_alpha = mask_alpha * blend_mask_3c
            output = final_alpha * output + (1 - final_alpha) * pixels_f
            return np.clip(output, 0, 255).astype(np.uint8)

        elif angle_int == 180:
            # Horizontal offset to left (negative horizontal offset)
            resampled_values = self._resample_values(height)
            offsets = (resampled_values * width).astype(int)
            output = np.empty_like(pixels_f)
            for y in range(height):
                offset = offsets[y]
                row = pixels_f[y]
                output[y] = np.roll(row, -offset, axis=0)

            blend_mask = np.ones((height, width), dtype=np.float32)
            if bw > 0:
                norm_offset = offsets[:, None]
                blend_dist_start = np.clip(norm_offset / bw, 0, 1)
                blend_dist_end = np.clip((width - norm_offset) / bw, 0, 1)
                blend_mask = np.minimum(blend_dist_start, blend_dist_end)
                blend_mask = np.repeat(blend_mask, width, axis=1)

            blend_mask_3c = blend_mask[:, :, None]

            if self._map.ndim == 2:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
                mask_alpha = mask_alpha[:, :, None]
            elif self._map.ndim == 3:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
            else:
                mask_alpha = np.ones((height, width, 1), dtype=np.float32)

            final_alpha = mask_alpha * blend_mask_3c
            output = final_alpha * output + (1 - final_alpha) * pixels_f
            return np.clip(output, 0, 255).astype(np.uint8)

        elif angle_int == 90:
            # Vertical offset downward
            resampled_values = self._resample_values(width)
            offsets = (resampled_values * height).astype(int)
            output = np.empty_like(pixels_f)
            for x in range(width):
                column = pixels_f[:, x, :]
                offset = offsets[x]
                output[:, x, :] = np.roll(column, offset, axis=0)

            blend_mask = np.ones((height, width), dtype=np.float32)
            if bw > 0:
                norm_offset = offsets[None, :]
                blend_dist_start = np.clip(norm_offset / bw, 0, 1)
                blend_dist_end = np.clip((height - norm_offset) / bw, 0, 1)
                blend_mask = np.minimum(blend_dist_start, blend_dist_end)
                blend_mask = np.repeat(blend_mask, height, axis=0)

            blend_mask_3c = blend_mask[:, :, None]

            if self._map.ndim == 2:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
                mask_alpha = mask_alpha[:, :, None]
            elif self._map.ndim == 3:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
            else:
                mask_alpha = np.ones((height, width, 1), dtype=np.float32)

            final_alpha = mask_alpha * blend_mask_3c
            output = final_alpha * output + (1 - final_alpha) * pixels_f
            return np.clip(output, 0, 255).astype(np.uint8)

        elif angle_int == 270:
            # Vertical offset upward
            resampled_values = self._resample_values(width)
            offsets = (resampled_values * height).astype(int)
            output = np.empty_like(pixels_f)
            for x in range(width):
                column = pixels_f[:, x, :]
                offset = offsets[x]
                output[:, x, :] = np.roll(column, -offset, axis=0)

            blend_mask = np.ones((height, width), dtype=np.float32)
            if bw > 0:
                norm_offset = offsets[None, :]
                blend_dist_start = np.clip(norm_offset / bw, 0, 1)
                blend_dist_end = np.clip((height - norm_offset) / bw, 0, 1)
                blend_mask = np.minimum(blend_dist_start, blend_dist_end)
                blend_mask = np.repeat(blend_mask, height, axis=0)

            blend_mask_3c = blend_mask[:, :, None]

            if self._map.ndim == 2:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
                mask_alpha = mask_alpha[:, :, None]
            elif self._map.ndim == 3:
                mask_alpha = self._map.astype(np.float32)
                max_alpha = np.max(mask_alpha)
                if max_alpha > 0:
                    mask_alpha /= max_alpha
            else:
                mask_alpha = np.ones((height, width, 1), dtype=np.float32)

            final_alpha = mask_alpha * blend_mask_3c
            output = final_alpha * output + (1 - final_alpha) * pixels_f
            return np.clip(output, 0, 255).astype(np.uint8)

        # Else for other angles execute original full calculation path
        output = np.empty_like(pixels_f)
        angle_rad = np.deg2rad(self.angle)
        sin_a = np.sin(angle_rad)
        cos_a = np.cos(angle_rad)

        dir_vec = np.array([cos_a, sin_a], dtype=np.float32)
        offset_vec = np.array([-sin_a, cos_a], dtype=np.float32)

        corners = np.array([[0,0], [width-1, 0], [0, height-1], [width-1, height-1]], dtype=np.float32)
        proj = np.dot(corners, dir_vec)
        min_proj = proj.min()
        max_proj = proj.max()
        line_length = max_proj - min_proj
        length_int = max(1, int(np.ceil(line_length)))
        resampled_values = self._resample_values(length_int)
        max_offset = max(height, width)

        xs, ys = np.meshgrid(np.arange(width), np.arange(height))
        coords = np.stack((xs, ys), axis=-1).astype(np.float32)

        proj_coords = np.dot(coords, dir_vec)
        proj_normalized = (proj_coords - min_proj) / line_length * (length_int - 1)
        proj_normalized_clipped = np.clip(proj_normalized, 0, length_int - 1)

        idx_low = np.floor(proj_normalized_clipped).astype(int)
        idx_high = np.clip(idx_low + 1, 0, length_int - 1)
        weight_high = proj_normalized_clipped - idx_low
        weight_low = 1.0 - weight_high

        try:
            offset_vals = weight_low * resampled_values[idx_low] + weight_high * resampled_values[idx_high]
        except IndexError:
            offset_vals = np.zeros_like(proj_normalized_clipped, dtype=np.float32)

        offsets = offset_vals * max_offset
        offset_dx = offset_vec[0] * offsets
        offset_dy = offset_vec[1] * offsets

        new_x = (xs + offset_dx).astype(np.float32)
        new_y = (ys + offset_dy).astype(np.float32)

        new_x_wrapped = np.mod(new_x, width)
        new_y_wrapped = np.mod(new_y, height)

        x0 = np.floor(new_x_wrapped).astype(int)
        x1 = (x0 + 1) % width
        y0 = np.floor(new_y_wrapped).astype(int)
        y1 = (y0 + 1) % height

        x_frac = new_x_wrapped - x0
        y_frac = new_y_wrapped - y0

        try:
            p00 = pixels_f[y0, x0]
            p10 = pixels_f[y0, x1]
            p01 = pixels_f[y1, x0]
            p11 = pixels_f[y1, x1]
        except IndexError:
            return pixels

        top = p00 * (1 - x_frac[..., None]) + p10 * x_frac[..., None]
        bottom = p01 * (1 - x_frac[..., None]) + p11 * x_frac[..., None]
        p = top * (1 - y_frac[..., None]) + bottom * y_frac[..., None]

        blend_mask = np.ones((height, width), dtype=np.float32)
        if bw > 0:
            norm_offset = offsets
            blend_dist_start = np.clip(norm_offset / bw, 0, 1)
            blend_dist_end = np.clip((max_offset - norm_offset) / bw, 0, 1)
            blend_mask = np.minimum(blend_dist_start, blend_dist_end)

        blend_mask_3c = blend_mask[:, :, None]

        if self._map.ndim == 2:
            mask_alpha = self._map.astype(np.float32)
            max_alpha = np.max(mask_alpha)
            if max_alpha > 0:
                mask_alpha /= max_alpha
            mask_alpha = mask_alpha[:, :, None]
        elif self._map.ndim == 3:
            mask_alpha = self._map.astype(np.float32)
            max_alpha = np.max(mask_alpha)
            if max_alpha > 0:
                mask_alpha /= max_alpha
        else:
            mask_alpha = np.ones((height, width, 1), dtype=np.float32)

        final_alpha = mask_alpha * blend_mask_3c
        output = final_alpha * p + (1 - final_alpha) * pixels_f

        return np.clip(output, 0, 255).astype(np.uint8)