# DECtalk DTC-01 explainer video — design

Date: 2026-10-24. Project: DECtalk DTC-01 v1.8 reverse engineering (`C:\Users\abart\Desktop\DECtalk-DTC-01-Reborn`).

## Intent

A single ~10-minute motion-graphics video that explains how the DECtalk DTC-01 turns typed text into speech, narrated
by DECtalk itself (Perfect Paul), built from the project's own C rebuild of the firmware. The point is to show *how the
sound is formed*: the DSP section is the heart of the video and gets the most time and detail. The earlier stages
(text to phonemes, timing and pitch, formants) are covered in detail too, but at a brisker pace, because they set up
what the DSP is given.

Why a single video: the user judged that a multi-part series would lose its audience. Why Paul: he is the
synthesizer's default voice, and the video narrating itself in the voice it explains is the hook.

## Goals

- One video, about 10:00 (a little either side is fine), 1920×1080, 30 fps.
- Motion graphics throughout (things draw in, signals flow, curves morph). Not a slideshow of still images.
- Every number, name and waveform on screen is real: taken from the C rebuild's output or from `REFERENCE.md` /
  the ROM, never invented. Labels may use the ROM's own names and addresses (`CASCADE_BRANCH 0x5E8`, `pht0draw`).
- Narration by Perfect Paul, rendered by the project's own speech code.
- Background music with no dead air: a quiet synth pad plus Paul humming/singing a melody with v1.8's singing syntax.

## Non-goals

- A series, or chapters released separately.
- Covering the host side (escape sequences, SETUP, phone/DTMF dialing, the RTOS) beyond a passing mention.
- Teaching DSP from first principles beyond what is needed to follow the resonators (no z-transforms).
- Comparing DECtalk with other synthesizers, or its history beyond a line or two.

## Anti-goals (would make it a failure even if it "works")

- Wrong facts on screen. This project's value is its accuracy; a made-up waveform or a wrong number undermines it.
- Paul unintelligible with nothing to read: narration without captions.
- Music fighting the narration (two Pauls at once, or a pad loud enough to mask 10 kHz speech).
- Long static stretches: a frame that doesn't move for more than a few seconds.
- Any part of the pipeline loading the ROMs (project rule, AGENTS.md §0.10: nothing in the product loads the ROMs; the
  video tooling should hold to the same rule and use the C rebuild).

## Constraints

- **Visual style: "C. DEC technical manual"** (chosen by the user from four mockups). Off-white paper, DEC red,
  engineering-drawing lines, matching the existing `pitch_graphs/`, `formants_graphs/` and `dsp/dsp_synth_topology`
  figures and the beige case in `dectalk_unit.png`. Mockup palette, as a starting point:
  paper `#f7f4ec`, ink `#1d1d1d`, secondary ink `#6b665c`, DEC red `#a3242c`, blue `#2b5d9e`, grid `#e4dfd2`
  (a light 12-px engineering grid), box fill `#fffdf7`, sans-serif type (Segoe UI / Helvetica), thin 1.2-1.6 px lines,
  near-square corners. The mockup scene (glottal source → F1/F2/F3 boxes → output wave + harmonic spectrum with
  formant envelope, equation caption at the bottom) is the reference frame.
- **Tool: Manim Community 0.22** (installed, renders through the installed ffmpeg). Python, like the graph scripts.
  **No LaTeX on the machine:** use `Text` with Unicode math (`y[n] = A·x[n] + B·y[n−1] + C·y[n−2]`), not `MathTex`.
  Also installed: Python 3.13, numpy, matplotlib, ImageMagick, ffmpeg (`C:\ffmpeg\bin`).
- **Speech and data come from the C rebuild in `src/`**, not the emulator or the ROM. `build/` does not exist yet.
  The graph loggers (`pitch_graphs/pitchlog.c`, `formants_graphs/formantlog.c`, with their `build_*.bat`, MSVC
  `cl` on `src/speech` + `src/kernel`) show how to run text through `src/speech/engine.c` and log per-frame data.
