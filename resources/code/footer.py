def run(setup, render):
    """Hand control to the engine for a 'standard' project.

    A standard project is built from two functions instead of a frame loop:

        def setup():
            # Runs once. Re-runs only when you edit this function.
            # Initialise variables / precompute expensive things here,
            # and declare them `global` so render() can read them.
            ...

        def render(f):
            # Renders ONE frame (index f), independently of every other
            # frame. Return a Frame (or raw HxWx3 uint8 pixels).
            return ...

        run(setup, render)   # <- hand control to the engine

    The engine keeps your namespace alive between edits, so editing only
    render() will NOT re-run setup() (your precomputed state is preserved).
    While playing, the engine renders whatever frame matches the current
    audio timestamp and skips frames it can't produce in time, so the
    effective frame rate adapts to how expensive render() is.
    """
    renderer._run_standard(setup, render)
