if _path != "":
    video = Video(_path)
    renderer = NonlinearRenderer(video)
    x_coords, y_coords = np.meshgrid(np.arange(video.width()), np.arange(video.height()))
    frame_indices = np.arange(video.frame_duration())

    video.audio.preload_data(video.frame_duration())