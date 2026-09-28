# PITCH_SYSTEM.md: how the DTC-01 v1.8 makes its pitch (F0)

This document describes the intonation of DECtalk I firmware v1.8, from the stress and boundary symbols of a clause
to the pitch period sent to the DSP every 6.4 ms. It follows the C rebuild, which is checked word for word against
the ROM (REFERENCE §15.19-15.21):

| Stage | C (`src/speech/`) | ROM | dapi counterpart |
|---|---|---|---|
| stress, boundaries, hat position | `phalloph`, `ph_alloph.c` | `0x843e` | `ph_aloph.c` (much later rewrite) |
| F0 command list (hat pattern) | `phtiming` → `make_f0_command`, `ph_timing.c` | `0x8ebe`, `0xa6e2` | `ph_inton1.c` `phinton`, `make_f0_command` |
| per-frame F0, T0, tilt | `pht0draw`, `ph_frame.c` | `0xc722` | `ph_drwt01.c` `pht0draw` |
| voice parameters → pitch scaling | `setspdef`, `ph_frame.c` | `0x81e6` | `ph_vset.c` |
| tables | `ph_rom.c`: `f0_stress_steps`, `f0segtars`, `notetab`, `sung_vibrato_steps`, `voice_defs` | `0x15f00`, `0x169c4`, `0x16172`, `0x1616a`, `0x15fbe…` | `p_us_rom.c` |

Confidence tags as in AGENTS.md: **[V]** read from the checked C / ROM, **[I]** an interpretation. Everything below
is [V] unless tagged.

The graphs in [`pitch_graphs/`](pitch_graphs/) were drawn from the same C code, run after the power-up greeting so
that its state matches the ROM's. Its per-frame T0 is identical to the emulator's ROM frame logs
(`decomp/reference/*.frames.tsv`) for all 18 plain-text corpus entries, 9,015 frames, with no difference. So the
curves are what the real unit sends to its DSP.

---

## 1. The picture in one paragraph

DECtalk uses **Klattalk's "hat pattern"** (Klatt 1982, after Maeda 1974; REFERENCE §14.10), not MITalk's
O'Shaughnessy algorithm. `phalloph` marks each syllable's stress and each boundary's strength, and chooses the
syllable where each phrase's "hat" ends. `phtiming` computes the durations and then walks the clause once, writing
a short list of **F0 commands**. Each command is a *step* (the hat goes up or down and stays there) or an
*impulse* (a 16-frame bump for a stressed syllable, a continuation rise or a final lowering), timed in frames
relative to a phone. Every frame, `pht0draw` reads the commands that are due and adds four terms: a **declining
baseline**, the **hat**, the **impulse**, and a small **segmental** term per phone (voiceless consonants raise F0,
voiced ones lower it). It smooths that sum with a **two-pole low-pass**, cuts a **dip** at glottal attacks, and
maps the result from its 120 Hz reference to the voice (`ap`, `pr`). The result becomes the pitch period T0 in the
DSP frame. Sung notes and explicit F0 targets (`<dur,f0>` in phonemic text) replace all of this with a separate
glide-and-vibrato path.

