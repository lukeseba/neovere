if len(_paths) != 0:
    class Field:
        """A vector-based field.

        A Field tracks only the *shape(s)* that define it (polygons / contours)
        rather than a full-canvas bitmap. There is no background: the geometry is
        stored resolution-independently and rasterized on demand by ``get_map``.

        Each field also carries reversible, render-time effects:
            * ``blur_amount``    - softens the whole mask (Gaussian).
            * ``feather_amount`` - fades the shape edges inward.
            * ``inverted``       - flips the mask (everything except the shape).
        These are applied only while rasterizing, so the underlying geometry is
        never destroyed and the effects can be undone (set the amount back to 0).
        """

        def __init_subclass__(cls, **kwargs):
            super().__init_subclass__(**kwargs)
            original_init = cls.__init__
            def profiled_init(self, *args, **kw):
                with _profile(f"construct_field:{cls.__name__}"):
                    original_init(self, *args, **kw)
            cls.__init__ = profiled_init

        def __init__(self) -> None:
            """Initialize an empty Field (no shapes, no background)."""
            self.shapes = []                       # list of shape dicts (vector or raster)
            self.blur_amount = 0.0                 # reversible Gaussian blur (px @ ref res)
            self.feather_amount = 0.0              # reversible edge feather (px @ ref res)
            self.inverted = False                  # invert the rasterized mask
            self._ref_w = int(renderer.width())    # resolution shapes were authored at
            self._ref_h = int(renderer.height())
            self._cache = {}                       # (h, w) -> cached CPU float32 mask
            self._device_cache = {}                # (h, w) -> cached backend (GPU/CPU) map

        # ------------------------------------------------------------------ #
        # Shape construction helpers (used by subclasses)
        # ------------------------------------------------------------------ #
        def _add_poly(self, contours, holes=None, value: float = 1.0, additive: bool = True) -> 'Field':
            """Append a filled polygon shape (with optional holes) to this field."""
            c = [rnp.asarray(x, dtype=rnp.float32).reshape(-1, 2) for x in contours]
            hl = [rnp.asarray(x, dtype=rnp.float32).reshape(-1, 2) for x in (holes or [])]
            self.shapes.append({
                'type': 'poly', 'contours': c, 'holes': hl,
                'value': float(value), 'additive': bool(additive),
            })
            return self

        def _add_contours_from_mask(self, mask, value: float = 1.0, additive: bool = True) -> 'Field':
            """Vectorize a binary/grayscale bitmap into polygon contours (with holes).

            Used for shapes that are easiest to draw first (e.g. text) and then
            convert to a resolution-independent vector form.
            """
            m = mask.get() if hasattr(mask, 'get') else rnp.asarray(mask)
            if m.dtype == rnp.uint8:
                binary = (m > 127).astype(rnp.uint8)
            else:
                binary = (m > 0.5).astype(rnp.uint8)
            found = cv2.findContours(binary, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_SIMPLE)
            contours = found[-2]
            hierarchy = found[-1]
            outers, holes = [], []
            if hierarchy is not None and len(contours):
                hierarchy = hierarchy[0]
                for i, cnt in enumerate(contours):
                    pts = cnt.reshape(-1, 2).astype(rnp.float32)
                    if len(pts) < 3:
                        continue
                    if hierarchy[i][3] == -1:
                        outers.append(pts)
                    else:
                        holes.append(pts)
            if outers or holes:
                self.shapes.append({
                    'type': 'poly', 'contours': outers, 'holes': holes,
                    'value': float(value), 'additive': bool(additive),
                })
            return self

        def _fullcanvas_shape(self, value: float, additive: bool = True) -> dict:
            """A shape covering the whole reference canvas (used by FOverlay / scalars)."""
            w, h = self._ref_w, self._ref_h
            rect = rnp.array([[0, 0], [w, 0], [w, h], [0, h]], dtype=rnp.float32)
            return {'type': 'poly', 'contours': [rect], 'holes': [],
                    'value': float(value), 'additive': bool(additive)}

        def _raster_shape(self, mask, additive: bool = True) -> dict:
            """Wrap a precomputed float mask as a (resolution-locked) raster shape."""
            m = mask.get() if hasattr(mask, 'get') else rnp.asarray(mask)
            m = m.astype(rnp.float32)
            if m.shape[0] != self._ref_h or m.shape[1] != self._ref_w:
                m = cv2.resize(m, (self._ref_w, self._ref_h), interpolation=cv2.INTER_LINEAR)
            return {'type': 'raster', 'mask': m, 'value': 1.0, 'additive': bool(additive)}

        # ------------------------------------------------------------------ #
        # Cloning
        # ------------------------------------------------------------------ #
        @staticmethod
        def _copy_shape(s: dict) -> dict:
            ns = dict(s)
            if s.get('type') == 'raster':
                ns['mask'] = s['mask'].copy()
            else:
                ns['contours'] = [c.copy() for c in s.get('contours', [])]
                ns['holes'] = [c.copy() for c in s.get('holes', [])]
            return ns

        def _clone(self) -> 'Field':
            """Fast, independent copy (deepcopy is far slower for our small state)."""
            clone = self.__class__.__new__(self.__class__)
            clone.shapes = [self._copy_shape(s) for s in self.shapes]
            clone.blur_amount = self.blur_amount
            clone.feather_amount = self.feather_amount
            clone.inverted = self.inverted
            clone._ref_w = self._ref_w
            clone._ref_h = self._ref_h
            clone._cache = {}
            clone._device_cache = {}
            return clone

        def _bump(self) -> None:
            """Invalidate cached rasterizations after any mutation to shape or effects."""
            if self._cache:
                self._cache.clear()
            if self._device_cache:
                self._device_cache.clear()

        @staticmethod
        def _scalar_value(other):
            """Return a float if ``other`` is a scalar, else None."""
            if isinstance(other, bool):
                return None
            if isinstance(other, (int, float)):
                return float(other)
            if hasattr(other, 'ndim') and getattr(other, 'ndim', None) == 0:
                try:
                    return float(other)
                except Exception:
                    return None
            if rnp.isscalar(other):
                try:
                    return float(other)
                except Exception:
                    return None
            return None

        def _is_pure(self) -> bool:
            """True if no render-time effects are baked in (safe to union by reference)."""
            return (not self.inverted) and (not self.blur_amount) and (not self.feather_amount)

        # ------------------------------------------------------------------ #
        # Rasterization (the only place a bitmap is produced)
        # ------------------------------------------------------------------ #
        @staticmethod
        def _scaled_int(contour, sx, sy):
            pts = rnp.asarray(contour, dtype=rnp.float32).reshape(-1, 2)
            pts = pts * rnp.array([sx, sy], dtype=rnp.float32)
            return rnp.round(pts).astype(rnp.int32)

        @staticmethod
        def _feather_mask(mask, amount):
            a = float(amount)
            if a <= 0:
                return mask
            binary = (mask > 0.004).astype(rnp.uint8)
            if int(binary.max()) == 0:
                return mask
            dist = cv2.distanceTransform(binary, cv2.DIST_L2, 3)
            ramp = rnp.clip(dist / a, 0.0, 1.0).astype(rnp.float32)
            return (mask * ramp).astype(rnp.float32)

        @staticmethod
        def _blur_mask(mask, amount):
            k = int(round(float(amount)))
            if k <= 1:
                return mask
            if k % 2 == 0:
                k += 1
            return cv2.GaussianBlur(mask.astype(rnp.float32), (k, k), 0)

        def _rasterize(self, height: int, width: int):
            """Render the field's shapes to a CPU float32 mask in [0, 1]."""
            height = int(height)
            width = int(width)

            # Legacy fallback: support custom fields that still assign a raw self._map.
            if not self.shapes:
                legacy = getattr(self, '_map', None)
                if legacy is not None:
                    m = legacy.get() if hasattr(legacy, 'get') else rnp.asarray(legacy)
                    m = m.astype(rnp.float32)
                    if m.ndim == 3:
                        m = m[..., 0]
                    if float(m.max() if m.size else 0) > 1.0001:
                        m = m / 255.0
                    if m.shape[0] != height or m.shape[1] != width:
                        m = cv2.resize(m, (width, height), interpolation=cv2.INTER_LINEAR)
                    if self.inverted:
                        m = 1.0 - m
                    return rnp.clip(m, 0.0, 1.0).astype(rnp.float32)

            key = (height, width)
            cached = self._cache.get(key)
            if cached is not None:
                return cached

            ref_w = self._ref_w if self._ref_w else width
            ref_h = self._ref_h if self._ref_h else height
            sx = width / float(ref_w)
            sy = height / float(ref_h)

            acc = rnp.zeros((height, width), dtype=rnp.float32)
            for s in self.shapes:
                if s.get('type') == 'raster':
                    m = s['mask']
                    m = m.get() if hasattr(m, 'get') else rnp.asarray(m)
                    if m.shape[0] != height or m.shape[1] != width:
                        m = cv2.resize(m.astype(rnp.float32), (width, height), interpolation=cv2.INTER_LINEAR)
                    layer = m.astype(rnp.float32)
                else:
                    layer8 = rnp.zeros((height, width), dtype=rnp.uint8)
                    cnts = [self._scaled_int(c, sx, sy) for c in s.get('contours', [])]
                    cnts = [c for c in cnts if len(c) >= 3]
                    if cnts:
                        cv2.fillPoly(layer8, cnts, 255)
                    holes = [self._scaled_int(c, sx, sy) for c in s.get('holes', [])]
                    holes = [c for c in holes if len(c) >= 3]
                    if holes:
                        cv2.fillPoly(layer8, holes, 0)
                    layer = layer8.astype(rnp.float32) / 255.0

                v = float(s.get('value', 1.0))
                if v != 1.0:
                    layer = layer * v
                if s.get('additive', True):
                    acc += layer
                else:
                    acc -= layer

            rnp.clip(acc, 0.0, 1.0, out=acc)

            if self.feather_amount and self.feather_amount > 0:
                acc = self._feather_mask(acc, self.feather_amount * sx)
            if self.blur_amount and self.blur_amount > 0:
                acc = self._blur_mask(acc, self.blur_amount * sx)
            if self.inverted:
                acc = 1.0 - acc

            if len(self._cache) > 8:
                self._cache.clear()
            self._cache[key] = acc
            return acc

        # ------------------------------------------------------------------ #
        # Combination operations
        # ------------------------------------------------------------------ #
        def _bake_self(self) -> 'Field':
            """Flatten this field's shapes + effects into a single raster shape.

            After this the field's geometry and its (formerly reversible) effects
            are fused into one mask, so anything composited on top is unaffected
            by those effects.
            """
            base = self._rasterize(self._ref_h, self._ref_w)
            self.shapes = [{'type': 'raster', 'mask': base.astype(rnp.float32),
                            'value': 1.0, 'additive': True}]
            self.blur_amount = 0.0
            self.feather_amount = 0.0
            self.inverted = False
            self._bump()
            return self

        def _absorb(self, other, additive: bool) -> 'Field':
            """Combine ``other`` into this field (union if additive, else difference).

            Fields carry their own (reversible) blur/feather/invert effects, so two
            fields with different looks can't simply share raw geometry. When this
            field already has effects baked in, it is flattened first and the
            other's *rendered* mask is composited on top — matching the old bitmap
            behaviour where each mask kept its own appearance.
            """
            sv = self._scalar_value(other)
            if sv is not None:
                if not self._is_pure():
                    self._bake_self()
                self.shapes.append(self._fullcanvas_shape(sv, additive))
                return self

            if isinstance(other, Field):
                if additive and self._is_pure() and other._is_pure():
                    # Pure geometry on both sides: union losslessly by copying shapes.
                    for s in other.shapes:
                        ns = self._copy_shape(s)
                        self._rescale_shape(ns, other._ref_w, other._ref_h, self._ref_w, self._ref_h)
                        self.shapes.append(ns)
                else:
                    # Effects present (or subtraction): composite rendered masks so
                    # each field keeps its own look.
                    if not self._is_pure():
                        self._bake_self()
                    m = other.get_map(self._ref_h, self._ref_w)
                    self.shapes.append(self._raster_shape(m, additive))
                return self

            # Raw array map (e.g. another field's get_map()).
            if hasattr(other, 'shape'):
                if not self._is_pure():
                    self._bake_self()
                m = other.get() if hasattr(other, 'get') else rnp.asarray(other)
                m = m.astype(rnp.float32)
                if m.ndim == 3:
                    m = m[..., 0]
                if float(m.max() if m.size else 0) > 1.0001:
                    m = m / 255.0
                self.shapes.append(self._raster_shape(m, additive))
                return self

            raise TypeError(f"Cannot combine Field with {type(other)!r}.")

        @staticmethod
        def _rescale_shape(s: dict, from_w, from_h, to_w, to_h) -> dict:
            if from_w == to_w and from_h == to_h:
                return s
            if s.get('type') == 'raster':
                s['mask'] = cv2.resize(s['mask'], (int(to_w), int(to_h)), interpolation=cv2.INTER_LINEAR)
            else:
                f = rnp.array([to_w / float(from_w), to_h / float(from_h)], dtype=rnp.float32)
                s['contours'] = [c * f for c in s['contours']]
                s['holes'] = [c * f for c in s['holes']]
            return s

        def _bake_combine(self, other, divide: bool) -> 'Field':
            """Multiply/divide by another field or map by baking to a raster shape."""
            base = self._rasterize(self._ref_h, self._ref_w)
            if isinstance(other, Field):
                om = other.get_map(self._ref_h, self._ref_w)
            else:
                om = other
            om = om.get() if hasattr(om, 'get') else rnp.asarray(om)
            om = om.astype(rnp.float32)
            if om.ndim == 3:
                om = om[..., 0]
            if float(om.max() if om.size else 0) > 1.0001:
                om = om / 255.0
            if om.shape[0] != self._ref_h or om.shape[1] != self._ref_w:
                om = cv2.resize(om, (self._ref_w, self._ref_h), interpolation=cv2.INTER_LINEAR)
            if divide:
                om = rnp.where(om <= 1e-6, 1e-6, om)
                res = rnp.clip(base / om, 0.0, 1.0)
            else:
                res = rnp.clip(base * om, 0.0, 1.0)
            # Effects are now baked into the raster; reset them.
            self.shapes = [{'type': 'raster', 'mask': res.astype(rnp.float32),
                            'value': 1.0, 'additive': True}]
            self.blur_amount = 0.0
            self.feather_amount = 0.0
            self.inverted = False
            self._bump()
            return self

        def add(self, other) -> 'Field':
            """Add another Field, a raw map, or a scalar to this Field (in place)."""
            self._absorb(other, True)
            self._bump()
            return self

        def sub(self, other) -> 'Field':
            """Subtract another Field, a raw map, or a scalar from this Field (in place)."""
            self._absorb(other, False)
            self._bump()
            return self

        def mult(self, other) -> 'Field':
            """Multiply this Field by a scalar (scales intensity) or another Field (in place)."""
            sv = self._scalar_value(other)
            if sv is not None:
                for s in self.shapes:
                    s['value'] = s.get('value', 1.0) * sv
                self._bump()
                return self
            return self._bake_combine(other, divide=False)

        def div(self, other) -> 'Field':
            """Divide this Field by a scalar or another Field (in place)."""
            sv = self._scalar_value(other)
            if sv is not None:
                if sv == 0:
                    raise ValueError("Cannot divide a Field by zero.")
                for s in self.shapes:
                    s['value'] = s.get('value', 1.0) / sv
                self._bump()
                return self
            return self._bake_combine(other, divide=True)

        def __add__(self, other) -> 'Field':
            with _profile("field.__add__"):
                return self._clone().add(other)

        def __sub__(self, other) -> 'Field':
            return self._clone().sub(other)

        def __mul__(self, other) -> 'Field':
            return self._clone().mult(other)

        def __truediv__(self, other) -> 'Field':
            return self._clone().div(other)

        # ------------------------------------------------------------------ #
        # Geometry transforms (lossless – operate on the vector shapes)
        # ------------------------------------------------------------------ #
        def _content_points(self):
            pts = []
            for s in self.shapes:
                if s.get('type') == 'poly':
                    for c in s['contours']:
                        pts.append(rnp.asarray(c, dtype=rnp.float32).reshape(-1, 2))
                    for c in s['holes']:
                        pts.append(rnp.asarray(c, dtype=rnp.float32).reshape(-1, 2))
            if pts:
                return rnp.concatenate(pts, axis=0)
            return None

        def _content_bbox(self):
            pts = self._content_points()
            if pts is not None and len(pts):
                x0, y0 = pts.min(axis=0)
                x1, y1 = pts.max(axis=0)
                return float(x0), float(y0), float(x1), float(y1)
            for s in self.shapes:
                if s.get('type') == 'raster':
                    ys, xs = rnp.where(s['mask'] > 0.004)
                    if len(xs):
                        return float(xs.min()), float(ys.min()), float(xs.max()), float(ys.max())
            return None

        def _content_center(self):
            bbox = self._content_bbox()
            if bbox is None:
                return self._ref_w / 2.0, self._ref_h / 2.0
            x0, y0, x1, y1 = bbox
            return (x0 + x1) / 2.0, (y0 + y1) / 2.0

        def set_position(self, position: tuple) -> 'Field':
            """Move the field so its center sits at the given point.

            Parameters:
                position (tuple) @position: (x, y) pixel coordinates for the field's center.
            """
            cx, cy = self._content_center()
            x, y = float(position[0]), float(position[1])
            dx, dy = x - cx, y - cy
            off = rnp.array([dx, dy], dtype=rnp.float32)
            for s in self.shapes:
                if s.get('type') == 'raster':
                    M = rnp.float32([[1, 0, dx], [0, 1, dy]])
                    s['mask'] = cv2.warpAffine(s['mask'], M, (self._ref_w, self._ref_h),
                                               borderMode=cv2.BORDER_CONSTANT, borderValue=0)
                else:
                    s['contours'] = [c + off for c in s['contours']]
                    s['holes'] = [c + off for c in s['holes']]
            self._bump()
            return self

        def resize(self, scale: float) -> 'Field':
            """Scale the field by ``scale``, anchored at its own center.

            Parameters:
                scale (float): Scale factor (>1 grows, <1 shrinks). The shape's
                    center stays put while it grows/shrinks around it.
            """
            scale = float(scale)
            if scale <= 0:
                raise ValueError("Resize scale must be positive.")
            cx, cy = self._content_center()
            center = rnp.array([cx, cy], dtype=rnp.float32)
            for s in self.shapes:
                if s.get('type') == 'raster':
                    M = cv2.getRotationMatrix2D((float(cx), float(cy)), 0.0, scale)
                    s['mask'] = cv2.warpAffine(s['mask'], M, (self._ref_w, self._ref_h),
                                               borderMode=cv2.BORDER_CONSTANT, borderValue=0)
                else:
                    s['contours'] = [((c - center) * scale + center) for c in s['contours']]
                    s['holes'] = [((c - center) * scale + center) for c in s['holes']]
            self._bump()
            return self

        def fit(self) -> 'Field':
            """Stretch the field's bounding box to fill the whole canvas."""
            bbox = self._content_bbox()
            if bbox is None:
                return self
            x0, y0, x1, y1 = bbox
            bw = max(x1 - x0, 1e-6)
            bh = max(y1 - y0, 1e-6)
            sx = self._ref_w / bw
            sy = self._ref_h / bh
            origin = rnp.array([x0, y0], dtype=rnp.float32)
            factor = rnp.array([sx, sy], dtype=rnp.float32)
            for s in self.shapes:
                if s.get('type') == 'raster':
                    M = rnp.float32([[sx, 0, -x0 * sx], [0, sy, -y0 * sy]])
                    s['mask'] = cv2.warpAffine(s['mask'], M, (self._ref_w, self._ref_h))
                else:
                    s['contours'] = [((c - origin) * factor) for c in s['contours']]
                    s['holes'] = [((c - origin) * factor) for c in s['holes']]
            self._bump()
            return self

        def mirror_x(self) -> 'Field':
            """Mirror the field along the vertical (X) axis."""
            w = self._ref_w
            for s in self.shapes:
                if s.get('type') == 'raster':
                    s['mask'] = cv2.flip(s['mask'], 1)
                else:
                    for c in s['contours']:
                        c[:, 0] = w - c[:, 0]
                    for c in s['holes']:
                        c[:, 0] = w - c[:, 0]
            self._bump()
            return self

        def mirror_y(self) -> 'Field':
            """Mirror the field along the horizontal (Y) axis."""
            h = self._ref_h
            for s in self.shapes:
                if s.get('type') == 'raster':
                    s['mask'] = cv2.flip(s['mask'], 0)
                else:
                    for c in s['contours']:
                        c[:, 1] = h - c[:, 1]
                    for c in s['holes']:
                        c[:, 1] = h - c[:, 1]
            self._bump()
            return self

        def invert(self) -> 'Field':
            """Invert the field (everything except the shape). Reversible."""
            self.inverted = not self.inverted
            self._bump()
            return self

        # ------------------------------------------------------------------ #
        # Reversible render-time effects
        # ------------------------------------------------------------------ #
        def blur(self, amount=5) -> 'Field':
            """Set the (reversible) blur amount in pixels. ``amount`` may also be a
            kernel tuple like ``(5, 5)`` for backwards compatibility. Use
            ``unblur()`` or ``blur(0)`` to remove it without touching the shape."""
            if isinstance(amount, (tuple, list)):
                amount = max(amount) if len(amount) else 0
            self.blur_amount = float(amount)
            self._bump()
            return self

        def feather(self, amount=5) -> 'Field':
            """Set the (reversible) feather amount in pixels (fades edges inward).
            Use ``unfeather()`` or ``feather(0)`` to remove it."""
            if isinstance(amount, (tuple, list)):
                amount = max(amount) if len(amount) else 0
            self.feather_amount = float(amount)
            self._bump()
            return self

        def unblur(self) -> 'Field':
            """Remove blur, restoring the crisp underlying shape."""
            self.blur_amount = 0.0
            self._bump()
            return self

        def unfeather(self) -> 'Field':
            """Remove feathering, restoring hard edges."""
            self.feather_amount = 0.0
            self._bump()
            return self

        # ------------------------------------------------------------------ #
        # Sampling / output
        # ------------------------------------------------------------------ #
        def get(self, x: int, y: int) -> float:
            """Sample the (rasterized) normalized value at a coordinate, in [0, 1]."""
            m = self._rasterize(self._ref_h, self._ref_w)
            yy, xx = int(y), int(x)
            if 0 <= yy < m.shape[0] and 0 <= xx < m.shape[1]:
                return float(m[yy, xx])
            return 0.0

        def preview(self, wait_for_exit: bool = False, title: str = "Field Preview") -> None:
            """Display the rasterized field using OpenCV."""
            m = self._rasterize(self._ref_h, self._ref_w)
            img = (rnp.clip(m, 0.0, 1.0) * 255).astype(rnp.uint8)
            cv2.imshow(title, img)
            if wait_for_exit:
                while cv2.getWindowProperty(title, cv2.WND_PROP_VISIBLE) >= 1:
                    if cv2.waitKey(100) & 0xFF == ord('q'):
                        break
            else:
                cv2.waitKey(1)

        def get_map(self, height: int = None, width: int = None) -> np.ndarray:
            """Rasterize the field to a normalized [0, 1] bitmap.

            Parameters:
                height (int, optional): Target height. Defaults to the renderer's.
                width (int, optional): Target width. Defaults to the renderer's.

            Returns:
                np.ndarray: A (height, width) float32 map in [0, 1]. Because the
                field is vector-based, it is rasterized at whatever resolution is
                requested, so it always matches the pixels it will be applied to.
            """
            if height is None:
                height = renderer.height()
            if width is None:
                width = renderer.width()
            height = int(height)
            width = int(width)
            key = (height, width)
            cached = self._device_cache.get(key)
            if cached is not None:
                return cached
            dm = np.asarray(self._rasterize(height, width))
            if len(self._device_cache) > 8:
                self._device_cache.clear()
            self._device_cache[key] = dm
            return dm


    class FOverlay(Field):
        def __init__(self, opacity: float = 1.0) -> None:
            """A uniform field covering the entire canvas at a constant opacity.

            Handy as a solid base layer or a tint: feed it to a color filter to
            flood the frame, or lower its opacity to dim whatever sits behind it.

            Parameters:
                opacity (float): Fill strength from 0.0 (fully transparent) to
                    1.0 (fully opaque). Defaults to 1.0. Values outside 0..1
                    raise a ValueError.
            """
            if not (0.0 <= opacity <= 1.0):
                raise ValueError(f"Opacity must be between 0 and 1, but got {opacity}.")
            super().__init__()
            self.shapes.append(self._fullcanvas_shape(float(opacity), True))


    class FLine(Field):
        def __init__(self, start: tuple, end: tuple, thickness: float) -> None:
            """A straight line segment of fixed thickness, stored as a vector rectangle.

            The line runs from start to end; its thickness is added evenly to both
            sides of that center line.

            Parameters:
                start (tuple) @position: (x, y) pixel coordinates of the start point.
                end (tuple) @position: (x, y) pixel coordinates of the end point.
                thickness (float): Line width in pixels, centered on the segment.
                    Must be greater than 0.
            """
            if thickness <= 0:
                raise ValueError(f"Thickness must be a positive number, but got {thickness}.")
            super().__init__()
            x1, y1 = float(start[0]), float(start[1])
            x2, y2 = float(end[0]), float(end[1])
            half = float(thickness) / 2.0
            dx, dy = x2 - x1, y2 - y1
            length = (dx * dx + dy * dy) ** 0.5
            if length < 1e-6:
                contour = rnp.array([
                    [x1 - half, y1 - half], [x1 + half, y1 - half],
                    [x1 + half, y1 + half], [x1 - half, y1 + half],
                ], dtype=rnp.float32)
            else:
                ux, uy = dx / length, dy / length
                nx, ny = -uy * half, ux * half
                contour = rnp.array([
                    [x1 + nx, y1 + ny], [x2 + nx, y2 + ny],
                    [x2 - nx, y2 - ny], [x1 - nx, y1 - ny],
                ], dtype=rnp.float32)
            self._add_poly([contour])


    class FRect(Field):
        def __init__(self, corner1: tuple, corner2: tuple, thickness: int = -1) -> None:
            """A rectangle, either filled solid or drawn as a hollow border ring.

            The two corners may be given in any order; the rectangle spans the
            bounding box between them.

            Parameters:
                corner1 (tuple) @position: (x, y) pixel coordinates of one corner.
                corner2 (tuple) @position: (x, y) pixel coordinates of the opposite corner.
                thickness (int): Border width in pixels, centered on the edges.
                    Use -1 (the default) to fill the rectangle solid; any other
                    value must be positive.
            """
            if thickness != -1 and thickness <= 0:
                raise ValueError(f"Thickness must be a positive number or -1 to fill, but got {thickness}.")
            super().__init__()
            x1, y1 = float(corner1[0]), float(corner1[1])
            x2, y2 = float(corner2[0]), float(corner2[1])
            ax, bx = min(x1, x2), max(x1, x2)
            ay, by = min(y1, y2), max(y1, y2)
            if thickness == -1:
                self._add_poly([self._rect_pts(ax, ay, bx, by)])
            else:
                h = float(thickness) / 2.0
                outer = self._rect_pts(ax - h, ay - h, bx + h, by + h)
                ix1, iy1, ix2, iy2 = ax + h, ay + h, bx - h, by - h
                holes = [self._rect_pts(ix1, iy1, ix2, iy2)] if (ix2 > ix1 and iy2 > iy1) else None
                self._add_poly([outer], holes=holes)

        @staticmethod
        def _rect_pts(ax, ay, bx, by):
            return rnp.array([[ax, ay], [bx, ay], [bx, by], [ax, by]], dtype=rnp.float32)


    class FEllipse(Field):
        def __init__(
                self,
                center: tuple,
                ellipse_width: float,
                ellipse_height: float,
                angle: float = 0,
                thickness: int = -1
        ) -> None:
            """An ellipse, approximated as a vector polygon, filled or as a border ring.

            Parameters:
                center (tuple) @position: (x, y) pixel coordinates of the ellipse's center.
                ellipse_width (float): Full width (horizontal diameter) in pixels,
                    measured before any rotation.
                ellipse_height (float): Full height (vertical diameter) in pixels,
                    measured before any rotation.
                angle (float): Clockwise rotation of the ellipse, in degrees.
                    Defaults to 0.
                thickness (int): Border width in pixels. Use -1 (the default) to
                    fill the ellipse solid; any other value must be positive.
            """
            if thickness != -1 and thickness <= 0:
                raise ValueError(f"Thickness must be a positive number or -1 to fill, but got {thickness}.")
            super().__init__()
            cx, cy = int(center[0]), int(center[1])
            ax = max(int(ellipse_width // 2), 1)
            ay = max(int(ellipse_height // 2), 1)
            ang = int(angle)
            if thickness == -1:
                pts = cv2.ellipse2Poly((cx, cy), (ax, ay), ang, 0, 360, 5)
                self._add_poly([rnp.asarray(pts, dtype=rnp.float32)])
            else:
                h = int(thickness // 2)
                outer = cv2.ellipse2Poly((cx, cy), (ax + h, ay + h), ang, 0, 360, 5)
                inner = cv2.ellipse2Poly((cx, cy), (max(ax - h, 1), max(ay - h, 1)), ang, 0, 360, 5)
                self._add_poly([rnp.asarray(outer, dtype=rnp.float32)],
                               holes=[rnp.asarray(inner, dtype=rnp.float32)])


    class FPoly(Field):
        def __init__(self, points: np.ndarray) -> None:
            """A filled polygon defined by an ordered list of (x, y) vertices.

            Parameters:
                points (np.ndarray): Vertex coordinates as an (N, 2) array or a
                    flat sequence of x, y pairs, in pixels. At least 3 vertices
                    are required; the outline closes automatically from the last
                    vertex back to the first.
            """
            pts = points.get() if hasattr(points, 'get') else rnp.asarray(points)
            pts = pts.reshape(-1, 2)
            if pts.shape[0] < 3:
                raise ValueError(f"A polygon requires at least 3 points, but received {pts.shape[0]}.")
            super().__init__()
            self._add_poly([pts.astype(rnp.float32)])


    class FText(Field):
        def __init__(
                self,
                text: str,
                position: tuple,
                font_scale: float,
                thickness: int = 1,
                custom_font: str = None
        ) -> None:
            """Text rendered once to a bitmap, then traced into vector contours.

            Because the glyphs become contours, the result behaves like any other
            vector field and can be moved, scaled, cropped, and so on.

            Parameters:
                text (str): The string to render.
                position (tuple) @position: (x, y) pixel coordinates of the text's center.
                font_scale (float): Glyph size multiplier. With a custom font this
                    maps to a pixel height; with the built-in font it is OpenCV's
                    font scale.
                thickness (int): Stroke width in pixels for the built-in font.
                    Defaults to 1. Ignored when custom_font is supplied.
                custom_font (str): Path to a .ttf/.otf font file, rendered via
                    Pillow. Defaults to None, which uses the built-in OpenCV font.
            """
            super().__init__()
            mask = rnp.zeros((self._ref_h, self._ref_w), dtype=rnp.uint8)
            if custom_font:
                mask = self._draw_with_pillow(mask, text, position, font_scale, custom_font)
            else:
                mask = self._draw_with_opencv(mask, text, position, font_scale, thickness)
            self._add_contours_from_mask(mask)

        def _draw_with_pillow(self, mask, text, position, font_scale, custom_font):
            pil_image = Image.fromarray(mask)
            draw = ImageDraw.Draw(pil_image)
            try:
                font_size = int(font_scale * 20)
                font = ImageFont.truetype(custom_font, font_size)
            except IOError:
                raise FileNotFoundError(f"Custom font file '{custom_font}' not found or could not be opened.")
            text_bbox = font.getbbox(text)
            text_width = text_bbox[2] - text_bbox[0]
            text_height = text_bbox[3] - text_bbox[1]
            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] - text_height / 2)
            draw.text((bottom_left_x, bottom_left_y), text, font=font, fill=255)
            return rnp.array(pil_image)

        def _draw_with_opencv(self, mask, text, position, font_scale, thickness):
            (text_width, text_height), baseline = cv2.getTextSize(
                text, cv2.FONT_HERSHEY_SIMPLEX, font_scale, thickness
            )
            bottom_left_x = int(position[0] - text_width / 2)
            bottom_left_y = int(position[1] + text_height / 2)
            cv2.putText(
                mask, text, (bottom_left_x, bottom_left_y),
                cv2.FONT_HERSHEY_SIMPLEX, font_scale, 255, thickness, lineType=cv2.LINE_AA
            )
            return mask


    class FAudio(Field):
        def __init__(self, aud: FrameAudio, start: int = 0, end: int = None) -> None:
            """Visualize an audio frame's frequency spectrum as a bar graph.

            Builds a full-canvas bar graph from the frame's frequency magnitudes,
            with a separate bar on the right showing the overall volume.

            Parameters:
                aud (FrameAudio): The audio frame to visualize; supplies the
                    per-frequency magnitudes and the overall volume.
                start (int): Lowest frequency to include, in hertz. Defaults to 0.
                end (int): Highest frequency to include, in hertz. Defaults to
                    None, which extends to the highest available frequency.
            """
            super().__init__()

            try:
                freqs = aud.list_frequencies()
                mags = aud.list_magnitudes()

                # Handle start and end indices based on the frequency bin width
                bin_width_hz = freqs[1] - freqs[0]

                if end is None:
                    end_idx = len(freqs)
                else:
                    end_idx = int(end / bin_width_hz)

                start_idx = int(start / bin_width_hz)

                if end_idx > len(freqs):
                    end_idx = len(freqs)
                if start_idx < 0 or start_idx >= len(freqs):
                    raise ValueError(f"Invalid range: start={start_idx}, end={end_idx}")

                # Normalize the magnitudes for visualization
                norm = max(mags) / renderer.height()
                if norm == 0 or np.isnan(norm) or np.isinf(norm):
                    return  # Silent frame, leave the field empty

                # Create the points for frequency bars
                total_bars = end_idx - start_idx
                if total_bars <= 0:
                    return

                bar_width = renderer.width() / total_bars
                points = []

                for i in range(start_idx, end_idx):
                    # Subtract start_idx so the first point is always drawn at x=0
                    x = (i - start_idx) * bar_width + bar_width / 2
                    y = renderer.height() - (mags[i] / norm)
                    points.extend([x, y])

                # Add the base of the visualization (polygon to close the bars)
                points.extend([renderer.width(), renderer.height(), 0, renderer.height()])
                self.add(FPoly(np.array(points, dtype=np.float32)))

                # Add the volume indicator as a rectangle
                self.add(FRect(
                    (renderer.width() - bar_width,
                     renderer.height() - aud.get_volume() * renderer.height()),
                    (renderer.width(), renderer.height())
                ))

            except ValueError as ve:
                print(f"Error in FAudio initialization: {ve}")
            except Exception as e:
                print(f"Unexpected error initializing FAudio: {e}")
