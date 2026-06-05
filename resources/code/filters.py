if len(_paths) != 0:
    class Filter:
        """A filter that aligns a Field object with the current renderer dimensions."""

        def __init_subclass__(cls, **kwargs):
            super().__init_subclass__(**kwargs)
            original_init = cls.__init__
            def profiled_init(self, *args, **kw):
                with _profile(f"construct_filter:{cls.__name__}"):
                    original_init(self, *args, **kw)
            cls.__init__ = profiled_init

        def __init__(self, field: Optional[Field] = None) -> None:
            """Initialize a Filter instance.

            Parameters:
                field (Optional[Field]): A Field object to apply the filter on.
                    If None, a default FOverlay() field will be used.
            """
            if field is None:
                field = FOverlay()
            self.set_field(field)

        def set_field(self, field: Field) -> 'Filter':
            """Set a new Field and crop its map to match the renderer dimensions.

            Parameters:
                field (Field): The Field object containing the source map.

            Returns:
                Filter: The Filter instance itself (for method chaining).
            """
            self.field = field
            full_map = self.field.get_map()[:, :, np.newaxis]

            render_height = renderer.height()
            render_width = renderer.width()

            # Fast path: If the map is already perfectly sized, use it instantly
            if full_map.shape[0] == render_height and full_map.shape[1] == render_width:
                self._map = full_map
                return self

            # Otherwise, force it to renderer dimensions to prevent broadcasting crashes
            target_map = np.zeros((render_height, render_width, 1), dtype=np.float32)

            orig_height, orig_width = full_map.shape[:2]

            start_y = max((render_height - orig_height) // 2, 0)
            start_x = max((render_width - orig_width) // 2, 0)

            copy_h = min(render_height, orig_height)
            copy_w = min(render_width, orig_width)

            src_start_y = max((orig_height - render_height) // 2, 0)
            src_start_x = max((orig_width - render_width) // 2, 0)

            # BRIDGE: Safely extract to CPU if it's on the GPU
            full_map_cpu = full_map.get() if hasattr(full_map, 'get') else full_map

            target_map[start_y:start_y+copy_h, start_x:start_x+copy_w] = full_map_cpu[src_start_y:src_start_y+copy_h, src_start_x:src_start_x+copy_w]

            # Push back to active hardware
            self._map = np.asarray(target_map)

            return self

        def _apply(self, pixels: np.ndarray) -> None:
            """Apply the filter to a set of pixels.

            Parameters:
                pixels (np.ndarray): The pixel data to process.

            Note:
                This method is currently a placeholder and must be implemented.
            """
            pass

    class Solid_Color(Filter):
        """A filter that overlays a solid color onto a Field-based mask."""

        def __init__(self, r: int, g: int, b: int, field: Optional[Field] = None) -> None:
            """Initialize a Solid_Color filter.

            Parameters:
                r (int): Red component of the color (0–255).
                g (int): Green component of the color (0–255).
                b (int): Blue component of the color (0–255).
                field (Optional[Field]): Field object providing the overlay mask.
                    If None, a default FOverlay() field will be used.
            """
            super().__init__(field)
            self.__r = r
            self.__g = g
            self.__b = b

        def invert(self) -> 'Solid_Color':
            """Invert the solid color (i.e., subtract each RGB component from 255).

            Returns:
                Solid_Color: The Solid_Color instance itself (for method chaining).
            """
            self.__r = 255 - self.__r
            self.__g = 255 - self.__g
            self.__b = 255 - self.__b
            return self

        def apply(self, pixels: np.ndarray) -> np.ndarray:
            """Apply the solid color filter to pixel data using the field mask."""

            # Explicitly convert the Python list to an array so CuPy can process it
            color_array = np.array([self.__b, self.__g, self.__r])

            pixels = np.clip(
                (pixels * (1 - self._map) + color_array * self._map),
                0, 255
            )
            return pixels

    class Invert(Filter):
        """A filter that inverts the colors of a Field-based mask."""

        def __init__(self, field: Optional[Field] = None) -> None:
            """Initialize an Invert filter.

            Parameters:
                field (Optional[Field]): Field object providing the overlay mask.
                    If None, a default FOverlay() field will be used.
            """
            super().__init__(field)

        def apply(self, pixels: np.ndarray) -> np.ndarray:
            """Apply the inversion filter to pixel data using the field mask.

            This method inverts the pixel colors and blends them based on the map values.

            Parameters:
                pixels (np.ndarray): The pixel data to apply the inversion filter to.

            Returns:
                np.ndarray: The color-inverted and blended pixel data, clipped to the valid range [0, 255].
            """
            # Invert the pixels
            inverted_pixels = 255 - pixels

            # Blend based on the map values
            filtered_pixels = (1 - self._map) * pixels + self._map * inverted_pixels

            return np.clip(filtered_pixels, 0, 255)

    class Draw_Frame(Filter):
        """A filter that draws a frame onto an image at a specified position."""

        def __init__(self, frame: Frame, x: Optional[int] = None, y: Optional[int] = None, field: Optional[Field] = None) -> None:
            """Initialize a Draw_Frame filter.

            Parameters:
                frame (Frame): The Frame object that will be drawn onto the image.
                x (Optional[int]): The x-coordinate for positioning the frame. If None, the frame is centered.
                y (Optional[int]): The y-coordinate for positioning the frame. If None, the frame is centered.
                field (Optional[Field]): Field object providing the overlay mask. Defaults to FOverlay() if None.
            """
            if field is None:
                field = FOverlay()
            super().__init__(field)
            self.frame = frame
            self.x = x
            self.y = y

        def apply(self, pixels: np.ndarray) -> np.ndarray:
            """Apply the frame to the given pixels at the specified (x, y) position.

            If `x` and `y` are None, the frame is centered. If the frame extends beyond the image bounds, it is cropped.
            Any uncovered space is filled with the original pixels.

            Parameters:
                pixels (np.ndarray): The pixel data onto which the frame will be applied.

            Returns:
                np.ndarray: The image with the frame applied at the specified position, blended with the field map.
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

        def set_position(self, x: int, y: int) -> 'Draw_Frame':
            """Updates the frame's position.

            Parameters:
                x (int): The new x-coordinate for the frame.
                y (int): The new y-coordinate for the frame.

            Returns:
                Draw_Frame: The current instance with updated position.
            """
            self.x = x
            self.y = y
            return self

        def invert(self) -> 'Draw_Frame':
            """Invert the frame colors.

            Returns:
                Draw_Frame: The current instance with the inverted frame.
            """
            self.frame = Frame(255 - self.frame.get_pixels())
            return self

        def mirror_x(self) -> 'Draw_Frame':
            """Mirror the frame along the x-axis (horizontal flip).

            Returns:
                Draw_Frame: The current instance with the frame mirrored along the x-axis.
            """
            self.frame = Frame(cv2.flip(self.frame.get_pixels(), 1))
            return self

        def mirror_y(self) -> 'Draw_Frame':
            """Mirror the frame along the y-axis (vertical flip).

            Returns:
                Draw_Frame: The current instance with the frame mirrored along the y-axis.
            """
            self.frame = Frame(cv2.flip(self.frame.get_pixels(), 0))
            return self

