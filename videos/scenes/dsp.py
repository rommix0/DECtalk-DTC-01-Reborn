"""Inside the DSP: the explainer video's scenes on the TMS32010 program (src/speech/dsp_synth.c, REFERENCE.md s16,
dsp/dsp_synth_topology). Every trace is dsplog's data for the example sentence (videos/data.py)."""
import sys
from pathlib import Path

import numpy as np
from manim import (DOWN, LEFT, RIGHT, UP, Arrow, Circle, Create, DashedLine, Dot, FadeIn, FadeOut, LaggedStart,
                   Line, Rectangle, Text, Transform, VGroup, VMobject, Write)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from scenes.common import (BLUE, FONT, INK, INK2, LINE_W, PAPER, RED, PaperScene, Scope, box,  # noqa: E402
                           glyphs, header)
import data                                     # noqa: E402
import make_video as mv                         # noqa: E402


def example():
    """dsplog of the example sentence: (S columns, phones)."""
    s, phones, _ = data.load_dsplog(mv.OUT / "data" / "dsp_example.tsv")
    return s, phones


def phone(phones, name):
    """The first phone of this name: (start sample, end sample)."""
    r = next(r for r in phones if r["name"] == name)
    return r["n"], r["n"] + r["samples"]


def label(text, size=20, color=INK2, **kw):
    return Text(text, font=FONT, color=color, font_size=size, **kw)


def frame_box(width, height, at, title=None):
    """A ruled panel, its title above its top-left corner."""
    r = Rectangle(width=width, height=height, stroke_color=INK, stroke_width=1).move_to(at)
    if not title:
        return VGroup(r)
    return VGroup(r, label(title, 18).next_to(r, UP, buff=0.08).align_to(r, LEFT))


def polyline(xs, ys, color=RED, width=LINE_W):
    pts = np.stack([np.asarray(xs, float), np.asarray(ys, float), np.zeros(len(xs))], axis=1)
    return VMobject(color=color, stroke_width=width).set_points_as_corners(pts)


def plot(x, panel, color=RED, peak=None, width=LINE_W):
    """x drawn across a frame_box, its peak at 45% of the height."""
    r = panel[0]
    x = np.asarray(x, dtype=float)
    peak = peak or np.abs(x).max() or 1.0
    xs = np.linspace(r.get_left()[0] + 0.1, r.get_right()[0] - 0.1, len(x))
    return polyline(xs, r.get_center()[1] + x / peak * r.height * 0.45, color, width)


def stems(x, width, height, at, color=RED):
    """One stem and dot per sample, as a DAC sees them."""
    x = np.asarray(x, dtype=float) / (np.abs(x).max() or 1.0)
    xs = np.linspace(-width / 2, width / 2, len(x)) + at[0]
    return VGroup(*(VGroup(Line([u, at[1], 0], [u, at[1] + v * height / 2, 0], color=color, stroke_width=1.2),
                           Dot([u, at[1] + v * height / 2, 0], radius=0.022, color=color)) for u, v in zip(xs, x)))


def card(title, rows, width=4.2):
    """A small table: the title in bold, then name / value rows."""
    lines = VGroup(label(title, 22, INK, weight="BOLD"))
    for n, v in rows:                   # name at the left, value flush right
        a, b = label(n, 18), label(str(v), 18, INK)
        lines.add(VGroup(a, b.next_to(a, RIGHT, buff=width - 0.4 - a.width - b.width)))
    lines.arrange(DOWN, buff=0.12, aligned_edge=LEFT)
    r = Rectangle(width=width, height=lines.height + 0.4, stroke_color=INK, stroke_width=LINE_W, fill_color="#fffdf7",
                  fill_opacity=1).move_to(lines)
    return VGroup(r, lines)


