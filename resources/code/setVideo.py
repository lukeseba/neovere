videos = {}
renderer = NonlinearRenderer(640, 480, 24)

#replace 'video' with renderer when getting video.height and such

if _paths:  # Ensure _paths is not empty
    # Create a dictionary to store Video objects with their names as keys

    for path in _paths:
        if path:  # Ensure the path is not empty
            # Extract the video name (file name without extension)
            video_name = os.path.splitext(os.path.basename(path))[0]
            videos[video_name] = Video(path)

    for video_name in videos:
        video = videos[video_name]
        video.audio.preload_data(video.frame_duration())