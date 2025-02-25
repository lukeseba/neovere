if len(_paths) != 0:
    class Filter:
        def __init__(self, field: Field = None):
            if field == None:
                field = FOverlay()
            self.set_field(field)

        def set_field(self, field: Field):
            self.field = field
            self._map = self.field.get_map()[:, :, np.newaxis]
            return self

        def apply(self, pixels):
            pass

    class Solid_Color(Filter):
        def __init__(self, r: int, g: int, b: int, field: Field = None):
            super().__init__(field)
            self.__r = r
            self.__g = g
            self.__b = b

        def invert(self):
            self.__r = 255 - self.__r
            self.__g = 255 - self.__g
            self.__b = 255 - self.__b
            return self

        def apply(self, pixels):
            pixels = np.clip((pixels * (1 - self._map) + [self.__b, self.__g, self.__r] * self._map), 0, 255)
            return pixels

    class Invert(Filter):
        def __init__(self, field: Field = None):
            super().__init__(field)

        def apply(self, pixels):
            # Invert the pixels
            inverted_pixels = 255 - pixels

            # Blend based on the map values
            filtered_pixels = (1 - self._map) * pixels + self._map * inverted_pixels

            return np.clip(filtered_pixels, 0, 255)

    class Draw_Frame(Filter):
        def __init__(self, frame: Frame, x: int = None, y: int = None, field: Field = None):
            if field is None:
                field = FOverlay()
            super().__init__(field)
            self.frame = frame
            self.x = x
            self.y = y

        def apply(self, pixels):
            """
            Applies the frame to the given pixels at the specified (x, y) position.
            - If `x` and `y` are None, the frame is centered.
            - If the frame extends beyond the image bounds, it is cropped.
            - Any uncovered space is filled with the original pixels.
            """
            frame_pixels = self.frame.get_pixels()
            frame_h, frame_w = frame_pixels.shape[:2]
            pixels_h, pixels_w = pixels.shape[:2]

            # Determine x and y position (centered if None)
            if self.x is None:
                x_offset = (pixels_w - frame_w) // 2
            else:
                x_offset = self.x

            if self.y is None:
                y_offset = (pixels_h - frame_h) // 2
            else:
                y_offset = self.y

            # Ensure the offsets are within bounds
            x_start = max(x_offset, 0)
            y_start = max(y_offset, 0)
            x_end = min(x_offset + frame_w, pixels_w)
            y_end = min(y_offset + frame_h, pixels_h)

            # Compute the region of the frame that fits within the image bounds
            frame_x_start = max(0, -x_offset)
            frame_y_start = max(0, -y_offset)
            frame_x_end = frame_x_start + (x_end - x_start)
            frame_y_end = frame_y_start + (y_end - y_start)

            # Prevent assignment if the cropped dimensions are invalid
            if x_end <= x_start or y_end <= y_start or frame_x_end <= frame_x_start or frame_y_end <= frame_y_start:
                return pixels  # Return unchanged pixels if there's nothing to draw

            # Create a copy of the original pixels
            new_frame = pixels.copy()

            # Apply the frame onto the new canvas only within valid bounds
            new_frame[y_start:y_end, x_start:x_end] = frame_pixels[frame_y_start:frame_y_end, frame_x_start:frame_x_end]

            # Blend with the field map
            return np.clip(self._map * new_frame + (1 - self._map) * pixels, 0, 255)

        def set_position(self, x: int, y: int):
            """Updates the frame's position."""
            self.x = x
            self.y = y
            return self

        def invert(self):
            self.frame = Frame(255 - self.frame.get_pixels())
            return self

        def mirror_x(self):
            """Mirror the frame along the x-axis (horizontal flip)."""
            self.frame = Frame(cv2.flip(self.frame.get_pixels(), 1))
            return self

        def mirror_y(self):
            """Mirror the frame along the y-axis (vertical flip)."""
            self.frame = Frame(cv2.flip(self.frame.get_pixels(), 0))
            return self