- **Exact v1.8, no library fixes (user).** Narration and every bit of data on screen are made with all the
  library-only fixes **off**: `engine_init(0, …)`, the ROM-exact path the project's checks use (`DSP_FIX_*`,
  `ENGINE_FIX_*`, REFERENCE §16.12, §17.13). SAY and the library turn them all on (`src/api/ttsapi.c:646`), so the
  narration is **not** rendered with SAY: a small renderer on `src/speech/engine.c` with fixes 0, built like the graph
  loggers, writes the narration waves, and the same path feeds the loggers, so what is heard is what is shown.
  v1.8's audible flaws (the thump when speech starts with aspiration after a reset, the small DC offset) stay in.
- **10 kHz.** The narration wave stays 10 kHz as rendered; it is resampled only when mixed into the video's audio
  track (ffmpeg, 48 kHz).

## Where it lives (user)

A committed top-level **`videos/`** folder (this file is `videos/DESIGN.md`), in the pattern of `pitch_graphs/` and `formants_graphs/`: this design,
the narration script, the renderer and loggers (C, with `build_*.bat` building into `build/` like
`build_formantlog.bat`), the Manim scenes, the music generator, and one script that runs the whole pipeline.
Generated output (waves, rendered scenes, the mix, the final MP4) stays out of git: `.gitignore` already covers
`build*/` and `*.wav`; add `*.mp4` and `*.m4a`, and render into `build/` or a git-ignored `videos/out/`.

## Structure (agreed with the user)

Timings are targets. Paul's default rate is 180 wpm, so ~1,300 words of narration once the hummed breaks are out.

| Time | Section | On screen |
|---|---|---|
| 0:00–0:40 | Cold open | The beige unit, the power switch, "DECtalk version 1.8 is running." Paul introduces himself, humming over the pad. |
| 0:40–1:30 | The big picture | Two chips, 68000 and TMS32010. Text in, a frame every 6.4 ms between them, a sample every 100 µs out. This map returns between sections as a "you are here". |
| 1:30–2:30 | Text to phonemes | A sentence meets the dictionary, then a few of the 373 letter-to-sound rules firing, then the phoneme string. |
| 2:30–3:30 | Timing and pitch | Phonemes stretch to their durations, the hat pattern rises and falls, stress impulses appear. |
| 3:30–4:30 | Formants and smoothing | F1/F2/F3 targets per phoneme, then the smoothing that bends them into tracks. |
| 4:30–5:00 | The frame | The 19 words fill up and are sent: T0, F1–F3, AV, AH, and the rest. |
| 5:00–8:50 | Inside the DSP | Glottal pulse, then tilt and AV; noise from the LFSR; the cascade resonators one by one, each adding its formant peak to a live spectrum; the parallel branch for fricatives; the sum into the DAC. Ends with the buzz turning into Paul's voice. |
| 8:50–9:25 | The voices | Same DSP, a different 24-word speaker frame: each v1.8 voice says a line, its speaker values changing on screen. |
| 9:25–10:00 | Outro | The whole chain at once, a sung line, credits. |

**The voices (user, added after the first plan).** Paul stays the narrator and the voice of every DSP demonstration;
the other voices appear only in their own scene. v1.8 has seven distinct voices: Perfect Paul, Beautiful Betty,
Huge Harry, Frail Frank, Kit the Kid, Rough Rita, Uppity Ursula (`:np :nb :nh :nf :nk :nr :nu`). Two oddities are
worth a line: `:nd` Doctor Dennis is a second pointer to Frank's record in v1.8 (REFERENCE §8.1), and `:nv` Variable
Val starts out as Paul. Whispery Wendy is 2.0-only and is not in v1.8. What changes between voices is the speaker
frame (head size and F4/F5, glottal settings, breathiness, pitch baseline and range: the 28 `[:dv]` values).

### What the DSP section must cover (from `dsp/dsp_synth_topology.png`, REFERENCE §16, `src/speech/dsp_synth.c`)

