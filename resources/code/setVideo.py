media = {}
renderer = NonlinearRenderer(640, 480, 24)

#replace 'video' with renderer when getting video.height and such

if _paths:  # Ensure _paths is not empty
    # Create a dictionary to store Video objects with their names as keys

    for path in _paths:
        if path:  # Ensure the path is not empty
            # Extract the media name (file name without extension)
            media_full_name = os.path.splitext(os.path.basename(path))
            media_name = media_full_name[0]
            media_type = media_full_name[1]
            if media_type == ".mp4":
                media[media_name] = Video(path)
            elif media_type == ".mp3":
                media[media_name] = Audio(path)

    for media_name in media:
        current_media = media[media_name]
        if isinstance(current_media, Video) and current_media.audio != None:
            try:
                current_media.audio.preload_data()
            except:
                print("Failed to preload audio data for " + media_name)
        elif isinstance(current_media, Audio):
            try:
                current_media.preload_data()
            except:
                print("Failed to preload audio data for " + media_name)
