"""The explainer video's house style (videos/DESIGN.md, style C "DEC technical manual"): the palette, the paper and its
engineering grid, the scene timeline read from out/timing.json, the caption bar, and small drawing helpers."""
import sys
import textwrap
from pathlib import Path

import numpy as np
from manim import (DOWN, ImageMobject, Line, Rectangle, RoundedRectangle, Scene, Text, VGroup, VMobject, config)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import make_video as mv                 # noqa: E402

PAPER = "#f7f4ec"
INK = "#1d1d1d"
INK2 = "#6b665c"
RED = "#a3242c"
BLUE = "#2b5d9e"
GRID = "#e4dfd2"
BOX = "#fffdf7"
FONT = "Segoe UI"
LINE_W = 2.0                            # Manim stroke width for the 1.2-1.6 px lines at 1080p
CAPTION_CHARS = 64                      # per caption line; captions are at most 110 characters, so two lines


class Timeline:
    """One scene's place in out/timing.json, in scene-local seconds."""

    def __init__(self, scene_id):
        s = next(s for s in mv.load_timing()["scenes"] if s["id"] == scene_id)
        self.duration = s["end"] - s["start"]
        self.lines = [(ln["start"] - s["start"], ln["end"] - s["start"], ln["caption"]) for ln in s["lines"]]

    def line(self, k):
        """(start, end) of the scene's line k."""
        return self.lines[k][:2]


def paper_png():
    """The paper with its 12-px engineering grid, drawn once at 1920x1080 (out/paper.png)."""
    import matplotlib.image
    png = mv.OUT / "paper.png"
    if not png.exists():
        rgb = lambda h: [int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)]
        img = np.empty((1080, 1920, 3))
        img[:] = rgb(PAPER)
        img[::12, :] = rgb(GRID)
        img[:, ::12] = rgb(GRID)
        png.parent.mkdir(parents=True, exist_ok=True)
        matplotlib.image.imsave(str(png), img)
    return png


class PaperScene(Scene):
    """A scene on the paper, with its narration's captions, ending exactly when its timeline does."""
    scene_id = None

    def setup(self):
        self.camera.background_color = PAPER
        bg = ImageMobject(str(paper_png()))
        bg.height = config.frame_height
        self.add(bg)
        self.timeline = Timeline(self.scene_id)
        self.add_captions()

    def now(self):
        return self.renderer.time

    def add_captions(self):
        """The narration's lines at the bottom, each shown only while Paul says it."""
        band = Rectangle(width=config.frame_width, height=1.0, stroke_width=0, fill_color=PAPER, fill_opacity=1)
        band.move_to([0, -config.frame_height / 2 + 0.5, 0])
        rule = VGroup(band, Line([-6.6, -3.0, 0], [6.6, -3.0, 0], color=INK2, stroke_width=1))
        caps = VGroup()
        for start, end, text in self.timeline.lines:
            t = Text("\n".join(textwrap.wrap(text, CAPTION_CHARS)), font=FONT, color=INK, font_size=24,
                     line_spacing=0.8)
            t.move_to([0, -3.45, 0])
            t.start, t.end = start, end
            t.set_opacity(0)
            caps.add(t)

        def show(group, dt):            # dt makes it time-based, so manim draws waits frame by frame
            for t in group:
                t.set_opacity(1 if t.start <= self.now() < t.end else 0)
        caps.add_updater(show)
        self.add(rule, caps)

    def sync(self, t):
        """Wait until scene time t (no-op if it has passed)."""
        if t - self.now() > 1e-6:
            self.wait(t - self.now())

    def finish(self):
        self.sync(self.timeline.duration)


def wave(samples, width, height, color, stroke_width=LINE_W):
    """A polyline of samples, width wide, its peak at +-height/2, centred on the origin."""
    x = np.asarray(samples, dtype=float)
    if len(x) > 2000:                   # more points than pixels gains nothing
        x = x[np.linspace(0, len(x) - 1, 2000).astype(int)]
    peak = np.abs(x).max() or 1.0
    pts = np.stack([np.linspace(-width / 2, width / 2, len(x)), x / peak * height / 2, np.zeros(len(x))], axis=1)
    return VMobject(color=color, stroke_width=stroke_width).set_points_as_corners(pts)


def box(label, sub=""):
    """A block of the mockup: near-square corners, paper-white fill, the label bold, the sub-label small below."""
    r = RoundedRectangle(width=1.1, height=1.25, corner_radius=0.04, stroke_color=INK, stroke_width=LINE_W,
                         fill_color=BOX, fill_opacity=1)
    g = VGroup(r, Text(label, font=FONT, color=INK, font_size=30, weight="BOLD").move_to(r.get_center() + [0, 0.15, 0]))
    if sub:
        g.add(Text(sub, font=FONT, color=INK2, font_size=18).next_to(g[1], DOWN, buff=0.12))
    return g
