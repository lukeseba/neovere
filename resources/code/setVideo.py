video = Video("[path]")
audio = Audio("[path]")
renderer = NonlinearRenderer(video)
arial = "[arial]"
if os.path.exists(arial):
    print(f"Font resource available at {arial}")
else:
    print("Font resource not found.")

x_coords, y_coords = np.meshgrid(np.arange(video.width()), np.arange(video.height()))