def voiced_longest(s, phones):
    """(start, end) of the longest phone that is voiced at its middle."""
    v = [r for r in phones if s["av_lin"][r["n"] + r["samples"] // 2] > 0]
    r = max(v, key=lambda r: r["samples"])
    return r["n"], r["n"] + r["samples"]


def periods(s, lo, hi):
    """Glottal period starts in [lo, hi): where nper went back to 0."""
    n = s["nper"][lo:hi]
    return [lo + i for i in range(1, len(n)) if n[i] < n[i - 1]]


class SpectrumPanel(VGroup):
    """A dB-over-Hz plot, 0 to 5 kHz and 0 to -60 dB, in a ruled frame; curve(x) gives the spectrum of x."""

    def __init__(self, width, height, at):
        super().__init__()
        self.w, self.h, self.at = width, height, np.array(at, dtype=float)
        frame = Rectangle(width=width, height=height, stroke_color=INK, stroke_width=1).move_to(at)
        ticks = VGroup()
        for khz in range(6):
            x = self.x(khz * 1000)
            ticks.add(Line([x, self.y(-60), 0], [x, self.y(-60) - 0.08, 0], color=INK, stroke_width=1),
                      label("%d" % khz, 16).move_to([x, self.y(-60) - 0.25, 0]))
        ticks.add(label("kHz", 16).move_to([self.x(5000) + 0.3, self.y(-60) - 0.25, 0]))
        ticks.add(label("dB", 16).move_to([self.x(0) - 0.3, self.y(0), 0]))
        self.add(frame, ticks)

    def x(self, hz):
        return self.at[0] - self.w / 2 + self.w * hz / 5000

    def y(self, db):
        return self.at[1] + self.h / 2 + self.h * max(db, -60) / 60

    def curve(self, x, color=RED):
        hz, db = data.spectrum_db(x)
        pts = np.array([[self.x(f), self.y(d), 0] for f, d in zip(hz, db) if f <= 5000])
        return VMobject(color=color, stroke_width=LINE_W).set_points_as_corners(pts)

    def marker(self, hz, text, color=BLUE, raise_by=0.0):
        line = DashedLine([self.x(hz), self.y(-60), 0], [self.x(hz), self.y(0) + raise_by, 0], color=color,
                          stroke_width=1.5)
        return VGroup(line, label(text, 16, color).next_to(line, UP, buff=0.05))


class DspCascade(PaperScene):
    """The cascade branch (CASCADE_BRANCH 0x5E8): nasal zero, nasal pole, F5 ... F1 in a row, each section's output
    on the scope and its spectrum, over the vowel of "Paul"."""
    scene_id = "dsp_cascade"

    def construct(self):
        s, phones = example()
        lo, hi = phone(phones, "ao")
        mid = (lo + hi) // 2
        win = slice(mid - 256, mid + 256)
        T = self.timeline

        self.play(FadeIn(header("The cascade branch",
                                "CASCADE_BRANCH 0x5E8 · the vocal tract as seven sections in a row")), run_time=1.0)

        # the chain: source -> NZ NP F5 F4 F3 F2 F1 -> x2
        stages = [("NZ", "nasal zero", "np"), ("NP", "nasal pole", "np"), ("F5", "head size", "cf5"),
                  ("F4", "head size", "cf4"), ("F3", "%d Hz" % s["f3"][mid], "cf3"),
                  ("F2", "%d Hz" % s["f2"][mid], "cf2"), ("F1", "%d Hz" % s["f1"][mid], "cf1")]
        boxes = VGroup(*(box(n, sub) for n, sub, _ in stages)).arrange(RIGHT, buff=0.22).move_to([0.2, 1.55, 0])
        wires = VGroup(*(Line(a[0].get_right(), b[0].get_left(), color=INK, stroke_width=LINE_W)
                         for a, b in zip(boxes, boxes[1:])))
        src_scope = Scope(s["src"], lo, hi, 1.6, 0.8, INK2, at=boxes.get_left() + LEFT * 1.25)
        src_lbl = label("source", 16).next_to(src_scope, UP, buff=0.25)
        into = Arrow(src_scope.get_right() + RIGHT * 0.05, boxes[0][0].get_left(), buff=0.02, color=INK,
                     stroke_width=LINE_W, max_tip_length_to_length_ratio=0.25)
        double = VGroup(Circle(radius=0.32, color=INK, stroke_width=LINE_W, fill_color=PAPER, fill_opacity=1),
                        label("×2", 22, INK)).next_to(boxes, RIGHT, buff=0.45)
        out = Arrow(boxes[-1][0].get_right(), double[0].get_left(), buff=0.02, color=INK, stroke_width=LINE_W,
                    max_tip_length_to_length_ratio=0.25)
        to_sum = Arrow(double[0].get_right(), double[0].get_right() + RIGHT * 0.7, buff=0.02, color=INK,
                       stroke_width=LINE_W, max_tip_length_to_length_ratio=0.3)
        sum_lbl = label("to the sum", 16).next_to(to_sum, DOWN, buff=0.1)

        self.sync(T.line(0)[0])
        self.add(src_scope)
        self.play(FadeIn(src_lbl), Create(into), run_time=0.8)
        self.play(LaggedStart(*(FadeIn(b, shift=DOWN * 0.15) for b in boxes), lag_ratio=0.25),
                  Create(wires), run_time=3.0)
        self.play(Create(out), FadeIn(double), run_time=1.0)

        # one line of arithmetic, in Q12
        self.sync(T.line(1)[0])
        self.play(boxes[-1][0].animate.set_stroke(RED, width=4), run_time=0.6)
        eq = Text("y[n] = A·x[n] + B·y[n−1] + C·y[n−2]", font=FONT, color=INK, font_size=38).move_to([0, -0.2, 0])
        self.sync(T.line(2)[0])
        self.play(Write(eq), run_time=2.5)
        notes = VGroup(label("the input", 18, RED), label("the last output", 18, RED),
                       label("the one before", 18, RED))
        terms = [glyphs(eq, part) for part in ("A·x[n]", "B·y[n−1]", "C·y[n−2]")]
        for n, term in zip(notes, terms):
            n.next_to(term, DOWN, buff=0.25)
        self.play(LaggedStart(*(FadeIn(n, shift=UP * 0.1) for n in notes), lag_ratio=0.5), run_time=2.0)
        self.sync(T.line(3)[0])
        coef = label("A, B, C  ←  the formant's frequency and bandwidth (a cosine table in the DSP ROM)", 22, INK)
        coef.next_to(eq, DOWN, buff=0.95)
        self.play(FadeIn(coef, shift=UP * 0.1), run_time=1.0)
        self.sync(T.line(4)[0])
        q12 = label("Q12: 16-bit words, 12 bits after the binary point (4096 = 1.0)", 22, BLUE).next_to(coef, DOWN,
                                                                                                    buff=0.3)
        self.play(FadeIn(q12, shift=UP * 0.1), run_time=1.0)
        self.pulse_until(T.line(5)[0], terms)

        # section by section: the scope and the spectrum
        self.sync(T.line(5)[0])
        scope_frame = Rectangle(width=5.6, height=2.2, stroke_color=INK, stroke_width=1).move_to([-3.3, -1.2, 0])
        scope = Scope(s["np"], lo, hi, 5.3, 1.75, RED, at=scope_frame.get_center())
        probe = label("probe: after the nasal pole", 18).next_to(scope_frame, UP, buff=0.08).align_to(scope_frame,
                                                                                                     LEFT)
        spec = SpectrumPanel(5.4, 2.2, [3.4, -1.15, 0])
        curve = spec.curve(s["src"][win], INK2)
        self.play(FadeOut(VGroup(eq, notes, coef, q12)), boxes[-1][0].animate.set_stroke(INK, width=LINE_W),
                  run_time=0.8)
        self.play(FadeIn(scope_frame), FadeIn(probe), FadeIn(spec), Create(curve), run_time=1.0)
        self.add(scope)

        def visit(k, *, run_time=1.2):
            name, _, col = stages[k]
            new_probe = label("probe: after %s" % ("the nasal pole" if k < 2 else name), 18)
            new_probe.move_to(probe, aligned_edge=LEFT)
            scope.set_signal(s[col])
            return [boxes[k][0].animate.set_stroke(RED, width=4), Transform(curve, spec.curve(s[col][win])),
                    Transform(probe, new_probe)]

        self.play(*visit(0), boxes[1][0].animate.set_stroke(RED, width=4), run_time=1.5)
        self.sync(T.line(6)[0])
        self.play(*visit(2), run_time=1.5)
        self.play(*visit(3), run_time=1.5)
        self.sync(T.line(7)[0])
        for k in (4, 5, 6):
            self.play(*visit(k), run_time=1.6)
        marks = VGroup(*(spec.marker(s[f][mid], "%s %d" % (f.upper(), s[f][mid]), raise_by=0.3 * (f == "f2"))
                         for f in ("f1", "f2", "f3")))
        self.play(FadeIn(marks), run_time=1.0)

        # the buzz in, the vowel out
        self.sync(T.line(8)[0])
        self.play(src_scope.animate.set_stroke(INK, width=3), scope.animate.set_stroke(RED, width=3), run_time=1.0)
        self.play(src_scope.animate.set_stroke(INK2, width=LINE_W), scope.animate.set_stroke(RED, width=LINE_W),
                  run_time=1.0)

        # doubled, on to the sum
        self.sync(T.line(9)[0])
        self.play(double[0].animate.set_stroke(RED, width=4), Create(to_sum), FadeIn(sum_lbl), run_time=1.2)
        self.finish()


class DspRhythm(PaperScene):
    """The DSP's clock: one sample per DAC interrupt, a frame every 64 samples, and the wait that paces the 68000
    (FRAME_TIMEOUT_CHECK 0x2DF, REFERENCE s16.10)."""
    scene_id = "dsp_rhythm"

    def construct(self):
        s, phones = example()
        lo, hi = phone(phones, "ow")
        f0 = (lo // 64 + 2) * 64                # three whole frames inside the vowel of "hello"
        T = self.timeline
        self.play(FadeIn(header("The DSP's rhythm", "TMS32010 · one sample per DAC interrupt, 10 kHz")),
                  run_time=1.0)
        chip = box("DSP", "TMS32010").move_to([-5.2, 1.6, 0])
        dac = box("DAC", "12 bits").move_to([5.2, 1.6, 0])
        wire = Arrow(chip[0].get_right(), dac[0].get_left(), buff=0.05, color=INK, stroke_width=LINE_W)
        out = Scope(s["pcm"], lo, hi, 2.4, 0.6, RED, at=[1.6, 2.25, 0], window=300, rate=600)
        self.sync(T.line(0)[0])
        self.play(FadeIn(chip), FadeIn(dac), Create(wire), run_time=1.2)
        self.add(out)

        st = stems(s["pcm"][f0:f0 + 192], 11.5, 2.2, [0, -1.0, 0])
        self.sync(T.line(1)[0])
        self.play(LaggedStart(*(FadeIn(x) for x in st), lag_ratio=0.02), run_time=4.0)
        a, b = st[20][0].get_start(), st[21][0].get_start()
        tick = VGroup(Line(a + DOWN * 1.35, b + DOWN * 1.35, color=BLUE, stroke_width=2),
                      label("100 µs", 18, BLUE).move_to((a + b) / 2 + DOWN * 1.65))
        self.play(FadeIn(tick), run_time=0.8)

        self.sync(T.line(2)[0])
        bounds = VGroup()
        for k in range(4):
            x = st[0][0].get_start()[0] + k * 64 * (11.5 / 191) - 0.03
            bounds.add(VGroup(DashedLine([x, -2.3, 0], [x, 0.4, 0], color=RED, stroke_width=1.5),
                              label("next frame" if k else "frame", 16, RED).move_to([x, 0.6, 0])))
        self.play(LaggedStart(*(Create(b) for b in bounds), lag_ratio=0.3), run_time=2.0)
        span = label("64 samples = 6.4 ms", 20, INK).move_to([0, -2.6, 0])
        self.play(FadeIn(span), run_time=0.8)

        self.sync(T.line(3)[0])
        self.play(FadeOut(VGroup(st, tick, bounds, span)), run_time=0.8)
        steps = VGroup(*(label(t, 22, c) for t, c in (("no frame", INK2), ("no frame", INK2), ("no frame", INK2),
                                                      ("stop: raise the semaphore", RED))))
        steps.arrange(RIGHT, buff=0.6).move_to([0, -1.0, 0])
        self.play(LaggedStart(*(FadeIn(t, shift=RIGHT * 0.2) for t in steps), lag_ratio=0.6), run_time=2.4)
        back = Arrow(steps[-1].get_top() + UP * 0.1, chip[0].get_bottom(), buff=0.1, color=RED,
                     stroke_width=LINE_W, path_arc=-0.6)
        self.play(Create(back), FadeIn(label("the 68000 answers with a frame", 18, RED).next_to(steps, DOWN, 0.4)),
                  run_time=1.2)
        self.finish()


class DspGlottal(PaperScene):
    """The voicing source (GLOTTAL_4X_LOOP 0x4C2, PITCH_SYNC_RESET 0x4DF): parwav's natural glottal source at four
    times the sample rate, the low-pass, tilt, the alternating skew, and AV through amptable."""
    scene_id = "dsp_glottal"

    def construct(self):
        s, phones = example()
        lo, hi = phone(phones, "ow")
        T = self.timeline
        self.play(FadeIn(header("The voicing source", "GLOTTAL_4X_LOOP 0x4C2 · parwav's natural glottal source")),
                  run_time=1.0)
        top = Scope(s["lp"], lo, hi, 3.0, 0.8, RED, at=[4.6, 2.6, 0], window=300, rate=500)
        self.sync(T.line(0)[0])
        self.add(top)
        self.play(FadeIn(label("the glottal source, live", 16).next_to(top, DOWN, buff=0.35)), run_time=0.8)

        b0 = data.dsp_table("dsp_b0")
        left = frame_box(5.6, 3.2, [-3.3, -0.6, 0], "B0, DSP ROM 0x1C7: 224 words from Klatt's parwav")
        bars = VGroup(*(Line([left[0].get_left()[0] + 0.1 + i * 5.4 / 224, -2.1, 0],
                             [left[0].get_left()[0] + 0.1 + i * 5.4 / 224, -2.1 + 2.9 * v / b0.max(), 0],
                             color=INK2, stroke_width=1.5) for i, v in enumerate(b0)))
        note = label("one coefficient for each open-phase length, 40 to 263 steps", 16).next_to(left, DOWN, 0.15)
        self.sync(T.line(1)[0])
        self.play(FadeIn(left), LaggedStart(*(Create(b) for b in bars), lag_ratio=0.01), run_time=2.5)
        self.play(FadeIn(note), run_time=0.6)

        p = periods(s, lo, hi)
        a, b = p[2], p[5]
        right = frame_box(5.6, 3.2, [3.3, -0.6, 0], "three glottal periods of \"hello\" (lp)")
        wave_ = plot(s["lp"][a:b], right)
        shades = VGroup()
        r = right[0]
        xs = np.linspace(r.get_left()[0] + 0.1, r.get_right()[0] - 0.1, b - a)
        open_ = s["nper"][a:b] <= s["nopen"][a:b]
        i = 0
        while i < len(open_):                       # one light band per open phase
            if open_[i]:
                j = i
                while j < len(open_) and open_[j]:
                    j += 1
                shades.add(Rectangle(width=xs[j - 1] - xs[i], height=r.height - 0.1, stroke_width=0,
                                     fill_color=RED, fill_opacity=0.12).move_to([(xs[i] + xs[j - 1]) / 2, r.get_y(), 0]))
                i = j
            i += 1
        tags = VGroup(label("open", 16, RED).next_to(shades[0], UP, 0.05).shift(DOWN * 0.45),
                      label("closed", 16, INK2).move_to([(shades[0].get_right()[0] + shades[1].get_left()[0]) / 2,
                                                         shades[0].get_top()[1] - 0.2, 0]))
        self.sync(T.line(2)[0])
        self.play(FadeIn(right), Create(wave_), run_time=1.8)
        self.play(FadeIn(shades), FadeIn(tags), run_time=1.0)

        t0 = int(s["t0"][p[3]])
        eq = label("T0 = 40000 / F0 = %d steps of 25 µs = %.1f ms  →  F0 ≈ %d Hz" % (t0, t0 / 40, round(40000 / t0)),
                   22, INK).move_to([-2.6, 1.75, 0])
        self.sync(T.line(3)[0])
        self.play(FadeOut(note), FadeIn(eq, shift=UP * 0.1), run_time=1.0)
        self.pulse_until(T.line(4)[0], [eq])

        chain = VGroup(box("4×", "40 kHz"), box("LP", "2-pole"), box("1×", "10 kHz")).arrange(RIGHT, buff=0.5)
        chain.scale(0.8).move_to(left[0])
        arrows = VGroup(*(Arrow(x[0].get_right(), y[0].get_left(), buff=0.05, color=INK, stroke_width=LINE_W)
                          for x, y in zip(chain, chain[1:])))
        self.play(FadeOut(bars), FadeOut(left[1]), run_time=0.6)
        self.play(FadeIn(chain), Create(arrows), run_time=1.2)

        tilt = plot(s["tilt"][a:b], right, BLUE)
        self.sync(T.line(5)[0])
        self.play(Create(tilt), FadeIn(label("after the tilt filter", 16, BLUE).next_to(right, DOWN, 0.15)),
                  run_time=1.5)
        breath = label("breath noise: br × T0 × noise, open phase only (Paul: br 0)", 16).next_to(left, DOWN, 0.15)
        self.play(FadeIn(breath), run_time=0.8)

        self.sync(T.line(6)[0])
        frank, fph, _ = data.load_dsplog(mv.OUT / "data" / "voice_nf.tsv")
        flo, fhi = voiced_longest(frank, fph)
        pp, fp = periods(s, lo, hi)[:12], periods(frank, flo, fhi)[:12]
        self.play(FadeOut(VGroup(chain, arrows)), run_time=0.6)
        dots = VGroup()
        for k, (src, ps, col, y) in enumerate(((s, pp, INK, 0.3), (frank, fp, RED, -1.0))):
            t = [int(src["t0"][i]) for i in ps]
            m = float(np.mean(t))
            for j, v in enumerate(t):
                dots.add(Dot([left[0].get_left()[0] + 0.5 + j * 0.42, y + (v - m) / m * 8, 0], radius=0.05, color=col))
        legend = VGroup(label("Paul, la 0: periods even", 16, INK).move_to([-3.3, 0.9, 0]),
                        label("Frank, la 100: long, short, long ...", 16, RED).move_to([-3.3, -0.4, 0]))
        self.play(LaggedStart(*(FadeIn(d) for d in dots), lag_ratio=0.05), FadeIn(legend), run_time=2.0)

        amp = data.dsp_table("dsp_amptable")
        av_lin = int(s["av_lin"][p[3]])
        db = int(np.argmin(np.abs(amp - av_lin))) - 4
        self.sync(T.line(7)[0])
        self.play(FadeOut(VGroup(dots, legend, breath)), run_time=0.5)
        curve = polyline(left[0].get_left()[0] + 0.1 + np.arange(88) * 5.4 / 87, -2.1 + 2.9 * amp / amp.max(), INK2)
        at = Dot([left[0].get_left()[0] + 0.1 + (db + 4) * 5.4 / 87, -2.1 + 2.9 * av_lin / amp.max(), 0], radius=0.08,
                 color=RED)
        self.play(Create(curve), run_time=1.2)
        gain = label("amptable: AV %d dB → %d (×%.2f)" % (db, av_lin, av_lin / 4096), 18, RED).next_to(left, DOWN, 0.15)
        self.play(FadeIn(at), FadeIn(gain), run_time=1.0)
        self.pulse_until(T.duration - 0.2, [gain, eq])
        self.finish()


class DspNoise(PaperScene):
    """The noise source (NOISE_AND_MIX 0x5CD): the 16-bit LFSR, its low-pass, halving after nmod, and AH into the
    cascade."""
    scene_id = "dsp_noise"

    def construct(self):
        s, phones = example()
        lo, hi = phone(phones, "ow")
        T = self.timeline
        self.play(FadeIn(header("The noise source", "16-bit LFSR · low-pass · halved after nmod")), run_time=1.0)
        trace = Scope(s["noise"], lo, hi, 4.6, 0.9, INK2, at=[3.6, 2.5, 0], window=300, rate=500)
        self.sync(T.line(0)[0])
        self.add(trace)

        def bits(v):
            cells = VGroup(*(VGroup(Rectangle(width=0.5, height=0.55, stroke_color=INK, stroke_width=1.5,
                                              fill_color=RED if (0x0641 >> (15 - i)) & 1 else "#fffdf7",
                                              fill_opacity=0.18 if (0x0641 >> (15 - i)) & 1 else 1),
                                    label(str((v >> (15 - i)) & 1), 22, INK)) for i in range(16)))
            return cells.arrange(RIGHT, buff=0.04).move_to([0, 0.6, 0])
        v = 0x2D61                              # the seed at every speaker frame
        reg = bits(v)
        val = label("0x%04X" % v, 22, INK).next_to(reg, DOWN, 0.3)
        self.sync(T.line(1)[0])
        mask = label("feedback mask 0x0641 (tinted)", 16, RED).next_to(reg, UP, 0.2)
        self.play(FadeIn(reg), FadeIn(val), FadeIn(mask), run_time=1.0)
        while T.line(2)[0] - self.now() > 0.6:  # the generator as speech_sample runs it
            v = (v << 1) & 0xFFFF if v < 0x8000 else ((v << 1) ^ 0x0641) & 0xFFFF
            self.play(Transform(reg, bits(v)), Transform(val, label("0x%04X" % v, 22, INK).next_to(reg, DOWN, 0.3)),
                      run_time=0.5)

        p = periods(s, lo, hi)
        a, b = p[2], p[4]
        panel = frame_box(8.0, 1.8, [0, -1.6, 0], "noise over two periods of \"hello\": halved after nmod = T0 / 2")
        r = panel[0]
        halves = VGroup()
        for q, e in ((p[2], p[3]), (p[3], p[4])):
            x0 = r.get_left()[0] + 0.1 + (q + (e - q) / 2 - a) * 7.8 / (b - a)
            x1 = r.get_left()[0] + 0.1 + (e - a) * 7.8 / (b - a)
            halves.add(Rectangle(width=x1 - x0, height=r.height - 0.1, stroke_width=0, fill_color=BLUE,
                                 fill_opacity=0.12).move_to([(x0 + x1) / 2, r.get_y(), 0]))
        self.sync(T.line(2)[0])
        self.play(FadeIn(panel), Create(plot(s["noise"][a:b], panel, INK)), run_time=1.6)
        self.play(FadeIn(halves), run_time=0.8)

        mix = VGroup(box("× AH", "noise"), box("× AV", "voicing")).arrange(DOWN, buff=0.3)
        mix.scale(0.75).move_to([-5.6, 0.6, 0])
        sigma = VGroup(Circle(radius=0.3, color=INK, stroke_width=LINE_W), label("Σ", 24, INK)).move_to([-3.9, 0.6, 0])
        to = Arrow(sigma.get_right(), sigma.get_right() + RIGHT * 0.6, buff=0.02, color=INK, stroke_width=LINE_W)
        tag = label("cascade", 16).next_to(to, UP, 0.05)
        self.sync(T.line(3)[0])
        self.play(reg.animate.scale(0.6).move_to([2.2, 0.6, 0]), val.animate.move_to([2.2, -0.05, 0]),
                  mask.animate.move_to([2.2, 1.05, 0]), run_time=0.8)
        self.play(FadeIn(mix), FadeIn(sigma),
                  *(Create(Arrow(m[0].get_right(), sigma.get_left(), buff=0.05, color=INK, stroke_width=LINE_W))
                    for m in mix), Create(to), FadeIn(tag), run_time=1.4)

        self.sync(T.line(4)[0])
        self.play(Transform(reg, bits(0x2D61).scale(0.6).move_to([2.2, 0.6, 0])),
                  Transform(val, label("0x2D61: the seed, at every speaker frame", 18, RED).move_to([2.2, -0.05, 0])),
                  run_time=1.0)
        self.pulse_until(T.duration - 0.2, [val])
        self.finish()


class DspParallel(PaperScene):
    """The parallel branch (PARALLEL_BRANCH 0x632): F6 ... F2 and the bypass on the noise, alternating signs."""
    scene_id = "dsp_parallel"

    def construct(self):
        s, phones = example()
        f_lo, f_hi = phone(phones, "f")
        T = self.timeline
        self.play(FadeIn(header("The parallel branch", "PARALLEL_BRANCH 0x632 · F6 … F2 and the bypass, on the noise")),
                  run_time=1.0)
        par = (s["pf2"] - s["pf3"] + s["pf4"] - s["pf5"] + s["pf6"]
               - (s["ab"] * s["noise"] >> 15))
        noise = Scope(s["noise"], f_lo, f_hi, 2.0, 0.8, INK2, at=[-5.4, 0.2, 0], window=200, rate=400)
        self.sync(T.line(0)[0])
        self.add(noise)
        self.play(FadeIn(label("noise", 18).next_to(noise, UP, 0.5)), run_time=0.8)

        names = [("F6", "fixed at reset", "+"), ("F5", "speaker p5", "−"), ("F4", "speaker p4", "+"),
                 ("F3", "frame", "−"), ("F2", "frame", "+"), ("AB", "bypass", "−")]
        rows = VGroup(*(VGroup(Rectangle(width=1.5, height=0.5, stroke_color=INK, stroke_width=LINE_W,
                                         fill_color="#fffdf7", fill_opacity=1),
                               label(n, 20, INK, weight="BOLD")) for n, _, _ in names))
        rows.arrange(DOWN, buff=0.12).move_to([-1.6, 0.0, 0])
        sigma = VGroup(Circle(radius=0.35, color=INK, stroke_width=LINE_W), label("Σ", 26, INK)).move_to([1.4, 0, 0])
        ins = VGroup(*(Line(noise.get_right() + RIGHT * 0.1, r[0].get_left(), color=INK2, stroke_width=1) for r in rows))
        outs = VGroup(*(Line(r[0].get_right(), sigma.get_left(), color=INK2, stroke_width=1) for r in rows))
        self.sync(T.line(1)[0])
        self.play(LaggedStart(*(FadeIn(r) for r in rows), lag_ratio=0.15), Create(ins), run_time=2.0)
        self.play(Create(outs), FadeIn(sigma), run_time=1.0)

        signs = VGroup(*(label(sg, 26, RED if sg == "−" else BLUE).next_to(r, RIGHT, 0.12)
                         for r, (_, _, sg) in zip(rows, names)))
        self.sync(T.line(2)[0])
        self.play(LaggedStart(*(FadeIn(x) for x in signs), lag_ratio=0.15), run_time=1.5)
        self.pulse_until(T.line(3)[0], list(signs))

        subs = VGroup(*(label(sub, 16).next_to(r, LEFT, 0.15) for r, (_, sub, _) in zip(rows, names)))
        self.play(LaggedStart(*(FadeIn(x) for x in subs), lag_ratio=0.15), run_time=1.5)

        a, b = f_lo, phone(phones, "t")[1]
        strip = frame_box(4.9, 1.7, [4.55, -1.75, 0], "the parallel branch in \"perfect\": f, eh, k, t")
        self.sync(T.line(4)[0])
        self.play(FadeIn(strip), Create(plot(par[a:b], strip, RED)), run_time=2.0)
        marks = VGroup()
        for r in phones:
            if a <= r["n"] < b and r["name"] in ("f", "eh", "k", "t"):
                x = strip[0].get_left()[0] + 0.1 + (r["n"] - a) * 4.7 / (b - a)
                marks.add(label(r["name"], 16, INK).move_to([x + 0.15, strip[0].get_bottom()[1] + 0.2, 0]))
        self.play(FadeIn(marks), run_time=0.8)
        self.finish()


class DspDac(PaperScene):
    """To the converter: the sum, the 12-bit DAC word (DAC_ISR 0x694), the 16-deep FIFO, and the two frame types."""
    scene_id = "dsp_dac"

    def construct(self):
        s, phones = example()
        lo, hi = phone(phones, "ow")
        T = self.timeline
        self.play(FadeIn(header("To the converter", "DAC_ISR 0x694 · 12 bits, 10 kHz, a 16-deep FIFO")), run_time=1.0)
        out = Scope(s["pcm"], lo, hi, 3.0, 0.8, RED, at=[4.6, 2.6, 0], window=300, rate=600)
        parts = VGroup(box("C", "cascade"), box("P", "parallel")).arrange(DOWN, buff=0.3).scale(0.8).move_to([-5.4, 0.6, 0])
        sigma = VGroup(Circle(radius=0.32, color=INK, stroke_width=LINE_W), label("Σ", 24, INK)).move_to([-3.8, 0.6, 0])
        self.sync(T.line(0)[0])
        self.add(out)
        self.play(FadeIn(parts), FadeIn(sigma),
                  *(Create(Arrow(p[0].get_right(), sigma.get_left(), buff=0.05, color=INK, stroke_width=LINE_W))
                    for p in parts), run_time=1.4)

        def word(v):
            v &= 0xFFFF
            cells = VGroup(*(VGroup(Rectangle(width=0.42, height=0.5, stroke_color=INK, stroke_width=1.5,
                                              fill_color=RED if i < 12 else "#fffdf7",
                                              fill_opacity=0.15 if i < 12 else 1),
                                    label(str((v >> (15 - i)) & 1), 20, INK if i < 12 else INK2)) for i in range(16)))
            return cells.arrange(RIGHT, buff=0.03).move_to([0.9, 0.6, 0])
        k = lo + 300
        reg = word(int(s["pcm"][k]))
        cap = label("the top 12 bits go to the DAC; the low 4 are dropped", 16).next_to(reg, DOWN, 0.25)
        self.sync(T.line(1)[0])
        self.play(Create(Arrow(sigma.get_right(), reg.get_left(), buff=0.1, color=INK, stroke_width=LINE_W)),
                  FadeIn(reg), FadeIn(cap), run_time=1.2)
        while T.line(2)[0] - self.now() > 0.5:
            k += 1
            self.play(Transform(reg, word(int(s["pcm"][k]))), run_time=0.4)

        slots = VGroup(*(Rectangle(width=0.36, height=0.36, stroke_color=INK, stroke_width=1.2) for _ in range(16)))
        slots.arrange(RIGHT, buff=0.06).move_to([0.9, -1.2, 0])
        fifo_lbl = label("FIFO, 16 samples", 16).next_to(slots, DOWN, 0.15)
        self.play(FadeIn(slots), FadeIn(fifo_lbl), run_time=0.8)
        fill = VGroup()
        for i in range(16):
            fill.add(Dot(slots[i].get_center(), radius=0.1, color=RED))
        self.play(LaggedStart(*(FadeIn(d) for d in fill), lag_ratio=0.2), run_time=2.0)
        while T.line(3)[0] - self.now() > 0.8:  # drain one, take one: the queue stays full
            self.play(fill.animate.shift(RIGHT * 0.42).set_opacity(0.4), run_time=0.35)
            self.play(fill.animate.shift(LEFT * 0.42).set_opacity(1), run_time=0.35)

        cards = VGroup(card("speech frame 0x4000", [("words", 19), ("sent", "every 6.4 ms"), ("sets", "F0, formants, levels")]),
                       card("speaker frame 0x6000", [("words", 24), ("sent", "on a voice change"),
                                                     ("sets", "head size, F4, F5, gains")]))
        cards.arrange(RIGHT, buff=0.5).move_to([0, 0.0, 0])
        self.sync(T.line(3)[0])
        self.play(FadeOut(VGroup(slots, fill, fifo_lbl, reg, cap)), run_time=0.6)
        self.play(LaggedStart(*(FadeIn(c, shift=UP * 0.2) for c in cards), lag_ratio=0.4), run_time=1.4)

        whole = frame_box(12.0, 1.6, [0, -2.0, 0], "Hello, I'm Perfect Paul.")
        self.sync(T.line(4)[0])
        self.play(FadeOut(cards), FadeIn(whole), run_time=0.6)
        self.play(Create(plot(s["src"], whole, INK2, width=1)), run_time=1.0)
        self.play(Create(plot(s["pcm"], whole, RED, width=1.4)), run_time=1.6)
        self.finish()


class DspVoices(PaperScene):
    """Seven voices, one program: what the speaker frame changes (the [:dv] values of each voice's record)."""
    scene_id = "voices"

    def construct(self):
        T = self.timeline
        logs = {c: data.load_dsplog(mv.OUT / "data" / ("voice_%s.tsv" % c)) for c in data.VOICES}
        val = {c: dict(zip(data.DV_NAMES, logs[c][2])) for c in data.VOICES}
        keys = list(data.DV_SHOWN)
        self.play(FadeIn(header("Seven voices, one synthesizer",
                                "the 24-word speaker frame (0x6000) changes; the DSP program does not")), run_time=1.0)

        def voice_card(c):
            g = card(data.VOICE_NAMES[c], [(data.DV_SHOWN[k], val[c][k]) for k in keys], width=4.6)
            for i, k in enumerate(keys):
                if val[c][k] != val["np"][k]:
                    g[1][i + 1][1].set_color(RED)
            return g.move_to([-3.9, -0.3, 0])
        cur = voice_card("np")
        panel = frame_box(6.4, 2.6, [3.3, -0.6, 0], "the voice's own line, as the DAC got it")
        trace = plot(logs["np"][0]["pcm"], panel, INK, width=1.2)
        self.sync(T.line(0)[0])
        self.play(FadeIn(cur), FadeIn(panel), Create(trace), run_time=1.5)
        note = label("red: differs from Perfect Paul", 16, RED).next_to(cur, DOWN, 0.12)
        self.play(FadeIn(note), run_time=0.6)

        for k, c in enumerate(("nb", "nh", "nf", "nk", "nr", "nu"), start=1):
            self.sync(T.line(k)[0])
            self.play(Transform(cur, voice_card(c)), Transform(trace, plot(logs[c][0]["pcm"], panel, RED, width=1.2)),
                      run_time=0.6)

        self.sync(T.line(7)[0])
        same = VGroup(label("Doctor Dennis  =  Frank's record", 22, INK),
                      label("Variable Val  =  Paul's, until you change it", 22, INK)).arrange(DOWN, buff=0.3)
        same.move_to(panel[0])
        self.play(FadeOut(trace), Transform(cur, voice_card("nf")), FadeIn(same[0]), run_time=1.0)
        self.play(Transform(cur, voice_card("np")), FadeIn(same[1]), run_time=1.0)
        self.pulse_until(T.duration - 0.2, list(same))
        self.finish()
