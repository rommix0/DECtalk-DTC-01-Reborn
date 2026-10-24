"""The explainer video's opening and closing scenes: the unit powering up, the big picture of the two chips, and the
outro with its song (videos/DESIGN.md)."""
import sys
import wave
from pathlib import Path

import numpy as np
from manim import (DOWN, LEFT, RIGHT, UP, Arrow, Circle, Create, Dot, FadeIn, FadeOut, ImageMobject, LaggedStart,
                   Line, MoveAlongPath, Rectangle, Transform, VGroup, Write)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from scenes.common import BLUE, BOX, INK, INK2, LINE_W, RED, PaperScene, Scope, box, header   # noqa: E402
from scenes.dsp import label                                                                 # noqa: E402
import make_video as mv                                                                      # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent


def narration(scene_id, k):
    """Paul's line k of a scene, as rendered (10 kHz samples)."""
    with wave.open(str(mv.OUT / "narration" / ("%s_%02d.wav" % (scene_id, k)))) as w:
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float)


def chip(title, sub, width, height, at):
    r = Rectangle(width=width, height=height, stroke_color=INK, stroke_width=LINE_W, fill_color=BOX,
                  fill_opacity=1).move_to(at)
    return VGroup(r, label(title, 26, INK, weight="BOLD").next_to(r.get_top(), DOWN, 0.15),
                  label(sub, 16).next_to(r.get_top(), DOWN, 0.55))


class ColdOpen(PaperScene):
    """The DTC-01 powers up and greets; Paul introduces himself."""
    scene_id = "cold_open"

    def construct(self):
        T = self.timeline
        unit = ImageMobject(str(ROOT / "dectalk_unit.png"))
        unit.width = 9.0
        unit.move_to([0, 0.4, 0])
        self.play(FadeIn(unit, shift=UP * 0.2), run_time=1.5)
        # the rocker switch sits at about (155, 745) of the 1959 x 898 photo
        sw = unit.get_corner(LEFT + UP) + RIGHT * unit.width * 155 / 1959 + DOWN * unit.height * 745 / 898
        ring = Circle(radius=0.35, color=RED, stroke_width=4).move_to(sw)
        self.play(Create(ring), run_time=0.8)
        self.play(ring.animate.scale(1.6).set_opacity(0), run_time=0.6)

        self.sync(T.line(0)[0])
        voice = Scope(narration("cold_open", 0), 0, len(narration("cold_open", 0)), 5.0, 0.9, RED, at=[0, -2.35, 0],
                      window=600, rate=10000 * 0.5)
        self.add(voice)
        said = label("the power-up greeting, as v1.8 says it", 16).next_to(voice, UP, 0.5)
        self.play(FadeIn(said), run_time=0.8)

        self.sync(T.line(1)[0])
        self.play(FadeOut(unit), FadeOut(voice), FadeOut(said), run_time=1.0)
        title = VGroup(label("Perfect Paul", 64, INK, weight="BOLD"),
                       label("DECtalk DTC-01 · firmware v1.8", 28, INK2)).arrange(DOWN, buff=0.3).move_to([0, 0.6, 0])
        self.play(Write(title[0]), FadeIn(title[1], shift=UP * 0.1), run_time=2.0)
        self.pulse_until(T.line(2)[0], [title[1]], color=RED)

        rom = box("ROM", "68000 code")
        c = box("C", "src/speech")
        pair = VGroup(rom, c).arrange(RIGHT, buff=4.2).move_to([0, -1.4, 0])
        link = Arrow(rom[0].get_right(), c[0].get_left(), buff=0.1, color=INK, stroke_width=LINE_W)
        note = label("checked word for word, sample for sample", 16).next_to(link, UP, 0.1)
        self.sync(T.line(2)[0])
        self.play(FadeIn(pair), Create(link), FadeIn(note), run_time=1.5)
        self.pulse_until(T.line(3)[0], [c[1], note], color=RED)

        self.sync(T.line(3)[0])
        self.play(FadeOut(VGroup(pair, link, note)), title.animate.shift(UP * 0.4), run_time=0.8)
        q = label("How does this box turn text into speech?", 30, RED).move_to([0, -1.0, 0])
        self.play(FadeIn(q, shift=UP * 0.2), run_time=1.0)
        self.pulse_until(T.duration - 0.2, [q])
        self.finish()


