class Media_Optimizer:
    @staticmethod
    def downscale_image(input_path: str, output_path: str, scale: float) -> bool:
        """
        Reads an image, downscales it by a float multiplier, and saves it.
        
        :param input_path: Path to the original image.
        :param output_path: Path to save the downscaled image.
        :param scale: Float multiplier (e.g., 0.5 for half size, 0.25 for quarter size).
        :return: True if successful, False if the image couldn't be read.
        """
        # 1. Load the original image
        img = cv2.imread(input_path)
        
        if img is None:
            print(f"Error: Could not read image at {input_path}")
            return False
            
        # 2. Calculate the exact new pixel dimensions
        new_width = int(img.shape[1] * scale)
        new_height = int(img.shape[0] * scale)
        
        # 3. Resize using INTER_AREA (best for crushing pixels down)
        resized_img = cv2.resize(img, (new_width, new_height), interpolation=cv2.INTER_AREA)
        
        # 4. Save to the new path
        cv2.imwrite(output_path, resized_img)
        return True