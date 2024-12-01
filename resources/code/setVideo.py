video = Video("[path]")
audio = Audio("[path]")
renderer = NonlinearRenderer(video)

x_coords, y_coords = np.meshgrid(np.arange(video.width()), np.arange(video.height()))