class BigPicture(PaperScene):
    """Two chips: the 68000 plans, the TMS32010 speaks; a frame every 6.4 ms between them, a sample every 100 µs out."""
    scene_id = "big_picture"

    def construct(self):
        T = self.timeline
        self.play(FadeIn(header("The big picture", "two processors, one frame every 6.4 ms between them")), run_time=1.0)
        cpu = chip("68000", "Motorola · the planner", 5.2, 3.0, [-3.3, 0.2, 0])
        dsp = chip("TMS32010", "Texas Instruments · the voice", 3.6, 3.0, [3.3, 0.2, 0])
        self.sync(T.line(0)[0])
        self.play(FadeIn(cpu, shift=RIGHT * 0.2), FadeIn(dsp, shift=LEFT * 0.2), run_time=1.5)

        stages = VGroup(*(box(n, s).scale(0.75) for n, s in (("TXT", "phonemes"), ("DUR", "timing, F0"),
                                                          ("FMT", "formants")))).arrange(RIGHT, buff=0.35)
        stages.move_to(cpu[0].get_center() + DOWN * 0.45)
        self.sync(T.line(1)[0])
        self.play(LaggedStart(*(FadeIn(s, shift=UP * 0.1) for s in stages), lag_ratio=0.4), run_time=2.0)
        self.pulse_until(T.line(2)[0], [s[1] for s in stages], color=RED)

        self.sync(T.line(2)[0])
        self.play(cpu[2].animate.set_color(RED), dsp[2].animate.set_color(RED), run_time=0.8)
        self.pulse_until(T.line(3)[0], [cpu[2], dsp[2]], color=BLUE)

        path = Line(cpu[0].get_right() + UP * 0.6, dsp[0].get_left() + UP * 0.6, color=INK, stroke_width=LINE_W)
        tag = label("a frame, every 6.4 ms", 16).next_to(path, UP, 0.1)
        self.sync(T.line(3)[0])
        self.play(Create(path), FadeIn(tag), run_time=1.0)

        def frame_dot():
            return Rectangle(width=0.32, height=0.22, stroke_color=RED, stroke_width=1.5, fill_color=RED,
                             fill_opacity=0.25).move_to(path.get_start())
        spk = VGroup(Rectangle(width=0.6, height=0.6, stroke_color=INK, stroke_width=LINE_W),
                     label("DAC", 14, INK)).move_to([6.4, -1.9, 0])
        out = Line(dsp[0].get_bottom(), spk[0].get_left() + LEFT * 0.0, color=INK, stroke_width=LINE_W)
        out_tag = label("64 samples per frame, one every 100 µs", 16).move_to([2.6, -2.3, 0])
        shown = {"tms": False, "out": False}
        while T.duration - self.now() > 1.2:    # frames keep coming; later lines add to the picture
            if not shown["tms"] and self.now() >= T.line(4)[0]:
                shown["tms"] = True
                self.play(dsp[1].animate.set_color(RED), Create(Rectangle(width=3.8, height=3.2, stroke_color=RED,
                                                                          stroke_width=2).move_to(dsp[0])),
                          run_time=0.8)
            if not shown["out"] and self.now() >= T.line(5)[0]:
                shown["out"] = True
                self.play(Create(out), FadeIn(spk), FadeIn(out_tag), run_time=0.8)
            if self.now() >= T.line(6)[0] and not shown.get("text"):
                shown["text"] = True
                self.play(FadeIn(label("“Hello, I'm Perfect Paul.”", 30, BLUE).move_to([-3.3, 2.35, 0]),
                                 shift=DOWN * 0.2), run_time=1.0)
            d = frame_dot()
            anims = [MoveAlongPath(d, path)]
            if shown["out"]:
                s = Dot(dsp[0].get_bottom(), radius=0.06, color=RED)
                anims.append(MoveAlongPath(s, out))
            self.play(*anims, run_time=1.0)
            self.remove(d, *([s] if shown["out"] else []))
        self.finish()


