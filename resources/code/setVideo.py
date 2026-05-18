media = {}
renderer = NonlinearRenderer(640, 480, 24)

#replace 'video' with renderer when getting video.height and such

if _paths:
    for path in _paths:
        if path:
            media_full_name = os.path.splitext(os.path.basename(path))
            media_name = media_full_name[0]
            media_type = media_full_name[1].lower()

            if media_type == ".mp4":
                media[media_name] = Video(path)
            elif media_type == ".mp3":
                media[media_name] = Audio(path)
            elif media_type in [".jpg", ".jpeg", ".png"]:
                media[media_name] = ImageFile(path)

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