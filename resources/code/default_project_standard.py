from neovere import *


def setup():
    # Runs once — and again only when you edit this function.
    # Initialise variables and precompute expensive things here. Declare
    # anything render() needs as `global` so it survives between frames.
    global video
    video = media["render"]
    renderer.set_resolution(video.width(), video.height())
    renderer.set_fps(video.fps())
    renderer.set_duration(video.frame_duration())
    renderer.attach_audio(video.audio)  # play the source clip's audio in the preview


def render(f):
    # Renders ONE frame (index f), independently of every other frame.
    # Return a Frame (or raw HxWx3 uint8 pixels).
    frame = video.get_frame(f)
    return frame


run(setup, render)