class Outro(PaperScene):
    """The whole chain at once, Klatt's design, the C rebuild, and the song."""
    scene_id = "outro"

    def construct(self):
        T = self.timeline
        chain = VGroup(*(box(n, s).scale(0.8) for n, s in (("TXT", "letters"), ("LTS", "rules"), ("TGT", "targets"),
                                                         ("TRK", "tracks"), ("FRM", "frames"), ("DSP", "samples"))))
        chain.arrange(RIGHT, buff=0.55).move_to([0, 1.6, 0])
        links = VGroup(*(Arrow(a[0].get_right(), b[0].get_left(), buff=0.05, color=INK, stroke_width=LINE_W)
                         for a, b in zip(chain, chain[1:])))
        self.sync(T.line(0)[0])
        for k in range(0, 6, 2):                # letters to rules, targets to tracks, frames to samples
            self.play(FadeIn(chain[k]), FadeIn(chain[k + 1]), Create(links[k]), *([Create(links[k - 1])] if k else []),
                      run_time=1.4)

        topo = ImageMobject(str(ROOT / "dsp" / "dsp_synth_topology.png"))
        topo.width = 8.6
        topo.move_to([0, -0.9, 0])
        cite = label("Dennis Klatt's cascade/parallel synthesizer, as the TMS32010 runs it (dsp/dsp_synth_topology)",
                     16).next_to(topo, DOWN, 0.08)
        self.sync(T.line(1)[0])
        self.play(FadeIn(topo), FadeIn(cite), run_time=1.5)
        self.pulse_until(T.line(2)[0], [chain[-1][1], cite], color=RED)

        self.sync(T.line(2)[0])
        self.play(FadeOut(topo), FadeOut(cite), run_time=0.8)
        rebuilt = label("v1.8's 68000 and DSP code, rebuilt in C, checked against the ROM", 24, INK).move_to([0, 0.0, 0])
        self.play(FadeIn(rebuilt, shift=UP * 0.1), run_time=1.0)
        self.pulse_until(T.line(3)[0], [rebuilt], color=RED)

        self.sync(T.line(3)[0])
        self.play(FadeOut(rebuilt), FadeOut(chain), FadeOut(links), run_time=0.8)
        thanks = label("Thank you for listening.", 40, INK, weight="BOLD").move_to([0, 1.4, 0])
        self.play(Write(thanks), run_time=1.5)

        # Daisy Bell: G4 E4 C4 G3, A3 B3 C4 A3 C4 G3 (script.DAISY's notes), one dot per note at its pitch
        notes = [(32, 0.9, "Dai-"), (29, 0.9, "sy"), (25, 0.9, "Dai-"), (20, 0.9, "sy"), (22, 0.3, "give"),
                 (24, 0.3, "me"), (25, 0.3, "your"), (22, 0.6, "an-"), (25, 0.3, "swer"), (20, 1.2, "do")]
        x = -5.4
        staff = VGroup(*(Line([-6, -1.9 + 0.12 * i, 0], [6, -1.9 + 0.12 * i, 0], color=INK2, stroke_width=0.8)
                         for i in range(0, 13, 3)))
        self.sync(T.line(4)[0] - 0.4)
        self.play(FadeIn(staff), run_time=0.4)
        a, b = T.line(4)
        stretch = (b - a) / sum(d for _, d, _ in notes)     # the sung line is longer than its notes' <dur> sum
        for n, dur, syl in notes:
            y = -1.9 + 0.12 * (n - 20)
            dot = Dot([x, y, 0], radius=0.11, color=RED)
            self.play(FadeIn(dot, scale=1.6), FadeIn(label(syl, 16, INK).move_to([x, -2.35, 0])), run_time=0.25)
            self.wait(max(0.01, dur * stretch - 0.25))
            x += 1.2 * dur / 0.9 + 0.1
        credits = label("DECtalk DTC-01 v1.8 · after Klatt (1980, 1982), Hunnicutt (1976), MITalk (1987) · made with Manim",
                        16).move_to([0, 0.6, 0])
        self.play(FadeIn(credits), run_time=0.8)
        self.pulse_until(T.duration - 0.2, [thanks, credits])
        self.finish()
