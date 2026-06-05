class Color_Picker:
    @staticmethod
    def average_color(frame: Frame, points: list[tuple[int, int]]) -> tuple[int, int, int]:
        """Calculate the average color within specified (x, y) points in the frame.

        Parameters:
            frame (Frame): The Frame object to sample colors from.
            points (List[Tuple[int, int]]): A list of (x, y) tuples representing pixel coordinates.

        Returns:
            Tuple[int, int, int]: The average color as an (R, G, B) tuple.

        Raises:
            ValueError: If points list is empty or contains out-of-bounds coordinates.
        """
        if not points:
            raise ValueError("Points list cannot be empty.")

        pixels = frame.get_pixels(standard_size=True)
        height, width = pixels.shape[:2]

        sum_r = 0
        sum_g = 0
        sum_b = 0
        count = 0

        for x, y in points:
            if 0 <= x < width and 0 <= y < height:
                b, g, r = pixels[y, x]
                sum_r += int(r)
                sum_g += int(g)
                sum_b += int(b)
                count += 1
            else:
                raise ValueError(f"Point {(x, y)} is out of frame bounds ({width}, {height}).")

        if count == 0:
            raise ValueError("No valid points found within frame bounds.")

        avg_r = sum_r // count
        avg_g = sum_g // count
        avg_b = sum_b // count

        return (avg_r, avg_g, avg_b)