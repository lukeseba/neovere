from neovere import *

for f in range(video.frame_duration()):
    frame = video.get_frame(f)
    audio = video.frame_audio(f)



    # preview frame before adding to renderer
    frame.preview()
    renderer.set_frame(f, frame)
# preview final render: True
renderer.render(True)