```
 text side (dttask)           klsyn task, one clause at a time                                  DSP
 ───────────────────   ───────────────────────────────────────────────────────────────────   ──────────
 phonemes + stress  →  phalloph ─ allofeats[]: stress 1/2/5/6, boundary 0x40…0x380,
 ' ` "  and boundary              hat bits 0x400 / 0x800
 symbols * space ( )          │
 [ , ! ? .                    ▼
                       phtiming ─ durations (allodurs[]) and the F0 command list
                                  f0tim[] (frames since the previous command), f0tar[] (the command)
                              │
                              ▼  every 6.4 ms frame (klclause frame loop)
                       pht0draw ─ due commands → tarhat / tarimp / reset
                                  target = baseline + tarhat + tarimp + f0seg
                                  two-pole smoother → glottal dip → clamp 50-512 Hz
                                  voice scaling (ap, pr) → T0 = 40000 / F0,  tilt from F0      →  T0 word of
                                                                                                  the 0x4000 frame
```

## 2. Units and state

| Quantity | Unit | Where |
|---|---|---|
| time | frames of **6.4 ms** (64 samples at 10 kHz) | everything |
| F0 inside the rules | **tenths of Hz** (1200 = 120.0 Hz) | `f0`, `f0in`, `f0baseline`, `tarhat`, `tarimp`, `f0seg` |
| F0 after the voice | tenths of Hz, clamped to **500-5121** (50.0-512.1 Hz) | `f0_tenths` (`0x81bd8`) |
| T0 (DSP frame word 1) | `400000 / f0_tenths` = 40000 / F0 in Hz, i.e. the period in 25 µs steps (the DSP's glottal source runs at 4 × 10 kHz, REFERENCE §16.6) | `parstochip[OUT_T0]` |
| F0 command | a signed short; see §4.1 | `f0tar[]` (`0x817b8`), times in `f0tim[]` (`0x81754`) |
| fractions | Q12 (`q12(x, y) = x·y >> 12`) and Q14 | `ph_math.h` |

Some state lives across clauses: the baseline counter `nframb`, the smoother's memory `f0las1`/`f0las2`, and the hat
state `hatstate`/`hatsize` (`0x822a0`, `0x822a2`; power-up values 1 = down and 200). Because of that, a clause's
pitch depends on the clause before it. In particular, the baseline is **not** restarted at a new clause, only by the
sentence-reset command (§4.2) or at power-up.

## 3. Stage 1: what `phalloph` marks

`phalloph` turns the clause's phoneme stream into allophones, and gives each one a feature word
`allofeats[]` (REFERENCE §15.20):

| Bits | Meaning |
|---|---|
| `& 7` | stress: 1 unstressed, **2** secondary `` ` ``, **5** primary `'`, **6** emphatic `"` |
| `& 0x18` | syllable position in a word of 2+ syllables: 8 first, 0x10 middle, 0x18 last (0 = monosyllable) |
| `& 0x20` | consonant before the word's first vowel |
| `& 0x3c0` | strength of the boundary that follows (copied back to the phrase's last syllable) |
| `0x400` | **the hat falls on this syllable** (the ROM's comments call it the hat position) |
| `0x800` | set together with 0x400 when the phrase ends a sentence (`.` `?` `!`) |

Boundary strengths (`bndval[]`, ROM `0x15eee`): `*` (morpheme) 0x40, space (word) 0x80, `(` and `)` (phrase)
0x100, `[` (clause start) and `,` 0x180, `.` and `!` **0x200**, `?` **0x380**. A boundary of 0x180 or more
inserts a silence phone.

**Where the hat goes.** While it reads a phrase, `phalloph` keeps a candidate `hatpos` with a priority. The first
syllable counts as 1, a secondary stress as 2, and every primary or emphatic stress as 5 and moves the candidate
forward. The candidate is therefore the **last primary stress** of the phrase, or else its last secondary stress,
or else its first syllable. A hat is closed at the candidate, which gets `0x400` and at least stress 5, when one of
these happens:
- a clause-level boundary (`[` `,` `.` `!` `?`) is reached;
- a `)` phrase boundary is reached after 2 or more stresses;
- a `(` phrase boundary is reached after 4 or more stresses.

At `.` `?` `!` it also gets `0x800`. At `!` the hat syllable (its vowel and the onset consonants that share its stress) is made
**emphatic** (stress 6). An emphatic symbol `"` anywhere in the clause sets `f0_halfsteps` = 1 (§4.3).

**Speaking rate changes the phrasing.** At `:ra` ≤ 120 a `(` or `)` boundary becomes a comma, and at ≤ 140 a `(`
becomes a `)`. Slow speech therefore gets more, shorter hats with continuation rises.

## 4. Stage 2: the F0 command list (`phtiming`)

`phtiming` computes each phone's duration first (REFERENCE §15.19). In the same pass it writes the commands with
`make_f0_command(cmd, delay)`. `delay` is in frames from the start of the current phone and can be negative. The
list stores each command's time relative to the previous command. There are at most 50 commands per clause; past
that the ROM logs `kl3.c: totev > MAXEV (50)` and the 51st overwrites the last slot's time.

### 4.1 Command encoding (as `pht0draw` reads it)

| Value | Kind | Effect |
|---|---|---|
| `0` | **reset** | `nframb = 0` (baseline back to `bf`), `tarhat = 0` |
| even, 2-1998 or negative | **step** | `tarhat += cmd` and it stays; a rise cancels a pending negative impulse and a fall a pending positive one |
| odd | **impulse** | `tarimp = 2·cmd` for **16 frames**, then 0 |
| ≥ 2000 | **user F0** (f0mode 1 only) | `cmd − 2000`: 1-37 a sung note, 50-511 a target in Hz (§6) |

So a command of 281 is a +56.2 Hz bump for 102 ms, 200 is a +20 Hz step, and −300 is a −30 Hz step. An impulse is
simply an odd number, and the stress table's values are all odd. Keeping the value even is why the hat-fall code
masks with `& 0x7ffe`.

### 4.2 The rules (f0mode 0), per phone in order

For each syllabic phone with stress 2, 5 or 6:
1. **Hat rise.** If the stress is 5 or 6 and the hat is down (`hatstate` = 1), the command is `+hatsize`, with
   delay 0 (−6 if this syllable also carries the fall). Then `hatsize = 200` and `hatstate = 2`. Normally that is
   +20 Hz. After an unreturned fall, `hatsize` still holds the fall, so the same step lifts the hat from below the
   baseline straight to the top (the "+30 Hz" steps in [07](pitch_graphs/07_emphasis.png)).
2. **Stress impulse.** `nstress` counts the stressed syllables of the clause, up to 4. The command is
   `f0_stress_steps[nstress] + f0_stress_steps[5 + stress]`, with delay +4 (−6 on the hat syllable, 0 when
   emphatic). The table (ROM `0x15f00`) is `0 210 70 20 0 | 0 0 1 31 51 71 261`. That gives:

   | stressed syllable no. | 1 | 2 | 3 | 4+ |
   |---|---|---|---|---|
   | primary (5) | 281 (+56.2 Hz) | 141 (+28.2) | 91 (+18.2) | 71 (+14.2) |
   | secondary (2) | 211 (+42.2) | 71 (+14.2) | 21 (+4.2) | 1 (+0.2) |
   | emphatic (6) | 471 (+94.2) | 331 (+66.2) | 281 (+56.2) | 261 (+52.2) |

   With `f0_halfsteps` set, every non-emphatic value becomes `(v >> 1) | 1`: 281 → 141, 141 → 71, 91 → 45, 71 → 35.
3. **Hat fall** on the `0x400` syllable, if the hat is up. The size `hatfall` depends on what follows:
   - **100** (10 Hz) if the syllable is not at the end of its phrase (boundary after < 0x100, e.g. *ROCK*-er); the
     delay is then `dur − 8`, near the end of the vowel;
   - **120** (12 Hz) if the boundary has the 0x100 bit (phrase, comma, question);
   - **312** (31.2 Hz) otherwise (`.` `!`).

   In the last two cases the delay is `max(4, (dur − 25) / 2)`. `hatfall` is scaled by assertiveness,
   `q12(hatfall, as·41)`. The step is `−(hatsize + hatfall)`: the hat drops from its top to `hatfall` *below* the
   baseline, and `hatsize` keeps that total. Then `hatstate` = 1. There is also a branch that would give 0x70,
   but it is unreachable: 0x400 is always set in this branch, so the `& 0xc00` test always picks 100.
4. **Continuation / question rise** (still on the hat syllable), if its boundary has both 0x180 bits (`,` or `?`).
   The impulse is **51** (+10.2 Hz) after a comma and **251** (+50.2 Hz) after a question (the 0x200 bit). Its
   delay is `max((3·(dur − 13) + 25) / 4, dur − 16)`. It is followed at the same time by `+hatfall`, which returns
   the hat to the baseline (`hatsize = 200`).

For each unstressed syllabic phone (stress < 2) that is the **last syllable of a longer word**:
- before `.` or `!` (boundary bits `& 0x380` = 0x200): **final lowering**, impulse `q12(−171, as·41) | 1` at delay
  +8, i.e. −34.2 Hz for Paul;
- before `,` or `?`: the continuation impulse 51 / 251 at delay `dur − 13`.

For each **silence** phone:
- if a fall is still unreturned (`hatsize` > 200), the command is `+hatfall` (back to the baseline);
- the stress count restarts at 1 (unless the silence is the clause's first phone);
- if its boundary has the 0x200 bit (`.` `!` `?`), the command is **0 (reset)** and the count restarts at 0.

The reset therefore fires at the *start* of the sentence-final pause, and the baseline falls again through the pause.
The next sentence starts from however far it has fallen by then (about 107 Hz for Paul after the power-up greeting;
[04](pitch_graphs/04_clauses_and_reset.png)).

### 4.3 Worked example: "The old man sat in a rocker." (Paul, [01](pitch_graphs/01_hat_pattern_anatomy.png))

Frames count from the clause's first frame; "read" is the frame `pht0draw` takes the command (§5.1: 4 frames before
its nominal time).

| Read at | Phone then | Command | Rule |
|---|---|---|---|
| 28 | `iy` (of "the") | +200 | hat rise, due at the start of `ow` (frame 32) |
| 32 | `ow` | 281 | 1st primary stress (210 + 71), due 4 frames into `ow` |
| 84 | `ae` of "man" | 141 | 2nd primary (70 + 71) |
| 131 | `ae` of "sat" | 91 | 3rd primary (20 + 71) |
| 187 | `r` | 71 | 4th primary on `aa`, the hat syllable: delay −6 |
| 216 | `aa` | −300 | hat fall: *rock* is not phrase-final, so 100 at `dur − 8` = 23 frames into `aa` |
| 243 | `rr` | −171 | final lowering on the unstressed last syllable before `.` |
| 262 | (end of `rr`) | +100, 0 | the pause: return the unreturned fall, then reset the baseline |

The F0 peaks at 149.6 Hz 19 frames after the first impulse is read, falls to 75 Hz at the end of *rocker*, and the
greeting's baseline (≈107 Hz at the start) jumps back to 115 Hz at the reset.

## 5. Stage 3: every frame (`pht0draw`)

### 5.1 Reading commands

At the start of each clause `pht0draw` resets its clocks. For rule F0 it starts the command clock at **4**, not 0,
so every command is taken **4 frames (25.6 ms) before** its nominal time. [I] This makes up for part of the
smoother's lag. All commands whose time has come are applied (§4.1), and several can take effect in one frame.

### 5.2 The four terms

`f0in = f0baseline + tarhat + tarimp + f0seg`, in tenths of Hz:

- **Baseline.** `f0baseline = bf·10 − nframb`. `nframb` grows by 1 per frame while the baseline is still above
  `ef·10`, i.e. the baseline falls **1 Hz per 10 frames (15.6 Hz/s)** until it reaches `ef`, and then holds. Only
  command 0 (or power-up) sets `nframb` back to 0. For Paul (`bf 115`, `ef 100`) that is a 15 Hz fall over about
  1 s. **Every other built-in voice has `bf = ef = 107`, so its baseline never falls**; Ursula has `bf 50` <
  `ef 107`, which also gives a flat baseline, at 50 Hz before scaling.
- **Hat** `tarhat`: the running sum of the steps (0 = on the baseline, +200 = on the hat).
- **Impulse** `tarimp`: `2 × command` for 16 frames.
- **Segmental** `f0seg`: `f0segtars[phone]` (ROM `0x169c4`, identical to dapi), quartered when the phone is
  unstressed (`allofeats & 6` = 0). In tenths of Hz:

  | f0segtars | phones |
  |---|---|
  | **+300** | `f th s sh p t k ch` (voiceless obstruents) |
  | +200 | `hx` |
  | +100 | `iy uw yu ir` (high vowels) |
  | +80 | `ur` |
  | +60 | `ih uh ix er w y` |
  | +50 | `_ oy rr` |
  | +40 / +30 / +20 | `ey` / `ow ax or` / `eh ah` |
  | 0 | `ae aa ay aw ao ar r l rx lx el tx q` |
  | −10 | `dx` |
  | **−50** | `m n nx em en v dh z zh b d g jh` (voiced consonants) |

  This is Klattalk's step 4 (segmental perturbations): intrinsic vowel pitch, and the raising after voiceless
  consonants. The term follows the phones on its own clock, which starts **8 frames ahead** of them. For male voices
  (`sex` 1) a phone's term is held 4 frames longer before a voiceless phone, and 11 frames longer on a stressed
  voiceless plosive (the `extrad` logic); female voices never hold it.

### 5.3 The smoother

```
f0las1 = q14(0x3000, f0in)   + q14(0x3a00, f0las1)     0.75·in + 0.906·f0las1
f0las2 = q14(0x0600, f0las1) + q14(0x3a00, f0las2)     0.094·f0las1 + 0.906·f0las2
f0     = f0las2 >> 3
```

These are two identical one-pole low-passes in series (pole 0.906, time constant ≈ 10 frames = 64 ms). The first
has a DC gain of 8, undone by the `>> 3`. A step reaches 63 % in about 140 ms. A 16-frame impulse comes out at about
half its height: 281 (+56.2 Hz) peaks near +29 Hz and 71 (+14.2) near +7 Hz, about 120 ms after it is read
([02](pitch_graphs/02_smoother_response.png)). The smoother makes the steps and bumps into the smooth hat. It runs
in frames, so its shape does not change with the speaking rate while the durations do: at fast rates the stress
peaks get flatter.

### 5.4 The glottal-stop dip

After the smoother, `f0 += |d|·100 − 600` whenever `d = nframg − tglstp` is within ±5 frames. That is a V-shaped dip
of up to **−60 Hz** (at the centre frame), 11 frames wide. The centre `tglstp` is placed at the boundary into the
next phone when one of these holds:
- the next phone is a stressed vowel that begins a word (not a middle or last syllable), the current phone ends a
  word or more, the current phone is not a plosive, and the next phone is not `yu`;
- either phone is `tx` or `q` (glottalized t, glottal stop).

This is the **glottal attack** on vowel-initial stressed words (*the* **o**ld, *the* **a**ir;
[01](pitch_graphs/01_hat_pattern_anatomy.png), [04](pitch_graphs/04_clauses_and_reset.png)). Because it is added
after the smoother, it is sharp. It is added before the voice scaling, so it scales with `pr`.

### 5.5 Clamp, voice scaling, T0

```
f0         = clamp(f0, 500, 5121)                                        50.0 … 512.1 Hz
f0_tenths  = q12(f0 − 1200, pr·41) + ap·10     (f0mode 0 only)          ≈ (F0 − 120 Hz)·pr/100 + ap
f0_tenths  = clamp(f0_tenths, 500, 5121)
T0         = 400000 / f0_tenths                                          → word 1 of the 0x4000 frame
```

The rules compute a contour around a fixed **120 Hz reference**. `pr` (pitch range, %) scales its excursions around
120 Hz, *baseline included*, and `ap` (average pitch) moves 120 Hz to the voice's pitch. `pr·41/4096` is `pr/100`
to within 0.1 %. At `pr 0` the voice is a monotone at `ap`. At `pr 200` Paul's low points hit the 50 Hz floor
([06](pitch_graphs/06_pitch_range_assertiveness.png)). Scaling is linear in Hz, not in semitones, so a high voice
with a large `pr` (Kit: 306 Hz, 180 %) swings far more than Harry (78 Hz, 50 %)
([05](pitch_graphs/05_voices.png)).

### 5.6 Spectral tilt from F0 (`ft`)

`pht0draw` also sets the tilt word of the frame from the *unscaled* rule F0:
`TLT = max(0, q12((1200 − f0) >> 3, ft·41)) + sm·36/100`, plus 18 on obstruents, capped at 28 (dB). Below the 120 Hz
reference the voice gets more tilt (a darker, softer source). Above it the F0 term is 0. `ft` is the voice's
"F0-dependent spectral tilt" (%).

## 6. Sung notes and F0 targets (f0mode 1)

A `<duration,f0>` value on any phone of a work item (`[ah<500,13>]`, `[_<,120>]`) switches the whole clause to
**f0mode 1** (`parse_phoneme_param_stream`). Then:
- `phtiming` writes no hat commands. Each phone with an F0 value gets the command `2000 + value` at its start
  (and `Too many sung notes` if the per-phone list collides with the commands).
- `pht0draw` reads commands without the 4-frame lead and uses no baseline, hat, impulses, segmental term,
  smoother, glottal dip or voice scaling. The value goes to the DSP as it is (`f0_tenths = f0out`).
- **Notes 1-37**: `notetab` (ROM `0x16172`, identical to dapi) runs from **C2 = 64.0 Hz** to C5 = 512.0 Hz in
  semitones (13 = C3 128.0, 25 = C4 256.0). F0 moves toward the note by a fixed step of 1/16 of the interval (±1)
  per frame, a linear glide of about 16 frames (100 ms). Vibrato is on: every frame `vibcum` changes by −6, 0, +6
  or 0 tenths, the step changing every 6 frames. That is a trapezoid of **±1.8 Hz with a 24-frame period
  (154 ms, 6.5 Hz)**. A note above 37 logs `Sung note %d > %d` and `If intended to be an F0 target, < 50`, and
  sings 37.
- **Targets 50-511**: F0 in Hz, reached **linearly over the current phone's duration**, without vibrato. A value of
  512 or more logs `F0 > 512` and is ignored.
- A phone without a value keeps the current F0.

See [08](pitch_graphs/08_singing_and_targets.png).

## 7. What the voice parameters do to pitch

| `[:dv]` | Used as | Effect |
|---|---|---|
| `ap` average pitch (Hz) | `SP_F0MINIMUM = ap·10` | where the 120 Hz reference lands (§5.5) |
| `pr` pitch range (%) | `SP_F0SCALEFAC = pr·41` | scales every excursion from 120 Hz (§5.5) |
| `as` assertiveness (%) | `assertiveness = as·41` | scales the hat fall and the final lowering (§4.2); nothing earlier in the sentence |
| `bf`, `ef` (Hz) | `f0basefall = bf·10`, `ef·10` | baseline start and floor (§5.2) |
| `ft` (%) | `f0_dep_tilt = ft·41` | tilt below 120 Hz (§5.6) |
| `la` laryngealization (%) | speaker packet word 8 | not a 68000 pitch rule: the DSP skews alternate pitch periods (REFERENCE §16.9) |

The built-in voices (`voice_defs`, ROM `0x15fbe` on):

| Voice | ap | pr | as | bf | ef | ft | la |
|---|---|---|---|---|---|---|---|
| Paul `:np` | 120 | 100 | 100 | **115** | **100** | 35 | 0 |
| Betty `:nb` | 222 | 160 | 50 | 107 | 107 | 50 | 0 |
| Harry `:nh` | 78 | 50 | 100 | 107 | 107 | 20 | 0 |
| Frank `:nf` (`:nd`) | 153 | 90 | 50 | 107 | 107 | 50 | 100 |
| Kit `:nk` | 306 | 180 | 40 | 107 | 107 | 50 | 0 |
| Ursula `:nu` | 264 | 135 | 100 | **50** | 107 | 50 | 100 |
| Rita `:nr` | 106 | 80 | 50 | 107 | 107 | 0 | 6 |

(Val `:nv` is a copy of Paul until `[:dv save]`.)

## 8. The graphs

All are 150 dpi PNGs in [`pitch_graphs/`](pitch_graphs/). The black line is the F0 where the frame is voiced
(AV > 0), which is what you hear. The pale purple line is the F0 sent to the DSP in every frame, voiceless ones
included. Markers along the top are the F0 commands at the frame they are read, labelled in Hz (steps as sent,
impulses at 2 × the command). Phone labels along the bottom: **bold** = primary stress, black = secondary, grey =
unstressed. A time axis in seconds is frames × 6.4 ms.

| File | Shows |
|---|---|
| [01_hat_pattern_anatomy.png](pitch_graphs/01_hat_pattern_anatomy.png) | the terms of §5.2 one by one, the target `f0in`, the smoothed output, the glottal dip on *old*, the hat fall on *rock-*, the final lowering on *-er*, and the reset |
| [02_smoother_response.png](pitch_graphs/02_smoother_response.png) | the smoother's response to a hat step and to a first and fourth stress impulse (§5.3) |
| [03_sentence_types.png](pitch_graphs/03_sentence_types.png) | *You are going home* with `.`, `?` and `!` (§4.2) |
| [04_clauses_and_reset.png](pitch_graphs/04_clauses_and_reset.png) | two sentences, one with a comma clause: a hat per clause, the continuation rise, the baseline that is reset only by the period, and the next sentence starting lower |
| [05_voices.png](pitch_graphs/05_voices.png) | one sentence in Paul, Harry, Betty and Kit (log scale) |
| [06_pitch_range_assertiveness.png](pitch_graphs/06_pitch_range_assertiveness.png) | `pr` 0/50/100/200 and `as` 0/50/100 on Paul |
| [07_emphasis.png](pitch_graphs/07_emphasis.png) | a plain sentence against the same sentence with `"` on *she*: the emphatic impulse and the halved others |
| [08_singing_and_targets.png](pitch_graphs/08_singing_and_targets.png) | f0mode 1: four sung notes with glides and vibrato, then two Hz targets |

## 9. v1.8 behaviour worth knowing

- **A yes/no question does not end high.** The hat still falls on the last stress (32 Hz instead of 51.2). The
  +50.2 Hz impulse that follows goes through the smoother and comes out as a bump of about 20 Hz, and the pitch
  returns to the baseline ([03](pitch_graphs/03_sentence_types.png)). If the last word ends in an unstressed
  syllable, that syllable gets a 251 impulse of its own as well.
- **Only Paul declines.** The other voices have `bf = ef`, so they have no baseline fall at all. Their sentences
  still fall at the end, through the hat fall and final lowering.
- **The baseline is sentence-wide, not clause-wide.** It is reset by the command 0 at the pause after `.` `?` `!`.
  It is not reset at a comma, and not at a new clause that follows a comma.
- **The reset comes at the start of the final pause**, so the next sentence starts below `bf` (the greeting leaves
  Paul at about 107 Hz).
- **The smoother and impulses are fixed in frames**, while durations scale with `:ra`. At fast rates the stress
  peaks are lower and the hat looks flatter.
- **Glottal dips reach −60 Hz** at stressed vowel-initial words, even in the middle of a phrase. At a large `pr` they
  can hit the 50 Hz floor (Paul at `pr 200`).
- **`as 0` removes the hat fall and final lowering entirely**: `q12(−171, 0) | 1` is +1, a +0.2 Hz "lowering".
- The unreachable `hatfall = 0x70` branch (§4.2) and `endtyp[aw]` = 5 instead of dapi's 3 (REFERENCE §15.14) are
  ROM facts the C reproduces.
- The hat-pattern constants differ from MITalk's (REFERENCE §14.4, background only): there is no 190 Hz first
  peak, no 110/125 Hz declination lines and no 75 Hz statement end. DECtalk's numbers are the ones above.

## 10. Where the numbers came from

- The code: `src/speech/ph_alloph.c`, `ph_timing.c` (`phtiming`, `make_f0_command`), `ph_frame.c` (`pht0draw`,
  `setspdef`), `ph_clause.c` (`parse_phoneme_param_stream`: f0mode), `ph_rom.c` (the tables), all checked against
  the ROM by `decomp/scripts/check_frames.py` (REFERENCE §15.18-15.21).
- The graphs come from **`pitch_graphs/pitchlog.c`**, a small logger on the speech engine (`engine.c` with a frame
  hook). It records the `pht0draw` globals (`f0baseline`, `tarhat`, `tarimp`, `f0seg`, `f0in`, `f0`, `f0_tenths`),
  each command as it is read, each phone and each new clause; its header describes the output. To regenerate them
  (Windows, MSVC; Python 3 with matplotlib):

  ```bat
  pitch_graphs\build_pitchlog.bat
  python pitch_graphs\make_pitch_graphs.py
  python pitch_graphs\make_pitch_graphs.py --check
  ```

  The build writes `build\pitchlog\pitchlog.exe` (set `PITCHLOG` to use another binary). The first Python command
  rewrites the eight PNGs. `--check` compares pitchlog's T0 with the ROM's frame logs in `decomp/reference/`, which
  must be present locally: it reports 9,015 frames, all matching.
- Background: Klatt 1982 (`docs/klatt1982.pdf`) for the hat pattern, REFERENCE §14.10 for the lineage and §15.14
  for the first reading of `pht0draw`.
