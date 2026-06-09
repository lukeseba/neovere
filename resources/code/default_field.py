class FCustom(Field):
    def __init__(self) -> None:
        """Initialize a custom vector field.

        A Field tracks only the shape(s) that define it (no background). Build
        your shape from the vector primitives (FRect, FEllipse, FPoly, FLine,
        FText, ...) and combine them with .add()/.sub() or the +, -, *, /
        operators. Edges can be softened with .feather(...) or the whole mask
        blurred with .blur(...); both are reversible (.unfeather()/.unblur()).
        """
        super().__init__()

        # Example: a rectangle covering the centre of the canvas.
        self.add(FRect(
            (renderer.width() * 0.25, renderer.height() * 0.25),
            (renderer.width() * 0.75, renderer.height() * 0.75)
        ))

        # self.feather(15)   # soften the shape's edges (reversible)
        # self.blur(15)      # blur the whole mask (reversible)
