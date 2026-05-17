from neovere import *

video = media["render"]

renderer.set_resolution(video.width(), video.height())
renderer.set_fps(video.fps())


for f in range(video.frame_duration()):
    frame = video.get_frame(f)
    audio = video.frame_audio(f)



    renderer.set_frame(f, frame)

renderer.attach_audio(video.audio)
renderer.render(False)