- The DSP's rhythm: one 10 kHz sample per DAC interrupt, 64 samples per 6.4 ms frame; the DSP looks for a new frame
  every 64 samples and paces the 68000.
- Voicing: parwav's natural glottal source (the `B0` table at DSP `0x1C7`), open and closed phase, one period every
  `T0 = 40000/F0` steps at 40 kHz (4 steps per sample), the 2-pole low-pass back down to 10 kHz; spectral tilt; ×AV
  via Klatt's `amptable` (dB → linear, in the DSP ROM); breathiness in the open phase; ×AH aspiration.
- Noise: the 16-bit LFSR, its low-pass, ×½ after `nmod` in voiced sounds.
- Cascade branch, in the DSP's order: nasal zero, nasal pole, F5, F4, F3, F2, F1. Each resonator is
  `y[n] = A·x[n] + B·y[n−1] + C·y[n−2]` in Q12; show the spectrum gaining one peak per stage. F4/F5 come from the
  speaker frame (head size).
- Parallel branch on the noise: F2–F6 and the bypass, with alternating signs, for fricatives and bursts.
- The sum, the 12-bit DAC word, 10 kHz output.
- The two frame types in passing: the 19-word speech frame (`0x4000`) every 6.4 ms and the 24-word speaker
  frame (`0x6000`) on a voice change.
- Optional if time allows: tone mode (the DTMF oscillators) as a 5-second aside.

## Audio

- **Narration:** Perfect Paul at the default rate, rendered by the fixes-off renderer (see Constraints), one wave
  file per narration line or scene, so each scene's length comes from its audio. The narration script is written as plain text
  per scene; pronunciation fixes go in as DECtalk phonemic text (`[…]`) or the user dictionary where Paul mangles a
  term (TMS32010, 68000, LFSR, Klatt…).
- **Captions:** on screen for all narration, plus a separate `.srt` file for upload.
- **Music:** a quiet synth pad generated in Python (numpy, 48 kHz) under the whole video, and Paul humming or singing a
  melody with v1.8's singing syntax (`<duration,note>` on a phoneme, e.g. `[l'aa<400,24>]`, notes 1-37 = C2-C5,
  REFERENCE §15.14, `notetab` 0x16172). The humming carries the cold open, the outro and the breaks between sections;
  the pad ducks or stops while Paul narrates, and the hummed melody never overlaps the narration.
- **Mix:** ffmpeg, 48 kHz AAC, narration as the loudest element.

## Pipeline (how it gets made)

1. Narration script (text per scene) → user review.
2. Render narration waves → their durations set each scene's length.
3. Data loggers run the same sentences through `src/speech/` and dump per-frame (and, for the DSP section,
   per-sample, per-stage) data; Manim scenes read that data.
4. Manim renders each scene at 1080p30.
5. Music: pad + hummed lines, ducked under the narration.
6. ffmpeg concatenates the scenes and muxes the audio; captions burned in and also written as `.srt`.

Review points with the user: the narration script before any rendering, then one finished DSP scene as a style test
before the rest is built.

## Decisions made so far

**By the user:** single ~10-minute video; motion graphics, not stills; style C; Perfect Paul narrates; music = Paul
humming + a quiet synth pad; the outline and timings above; the DSP section in depth, the others detailed but quicker;
Manim (installed with the user's OK); exact v1.8 with the library fixes off, for narration and data alike; the
work lives in a committed `videos/` folder; a scene demonstrating the other voices, with Paul still the main voice.

**Calls made by Claude (open to change):** captions on screen and as `.srt`; equations as plain text (no LaTeX);
narration rendered per scene so audio drives timing; the music generated in-house (no licensing); the narration
resampled only in the final mix; 1080p30 H.264/AAC.

## Left to the builder

The exact animations and transitions per beat; the pad's chords and the hummed melody; caption styling; how the
DSP stage data is captured (a logger in the style of `formantlog.c` over `src/speech/dsp_synth.c`, whose stage
functions are static today; the logger can `#include` the .c file rather than change `src/`); the file layout
inside `videos/`.
