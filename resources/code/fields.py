class Field:
    def __init__(self):
        self._map = np.zeros((video.height(), video.width()), dtype = np.uint8)

    def get(self, x: int, y: int):
        return (self._map[y][x] / 255).astype(np.float16)

    def set(self, value: float, x: int, y: int):
        self._map[y][x] = int(value * 255)

    def add(self, other):
        if isinstance(other, Field):
            return self.add(other.get_map())
        else:
            self._map = np.clip(self._map.astype(np.int16) + other*255, 0, 255).astype(np.uint8)
            return self
    def sub(self, other):
        if isinstance(other, Field):
            return self.sub(other.get_map())
        else:
            self._map = self.add(other * -1)
            return self
    def mult(self, other):
        if isinstance(other, Field):
            return self.mult(other.get_map())
        else:
            self._map = np.clip(self._map.astype(np.int16) * other, 0, 255).astype(np.uint8)
            return self
    def div(self, other):
        if isinstance(other, Field):
            return self.div(other.get_map())
        else:
            self._map = np.clip(self._map.astype(np.int16) / other, 0, 255).astype(np.uint8)
            return self

    def __add__(self, other):
        if isinstance(other, Field):
            return self.__add__(other.get_map())
        else:
            clone = copy.deepcopy(self)
            clone.add(other)
            return clone

    def __sub__(self, other):
        if isinstance(other, Field):
            return self.__sub__(other.get_map())
        else:
            return self.__add__(other * -1)

    def __mul__(self, other):
        if isinstance(other, Field):
            return self.__mul__(other.get_map())
        else:
            clone = copy.deepcopy(self)
            clone.mult(other)
            return clone

    def __truediv__(self, other):
        if isinstance(other, Field):
            return self.__truediv__(other.get_map())
        else:
            clone = copy.deepcopy(self)
            clone.set_map(np.clip(self._map.astype(np.float16) / other, 0, 255).astype(np.uint8))
            return clone

    def get_map(self):
        return self._map.astype(np.float16) / 255

    def set_map(self, map: np.ndarray):
        self._map = map

class FOverlay(Field):
    def __init__(self, opacity: int = 1.0):
        super().__init__()
        self._map = np.full((video.height(), video.width()), opacity*255, dtype=np.uint8)

class FPerlin(Field):
    def __init__(self, seed: int = 0, scale: int = 100, octaves: int = 4, persistence: int = 0.2, lacunarity: int = 2.0, contrast: int = 0.0, midpoint=0.5):
        super().__init__()

        # Parameters
        self.width, self.height = video.width(), video.height()
        self.scale = scale
        self.octaves = octaves
        self.persistence = persistence
        self.lacunarity = lacunarity
        self.seed = seed
        self.contrast = contrast
        self.midpoint = midpoint


        # Generate grid of coordinates
        self.update()

    def update(self):
        x = np.linspace(0, self.width / self.scale, self.width)
        y = np.linspace(0, self.height / self.scale, self.height)
        x_coords, y_coords = np.meshgrid(x, y)


        # Vectorized noise function
        vectorized_pnoise2 = np.vectorize(lambda x, y: pnoise2(x, y, octaves=self.octaves, persistence=self.persistence, lacunarity=self.lacunarity, base=self.seed))

        # Apply Perlin noise
        self._map = (vectorized_pnoise2(x_coords, y_coords) + 1)/2
        if (self.contrast != 0):
            self._map = 1 / (1 + np.exp(-self.contrast * (self._map - self.midpoint)))
        self._map *= 255

class FLine(Field):
    def __init__(self, x1, y1, x2, y2, thickness):
        super().__init__()
        cv2.line(self._map,(int(x1), int(y1)), (int(x2), int(y2)), 255, int(thickness))

class FRect(Field):
    def __init__(self, x1, y1, x2, y2, thickness = -1):
        super().__init__()
        cv2.rectangle(self._map,(int(x1), int(y1)), (int(x2), int(y2)), 255, int(thickness))

class FEllipse(Field):
    def __init__(self, center, ellipse_width, ellipse_height, angle=0, thickness=-1):
        """
        Initialize an FEllipse object, automatically drawing the ellipse onto the map.

        Parameters:
        - width (int): Width of the field (canvas).
        - height (int): Height of the field (canvas).
        - center (tuple): The (x, y) center of the ellipse.
        - ellipse_width (int): The total width of the ellipse (bounding box width).
        - ellipse_height (int): The total height of the ellipse (bounding box height).
        - angle (float): The rotation angle of the ellipse in degrees (default 0).
        - thickness (int): Thickness of the ellipse border (-1 for filled ellipse, default).
        """
        super().__init__()

        # Convert width and height to axes (semi-width and semi-height)
        axes = (ellipse_width // 2, ellipse_height // 2)

        # Draw the ellipse directly on the map
        cv2.ellipse(
            self._map,
            center,
            axes,
            angle,
            0, 360,  # Full ellipse
            255,  # White ellipse
            thickness
        )

class FPoly(Field):
    def __init__(self, points: np.ndarray):
        super().__init__()

        # Reshape the points for OpenCV (required shape: number_of_points x 1 x 2)
        points = points.reshape((-1, 1, 2)).astype(np.int32)

        # Draw the polygon outline
        cv2.fillPoly(self._map, [points], color=255)

class FAudio(Field):
    def __init__(self, frame_index: int, start: int=0, end: int=None):
        super().__init__()
        audio_data = audio.frame_audio(frame_index)
        volume = audio_data["volume"]
        freqs = audio_data["frequencies"]
        mags = audio_data["magnitude"]
        if end == None:
            end = len(freqs)
        else:
            end = int(end/5)
        start = int(start/5)

        total_bars = end-start
        bar_width = video.width()/(total_bars)
        points = np.array([], dtype=np.float32)
        norm = max(mags)/video.height()

        for i in range(start, end):
           points = np.append(points, [i*bar_width+bar_width/2, video.height()-mags[i]/norm])
        points = np.append(points, [video.width()-bar_width, video.height()])
        points = np.append(points, [0, video.height()])
        self.add(FPoly(
            points
        ))

        self.add(FRect(
            video.width()-bar_width,
            video.height()-volume*video.height(),
            video.width(),
            video.height()
        ))