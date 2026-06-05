class Audio_Analyzer:
    """A suite of tools for advanced audio frequency and transient analysis."""

    @staticmethod
    def get_range_mag(freqs: rnp.ndarray, mags: rnp.ndarray, start_hz: float, end_hz: float) -> float:
        """
        Calculates the energy of a specific frequency band relative to the 
        total energy of the entire audio frame.
        Returns a value from 0.0 (silent) to 1.0 (all audio energy is in this band).
        """
        # Guard against completely silent frames to prevent divide-by-zero
        total_mag = float(rnp.sum(mags))
        if total_mag <= 0.0001:
            return 0.0

        bin_width = freqs[1] - freqs[0]
        
        # Convert Hz boundaries to array indices safely
        start_idx = max(0, int(start_hz / bin_width))
        end_idx = min(int(end_hz / bin_width), len(freqs))
        
        # Guard against reversed or invalid ranges
        if start_idx >= end_idx:
            return 0.0
            
        segment_mags = mags[start_idx:end_idx]
        
        # Sum the energy in our target band
        segment_mag_sum = float(rnp.sum(segment_mags))
        
        # Return the ratio (0.0 to 1.0)
        return min(1.0, segment_mag_sum / total_mag)
        
    @staticmethod
    def calculate_bloom(freqs: rnp.ndarray, mags: rnp.ndarray, peak_global_idx: int, window_hz: float = 40.0) -> float:
        """
        Calculates the transient splash relative to the peak magnitude.
        Returns a value from 0.0 (pure tone) to 1.0 (maximum noisy splash).
        """
        peak_freq = freqs[peak_global_idx]
        peak_mag = mags[peak_global_idx]
        
        # Silence guard: If the main note is basically dead, there is no bloom.
        if peak_mag <= 0.0001:
            return 0.0

        bin_width = freqs[1] - freqs[0]
        bin_range = int(window_hz / bin_width)
        
        start_i = max(0, peak_global_idx - bin_range)
        end_i = min(len(freqs), peak_global_idx + bin_range + 1)
        
        neighborhood_freqs = freqs[start_i:end_i]
        neighborhood_mags = mags[start_i:end_i]
        
        weights = 1.0 - (rnp.abs(neighborhood_freqs - peak_freq) / window_hz)
        weights = rnp.clip(weights, 0.0, 1.0)
        
        local_peak_idx = peak_global_idx - start_i
        weights[local_peak_idx] = 0.0
        
        # 1. Calculate actual splash
        raw_bloom = float(rnp.sum(neighborhood_mags * weights))
        
        # 2. Calculate maximum possible splash (if all neighbors were as loud as the peak)
        max_possible_bloom = float(peak_mag * rnp.sum(weights))
        
        if max_possible_bloom <= 0:
            return 0.0
            
        # 3. Return the ratio
        return min(1.0, raw_bloom / max_possible_bloom)
        
    @staticmethod
    def get_peak_data(freqs: rnp.ndarray, mags: rnp.ndarray, start_hz: float, end_hz: float) -> tuple[float, int]:
        """
        Finds the loudest frequency within a range.
        Returns a tuple containing:
        1. peak_norm: The normalized position of the peak (0.0 to 1.0) between start_hz and end_hz.
        2. global_peak_idx: The exact array index of the peak (useful for the bloom calculator).
        """
        bin_width = freqs[1] - freqs[0]
        
        # Convert Hz to array indices
        start_idx = max(0, int(start_hz / bin_width))
        end_idx = min(int(end_hz / bin_width), len(freqs))

        segment_mags = mags[start_idx:end_idx]
        
        # Guard against silent or empty segments
        if len(segment_mags) == 0 or rnp.max(segment_mags) <= 0.0001:
            return 0.0, 0
            
        # Find the peak within the slice, then calculate its global position
        max_idx = int(segment_mags.argmax())
        global_peak_idx = start_idx + max_idx
        peak_freq = freqs[global_peak_idx]
        
        # Normalize the peak's position (0.0 is start_hz, 1.0 is end_hz)
        # Note: Your original code used 350 here. This uses the true range (end_hz - start_hz).
        normalization_range = end_hz - start_hz
        peak_norm = float(rnp.clip((peak_freq - start_hz) / normalization_range, 0.0, 1.0))
        
        return peak_norm, global_peak_idx
    
    @staticmethod
    def get_mean_frequency(freqs: rnp.ndarray, mags: rnp.ndarray, start_hz: float, end_hz: float) -> tuple[float, float]:
        """
        Calculates the mean frequency (spectral centroid) within a specific range.
        Returns a tuple containing:
        1. mean_norm: The normalized position of the mean frequency (0.0 to 1.0) between start_hz and end_hz.
        2. exact_freq: The exact calculated mean frequency in Hz.
        """
        bin_width = freqs[1] - freqs[0]
        
        # Convert Hz to array indices safely
        start_idx = max(0, int(start_hz / bin_width))
        end_idx = min(int(end_hz / bin_width), len(freqs))

        segment_freqs = freqs[start_idx:end_idx]
        segment_mags = mags[start_idx:end_idx]
        
        # Guard against silent or empty segments to prevent division by zero
        total_mag = float(rnp.sum(segment_mags))
        if len(segment_mags) == 0 or total_mag <= 0.0001:
            return 0.0, float(start_hz)
            
        # Calculate the spectral centroid (weighted average of frequencies)
        # Formula: Sum(Frequency * Magnitude) / Sum(Magnitude)
        mean_freq = float(rnp.sum(segment_freqs * segment_mags) / total_mag)
        
        # Normalize the mean frequency's position (0.0 is start_hz, 1.0 is end_hz)
        normalization_range = end_hz - start_hz
        if normalization_range <= 0:
             return 0.0, float(start_hz)
             
        mean_norm = float(rnp.clip((mean_freq - start_hz) / normalization_range, 0.0, 1.0))
        
        return mean_norm, mean_freq
        
    @staticmethod
    def draw_visualizer(frame, frame_audio, x_range: tuple = (0.0, 0.5), y_range: tuple = (0.5, 1.0), freq_range: tuple = None, color: tuple = (255, 0, 0)):
        """
        Draws an audio visualizer graph directly onto the provided frame.
        x_range and y_range map the placement via normalized coordinates (0.0 to 1.0).
        """
        # Safely extract the dimensions of the current frame (works for both CPU and GPU arrays)
        pixels = frame.get_pixels(standard_size=True)
        h, w = pixels.shape[:2]

        # Calculate exact pixel sizes and positions based on the float ranges
        vis_width = w * (x_range[1] - x_range[0])
        vis_height = h * (y_range[1] - y_range[0])
        vis_x = w * x_range[0]
        vis_y = h * y_range[0]

        # Initialize the visualizer field with or without specific frequency bounds
        if freq_range is not None:
            audio_vis_field = FAudio(frame_audio, start=freq_range[0], end=freq_range[1])
        else:
            audio_vis_field = FAudio(frame_audio)

        # 1. Scale the graph down to the target box size
        audio_vis_field.scale(vis_width / w, vis_height / h)
        
        # 2. Expand its virtual bounding box back to full screen so it doesn't clip
        audio_vis_field.resize(w, h)
        
        # 3. Move it into the exact calculated screen coordinates
        audio_vis_field.move(int(vis_x), int(vis_y))

        # Unpack the color tuple into the filter and apply it to the frame
        frame.apply_filter(Solid_Color(color[0], color[1], color[2]).set_field(audio_vis_field))

        return frame