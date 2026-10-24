# DECtalk DTC-01 explainer video — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A ~10-minute 1080p30 motion-graphics video, narrated by v1.8's Perfect Paul, explaining how the DTC-01 turns
text into speech, with the DSP in depth.

**Architecture:** Three small C tools on the project's speech engine (`src/speech/`, all fixes off) render the
narration and dump the data shown on screen. Python script `videos/make_video.py` drives everything in stages:
tools → data → narration (which fixes every scene's timing) → Manim scenes → music → ffmpeg assembly. Manim scenes
read the same timing file, so pictures, captions and audio line up.

**Tech Stack:** MSVC `cl` (C tools, like `formants_graphs/build_formantlog.bat`), Python 3.13, numpy, Manim
Community 0.22 (no LaTeX), ffmpeg (`C:\ffmpeg\bin`).

**Spec:** [`videos/DESIGN.md`](DESIGN.md). Read it first; this plan argues from it.

## Global Constraints

- Exact v1.8: every tool calls `engine_init(0, …)`. Never SAY, never the library (it turns all fixes on).
- No ROM files are read by anything under `videos/` (no `merges/`, no `.bin`).
- Every render (narration and data) speaks `GREETING = 'DECtalk version 1.8 is running.'` first as an unrecorded
  lead-in, as `pitch_graphs/make_pitch_graphs.py` does. The exception is the greeting line itself in the cold open.
- Narration and data stay at 10 kHz. Resampling to 48 kHz happens only in the final mix.
- Output: 1920×1080, 30 fps, H.264 + AAC 48 kHz, total 9:30–10:30.
- Style C palette: paper `#f7f4ec`, ink `#1d1d1d`, ink 2 `#6b665c`, DEC red `#a3242c`, blue `#2b5d9e`, grid
  `#e4dfd2`, box fill `#fffdf7`; font Segoe UI; 1.2–1.6 px lines; near-square corners.
- Equations use `Text` with Unicode (`y[n] = A·x[n] + B·y[n−1] + C·y[n−2]`), never `MathTex`.
- Generated files go in `videos/out/` (git-ignored) or `build/`. Never commit waves, MP4s or `out/`.
- Comments and names follow the repo's style: plain prose comments, snake_case, ROM names and addresses where they
  exist.

## Review Focus

1. **Paul mispronounces a term** (TMS32010, F1, µs, LFSR). Expect: the `say` text spells it so Paul gets it
   right, while the caption shows the real spelling. Pinned by Task 4's `test_script_say_is_speakable`.
2. **A caption too long for the screen.** Expect: at most two lines at 1080p. Pinned by Task 4's
   `test_script_captions_fit`.
3. **A scene longer or shorter than its audio,** which makes captions drift from the voice. Expect: every scene
   video is the timeline's length to one frame. Pinned by Task 6's `test_scene_durations`, run again in Task 12.
4. **A still picture for many seconds** (a design anti-goal). Expect: nothing frozen for 4 s or more. Pinned by
   Task 6's `test_no_freezes`, run on the final MP4 in Task 12.
5. **Hum or pad masking the voice.** Expect: the hum never overlaps the narration, and the pad is at least 15 dB
   quieter under speech. Pinned by Task 11's `test_hum_clear_of_narration` and `test_pad_ducked`.

---

## File structure

```
videos/
  DESIGN.md, PLAN.md
  tools/narrate.c         text -> 10 kHz wave (Task 1)
  tools/dsplog.c          per-sample DSP stage taps + phone starts (Task 2)
  tools/ltslog.c          dictionary lookups + LTS rules as they fire (Task 3)
  tools/build_tools.bat   builds the three into build\videos\ (Task 1)
  make_video.py           the stages: tools, data, narration, scenes, music, assemble, all
  script.py               the narration: scenes, lines, budgets (Task 4)
  data.py                 loaders for the tools' TSVs and pitchlog/formantlog (Tasks 2, 3, 9)
  music.py                pad + hummed lines (Task 11)
  scenes/common.py        palette, PaperScene, Timeline, captions, helpers (Task 6)
  scenes/dsp.py           DspRhythm DspGlottal DspNoise DspCascade DspParallel DspDac (Tasks 7, 8)
  scenes/pipeline.py      TextToPhonemes TimingPitch Formants Frame (Task 9)
  scenes/bookends.py      ColdOpen BigPicture Outro (Task 10)
  test_videos.py          plain-assert tests; `python videos/test_videos.py [name ...]`
  out/                    generated, git-ignored
src/speech/tx_lts.c, tx_text.h   one NULL-by-default hook (Task 3)
.gitignore                       + *.mp4, *.m4a, videos/out/
```

`test_videos.py` is plain Python, with no pytest because it isn't installed. It runs every `test_*` function, or only
the ones named on the command line, and prints `PASS name` or the traceback. It exits non-zero on any failure.

---

### Task 1: The narration tool and the skeleton

**Files:**
- Create: `videos/tools/narrate.c`, `videos/tools/build_tools.bat`, `videos/make_video.py`, `videos/test_videos.py`
- Modify: `.gitignore` (append `*.mp4`, `*.m4a`, `videos/out/`)

**Interfaces:**
- Produces: `build\videos\narrate.exe` — `narrate out.wav "text" ["lead-in"]`: a RIFF/WAVE, PCM 16-bit, mono,
  10000 Hz, holding exactly the samples `engine_run` returns for `text`. The boot queue is drained and the lead-in
  spoken first, and neither is written (structure: `pitch_graphs/pitchlog.c`'s `main`/`say`). Exit 0 on success.
- Produces in `make_video.py`: `ROOT: Path` (repo), `OUT: Path` (= `videos/out`), `BUILD: Path` (= `build/videos`),
  `GREETING: str`, `build_tools() -> None` (runs `build_tools.bat` if any exe is missing or older than its .c),
  `run_narrate(text: str, out_wav: Path, lead: str = GREETING) -> float` (returns seconds), and `main(argv)`
  dispatching the stage names `tools data narration scenes music assemble all`.
- Produces in `test_videos.py`: `read_wav(path) -> tuple[int, np.ndarray]` (rate, int16 samples).

- [ ] **Step 1: Write the failing tests** in `videos/test_videos.py`:
  - `test_narrate_format`: `run_narrate("Hello.", tmp)` gives rate 10000, 1 channel, 2-byte samples,
    `0.2 < seconds < 2.0`.
  - `test_narrate_deterministic`: two renders of `"Hello."` are byte-identical.
  - `test_narrate_lead_not_written`: the duration of `run_narrate("Hello.", a)` is less than
    `run_narrate(GREETING + " Hello.", b, lead="")` minus 1.0 s.
  - `test_no_rom_paths`: no file under `videos/` other than `*.md` and `out/` contains `merges`,
    `dectalk_v1.8_full` or `dsp_v1.8`.
- [ ] **Step 2: Run** `python videos/test_videos.py`. Expected: the narrate tests fail (no exe) and
  `test_no_rom_paths` passes.
- [ ] **Step 3: Write `narrate.c` and `build_tools.bat`.** The bat copies the `cl` line of
  `formants_graphs/build_formantlog.bat` (same vcvars lookup, same `src` file list) and builds into
  `build\videos\`, one `cl` per tool (dsplog and ltslog are added in Tasks 2-3). The `.wav` header is written by
  hand (44 bytes) and the data size patched at the end.
- [ ] **Step 4: Write `make_video.py`'s skeleton**, with the `tools` stage working and the other stages raising
  `SystemExit("stage not built yet")`.
- [ ] **Step 5: Run** `python videos/make_video.py tools` and then `python videos/test_videos.py`. Expected: all PASS.
- [ ] **Step 6: Commit** `git add .gitignore videos/tools videos/make_video.py videos/test_videos.py` and
  `git commit -m "videos: narrate tool and pipeline skeleton"`.

### Task 2: DSP stage taps

**Files:**
- Create: `videos/tools/dsplog.c`, `videos/data.py`
- Modify: `videos/tools/build_tools.bat`, `videos/make_video.py` (the `data` stage), `videos/test_videos.py`

**Interfaces:**
- Consumes: `run_narrate`, `GREETING`, `build_tools` (Task 1).
- Produces: `build\videos\dsplog.exe` — `dsplog out.tsv "text" ["lead-in"]`. Tab-separated records, numbered by
  sample from 0 at the start of `text`:
  - `S n pcm lp tilt glot av_lin src noise np cf5 cf4 cf3 cf2 cf1 pf6 pf5 pf4 pf3 pf2 ab nper t0 nopen f1 f2 f3 fnz`
    — one per sample the link delivers.
  - `P n code samples name` — a phone starts (from `engine_hooks_t.phone`; the name as `pitchlog.c` gets it from
    `sym_table`).
  - `V v0 … v27` — once, at the end: `engine_voice(-1, v)`, the voice the text left in force (REFERENCE §8.1 order).
- Produces in `data.py`: `EXAMPLE = "Hello, I'm Perfect Paul."`, `load_dsplog(path) -> tuple[dict[str,
  np.ndarray], list[dict], list[int]]` (S columns as int arrays, P records as dicts with `n code samples name`, the
  V values), and `i16(x: int) -> int` (wrap to int16).
- Produces in the `data` stage, for Task 8's voices scene: `out/data/voice_<cmd>.tsv` = `dsplog` of
  `"[:<cmd>] " + VOICE_LINES[cmd]` for each `cmd` in `VOICES`. In `data.py`: `VOICES = ["np", "nb", "nh", "nf",
  "nk", "nr", "nu", "nd", "nv"]`, with `VOICE_NAMES` (Perfect Paul, Beautiful Betty, Huge Harry, Frail Frank,
  Kit the Kid, Rough Rita, Uppity Ursula, Doctor Dennis, Variable Val) and `VOICE_LINES` (each voice introducing
  itself, e.g. `"I'm Huge Harry."`; the wording is the builder's).
- Produces in `make_video.py`'s `data` stage: `out/data/dsp_example.tsv` = `dsplog` of `EXAMPLE` with `GREETING`
  as the lead-in.

How the taps are taken (no change to `src/`). `dsplog.c` does `#include "../../src/speech/engine.c"` to reach the
static `E` (so `engine.c` is left out of its `cl` list). After `engine_init`, it sets
`E.link.hooks.sample = on_sample`. The link calls that hook in `pass()` only for a sample that reaches the DAC, after
`l->samples++`, with the DSP state as that sample left it. So, with `d = &E.link.dsp`:
`n = E.link.samples - 1 - base`, `pcm = (int16_t)(w & 0xFFF0)`, `lp = d->lp[1]`, `tilt = d->tilt_y`,
`glot = d->glot`, `av_lin = d->av_lin`, `src = d->hist[2]` (the cascade's input), `noise = d->noise`, and section
`s`'s output (`DSP_NP` … `DSP_PF2`) is `d->hist[2 + 2*s]`. `ab nper t0 nopen f1 f2 f3 fnz` are the `dsp_t` fields of
those names. `base` is `E.link.samples` when logging starts (after the lead-in, as in `pitchlog.c`).

- [ ] **Step 1: Write the failing tests:**
  - `test_dsplog_matches_narration`: the `pcm` column of `dsplog` on `EXAMPLE` (lead `GREETING`) equals, sample for
    sample and in length, the wave `run_narrate(EXAMPLE, …)` writes.
  - `test_dsplog_taps_rebuild_sample`: for every S row,
    `out = i16(2*cf1); for p in (pf6, pf5, pf4, pf3, pf2): out = i16(p - out); s = i16(out - i16((ab*noise) >> 15))`
    and then `s & 0xFFF0 == pcm & 0xFFF0` (as unsigned 16-bit). This is `speech_sample`'s last lines
    (`src/speech/dsp_synth.c:296-300`).
  - `test_dsplog_phones`: there is at least one P record, the `n` values strictly increase, and each is below the
    number of S rows.
  - `test_voices`: the V values of the seven voices `np nb nh nf nk nr nu` are pairwise different; `nd`'s equal
    `nf`'s (v1.8's Dennis is Frank's record); `nv`'s equal `np`'s (Val starts as Paul).
- [ ] **Step 2: Run** `python videos/test_videos.py test_dsplog_matches_narration test_dsplog_taps_rebuild_sample
  test_dsplog_phones`. Expected: FAIL (no exe).
- [ ] **Step 3: Write `dsplog.c`** (above), add its `cl` line, and write `load_dsplog`, `i16` and the `data` stage.
- [ ] **Step 4: Run** the same tests. Expected: PASS. A failure of the rebuild test means a tap is read from the
  wrong `hist` slot. Fix the mapping, not the test.
- [ ] **Step 5: Commit** `videos: dsplog, per-sample DSP stage taps`.

### Task 3: Dictionary and letter-to-sound log

**Files:**
- Modify: `src/speech/tx_text.h`, `src/speech/tx_lts.c` (the hook)
- Create: `videos/tools/ltslog.c`
- Modify: `videos/tools/build_tools.bat`, `videos/data.py`, `videos/make_video.py`, `videos/test_videos.py`

**Interfaces:**
- Produces in `src/`: `extern void (*lts_rule_hook)(int rule, int pos);` in `tx_text.h`, under the LTS block, with
  the comment `/* tools: rule (1-373, ROM order) matched at list position pos and was applied; NULL = none */`.
  It is defined in `tx_lts.c` beside `lts_rule`. In `lts_rule_engine`, it is called right after a successful
  `lts_rule_apply` with `lts_rule` (already 1-based there) and the `lts_pos` from before the call. NULL does
  nothing, so the ROM path is unchanged.
- Produces: `build\videos\ltslog.exe` — `ltslog out.tsv word ...` (lower-case words). For each word:
  - `W word dict pron` (`engine_dict_builtin`; `pron` is `-` when not found);
  - `R word rule pos codes` for every rule the hook reports while the word is spoken, where `codes` is the LTS
    node list after the rule (`lts_hdr.next` to `lts_nil`), as comma-separated integers;
  - `X word phonemes` — what `engine_convert_begin(1)` / `engine_convert_end` give for the word.
- Produces in `data.py`: `LTS_WORD = "klatt"`, `load_ltslog(path) -> dict[str, dict]` (per word: `dict`, `pron`,
  `rules` as a list of `(rule, pos, codes)`, `phonemes`), and `lts_code_name(code: int) -> str`. Its mapping
  follows REFERENCE §15.10: codes below 56 are phoneme names (REFERENCE §8.3, the table at `0x19704`), 70-79 are
  the letters `A E I O U H X C J Q`, 61 is `*` and 62 is `#`. Any other code is shown as its number.
- Produces in the `data` stage: `out/data/lts.tsv` for `hello`, `i'm`, `perfect`, `paul` and `LTS_WORD`.

- [ ] **Step 1: Write the failing tests:**
  - `test_ltslog_dictionary_word`: `hello` has `dict == 1` and no rules.
  - `test_ltslog_rule_word`: `LTS_WORD` has `dict == 0`, at least 3 rules, rule numbers in 1..373 and non-empty
    phonemes. If `klatt` turns out to be a dictionary word, set `LTS_WORD = "frobnitz"` (a word the video can call
    made-up) and keep the test.
- [ ] **Step 2: Run** them. Expected: FAIL.
- [ ] **Step 3: Add the hook,** write `ltslog.c` (it links `engine.c` normally and sets `lts_rule_hook` after
  `engine_init(0, …)`), and add `load_ltslog`, `lts_code_name` and the data-stage call.
- [ ] **Step 4: Run** the tests. Expected: PASS. Then check that the hook changed nothing:
  `python decomp/scripts/check_frames.py` passes every mode, and `cmake --build --preset x64` (x64 developer prompt)
  builds with no warnings.
- [ ] **Step 5: Commit** `tx_lts: lts_rule_hook for tools; videos: ltslog`.

### Task 4: The narration script

**Files:**
- Create: `videos/script.py`
- Modify: `videos/test_videos.py`

**Interfaces:**
- Produces:
  ```python
  @dataclass
  class Line:
      say: str                 # what Paul is sent: ASCII DECtalk text, phonemic [..] allowed
      show: str | None = None  # the caption when it differs from say (real spellings: F1, TMS32010, µs)
      pause: float = 0.6       # seconds of silence after the line
      boot: bool = False       # rendered with no lead-in (the power-up greeting itself)
  @dataclass
  class Scene:
      id: str
      budget: float            # target seconds (DESIGN.md's structure)
      lead: float              # seconds before the first line
      tail: float              # seconds after the last line
      lines: list[Line]
  SCENES: list[Scene]
  ```
- Scene ids, their Manim classes and budgets in seconds (they sum to 600):
  `cold_open` ColdOpen 35 · `big_picture` BigPicture 45 · `text_to_phonemes` TextToPhonemes 60 ·
  `timing_pitch` TimingPitch 55 · `formants` Formants 60 · `frame` Frame 30 · `dsp_rhythm` DspRhythm 30 ·
  `dsp_glottal` DspGlottal 55 · `dsp_noise` DspNoise 30 · `dsp_cascade` DspCascade 70 · `dsp_parallel` DspParallel 35 ·
  `dsp_dac` DspDac 25 · `voices` DspVoices 35 · `outro` Outro 35. `scene_class(id) -> str` gives the class name.
- Paul says every line except the `voices` scene's demo lines. Those are `Line(say="[:nb] " + VOICE_LINES["nb"],
  show="Beautiful Betty: …")` and so on (one render per line, so Paul is back for the next line). Paul introduces
  the scene and remarks on Dennis and Val.
- Content: in Paul's first person, covering each DESIGN.md section and **every bullet of "What the DSP section must
  cover"**. Every fact comes from REFERENCE.md, the topology figure or the graph write-ups (PITCH_SYSTEM.md,
  FORMANT_SYSTEM.md, SMOOTHING_SYSTEM.md). `cold_open`'s first line is `Line(GREETING, boot=True)`. The examples
  are `data.EXAMPLE` and `data.LTS_WORD`.

- [ ] **Step 1: Write the failing tests:**
  - `test_script_budgets`: the ids are the 14 above, in that order, and the budgets sum to 600.
  - `test_script_voices_only_in_voices`: no `say` outside the `voices` scene contains `[:n`.
  - `test_script_say_is_speakable`: every `say` is ASCII, and outside `[...]` no token matches
    `r"\b(?=\w*[A-Za-z])(?=\w*\d)\w+\b"` (letters mixed with digits, e.g. `F1`, `TMS32010`: write "F one" and put
    `F1` in `show`).
  - `test_script_word_count`: the `say` words total 1150–1400.
  - `test_script_captions_fit`: every caption (`show or say`, with `[...]` removed) is at most 110 characters.
- [ ] **Step 2: Run** them. Expected: FAIL (no module).
- [ ] **Step 3: Write `script.py`.**
- [ ] **Step 4: Run** the tests. Expected: PASS.
- [ ] **Step 5: Commit** `videos: narration script`.

### Task 5: Narration render and the timeline — USER REVIEW GATE 1

**Files:**
- Modify: `videos/make_video.py` (the `narration` stage), `videos/test_videos.py`

**Interfaces:**
- Consumes: `SCENES`, `run_narrate`.
- Produces `out/narration/<scene>_<k>.wav` (one per line; `lead=""` when `boot`), plus:
  - `out/timing.json`: `{"total": s, "scenes": [{"id", "start", "end", "lines": [{"start", "end", "wav",
    "caption"}]}]}`, in seconds on the video's clock. The rule: a scene starts where the previous one ended; its
    first line starts at `start + lead`; each next line starts at the previous line's end plus its `pause`; and
    `end = max(start + budget, last line end + tail)`.
  - `out/captions.srt` (one entry per line, same times), and `out/narration_10k.wav`, the whole video's narration
    at 10 kHz with each line placed at its start.
- Produces in `make_video.py`: `load_timing() -> dict`.

- [ ] **Step 1: Write the failing tests:**
  - `test_timing_monotonic`: lines are ordered, don't overlap, and lie inside their scene; scenes tile the
    timeline with no gaps.
  - `test_timing_total`: `570 <= total <= 630`.
  - `test_srt_matches_timing`: the SRT has one entry per line and its times equal the timeline's to 1 ms.
- [ ] **Step 2: Run** them. Expected: FAIL.
- [ ] **Step 3: Implement** the `narration` stage.
- [ ] **Step 4: Run** `python videos/make_video.py narration`, then the tests. Expected: PASS. If the total is out of
  range, adjust `script.py` (pauses, leads, words), not the rule.
- [ ] **Step 5: Commit** `videos: narration render and timeline`.
- [ ] **Step 6: STOP for the user.** Hand over `videos/script.py` and `videos/out/narration_10k.wav`. List any terms
  Paul still says oddly. Do not start Task 6 until the user approves the script and the voice; re-run Tasks 4-5
  on their changes.

### Task 6: The house style

**Files:**
- Create: `videos/scenes/common.py`
- Modify: `videos/make_video.py` (the `scenes` stage), `videos/test_videos.py`

**Interfaces:**
- Produces in `common.py`:
  - the constants `PAPER INK INK2 RED BLUE GRID BOX` (the Global Constraints values) and `FONT = "Segoe UI"`;
  - `class Timeline`: `Timeline(scene_id)` reads `out/timing.json`; `.duration: float`; `.lines: list[tuple[float,
    float, str]]` (scene-local start, end, caption);
  - `class PaperScene(Scene)`: the class attribute `scene_id: str`. `setup()` sets the paper background, draws the
    engineering grid (12 px at 1920 wide = `config.frame_width * 12 / 1920` units, `GRID`, 0.5 px) and adds the
    captions: a bottom bar of pre-built `Text` (ink, Segoe UI, at most 2 lines), each shown by an updater only
    between its line's start and end. `sync(t: float)` waits until scene time `t`. `finish()` waits until
    `Timeline.duration`, and every scene ends with it;
  - `wave(samples: np.ndarray, width: float, height: float, color: str) -> VMobject`, a polyline scaled to the
    peak of `abs(samples)`;
  - `box(label: str, sub: str = "") -> VGroup`, the near-square `BOX`-filled block of the mockup.
- Produces in `make_video.py`'s `scenes` stage: `render_scene(scene_id: str, quality: str = "final") -> Path`.
  It runs `python -m manim` on the module holding `scene_class(id)` with `--resolution 1920,1080 --frame_rate 30`
  (`"final"`) or `-ql` (`"draft"`) and `--disable_caching`, then moves the MP4 to `out/scenes/<id>.mp4`.
  `scenes [id ...]` renders the given scenes, or all of them.
- Produces in `test_videos.py`: `probe(path) -> dict` (ffprobe JSON), `test_scene_durations` (each
  `out/scenes/<id>.mp4` lasts the timeline's duration ± 1/30 s), and `test_no_freezes` (ffmpeg `freezedetect`
  with `n=0.001:d=4` reports nothing for each rendered scene). Both skip scenes not yet rendered.

- [ ] **Step 1: Write the failing test** `test_paper_smoke`: render a `SmokeScene(PaperScene)` with
  `scene_id = "frame"` that draws one `box("F1", "700")` and calls `finish()`. Use `-ql` and keep the scene in
  `test_videos.py`. Its MP4 lasts `Timeline("frame").duration` ± 1/15 s, and its first frame's corner pixel is
  `PAPER` within ±3 per channel (via `ffmpeg -frames:v 1` to PNG; numpy reads it through matplotlib).
- [ ] **Step 2: Run** it. Expected: FAIL.
- [ ] **Step 3: Write `common.py`** and the `scenes` stage.
- [ ] **Step 4: Run** it. Expected: PASS.
- [ ] **Step 5: Commit** `videos: house style, timeline, captions`.

### Task 7: DspCascade, the style test — USER REVIEW GATE 2

**Files:**
- Create: `videos/scenes/dsp.py` (`DspCascade` first)
- Modify: `videos/data.py` (`spectrum_db(x: np.ndarray, n: int = 512) -> tuple[np.ndarray, np.ndarray]`, which
  gives Hz and dB of a Hann-windowed FFT at 10 kHz)

**Interfaces:**
- Consumes: `load_dsplog`, `PaperScene`, `wave`, `box`, `Timeline`.
- What the scene must show (DESIGN.md, the cascade bullet; the order is the DSP's): the resonator equation written
  on; `src` going in; then nasal zero, nasal pole, F5, F4, F3, F2, F1 lighting up in turn, each with its output wave
  (`np`, `cf5` … `cf1`) and the spectrum of that output over one window of a vowel in `EXAMPLE` (the window and
  vowel are the builder's choice). This shows the envelope gaining peaks, labelled with that window's `f1 f2 f3`.
  It ends with the `×2` into the sum. Its beats land on the line times by `sync`.

- [ ] **Step 1: Render** `python videos/make_video.py scenes dsp_cascade`, then run
  `python videos/test_videos.py test_scene_durations test_no_freezes`. Expected: PASS.
- [ ] **Step 2: Commit** `videos: DspCascade scene`.
- [ ] **Step 3: STOP for the user.** Hand over `videos/out/scenes/dsp_cascade.mp4` as the style test. Apply their
  changes to `common.py` and the scene before Task 8.

### Task 8: The other DSP scenes

**Files:** Modify `videos/scenes/dsp.py` (`DspRhythm`, `DspGlottal`, `DspNoise`, `DspParallel`, `DspDac`,
`DspVoices`).

**Interfaces:** Consumes Task 7's helpers and `load_dsplog`. What each scene must show is DESIGN.md's DSP list:
- `DspRhythm`: 64 samples per 6.4 ms frame, polling every 64 samples, the DSP pacing the 68000.
- `DspGlottal`: the `B0` table, open and closed phase (`nper` vs `nopen`), `T0 = 40000/F0`, the 4× steps and the
  2-pole low-pass (`lp`), tilt (`tilt`), breathiness, and ×AV via `amptable` (`av_lin`, `glot`).
- `DspNoise`: the 16-bit LFSR, its low-pass, and the ×½ after `nmod` (`noise` against `nper`).
- `DspParallel`: F2–F6 and the bypass on the noise, with alternating signs, on a fricative of `EXAMPLE` (`pf2` …
  `pf6`).
- `DspDac`: the sum, the 12-bit word (`pcm`), 10 kHz; the 19-word and 24-word frame types; the buzz becoming the
  voice (the `src` wave cross-fading into `pcm` while `pcm` plays).
- `DspVoices`: the same block diagram, with the speaker frame swapped. For each voice in `VOICES` order, as its line
  plays: its name, the `[:dv]` values that differ from Paul's (from the V records, by name, REFERENCE §8.1), and its
  `pcm` wave from `voice_<cmd>.tsv`. Dennis is shown pointing at Frank's record and Val at Paul's.

- [ ] **Step 1: Render** each scene and run `test_scene_durations test_no_freezes`. Expected: PASS for all seven DSP
  scenes.
- [ ] **Step 2: Commit** `videos: DSP scenes`.

### Task 9: The pipeline scenes

**Files:**
- Create: `videos/scenes/pipeline.py`
- Modify: `videos/data.py`, `videos/make_video.py`

**Interfaces:**
- The `data` stage also builds `pitchlog` and `formantlog` with their existing `build_*.bat` (if missing) and
  writes `out/data/pitch_example.tsv` and `out/data/formant_example.tsv` (`EXAMPLE`, lead `GREETING`). Record
  formats: the header comments of `pitch_graphs/pitchlog.c` and `formants_graphs/formantlog.c`.
- `data.py` adds `load_pitchlog(path) -> dict` and `load_formantlog(path) -> dict` (records grouped by type,
  numeric fields as numbers). Reuse the parsing in `make_pitch_graphs.py` / `make_formant_graphs.py` where it fits.
- Scenes:
  - `TextToPhonemes`: from `lts.tsv`, the dictionary hits and `LTS_WORD`'s rules firing, the list shown before and
    after each rule (`lts_code_name`).
  - `TimingPitch`: phone durations, the hat pattern, stress impulses (the F records' terms, as in
    `01_hat_pattern_anatomy.png`).
  - `Formants`: F1–F3 targets per phone, then the tracks after the transitions (formantlog P and F records, as in
    `formants_graphs/10_*` and `11_*`).
  - `Frame`: the 19 words of one frame filling up (`0x4000`, the 17 F-record words, the trailer `0x43D4`), sent every
    6.4 ms.
- [ ] **Step 1: Write the failing test** `test_pitch_formant_data`: both TSVs exist after `make_video.py data`, have
  F records, and the T0 column agrees between them frame for frame.
- [ ] **Step 2: Implement** the data, loaders and scenes. Render them and run `test_pitch_formant_data
  test_scene_durations test_no_freezes`. Expected: PASS.
- [ ] **Step 3: Commit** `videos: pipeline scenes`.

### Task 10: Cold open, big picture, outro

**Files:** Create `videos/scenes/bookends.py`.

**Interfaces:**
- `ColdOpen`: `dectalk_unit.png` (`ImageMobject`), the switch, and the greeting line.
- `BigPicture`: the 68000 → frame every 6.4 ms → TMS32010 → sample every 100 µs map, exposed as
  `big_picture_map(highlight: str | None) -> VGroup` with `highlight` in
  `{"text", "timing", "formants", "frame", "dsp"}`. Once it exists, `TextToPhonemes`, `TimingPitch`,
  `Formants`, `Frame` and `DspRhythm` open with it as the "you are here" (re-render them).
- `Outro`: the whole chain, a sung line and credits (sources: Klatt, Hunnicutt, MITalk, DEC; wording is the
  builder's).
- [ ] **Step 1: Render** them and run `test_scene_durations test_no_freezes`. Expected: PASS.
- [ ] **Step 2: Commit** `videos: cold open, big picture, outro`.

### Task 11: Music

**Files:**
- Create: `videos/music.py`
- Modify: `videos/make_video.py` (the `music` stage), `videos/test_videos.py`

**Interfaces:**
- Consumes `load_timing`, `run_narrate`.
- Produces `out/music_48k.wav` (mono, float or int16), the length of `timing["total"]`:
  - a pad (numpy, 48 kHz; chords are the builder's), −18 dBFS RMS in the gaps and ducked by 18 dB under every
    narration line, with 0.3 s ramps;
  - `HUM: list[tuple[float, str]]`: (start in video seconds, DECtalk singing text, e.g. `"[m<400,20>m<400,22>]"`;
    notes 1-37 = C2-C5, REFERENCE §15.14). Each is rendered with `run_narrate` (lead `GREETING`), resampled to
    48 kHz, and placed at its start. Hums go in the cold open, the outro and the gaps between sections.
- Produces `narration_intervals(timing) -> list[tuple[float, float]]`.
- [ ] **Step 1: Write the failing tests:**
  - `test_hum_clear_of_narration`: every hum's interval, padded by 0.5 s, overlaps no narration interval.
  - `test_pad_ducked`: the pad's RMS inside the narration intervals is at least 15 dB below its RMS in the gaps
    (`music.py` also writes `out/pad_48k.wav` alone, for this test).
- [ ] **Step 2: Run** them. Expected: FAIL.
- [ ] **Step 3: Implement** them. **Step 4: Run** `make_video.py music` and the tests. Expected: PASS.
- [ ] **Step 5: Commit** `videos: pad and hummed lines`.

### Task 12: Assembly

**Files:** Modify `videos/make_video.py` (the `assemble` and `all` stages) and `videos/test_videos.py`.

**Interfaces:**
- `assemble`: concatenates `out/scenes/<id>.mp4` in `SCENES` order (ffmpeg concat demuxer, no re-encode if the
  streams match). It resamples `narration_10k.wav` to 48 kHz with ffmpeg (`-ar 48000`), adds `music_48k.wav`,
  scales so the peak is −1 dBFS, encodes AAC 192 kbit/s, and muxes. Output: `out/dectalk_dtc01_v18_explainer.mp4`
  and a copy of `captions.srt` beside it.
- `all`: tools → data → narration → scenes → music → assemble.
- [ ] **Step 1: Write the failing tests:**
  - `test_final_format`: by `probe`: 1920×1080, 30 fps, h264, aac at 48000 Hz, duration 570–630 s, and video and
    audio lengths within 0.1 s of each other.
  - `test_final_peak`: ffmpeg `volumedetect` reports `max_volume <= -1.0 dB`.
  - `test_final_no_freezes`: `freezedetect` on the final MP4 reports nothing.
- [ ] **Step 2: Run** them. Expected: FAIL.
- [ ] **Step 3: Implement** them. **Step 4: Run** `python videos/make_video.py all` from a tree with no
  `videos/out/` and no `build/videos/`, then the whole `python videos/test_videos.py`. Expected: all PASS.
- [ ] **Step 5: Commit** `videos: assembly`.
- [ ] **Step 6: Hand the final MP4 and SRT to the user.**
