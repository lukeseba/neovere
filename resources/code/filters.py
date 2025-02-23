if len(_paths) != 0:
    class Filter:
        def __init__(self, field: Field = FOverlay()):
            self.set_field(field)

        def set_field(self, field: Field):
            self.field = field
            self._map = self.field.get_map()[:, :, np.newaxis]
            return self

        def apply(self, pixels):
            pass

    class Solid_Color(Filter):
        def __init__(self, r: int, g: int, b: int, field: Field = FOverlay()):
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
        def __init__(self, field: Field = FOverlay()):
            super().__init__(field)

        def apply(self, pixels):
            # Invert the pixels
            inverted_pixels = 255 - pixels

            # Blend based on the map values
            filtered_pixels = (1 - self._map) * pixels + self._map * inverted_pixels

            return np.clip(filtered_pixels, 0, 255)

    class Draw_Frame(Filter):
        def __init__(self, frame: Frame, field: Field = FOverlay()):
            super().__init__(field)
            self.frame = frame

        def apply(self, pixels):
            return np.clip(self._map * self.frame.get_pixels() + (1 - self._map) * pixels, 0, 255)

        def invert(self):
            self.frame = Frame(255-self.frame.get_pixels())
            return self

        def mirror_x(self):
            """
            Mirror the frame along the x-axis (horizontal flip).
            """
            self.frame = Frame(cv2.flip(self.frame.get_pixels(), 1))  # Flip around the vertical axis
            return self

        def mirror_y(self):
            """
            Mirror the frame along the y-axis (vertical flip).
            """
            self.frame = Frame(cv2.flip(self.frame.get_pixels(), 0))  # Flip around the horizontal axis
            return self