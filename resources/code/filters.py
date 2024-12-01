class Filter:
    def __init__(self, field: Field = FOverlay()):
        self.field = field
        self._map = self.field.get_map()[:, :, np.newaxis]

    def apply(self, pixels):
        pass

class Solid_Color(Filter):
    def __init__(self, r: int, g: int, b: int, field: Field = FOverlay()):
        super().__init__(field)
        self.__r = r
        self.__g = g
        self.__b = b

    def apply(self, pixels):
        pixels = np.clip((pixels * (1 - self._map) + [self.__b, self.__g, self.__r] * self._map), 0, 255)
        return pixels

class Draw_Frame(Filter):
    def __init__(self, frame: Frame, field: Field = FOverlay()):
        super().__init__(field)
        self.frame = frame

    def apply(self, pixels):
        return np.clip(self._map * self.frame.get_pixels() + (1 - self._map) * pixels, 0, 255)