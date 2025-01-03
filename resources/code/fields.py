if len(_paths) != 0:
    class Field:
        def __init__(self):
            self._map = np.zeros((renderer.height(), renderer.width()), dtype = np.uint8)
            self.inverted = False

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

        def invert(self):
            self._map = 255 - self._map
            self.inverted = not self.inverted
            return self


        def move(self, x: int, y: int):
            """
            Efficiently shift the _map by (x, y) using OpenCV.
            Pixels shifted outside the bounds are filled with 0s or 1s based on `self.inverted`.

            :param x: Amount to shift in the x-direction (positive is right, negative is left).
            :param y: Amount to shift in the y-direction (positive is down, negative is up).
            """
            # Determine the fill value based on inversion
            fill_value = 255 if self.inverted else 0

            # Create the transformation matrix for shifting
            translation_matrix = np.float32([[1, 0, x], [0, 1, y]])

            # Apply the translation
            self._map = cv2.warpAffine(
                self._map, translation_matrix,
                (self._map.shape[1], self._map.shape[0]),  # Output size
                borderMode=cv2.BORDER_CONSTANT,
                borderValue=fill_value
            )
            return self


        def resize(self, width: int, height: int):
            """
            Resize the _map to the specified width and height using OpenCV.
            - If the new size is larger, fill new areas with 0s or 1s depending on `self.inverted`.
            - If the new size is smaller, crop the current map.

            :param width: New width of the _map.
            :param height: New height of the _map.
            """
            # Determine the fill value based on inversion
            fill_value = 255 if self.inverted else 0

            # If expanding, create a new map filled with the fill value
            if width > self._map.shape[1] or height > self._map.shape[0]:
                # Create a larger map filled with the fill value
                new_map = np.full((height, width), fill_value, dtype=np.uint8)
                # Determine the overlap region
                overlap_x_end = min(self._map.shape[1], width)
                overlap_y_end = min(self._map.shape[0], height)
                # Copy the old map into the new map
                new_map[:overlap_y_end, :overlap_x_end] = self._map[:overlap_y_end, :overlap_x_end]
                self._map = new_map
            else:
                # Use OpenCV to resize the map directly if shrinking or reshaping
                self._map = cv2.resize(self._map, (width, height), interpolation=cv2.INTER_AREA)

            return self

        def fit(self):
            """
            Stretches the region of interest (ROI) containing all the `255`s (or `0`s if inverted) to the edges of the canvas.
            """
            # Determine the target value based on whether the field is inverted
            target_value = 0 if self.inverted else 255

            # Find the bounding box of the region containing the target value
            coords = cv2.findNonZero((self._map == target_value).astype(np.uint8))
            if coords is None:
                return self  # If no target value exists, do nothing

            # Get the bounding box (x, y, width, height) of the region
            x, y, w, h = cv2.boundingRect(coords)

            # Extract the region of interest (ROI)
            roi = self._map[y:y+h, x:x+w]

            # Resize the ROI to fit the full canvas size
            resized_roi = cv2.resize(roi, (self._map.shape[1], self._map.shape[0]), interpolation=cv2.INTER_NEAREST)

            # Fill the map with the resized ROI
            self._map = resized_roi

            # Ensure binary values remain consistent after stretching
            if not self.inverted:
                self._map[self._map != 255] = 0  # Enforce binary values: 255 for target, 0 otherwise
            else:
                self._map[self._map != 0] = 255  # Enforce binary values: 0 for target, 255 otherwise

            return self


        def get_map(self):
            return self._map.astype(np.float16) / 255

        def set_map(self, map: np.ndarray):
            self._map = map

        def blur(self, param: tuple = (5, 5)):
            self._map = cv2.blur(self._map, param)
            return self

        def mirror_x(self):
            """
            Mirror the field along the x-axis (horizontal flip).
            """
            self._map = cv2.flip(self._map, 1)  # Flip around the vertical axis
            return self

        def mirror_y(self):
            """
            Mirror the field along the y-axis (vertical flip).
            """
            self._map = cv2.flip(self._map, 0)  # Flip around the horizontal axis
            return self

    class FOverlay(Field):
        def __init__(self, opacity: int = 1.0):
            super().__init__()
            self._map = np.full((renderer.height(), renderer.width()), opacity*255, dtype=np.uint8)

    class FPerlin(Field):
        def __init__(self, seed: int = 0, scale: int = 100, octaves: int = 4, persistence: int = 0.2, lacunarity: int = 2.0, contrast: int = 0.0, midpoint=0.5):
            super().__init__()

            # Parameters
            self.width, self.height = renderer.width(), renderer.height()
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
                (int(center[0]), int(center[1])),
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

    class FText(Field):
        def __init__(self, text: str, position: tuple, font_scale: float, thickness: int = 1, custom_font=None):
            """
            Initialize an FText object, automatically drawing the text onto the map.

            Parameters:
            - text (str): The text to render.
            - position (tuple): The (x, y) position for the center of the text.
            - font_scale (float): Scale of the text.
            - thickness (int): Thickness of the text strokes for OpenCV font.
            - custom_font (str or None): Path to a custom TTF font file. If None, uses OpenCV's default font.
            """
            super().__init__()

            if custom_font:
                # Use Pillow for custom fonts
                self._draw_with_pillow(text, position, font_scale, custom_font)
            else:
                # Fall back to OpenCV's putText
                self._draw_with_opencv(text, position, font_scale, thickness)

        def _draw_with_pillow(self, text, position, font_scale, custom_font):
            """Draw the text using Pillow and a custom font."""
            # Convert the Field map to a Pillow image
            pil_image = Image.fromarray(self._map)

            # Create a drawing context
            draw = ImageDraw.Draw(pil_image)

            # Load the custom font
            font_size = int(font_scale * 20)  # Scale font size appropriately
            font = ImageFont.truetype(custom_font, font_size)

            # Calculate text size and alignment using font.getbbox()
            text_bbox = font.getbbox(text)  # (left, top, right, bottom)
            text_width = text_bbox[2] - text_bbox[0]
            text_height = text_bbox[3] - text_bbox[1]

            # Calculate the top-left corner position for center alignment
            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] - text_height / 2)

            # Draw the text
            draw.text((bottom_left_x, bottom_left_y), text, font=font, fill=255)

            # Convert the Pillow image back to a NumPy array
            self._map = np.array(pil_image)

        def _draw_with_opencv(self, text, position, font_scale, thickness):
            """Draw the text using OpenCV's putText."""
            # Calculate the text size
            (text_width, text_height), baseline = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, font_scale, thickness)

            # Calculate the bottom-left corner position for center alignment
            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] + text_height / 2)

            # Draw the text centered on the map
            cv2.putText(
                self._map,
                text,
                (bottom_left_x, bottom_left_y),
                cv2.FONT_HERSHEY_SIMPLEX,
                font_scale,
                255,  # White text
                thickness,
                lineType=cv2.LINE_AA
            )



    class FAudio(Field):
        def __init__(self, aud: Frame_Audio, start: int = 0, end: int = None):
            super().__init__()
            try:
                freqs = aud.list_frequencies()
                mags = aud.list_magnitudes()

                # Handle start and end indices
                if end is None:
                    end = len(freqs)
                else:
                    end = int(end / (aud.list_frequencies()[1]-aud.list_frequencies()[0]))
                start = int(start / (aud.list_frequencies()[1]-aud.list_frequencies()[0]))

                if end > len(freqs):
                    end = len(freqs)
                if start < 0 or start >= len(freqs):
                    print(f"Invalid range: start={start}, end={end}")
                    return

                norm = max(mags) / renderer.height()
                if norm == 0 or np.isnan(norm) or np.isinf(norm):
                    print(f"Normalization error, mags={mags}")
                    return

                # Create visualization
                total_bars = end - start
                bar_width = renderer.width() / total_bars
                points = []

                for i in range(start, end):
                    x = i * bar_width + bar_width / 2
                    y = renderer.height() - mags[i] / norm
                    points.extend([x, y])

                points.extend([
                    renderer.width() - bar_width, renderer.height(),
                    0, renderer.height()
                ])

                self.add(FPoly(np.array(points, dtype=np.float32)))

                # Add volume indicator
                self.add(FRect(
                    renderer.width() - bar_width,
                    renderer.height() - aud.get_volume() * renderer.height(),
                    renderer.width(),
                    renderer.height()
                ))

            except Exception as e:
                print(f"Error initializing FAudio: {e}")
