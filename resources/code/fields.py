if len(_paths) != 0:
    class Field:
        def __init_subclass__(cls, **kwargs):
            super().__init_subclass__(**kwargs)
            original_init = cls.__init__
            def profiled_init(self, *args, **kw):
                with _profile(f"construct_field:{cls.__name__}"):
                    original_init(self, *args, **kw)
            cls.__init__ = profiled_init

        def __init__(self) -> None:
            """Initialize a Field object with a blank, zero-filled map.

            The map is initialized with dimensions based on the renderer and
            stores 8-bit unsigned integer values (0–255).
            """
            self._map = np.zeros((renderer.height(), renderer.width()), dtype=np.uint8)
            self.inverted = False

        def get(self, x: int, y: int) -> np.float16:
            """Retrieve the normalized value at a given coordinate.

            Parameters:
                x (int): X-coordinate (column index).
                y (int): Y-coordinate (row index).

            Returns:
                np.float16: The value at (x, y), normalized to [0, 1].
            """
            return (self._map[y][x] / 255).astype(np.float16)

        def set(self, value: float, x: int, y: int) -> None:
            """Set a normalized value at a given coordinate.

            Parameters:
                value (float): Value between 0 and 1.
                x (int): X-coordinate.
                y (int): Y-coordinate.
            """
            self._map[y][x] = int(value * 255)

        def add(self, other: 'Field' or float) -> 'Field':
            """Add another Field or a scalar to this Field.

            Parameters:
                other (Field or float): Field or scalar value to add.

            Returns:
                Field: The updated Field object.
            """
            if isinstance(other, Field):
                return self.add(other.get_map())
            self._map = np.clip(self._map.astype(np.int16) + other * 255, 0, 255).astype(np.uint8)
            return self

        def sub(self, other: 'Field' or float) -> 'Field':
            """Subtract another Field or a scalar from this Field.

            Parameters:
                other (Field or float): Field or scalar value to subtract.

            Returns:
                Field: The updated Field object.
            """
            if isinstance(other, Field):
                return self.sub(other.get_map())
            self._map = self.add(other * -1)
            return self

        def mult(self, other: 'Field' or float) -> 'Field':
            """Multiply this Field by another Field or scalar.

            Parameters:
                other (Field or float): Field or scalar multiplier.

            Returns:
                Field: The updated Field object.
            """
            if isinstance(other, Field):
                return self.mult(other.get_map())
            self._map = np.clip(self._map.astype(np.int16) * other, 0, 255).astype(np.uint8)
            return self

        def div(self, other: 'Field' or float) -> 'Field':
            """Divide this Field by another Field or scalar.

            Parameters:
                other (Field or float): Field or scalar divisor.

            Returns:
                Field: The updated Field object.
            """
            if isinstance(other, Field):
                return self.div(other.get_map())
            self._map = np.clip(self._map.astype(np.int16) / other, 0, 255).astype(np.uint8)
            return self

        def __add__(self, other: 'Field' or float) -> 'Field':
            """Return a new Field representing this + other.

            Parameters:
                other (Field or float): Field or scalar value to add.

            Returns:
                Field: A new Field with the result.
            """
            with _profile("field.__add__"):
                # Fast clone — Field only carries _map and inverted.
                # copy.deepcopy is ~5-10x slower because it recursively traverses everything.
                clone = self.__class__.__new__(self.__class__)
                clone._map = self._map.copy()
                clone.inverted = self.inverted
                return clone.add(other)

        def __sub__(self, other: 'Field' or float) -> 'Field':
            """Return a new Field representing this - other.

            Parameters:
                other (Field or float): Field or scalar value to subtract.

            Returns:
                Field: A new Field with the result.
            """
            return self.__add__(other * -1)

        def __mul__(self, other: 'Field' or float) -> 'Field':
            """Return a new Field representing this * other.

            Parameters:
                other (Field or float): Field or scalar multiplier.

            Returns:
                Field: A new Field with the result.
            """
            clone = copy.deepcopy(self)
            return clone.mult(other)

        def __truediv__(self, other: 'Field' or float) -> 'Field':
            """Return a new Field representing this / other.

            Parameters:
                other (Field or float): Field or scalar divisor.

            Returns:
                Field: A new Field with the result.
            """
            clone = copy.deepcopy(self)
            clone.set_map(np.clip(self._map.astype(np.float16) / other, 0, 255).astype(np.uint8))
            return clone

        def invert(self) -> 'Field':
            """Invert the field, flipping 0s to 255s and vice versa.

            Returns:
                Field: The updated Field object.
            """
            self._map = 255 - self._map
            self.inverted = not self.inverted
            return self

        def move(self, dx: int, dy: int) -> 'Field':
            """Translate the field by (dx, dy) using an affine transform.

            Parameters:
                dx (int): Horizontal shift (positive is right).
                dy (int): Vertical shift (positive is down).

            Returns:
                Field: The updated Field object.

            Raises:
                ValueError: If the image exceeds OpenCV's size limits.
            """
            height, width = self._map.shape[:2]

            if width >= 32000 or height >= 32000:
                raise ValueError("Image too large for OpenCV warpAffine.")

            translation_matrix = np.float32([[1, 0, dx], [0, 1, dy]])
            self._map = cv2.warpAffine(
                self._map, translation_matrix, (width, height),
                borderMode=cv2.BORDER_CONSTANT,
                borderValue=255 if self.inverted else 0
            )
            return self

        def resize(self, width_or_scale: int or float, height: int = None) -> 'Field':
            """Resize the field to specific dimensions or by a scale factor.

            Parameters:
                width_or_scale (int or float): Target width or a scale factor.
                height (int, optional): Target height (only if resizing by explicit dimensions).

            Returns:
                Field: The resized Field object.
            """
            if isinstance(width_or_scale, (int, float)) and height is None:
                scale = width_or_scale
                width = int(self._map.shape[1] * scale)
                height = int(self._map.shape[0] * scale)
            else:
                width = int(width_or_scale)
                height = int(height)

            fill_value = 255 if self.inverted else 0

            if width > self._map.shape[1] or height > self._map.shape[0]:
                new_map = np.full((height, width), fill_value, dtype=np.uint8)
                overlap_x_end = min(self._map.shape[1], width)
                overlap_y_end = min(self._map.shape[0], height)
                new_map[:overlap_y_end, :overlap_x_end] = self._map[:overlap_y_end, :overlap_x_end]
                self._map = new_map
            else:
                self._map = cv2.resize(self._map, (width, height), interpolation=cv2.INTER_AREA)

            return self

        def scale(self, scale_x: float, scale_y: float = None) -> 'Field':
            """Scale the field by a factor along the x and y axes.

            Parameters:
                scale_x (float): Scaling factor for width.
                scale_y (float, optional): Scaling factor for height (defaults to scale_x if None).

            Returns:
                Field: The scaled Field object.

            Raises:
                ValueError: If any scale factor is non-positive.
            """
            if scale_x <= 0 or (scale_y is not None and scale_y <= 0):
                raise ValueError("Scale factors must be positive.")

            if scale_y is None:
                scale_y = scale_x

            new_width = int(self._map.shape[1] * scale_x)
            new_height = int(self._map.shape[0] * scale_y)
            self._map = cv2.resize(self._map, (new_width, new_height), interpolation=cv2.INTER_LINEAR)
            return self

        def fit(self) -> 'Field':
            """Stretch the region containing active values to fill the canvas.

            Returns:
                Field: The updated Field object.
            """
            target_value = 0 if self.inverted else 255
            coords = cv2.findNonZero((self._map == target_value).astype(np.uint8))

            if coords is None:
                return self

            x, y, w, h = cv2.boundingRect(coords)
            roi = self._map[y:y+h, x:x+w]
            resized_roi = cv2.resize(roi, (self._map.shape[1], self._map.shape[0]), interpolation=cv2.INTER_LINEAR)

            self._map = resized_roi
            if not self.inverted:
                self._map[self._map != 255] = 0
            else:
                self._map[self._map != 0] = 255

            return self

        def preview(self, wait_for_exit: bool = False, title: str = "Field Preview") -> None:
            """Display the field using OpenCV.

            Parameters:
                wait_for_exit (bool): Whether to wait for a user key press before exiting.
                title (str): Title of the display window.
            """
            window_name = title
            cv2.imshow(window_name, self._map)
            if wait_for_exit:
                while cv2.getWindowProperty(window_name, cv2.WND_PROP_VISIBLE) >= 1:
                    if cv2.waitKey(100) & 0xFF == ord('q'):
                        break
            else:
                cv2.waitKey(1)

        def get_map(self) -> np.ndarray:
            """Get the normalized field map.

            Returns:
                np.ndarray: The map normalized to [0, 1].
            """
            return self._map.astype(np.float16) / 255

        def set_map(self, map: np.ndarray) -> None:
            """Set the internal map.

            Parameters:
                map (np.ndarray): New map data (must match expected shape).
            """
            self._map = map

        def blur(self, param: tuple = (5, 5)) -> 'Field':
            """Apply a blur to the field.

            Parameters:
                param (tuple): Kernel size for blurring.

            Returns:
                Field: The blurred Field object.
            """
            self._map = cv2.blur(self._map, param)
            return self

        def mirror_x(self) -> 'Field':
            """Mirror the field along the vertical (X) axis.

            Returns:
                Field: The mirrored Field object.
            """
            self._map = cv2.flip(self._map, 1)
            return self

        def mirror_y(self) -> 'Field':
            """Mirror the field along the horizontal (Y) axis.

            Returns:
                Field: The mirrored Field object.
            """
            self._map = cv2.flip(self._map, 0)
            return self

        def crop(self, top_left: tuple, bottom_right: tuple) -> 'Field':
            """Crop the field to a rectangle defined by two points.

            Parameters:
                top_left (tuple): (x, y) coordinates for the top-left corner.
                bottom_right (tuple): (x, y) coordinates for the bottom-right corner.

            Returns:
                Field: The cropped Field object.
            """
            x1, y1 = map(int, top_left)
            x2, y2 = map(int, bottom_right)

            x1, x2 = max(0, min(x1, self._map.shape[1])), max(0, min(x2, self._map.shape[1]))
            y1, y2 = max(0, min(y1, self._map.shape[0])), max(0, min(y2, self._map.shape[0]))

            if x1 > x2:
                x1, x2 = x2, x1
            if y1 > y2:
                y1, y2 = y2, y1

            self._map = self._map[y1:y2, x1:x2]
            return self


    class FOverlay(Field):
        def __init__(self, opacity: float = 1.0) -> None:
            """Initialize an FOverlay object with a uniform opacity field.

            The overlay is initialized to a constant opacity across the entire canvas.
            Opacity should be a value between 0 (fully transparent) and 1 (fully opaque).

            Parameters:
                opacity (float, optional): Initial opacity value for the overlay.
                    Defaults to 1.0 (fully opaque).

            Raises:
                ValueError: If opacity is not within the range [0, 1].
            """
            if not (0.0 <= opacity <= 1.0):
                raise ValueError(f"Opacity must be between 0 and 1, but got {opacity}.")

            super().__init__()
            self._map = np.full(
                (renderer.height(), renderer.width()),
                int(opacity * 255),
                dtype=np.uint8
            )

    class FLine(Field):
        def __init__(self, x1: float, y1: float, x2: float, y2: float, thickness: float) -> None:
            """Initialize an FLine object that draws a straight line on the field.

            Creates a binary line between two points with a specified thickness.
            The line is drawn onto the field's internal map immediately upon initialization.

            Parameters:
                x1 (float): X-coordinate of the start point.
                y1 (float): Y-coordinate of the start point.
                x2 (float): X-coordinate of the end point.
                y2 (float): Y-coordinate of the end point.
                thickness (float): Thickness of the line in pixels.

            Raises:
                ValueError: If thickness is not a positive number.
            """
            if thickness <= 0:
                raise ValueError(f"Thickness must be a positive number, but got {thickness}.")

            super().__init__()

            # Safely draw on CPU
            map_cpu = rnp.zeros((renderer.height(), renderer.width()), dtype=rnp.uint8)
            cv2.line(
                map_cpu,
                (int(x1), int(y1)),
                (int(x2), int(y2)),
                color=255,
                thickness=int(thickness)
            )
            # Push back to GPU
            self._map = np.asarray(map_cpu)


    class FRect(Field):
        def __init__(self, x1: float, y1: float, x2: float, y2: float, thickness: int = -1) -> None:
            """Initialize an FRect object that draws a rectangle on the field.

            Creates a rectangle between two points, with customizable thickness.
            By default, the rectangle is filled if thickness is set to -1.

            Parameters:
                x1 (float): X-coordinate of the top-left corner.
                y1 (float): Y-coordinate of the top-left corner.
                x2 (float): X-coordinate of the bottom-right corner.
                y2 (float): Y-coordinate of the bottom-right corner.
                thickness (int, optional): Thickness of the rectangle border.
                    - Set to -1 to fill the rectangle. Defaults to -1.

            Raises:
                ValueError: If thickness is not -1 and is less than or equal to 0.
            """
            if thickness != -1 and thickness <= 0:
                raise ValueError(f"Thickness must be a positive number or -1 to fill, but got {thickness}.")

            super().__init__()

            # Safely draw on CPU
            map_cpu = rnp.zeros((renderer.height(), renderer.width()), dtype=rnp.uint8)
            cv2.rectangle(
                map_cpu,
                (int(x1), int(y1)),
                (int(x2), int(y2)),
                color=255,
                thickness=int(thickness)
            )
            # Push back to GPU
            self._map = np.asarray(map_cpu)


    class FEllipse(Field):
        def __init__(
                self,
                center: tuple[float, float],
                ellipse_width: float,
                ellipse_height: float,
                angle: float = 0,
                thickness: int = -1
        ) -> None:
            """Initialize an FEllipse object that draws an ellipse on the field.

            Creates an ellipse centered at a given point with specified width, height,
            rotation angle, and border thickness. By default, the ellipse is filled
            if thickness is set to -1.

            Parameters:
                center (tuple[float, float]): (x, y) coordinates for the center of the ellipse.
                ellipse_width (float): Total width of the ellipse's bounding box.
                ellipse_height (float): Total height of the ellipse's bounding box.
                angle (float, optional): Rotation angle of the ellipse in degrees.
                    Defaults to 0 (no rotation).
                thickness (int, optional): Thickness of the ellipse border.
                    - Set to -1 to fill the ellipse (default).

            Raises:
                ValueError: If thickness is not -1 and is less than or equal to 0.
            """
            if thickness != -1 and thickness <= 0:
                raise ValueError(f"Thickness must be a positive number or -1 to fill, but got {thickness}.")

            super().__init__()

            # Convert total width and height into semi-axes
            axes = (int(ellipse_width // 2), int(ellipse_height // 2))

            # Safely draw on CPU
            map_cpu = rnp.zeros((renderer.height(), renderer.width()), dtype=rnp.uint8)
            cv2.ellipse(
                map_cpu,
                (int(center[0]), int(center[1])),
                axes,
                angle,
                0, 360,  # Cover the full 360 degrees
                255,     # White color
                thickness
            )
            # Push back to GPU
            self._map = np.asarray(map_cpu)


    class FPoly(Field):
        def __init__(self, points: np.ndarray) -> None:
            """Initialize an FPoly object that draws a filled polygon on the field.

            Takes a set of points and fills a polygon based on their coordinates.
            The polygon will be drawn in white (value 255) onto the field map.

            Parameters:
                points (np.ndarray): A NumPy array of shape (N, 2) containing (x, y) coordinates
                    for the vertices of the polygon.

                    - N must be at least 3 to form a valid polygon.
                    - Points are automatically reshaped as required by OpenCV.

            Raises:
                ValueError: If fewer than 3 points are provided.
            """
            if points.shape[0] < 3:
                raise ValueError(f"A polygon requires at least 3 points, but received {points.shape[0]}.")

            super().__init__()

            # Safely pull the points array down to the CPU if it was created on the GPU
            if hasattr(points, 'get'):
                pts_cpu = points.get()
            else:
                pts_cpu = rnp.asarray(points)

            # Reshape points to (N, 1, 2) as expected by OpenCV
            pts_cpu = pts_cpu.reshape((-1, 1, 2)).astype(rnp.int32)

            # Safely draw on CPU
            map_cpu = rnp.zeros((renderer.height(), renderer.width()), dtype=rnp.uint8)
            cv2.fillPoly(map_cpu, [pts_cpu], color=255)

            # Push back to GPU
            self._map = np.asarray(map_cpu)


    class FText(Field):
        def __init__(
                self,
                text: str,
                position: tuple,
                font_scale: float,
                thickness: int = 1,
                custom_font: str = None
        ) -> None:
            """Initialize an FText object that renders text onto the field map.

            Allows rendering text either using a custom TrueType font (via Pillow) or using
            OpenCV's built-in fonts. The text is automatically center-aligned based on the
            provided position.

            Parameters:
                text (str): The text string to render.
                position (tuple): A tuple (x, y) representing the center position for the text.
                font_scale (float): Scale factor to size the text.
                thickness (int, optional): Thickness of the text stroke (default is 1).
                custom_font (str, optional): Path to a custom TTF font file.
                    If None, OpenCV's default font is used.

            Raises:
                FileNotFoundError: If a custom font path is provided but the file cannot be found.
            """
            super().__init__()

            if custom_font:
                self._draw_with_pillow(text, position, font_scale, custom_font)
            else:
                self._draw_with_opencv(text, position, font_scale, thickness)

        def _draw_with_pillow(
                self,
                text: str,
                position: tuple,
                font_scale: float,
                custom_font: str
        ) -> None:
            """Render text using Pillow with a custom TrueType font.

            Converts the internal field map to a Pillow image, draws the text,
            and converts it back to a NumPy array.

            Parameters:
                text (str): Text to render.
                position (tuple): Center position (x, y) for the text.
                font_scale (float): Scale factor for the font size.
                custom_font (str): Path to a .ttf font file.
            """
            # Create a blank CPU canvas so Pillow doesn't crash on CuPy arrays
            map_cpu = rnp.zeros((renderer.height(), renderer.width()), dtype=rnp.uint8)
            pil_image = Image.fromarray(map_cpu)
            draw = ImageDraw.Draw(pil_image)

            try:
                font_size = int(font_scale * 20)
                font = ImageFont.truetype(custom_font, font_size)
            except IOError:
                raise FileNotFoundError(f"Custom font file '{custom_font}' not found or could not be opened.")

            text_bbox = font.getbbox(text)  # (left, top, right, bottom)
            text_width = text_bbox[2] - text_bbox[0]
            text_height = text_bbox[3] - text_bbox[1]

            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] - text_height / 2)

            draw.text((bottom_left_x, bottom_left_y), text, font=font, fill=255)

            # Push back to GPU
            self._map = np.asarray(rnp.array(pil_image))

        def _draw_with_opencv(
                self,
                text: str,
                position: tuple,
                font_scale: float,
                thickness: int
        ) -> None:
            """Render text using OpenCV's built-in font.

            Parameters:
                text (str): Text to render.
                position (tuple): Center position (x, y) for the text.
                font_scale (float): Scale factor for the font size.
                thickness (int): Stroke thickness for the text.
            """
            (text_width, text_height), baseline = cv2.getTextSize(
                text, cv2.FONT_HERSHEY_SIMPLEX, font_scale, thickness
            )

            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] + text_height / 2)

            # Safely draw on CPU
            map_cpu = rnp.zeros((renderer.height(), renderer.width()), dtype=rnp.uint8)
            cv2.putText(
                map_cpu,
                text,
                (bottom_left_x, bottom_left_y),
                cv2.FONT_HERSHEY_SIMPLEX,
                font_scale,
                255,
                thickness,
                lineType=cv2.LINE_AA
            )
            # Push back to GPU
            self._map = np.asarray(map_cpu)


    class FAudio(Field):
        def __init__(self, aud: FrameAudio, start: int = 0, end: int = None) -> None:
            """Initialize an FAudio object to visualize the audio frame data on the field map.

            This class generates a visualization of the audio frame's magnitudes within the
            specified frequency range. It creates bars corresponding to the frequencies and
            displays a volume indicator based on the RMS volume of the frame.

            Parameters:
                aud (FrameAudio): The FrameAudio object containing the audio data (frequencies and magnitudes).
                start (int, optional): The starting index of the frequency range to visualize (default is 0).
                end (int, optional): The ending index of the frequency range to visualize. If None, uses the full range.

            Raises:
                ValueError: If the start or end indices are invalid.
                Exception: If an error occurs during the visualization process.
            """
            super().__init__()

            try:
                freqs = aud.list_frequencies()
                mags = aud.list_magnitudes()

                # Handle start and end indices, adjust for the frequency bin width
                if end is None:
                    end = len(freqs)
                else:
                    end = int(end / (freqs[1] - freqs[0]))  # Convert to index

                start = int(start / (freqs[1] - freqs[0]))  # Convert to index

                if end > len(freqs):
                    end = len(freqs)
                if start < 0 or start >= len(freqs):
                    raise ValueError(f"Invalid range: start={start}, end={end}")

                # Normalize the magnitudes for visualization
                norm = max(mags) / renderer.height()
                if norm == 0 or np.isnan(norm) or np.isinf(norm):
                    print(f"Normalization error, mags={mags}")
                    return

                # Create the points for frequency bars
                total_bars = end - start
                bar_width = renderer.width() / total_bars
                points = []

                for i in range(start, end):
                    x = i * bar_width + bar_width / 2
                    y = renderer.height() - mags[i] / norm
                    points.extend([x, y])

                # Add the base of the visualization (polygon to close the bars)
                points.extend([renderer.width() - bar_width, renderer.height(), 0, renderer.height()])
                self.add(FPoly(np.array(points, dtype=np.float32)))

                # Add the volume indicator as a rectangle
                self.add(FRect(
                    renderer.width() - bar_width,
                    renderer.height() - aud.get_volume() * renderer.height(),
                    renderer.width(),
                    renderer.height()
                ))

            except ValueError as ve:
                print(f"Error in FAudio initialization: {ve}")
            except Exception as e:
                print(f"Unexpected error initializing FAudio: {e}")

