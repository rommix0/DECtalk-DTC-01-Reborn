# FORMANT_SYSTEM.md: how the DTC-01 v1.8 moves its formants

This document describes how DECtalk I firmware v1.8 turns a clause's allophones into the formant, bandwidth and
amplitude tracks sent to the DSP every 6.4 ms, and how neighbouring phones shape each other (coarticulation). It
follows the C rebuild, which is checked word for word against the ROM (REFERENCE §15.17-15.18). Its companion for
pitch is [PITCH_SYSTEM.md](PITCH_SYSTEM.md). The forward and backward transitions of §4.4-4.6 are described in full,
frame by frame and rule by rule, in [SMOOTHING_SYSTEM.md](SMOOTHING_SYSTEM.md).

| Stage | C (`src/speech/`) | ROM | dapi counterpart |
|---|---|---|---|
| per phone: targets, coarticulation, transitions, loci, bursts, aspiration | `phsettar`, `ph_frame.c` | `0xaa08` | `ph_setar.c` |
| locus rule | `setloc`, `ph_frame.c` | `0xc420` | `ph_setar.c` `setloc` |
| diphthong time points | `diph_time_scale`, `ph_frame.c` | `0xc570` | (later rewritten) |
| per frame: draw the tracks | `phdraw`, `ph_frame.c` | `0xa804` | `ph_draw.c` (DK 1984) |
| frame loop | `phclause_draw_frames`, `ph_frame.c` | `0xa782` | `ph_claus.c` |
| voice → F2/F3 limits, F4/F5, head size | `set_formant_limits`, `setspdef`, `ph_frame.c` | `0x7d42`, `0x81e6` | `ph_vset.c` |
| the resonators | `dsp_synth.c` | DSP ROM | `klsyn/parwav.c` |
| tables | `ph_rom.c`: `maltar`/`femtar`, `maldip`/`femdip`, `maleloc`/`femloc`, `plocu`, `malamp`/`femamp`, `burst_amp_set`, `featb`, `begtyp`, `endtyp`, `partyp`, `divtab`, … | `0x161fe`-`0x18226` | `p_us_rom.c`, `ph_romi.c` (values differ) |

Confidence tags as in AGENTS.md: **[V]** read from the checked C / ROM, **[I]** an interpretation. Everything below
is [V] unless tagged.

The graphs in [`formants_graphs/`](formants_graphs/) were drawn from the same C code, run after the power-up
greeting so that its state matches the ROM's. All 17 parameter words of every frame it produced (T0, F1 … AB, TLT)
are identical to the emulator's ROM frame logs (`decomp/reference/*.frames.tsv`) for all 18 plain-text corpus entries:
9,015 frames, with no difference. So the curves are what the real unit sends to its DSP.

---

## 1. The picture in one paragraph

DECtalk draws its formants the way Klatt's synthesis-by-rule programs of 1979-86 do (MITalk Ch. 11 and App. C,
REFERENCE §14): **each phone has a target for every track, and the track moves between targets in straight lines.**
At each phone boundary `phsettar` looks up the phone's targets in a table (one table for male voices, one for
female), moves F1-F3 **15 % toward the neighbouring phones** (coarticulation), and plans two transitions: a
**forward** one that starts at a boundary value and fades into the target over the first frames of the phone, and a
**backward** one that leaves the target over the last frames toward the next boundary value. The boundary value is
normally the midpoint of the two targets. Rules change it and the transition lengths by phone class: glides and
liquids pull it toward themselves, stops and nasals move their formants through their whole length, and at a
**consonant-vowel boundary** it comes from the consonant's **locus**: `locus + pct × (vowel target − locus)`, with
one locus set per consonant and vowel class (front, back unrounded, rounded). Vowels have no single target: each is
a short list of **straight lines** between break points, rescaled to the vowel's duration. Every 6.4 ms `phdraw`
adds up target, diphthong line and the two transitions for each of the 15 tracks, and the frame goes to the DSP, which
applies the head size and turns the values into resonator coefficients.

```
 klsyn task, one clause at a time                                                            DSP
 ──────────────────────────────────────────────────────────────────────────────────────────  ──────────────
 phalloph ─ allophons[], allofeats[] (stress, boundaries)
 phtiming ─ allodurs[] (frames)
    │
    ▼  per 6.4 ms frame (phclause_draw_frames)
 at each phone start: phsettar ─ for each of 15 tracks (F1 F2 F3 FNZ B1 B2 B3 AV AH A2-A6 AB):
                                   target  (maltar/femtar; -1 = a neighbour's; < -1 = diphthong lines in maldip)
                                   F1-F3: 0.85·own + 0.15·neighbours
                                   forward transition:  boundary value, length (midpoint, class rules, setloc locus)
                                   backward transition: boundary value, length
                                   bursts, aspiration (tspesh/pspesh), B1/B2 widening
 every frame:         phdraw   ─ value = tarcur + dipcum/8 + ftran/8 [+ btran/8]   (special value if one is due)
                                   F2 ≤ f2max, F3 ≤ f3max                                       → words 2-16 of
 (pht0draw: T0 and TLT, PITCH_SYSTEM.md)                                                            the 0x4000 frame
                                                                                                 → head size, resonators
```

## 2. Units and the tracks

| Quantity | Unit | Where |
|---|---|---|
| time | frames of **6.4 ms** (64 samples at 10 kHz) | `durfon`, `tcum`, every duration below |
| F1 F2 F3 FNZ | Hz | frame words 2-5 |
| B1 B2 B3 | Hz as the tables give them; the DSP realizes about 0.78 × the value (§8) | frame words 6-8 |
| AV AH A2-A6 AB | dB (0 = off) | frame words 9-16 |
| slopes and transitions | Hz or dB **× 8** (`ftran`, `btran`, `dipcum`, `deldip`), so a slope keeps 3 fraction bits | `ph_param_t` |
| fractions | Q14 (`q14(x, y) = x·y >> 14`), e.g. 0.85 = `0x3667`, 0.15 = `0x999` | `ph_math.h` |

There are **15 tracks** (`ph_params[]`, ROM `0x8197e`, dapi's `PARAMETER`), in frame order. `partyp` (`0x16d8c`)
gives each a type, and the type decides where its target comes from and which transition rules apply:

| Type | Tracks | Target from |
|---|---|---|
| 3 | F1 F2 F3 | `p_tar` rows 0-2, or diphthong lines; coarticulated |
| 1 | FNZ | 300 Hz, 527 Hz in a nasal |
| 4 | B1 B2 B3 | `p_tar` rows 3-5, or diphthong lines |
| 0 | AV | `p_tar` row 6; AH: 58 dB for `hx`, else 0 |
| 2 | A2 A3 A4 A5 A6 AB | `p_amp`, a burst/frication amplitude set per phone (`burst_amp_set`) |

TLT and T0 are not tracks: `pht0draw` makes them ([PITCH_SYSTEM.md](PITCH_SYSTEM.md) §5.5-5.6). F4, F5 and their
bandwidths are not tracks either: they are per voice, in the speaker packet (§9).

Each track keeps (`ph_frame.h`): `tarlas`, `tarcur`, `tarend`, `tarnex` (the last phone's end target, this phone's
start and end targets, the next phone's target); `ftran`/`dftran` (forward transition and its step); `btran`/
`dbtran`/`tbacktr` (backward transition, its step and its first frame); `durlin`/`deldip`/`dipcum`/`ndip` (the
diphthong line being drawn); `tspesh`/`pspesh` (a special value for the first `tspesh` frames).

## 3. The tables

### 3.1 Targets: `maltar` / `femtar`

Seven rows (F1 F2 F3 B1 B2 B3 AV) of 56 phones. `phsettar` picks the male or female set by the voice's `sex`
(`malfem`), together with the diphthong, locus and amplitude tables (REFERENCE §15.17). A value of

- **≥ 0** is the target;
- **−1** means "no target of my own": take the next phone's (if that is −1 too, the one after; then the last
  phone's end). `hx` has −1 for F1-F3 and `q` (glottal stop) for every row, so **/h/ and the glottal stop take the
  formants of the vowel that follows**. Silence `_` has −1 for F1-F3 too;
- **< −1** is an index into `maldip` / `femdip`: a list of straight lines (§3.2).

Every vowel (codes 1-23) has lines for F1, F2 and F3; there are no single-target vowels. Some vowels have lines for a
bandwidth as well: `ae` B2, `ay` B1, `aw` B1, `oy` B2, `yu` B2 and B3.

Male consonant targets (Hz; AV in dB):

| | F1 | F2 | F3 | B1 | B2 | B3 | AV | | | F1 | F2 | F3 | B1 | B2 | B3 | AV |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `w` | 295 | 610 | 2250 | 50 | 80 | 60 | 61 | | `s` | 320 | 1420 | 2550 | 300 | 80 | 200 | 0 |
| `y` | 260 | 2070 | 2570 | 50 | 200 | 300 | 61 | | `z` | 240 | 1420 | 2600 | 140 | 120 | 200 | 47 |
| `r` | 360 | 1060 | 1380 | 70 | 80 | 100 | 64 | | `sh` | 300 | 1650 | 2550 | 300 | 100 | 300 | 0 |
| `l` | 340 | 800 | 2950 | 60 | 130 | 120 | 61 | | `zh` | 250 | 1650 | 2550 | 120 | 120 | 300 | 47 |
| `rx` | 470 | 1270 | 1540 | 80 | 80 | 130 | 62 | | `p` | 350 | 1100 | 2150 | 300 | 180 | 180 | 0 |
| `lx` | 450 | 760 | 2990 | 60 | 90 | 160 | 61 | | `b` | 210 | 1100 | 2150 | 150 | 80 | 130 | 40 |
| `el` | 450 | 760 | 2850 | 50 | 90 | 200 | 61 | | `t` | 350 | 1600 | 2600 | 300 | 150 | 250 | 0 |
| `m` `em` | 420 | 1270 | 2130 | 90 | 240 | 120 | 57 | | `d` | 210 | 1600 | 2600 | 150 | 100 | 240 | 40 |
| `n` `en` | 420 | 1340 | 2500 | 90 | 300 | 160 | 57 | | `k` | 350 | 1790 | 2250 | 300 | 160 | 280 | 0 |
| `nx` | 420 | 1600 | 2000 | 90 | 250 | 200 | 57 | | `g` | 210 | 1790 | 2520 | 150 | 120 | 180 | 40 |
| `f` | 340 | 1100 | 2080 | 300 | 120 | 150 | 0 | | `dx` | 350 | 1600 | 2600 | 110 | 100 | 170 | 34 |
| `v` | 220 | 1100 | 2080 | 140 | 120 | 120 | 47 | | `tx` | 210 | 1600 | 2600 | 110 | 100 | 170 | 0 |
| `th` | 320 | 1300 | 2520 | 300 | 90 | 150 | 0 | | `ch` | 350 | 1750 | 2700 | 300 | 150 | 250 | 0 |
| `dh` | 270 | 1270 | 2560 | 120 | 120 | 170 | 47 | | `jh` | 260 | 1730 | 2700 | 50 | 150 | 250 | 40 |
| `hx` | −1 | −1 | −1 | 400 | 300 | 300 | 0 | | `q` | −1 | −1 | −1 | −1 | −1 | −1 | 60 |

The female set is the same kind of table with higher formants (e.g. `iy` F2 2850 instead of 2100, `aa` F1 970
instead of 740) and some different bandwidths; see `femtar` in `ph_rom.c`.

### 3.2 Vowel lines: `maldip` / `femdip`

A vowel's entry is `v0, t1, v1, t2, v2, …, vn, −1`: **v0 is held until frame t1, then the track moves in a straight
line to v1 at t2, to v2 at t3, and so on; the last value is reached at the phone's end**. The times are frames of the
vowel's inherent duration `inhdr` and are rescaled to its actual duration (§4.3). The male lines, written as
`frame:Hz` corners (the line starts at frame 0 with the first value):

| Vowel | inhdr | F1 | F2 | F3 |
|---|---|---|---|---|
| `iy` | 25 | 2:365 end:340 | 2:2100 end:2190 | 2:2800 end:2920 |
| `ih` | 24 | 10:460 end:530 | 10:1750 end:1550 | 10:2520 end:2520 |
| `ey` | 30 | 5:580 27:440 end:440 | 5:1640 27:1980 end:1980 | 5:2520 27:2520 end:2520 |
| `eh` | 24 | 14:580 22:650 end:650 | 13:1680 22:1480 end:1480 | 14:2500 22:2530 end:2530 |
| `ae` | 36 | 20:690 33:750 end:750 | 20:1630 33:1490 end:1490 | 20:2430 33:2470 end:2470 |
| `aa` | 38 | 6:740 31:740 end:740 | 6:1200 31:1200 end:1200 | 6:2600 31:2600 end:2600 |
| `ay` | 39 | 6:650 19:680 31:530 end:530 | 14:1200 31:1850 end:1850 | 24:2580 31:2350 38:2450 end:2450 |
| `aw` | 41 | 19:660 35:550 end:550 | 19:1200 35:900 end:900 | 19:2550 35:2500 end:2500 |
| `ah` | 22 | 6:620 16:620 end:620 | 6:1170 16:1170 end:1170 | 6:2550 16:2550 end:2550 |
| `ao` | 38 | 19:600 35:660 end:660 | 19:990 35:1040 end:1040 | 6:2580 31:2580 end:2580 |
| `ow` | 35 | 6:540 27:490 end:490 | 6:1000 27:880 end:880 | 6:2370 27:2250 end:2250 |
| `oy` | 44 | 2:540 14:580 35:490 end:490 | 8:900 14:920 35:1860 end:1860 | 22:2400 31:2250 44:2400 end:2400 |
| `uh` | 25 | 8:480 20:530 end:530 | 8:1100 20:1110 end:1110 | 8:2320 20:2400 end:2400 |
| `uw` | 33 | 6:350 28:340 end:340 | 6:1150 28:840 end:840 | 6:2200 28:2200 end:2200 |
| `rr` | 28 | 5:470 27:440 end:440 | 5:1250 27:1250 end:1250 | 5:1440 27:1440 end:1440 |
| `yu` | 36 | 11:320 24:350 end:350 | 13:1900 24:1050 end:1000 | 11:2600 17:2200 end:2200 |
| `ax` | 19 | 5:550 14:550 end:550 | 5:1260 14:1260 end:1260 | 5:2600 14:2600 end:2600 |
| `ix` | 19 | 5:460 14:460 end:460 | 5:1680 14:1680 end:1680 | 5:2470 14:2470 end:2470 |
| `ir` | 36 | 16:350 27:420 end:420 | 16:1850 27:1540 end:1510 | 11:2700 25:1830 end:1800 |
| `er` | 42 | 6:490 36:490 end:490 | 17:1650 30:1500 end:1500 | 16:2500 30:1790 end:1750 |
| `ar` | 41 | 16:660 28:570 end:570 | 16:1210 28:1280 end:1280 | 14:2300 28:1720 end:1680 |
| `or` | 41 | 14:510 28:490 end:490 | 16:870 30:1200 end:1200 | 14:2150 28:1530 end:1500 |
| `ur` | 36 | 13:420 27:440 end:440 | 13:900 27:1150 end:1150 | 13:2000 28:1540 end:1500 |

So the "monophthongs" move too, if only a little (`iy` F2 2100 → 2190, `ih` F2 1750 → 1550, `uw` F2 1150 → 840),
and the r-coloured vowels are diphthongs toward an `r` (F3 falling to about 1500-1800 Hz).

### 3.3 Phone classes: `featb`, `begtyp`, `endtyp`

The transition rules test these `featb` bits (dapi names; `ph_timing.c`): `0x02` voiced, `0x10` sonorant (vowels,
glides, liquids, nasals, `hx`, and also silence `_`), `0x20` obstruent, `0x40` plosive, `0x80` nasal, `0x200`
**sonorant consonant** (`w y r l rx lx`), `0x800` (`sh zh ch jh`, used by `setloc`).

`begtyp` / `endtyp` (`0x16aa4` / `0x16b14`) give the class of a phone's start and end for the locus rule:

| Class | Phones (begtyp) |
|---|---|
| 1 front | `iy ih ey eh ae yu ix ir er y` |
| 2 back unrounded | `aa ay aw ah ax ar` |
| 3 rounded | `ao ow oy uh uw rr or ur rx` |
| 4 consonant | silence, `hx`, nasals, obstruents |
| 5 sonorant consonant | `w r l lx el` |

`endtyp` differs from `begtyp` where a vowel ends in another class: `ay` and `oy` end front (1), `yu ir er ar` end
rounded (3), and `ow`, `uw` and `aw` end in class 5. dapi has 3 for `aw` (REFERENCE §15.14).

### 3.4 Loci: `plocu`, `maleloc` / `femloc`

`plocu[class − 1][consonant]` (class 1-3; 5 counts as 3) picks one of 50 locus sets, and each set holds
`{locus, pct, durtran}` for F1, F2 and F3. Male sets for the stops (locus Hz, pct, transition frames):

| | before front (1) | before back unrounded (2) | before rounded (3) |
|---|---|---|---|
| `p` | 360 .50 3 / 950 .66 5 / 2300 .15 7 | 360 .50 4 / 1020 .66 6 / 2250 .58 8 | same as 2 |
| `b` | 340 .50 3 / 900 .66 5 / 2300 .15 7 | 340 .50 4 / 1020 .66 6 / 2250 .58 8 | same as 2 |
| `t` | 350 .43 6 / 1700 .75 6 / 2600 .30 7 | 350 .43 7 / 1500 .00 12 / 2600 .00 8 | 350 .43 6 / 1500 .00 15 / 2300 .00 15 |
| `d` | 250 .43 6 / 1700 .75 6 / 2550 .30 7 | 250 .43 7 / 1500 .00 12 / 2600 .00 8 | 250 .43 6 / 1500 .00 15 / 2300 .00 15 |
| `k` | 350 .33 7 / 1990 .20 8 / 3000 1.17 8 | 350 .33 8 / 1700 .00 10 / 2150 .00 14 | 350 .33 6 / 1700 .42 10 / 1920 .15 13 |
| `g` | 250 .33 7 / 1990 .20 8 / 3000 1.13 8 | 300 .33 8 / 1680 .00 10 / 2150 .00 14 | 290 .45 6 / 1700 .42 10 / 1920 .15 13 |

Three kinds of entry are worth knowing:
- **pct 0**: the boundary value is the locus itself, whatever the vowel (`t d` before back vowels, `k g` before back
  unrounded ones).
- **pct > 1**: the boundary overshoots the vowel target away from the locus (`k g` F3 before front vowels: with
  locus 3000 and pct 1.13, a vowel F3 of 2700 gives a boundary of 2661).
- **locus 1**: a proportional rule, boundary = pct × target (with 1 Hz of offset): labiodentals `f v` have F2 at 0.85-0.91 of the
  vowel's, `m` has F2 (and before back vowels F3) at 0.85-0.97 of it.

The fricatives, affricates and nasals have their own sets; `dx` and `tx` use `t`'s. Of codes 31-55 only `el` and `q`
have none, and neither do glides, liquids, `hx` or vowels.

### 3.5 Amplitudes: `malamp` / `femamp`

`burst_amp_set[phone]` (1-16; 0 = none) picks a set for the fricatives `f v th dh s z sh zh`, the stops and the
affricates. Each set has four rows (the class of the *following* phone, 1-4; 5 uses 3) of six values, A2 A3 A4 A5 A6
AB in dB. For a fricative they are the frication spectrum, sent for the whole phone. For a stop they are the burst,
sent only in its last frames (§4.8). Examples (male, before a front vowel): `f` AB 47; `s` A6 55; `sh` A3 50,
A4 67, A5 59, A6 50; `p` AB 60; `t` A3 22, A4 55, A5 61, A6 64; `k` A3 48, A4 59, A5 52, A6 44. `dx` and `q` have no
set; `tx` uses `d`'s.

## 4. Stage 1: `phsettar`, once per phone

### 4.1 Context and the duration ratio

`phsettar` reads the phone (`phcur`), the one before (`pholas`) and the two after (`phonex`, `phonex2`), their
`featb`, `begtyp`/`endtyp`, the duration `durfon` and the stress. For a phone that is not an obstruent it computes

```
dur_ratio   = durfon / inhdr[phcur]        (Q14, capped just under 2.0)
ftran_scale = 0.5  + dur_ratio / 2         (forward transitions)
btran_scale = 0.36 + dur_ratio / 2         (backward transitions)
```

An obstruent keeps the previous phone's values; they are only applied to non-obstruents anyway.

### 4.2 Targets and coarticulation

For each track, `phsettar` finds the **next phone's target** first (`tarnex`; for a diphthong, its first value),
then **this phone's**. For F1-F3 (and only these) the target is then coarticulated with Q14 `0.85·own + 0.15·other`
(dapi `N85PRCNT`/`N15PRCNT`):

| Target | "other" |
|---|---|
| a single target (consonants) | the mean of the next phone's target and the last phone's end target (or only the last, when the next has none) |
| a vowel's first line value | the last phone's end target |
| each later line value | the next phone's target |
| `tarnex` (used by the backward transition) | this phone's end target |

So a vowel starts a little toward the phone before it and ends a little toward the phone after it, and a consonant
sits a little toward both. In [02](formants_graphs/02_track_anatomy.png): `d`'s F2 1600 becomes
0.85·1600 + 0.15·(1310 + 1200)/2 = 1548; `ay`'s first value 1200 becomes 0.85·1200 + 0.15·1548 = 1252.

### 4.3 Diphthong lines

A target < −1 is expanded into `dipspec_buf` as `{end frame, slope × 8}` pairs, the slope taken from `divtab`
(16384/n) or by a long division for lines of 50 frames or more. Each time point `t` (in inherent-duration frames) is
first rescaled by `diph_time_scale`, with `r = dur_ratio`, `d = inhdr`, and `t` measured from the nearer end:

```
t < d/4 (an outer quarter):   t1 = t·b,              b = 3/4 + r/4        (r > 1)   or  2r − 1         (r ≤ 1)
otherwise (the middle half):  t1 = d/2 − (d/2 − t)·b, b = 3/2 − r/2      (r > 1)   or  3 − 2·max(r, 0.5)
then                          t' = t1·r + 1            (t measured back from the start)
```

Lengthening a vowel therefore stretches its outer quarters more than in proportion and draws the middle points toward
its centre; shortening shrinks the outer quarters more than in proportion and pushes the middle points outward. The
last point is the phone's end (`−1` → `durfon`). Two rescaled points can come out one frame backwards; the slope is
then `divtab[−1]`, the ROM word before the table (REFERENCE §15.31), which the C keeps.

### 4.4 The forward transition (into the phone)

The forward transition is described by a **boundary value** `bouval` and a length `durtran` in frames. `phdraw`
starts the phone at `bouval` and moves linearly to the target over `durtran` frames (`ftran = 8·(bouval − tarcur)`,
decreased by `dftran = ftran/durtran` each frame). For F1-F3 the rules are applied in this order; a later rule
overrides an earlier one:

| Case | bouval | durtran |
|---|---|---|
| default | midpoint of `tarlas` and `tarcur` | 5 |
| a sonorant that is not `w y r l` (vowels, `hx`) | | 8 |
| … after `w y r l` | ¾ of the way toward the consonant | 9; F1 after `l`: bouval + 80 Hz |
| `w y r l` after a non-`0x200` phone | ¾ of the way toward itself | 3 |
| **silence** | `tarlas`: the pause glides from the last value toward the next phone's targets | `durfon` (capped at 20 below) |
| **consonant-vowel boundary** (`setloc`, §4.5) | locus + pct·(target − locus) | the locus set's |
| an obstruent | | 5; a **plosive**: `durfon` |
| a **nasal** | | `durfon`; F1: **0** (it jumps) |
| F1 after a nasal | ≥ 425 Hz | 16 |
| then, for a non-obstruent | | `durtran·ftran_scale + 1` |
| always | ≥ 0 | ≤ `durfon`, ≤ 20 |

### 4.5 The locus rule (`setloc`)

`setloc(consonant, its class, the other phone's class, track, target)` applies only to F1-F3, only when the consonant's
class is 4 and the other phone's is not 4 (a vowel, glide or liquid), and only when the consonant has a locus set for
that class. Then

```
bouval  = locus + pct × (target − locus)          target = the vowel's coarticulated target at the boundary
durtran = the set's length
```

When the other phone is class 5 (`w r l lx el`, and the end of `ow uw aw`), F2 and F3 use `pct/2 + 0.5`, halving the
consonant's pull, unless the consonant has `featb 0x800` (`sh zh ch jh`). It is called twice at each boundary, once
for each side, so it works for CV and for VC: a vowel's backward transition into a stop uses the stop's locus the same
way. This is Klatt's "modified locus theory" (MITalk §11, App. C `Bper`, "the percent of movement from locus toward
target in a CV or VC transition"; REFERENCE §14). MITalk computes the velar locus from the vowel's F1 and F2; DECtalk
has a table per vowel class instead. See [04](formants_graphs/04_consonant_loci.png): before `aa`, `d` starts F2 at
1500 and `g` at 1680 whatever the vowel; before `iy`, `g`'s F2 and F3 start 700 Hz apart and close together (the
velar pinch).

### 4.6 The backward transition (out of the phone)

The same kind of rules give the value at the end of the phone and the number of frames before the end over which the
track leaves the target (`tbacktr = durfon − durtran`; `btran` grows by `dbtran` each frame):

| Case | bouval | durtran |
|---|---|---|
| default | midpoint of `tarend` and `tarnex` | 4 |
| a vowel | | 8; before `w y r l`: ¾ toward the consonant, 9; F1 before `l` + 80 |
| `w y r l` before a non-`0x200` phone | ¾ toward itself | 3 |
| before silence | (no transition) | 0 |
| locus (`setloc`, both sides) | locus + pct·(target − locus) | the set's |
| an obstruent / a plosive / a nasal (F1: 0) | as in §4.4 | 5 / `durfon` / `durfon` |
| F1 before a nasal | ≥ 480 Hz | 20 |
| then, for a non-obstruent | | `durtran·btran_scale + 1` |

Because both sides of a boundary compute the same midpoint or locus value, the track is continuous at nearly every
boundary: in five test sentences (357 formant boundaries) the two boundary values differ by 5 Hz or less except
where a rule makes them differ on purpose (F1 into and out of nasals, F1 after `l`, and a few cluster cases of 6-80 Hz).
A **stop** has both transitions over its whole length, so its closure runs in a straight line from one boundary
value to the next ([02](formants_graphs/02_track_anatomy.png): `d` holds 1500 Hz, its locus before `ay`).

### 4.7 Bandwidths and the nasal zero

**B1-B3** (type 4) use the midpoint with 6 frames (3 when the phone is voiceless). Special cases:
- B1 of a voiced phone after a voiceless one: +20 Hz, 8 frames.
- Next to silence the boundary is the target + 100 (B1), +50 (B2), +0 (B3), 8 frames.
- B1 next to a nasal: +70 Hz, 16 frames on both sides.
- During aspiration (§4.8) B1 is target + 250 and B2 target + 80.

**FNZ** (type 1) is 300 Hz, the frequency of the DSP's fixed nasal pole (298 Hz, bandwidth ≈ 63 Hz, computed from its
reset coefficients), so pole and zero cancel outside nasals. In a nasal it is 527 Hz. It jumps at a nasal's edges,
except that before a nasal it rises to 350 Hz over the last 20 frames, and after one it starts at 350 Hz and returns
over 16 ([06](formants_graphs/06_nasals.png)).

### 4.8 Amplitudes, bursts and aspiration

AV, AH and A2-AB (types 0 and 2) use the same two transitions with rules of their own:
- **Onsets** start at most 12 dB below the target. AV then takes 2 frames; after silence, on a stressed phone or
  `w y r l`, 7 frames from 20 dB below; after an obstruent it starts 6 dB below.
- **Offsets** fall from 15 dB below the last phone's value (an amplitude fading into this phone over 5 frames, 11 in a
  silence). AV has no fade into a new phone: it drops at once.
- **Before a stop** amplitudes are cut at once. A voiced stop's own voice bar (AV) falls to 20 dB over its last 7
  frames; a voiceless stop's AV is 0 throughout.
- `ch`/`jh`: A4 rises over the phone from 40 dB below its target.

**Bursts.** For a stop or affricate (codes 45-55) A2-AB are held at 0 (`pspesh`) until `durfon − burst`, where
`burst` is `stop_burst_dur`: `p b` 1, `t d` 2, `k g` 3, `ch` 13, `jh` 6, `dx tx q` 0. A burst longer than 1 frame
loses one when fewer than 8 closure frames would be left. A plosive's burst is 1 frame before an obstruent, and none
before a nasal or plosive made at the same place or further forward (`burst_place` ranks labial 1, alveolar 3,
velar 5; [I] that it is place of articulation). The burst frames carry the `malamp` values for the next phone's
class; the second one is 10 dB weaker (`phdraw`), and the vowel's onset rule then fades them out from 15 dB below
over 5 frames.

**Aspiration (voice onset time).** When a voiceless plosive is followed by a sonorant, the sonorant starts with
`vot` frames of AV = 0 and AH = 58 dB: 6 frames before a stressed vowel; 4, with AH 55 dB, before an unstressed one;
2 after `s` in the same word (`sp st sk`); 3 more when the sonorant is `w y r l` (so the glide itself is devoiced).
A stressed vowel's VOT is at most half its length. Then **AV starts 8 dB low and rises 1 dB per frame** for 8 frames
([07](formants_graphs/07_stops_amplitudes.png)).

## 5. Stage 2: `phdraw`, every frame

For F1-F3 and B1-B3:

```
if tcum > durlin:  next diphthong line (durlin, deldip from ndip); tarcur += dipcum/8; dipcum = 0
dipcum += deldip
value   = tarcur + dipcum/8 + ftran/8;        ftran -= dftran (until 0)
if tcum ≥ tbacktr:  value += btran/8;          btran += dbtran
if tcum < tspesh:   value = pspesh             (aspiration bandwidths)
F2 = min(F2, f2max);  F3 = min(F3, f3max)
```

For FNZ and AV-AB the same without diphthong lines, plus the second-burst and AV-onset rules of §4.8. Every track is
sent in every frame, voiced or not: during a closure or a fricative the formant values still move, and the DSP's
parallel F2/F3 resonators use them for frication (§8).

The **F2/F3 ceilings** are per sex (`set_formant_limits`): male 2750/3050 Hz, female 3050/3350 Hz (dapi: 2500/3500).
If the voice's `f4` is set and below `f3max + 300`, they become `f4 − 600` / `f4 − 300`. Betty's `y` (table F3
3400) is capped at 3350 in "a year" ([08](formants_graphs/08_voices_head_size.png)).

## 6. Worked example: F2 of `[ax d'ay]` (Paul, [02](formants_graphs/02_track_anatomy.png))

| Phone | Frames | Table | Target (coarticulated) | Forward: from, frames | Backward: to, frames |
|---|---|---|---|---|---|
| `ax` | 10-21 | 1260 (flat lines) | 1263 → 1310 | 1273, 7 | 1499, 9 (the `d` locus) |
| `d` | 22-34 | 1600 | 1548 | 1499, 13 (= `durfon`) | 1499, 13 (= `durfon`) |
| `ay` | 35-83 | 1200 to 14, 1850 at 31 (of 39) | 1252 → 1850 | 1498, 14 | (silence next: none) |

- `d` before class-2 `ay`: F2 locus 1500 Hz, pct 0, 12 frames. The boundary is 1500 on both sides, whatever the
  vowels.
- The `d` closure is flat at 1499: its forward transition (−49 Hz, fading) and backward transition (0 → −49 Hz)
  cancel.
- `ay`: 49 frames against an inherent 39, so `ftran_scale` = 0.5 + 1.26/2 = 1.13, and the locus set's 12 frames
  become 12·1.13 + 1 = 14. Its break points 14 and 31 become frames 19 and 39 of the phone. F2 falls from 1498 to
  1252 over 14 frames, holds, rises to 1850 at frame 39 and holds to the end.

## 7. Glides, liquids, /h/ and the glottal stop

- **`w y r l`** are short targets that the vowels glide into and out of: the vowel side of the boundary is ¾ of the
  way toward the consonant and moves over 9 frames (scaled by the vowel's length), the consonant side over 3
  ([05](formants_graphs/05_glides_liquids_h.png)). With only 3-6 frames of its own, a glide's target is often not
  reached (F2 of `w` in `[ax w'aa]`: target 695, lowest value 724).
- **`hx`** has no formant targets: it takes the next phone's (and B1-B3 of 400/300/300 Hz, AV 0, AH 58 dB). Its
  aspiration therefore already has the vowel's formants, and between vowels F1-F3 glide straight through it.
- **`q`** (glottal stop) has −1 in every row, formants and bandwidths: it takes the next phone's values and has AV 60.
  Its pitch dip is `pht0draw`'s (PITCH_SYSTEM.md §5.4).
- **Silence** takes the next phone's formants too (−1), and its forward transition lasts the pause, at most 20 frames,
  so during a pause the (silent) tracks glide from the last phone toward the next one.

## 8. What the DSP does with the tracks

The DSP (`dsp_synth.c`, REFERENCE §16.10) turns the frame into resonator coefficients:
- **Head size.** `FRAME_COEFFS` scales `F1' = 256 + (F1 − 256)·s`, `F2' = 512 + (F2 − 512)·s`, `F3' = F3·s`, with
  `s` = speaker word 18 = `(200 − hs)·41/4096` ≈ (200 − hs)/100. `setspdef` scales F4 and F5 by the same factor on
  the 68000 (and zaps them to 2500 Hz above 4950). So the 68000's frames are the same for every head size, and the
  F2/F3 ceilings of §5 apply before it. hs 80 raises Paul's F3 by 20 %, hs 120 lowers it by 20 %
  ([08](formants_graphs/08_voices_head_size.png)).
- **Update rate.** The cascade's F1-F3 and B1-B3 (and AV and the nasal zero) are loaded **once per glottal period**,
  at its start, not every frame. The parallel F2 and F3 are loaded every frame.
- **Frequency resolution.** Frequencies go through a piecewise cosine table: below 200 Hz the first entry;
  200-400 Hz in 4 Hz steps, 400-800 in 8, 800-1600 in 16, 1600-3200 in 32, above in 64.
- **Bandwidth.** A resonator's coefficients are `C = 2b/4096 − 1` and `B = cos·(1 − b/4096)` with `b` the bandwidth
  word, i.e. a pole radius of about `1 − b/4096`. That is a −3 dB bandwidth of about **0.78·b Hz** at 10 kHz (160 →
  130 Hz, 60 → 47 Hz, 300 → 252 Hz): the words are larger than the bandwidths the DSP realizes by about 1.29 [V:
  computed from `tune` in `dsp_synth.c`; whether the table values were chosen for this is [I]].
- **Where the tracks go.** Voicing (AV) and aspiration (AH) go through the cascade: the nasal zero (FNZ), the fixed
  nasal pole, F5, F4 (the voice's), F3, F2, F1. Frication and bursts (A2-A6, AB) go through the parallel branch: F2 and
  F3 from the frame with fixed bandwidths (210 and 280), the voice's `p4`/`p5`, and a fixed F6, plus the bypass AB.

## 9. What the voice parameters do to formants

| `[:dv]` | Effect |
|---|---|
| `sex` | chooses the male or female target, diphthong, locus and amplitude tables and the F2/F3 ceilings; changing it also moves `hs` by ∓18 and scales F4/F5 (REFERENCE §8.1) |
| `hs` head size (%) | the DSP's scale of F1-F3 (§8) and the 68000's of F4/F5 |
| `f4 b4 f5 b5` | the cascade's fixed F4/F5 (scaled by `hs`); a low `f4` lowers the F2/F3 ceilings, and `f5` is kept ≥ `f4 + 250` |
| `p4 p5` | the parallel branch's F4/F5 (frication) |
| `g1 … g5` | cascade gains of F5 … F1 (dB) |
| `gf gh gv gn` | gains of frication (A2-AB), aspiration (AH), voicing, the nasal pole |

The rules themselves (targets, loci, transitions) are the same for every voice; only the table set differs, by sex.

## 10. The graphs

All are 150 dpi PNGs in [`formants_graphs/`](formants_graphs/). Thick lines are the values in voiced frames
(AV > 0), what you hear as formants; thin pale lines are the values sent while AV = 0. Dashed lines are `phdraw`'s
target (`tarcur` + the diphthong line), from which the transitions depart. Phone labels: **bold** = primary stress,
black = secondary, grey = unstressed. Time is frames × 6.4 ms.

| File | Shows |
|---|---|
| [01_formant_tracks.png](formants_graphs/01_formant_tracks.png) | F1-F3 of "The old man sat in a rocker." against `phdraw`'s targets |
| [02_track_anatomy.png](formants_graphs/02_track_anatomy.png) | F2 of `[ax d'ay]` taken apart: table target, coarticulated target, forward and backward transitions with their boundary values and lengths, the `d` locus, the `ay` break points; below, the three terms `phdraw` adds |
| [03_diphthong_lines.png](formants_graphs/03_diphthong_lines.png) | eight vowels in `[hx'Vd]`: every vowel is straight lines between break points, and `hx` has the vowel's formants |
| [04_consonant_loci.png](formants_graphs/04_consonant_loci.png) | F2 and F3 of `b d g` before `iy aa uw`: loci, pct, the velar pinch |
| [05_glides_liquids_h.png](formants_graphs/05_glides_liquids_h.png) | `w y r l` between `ax` and `aa`, and `hx` before three vowels and between two |
| [06_nasals.png](formants_graphs/06_nasals.png) | "Many men.": FNZ 300/350/527, F1 jumping at the nasals, B1 widening, AV |
| [07_stops_amplitudes.png](formants_graphs/07_stops_amplitudes.png) | the amplitude tracks of `p t k b d g` before `aa`: closure, voice bar, burst, aspiration, voicing onset |
| [08_voices_head_size.png](formants_graphs/08_voices_head_size.png) | Paul (male tables) against Betty (female tables) with the F2/F3 ceilings; Paul at `hs` 80, 100 and 120 as the DSP scales him |
| [09_speaking_rate.png](formants_graphs/09_speaking_rate.png) | "Why go away?" at `:ra` 90, 180 and 400: durations, rescaled break points, transitions |

Graphs 10-14 (the transitions frame by frame, CV and VC loci, the rules by context, continuity at boundaries,
overlapping ramps) belong to [SMOOTHING_SYSTEM.md](SMOOTHING_SYSTEM.md) §11.

## 11. v1.8 behaviour worth knowing

- **There are no steady-state vowels.** Every vowel is lines between break points, and most short vowels are all
  transition: the tracks reach a target only in long phones.
- **Only F1-F3 are coarticulated.** Bandwidths, FNZ and amplitudes use targets as they are.
- **Coarticulation is local.** It looks one phone either way (15 %), plus the transitions; there is no longer-range
  vowel-to-vowel rule (dapi later added F2 vowel-vowel coarticulation, breathy B1 widening, trills and a glottal
  stop rule to `phdraw`; v1.8 has none of them, REFERENCE §15.17).
- **Stops and nasals move through their whole length**; a fricative's own transitions take 5 frames; a vowel's 8-9
  frames or its locus set's, scaled by its length. A stop's closure is a straight line between its two boundary values.
- **F1 jumps into and out of nasals** (with the rules of §4.4/4.6 around it), and FNZ steps between 300/350 and
  527 Hz.
- **Silence and `hx` anticipate the next phone**: both take its formants.
- **`endtyp` of `aw`, `ow` and `uw` is 5** (class 5 halves a consonant's F2/F3 locus pull); dapi has 3 for `aw`.
- **Transitions are fixed in frames, durations are not.** Rate changes the durations; only the non-obstruent
  transitions are scaled with them, and never beyond 20 frames ([09](formants_graphs/09_speaking_rate.png)).
- **Head size is the DSP's**, not the 68000's: the frames and the F2/F3 ceilings do not change with `hs`.
- **`divtab[−1]`** is read when two rescaled diphthong points step back a frame (the ROM word before the table; the
  C reproduces it, REFERENCE §15.31).

## 12. Where the numbers came from

- The code: `src/speech/ph_frame.c` (`phsettar`, `setloc`, `diph_time_scale`, `phdraw`, `set_formant_limits`,
  `setspdef`), `ph_rom.c` (the tables), `dsp_synth.c` (§8), all checked against the ROM by
  `decomp/scripts/check_frames.py` (REFERENCE §15.18) and `test_dsp` (§16.10).
- The graphs come from **`formants_graphs/formantlog.c`**, a small logger on the speech engine (`engine.c` with the
  frame hook, and a phone hook in front of the engine's). For each frame it records the 17 parameter words and the
  terms `phdraw` added for F1-F3; for each phone, what `phsettar` set up for F1-F3, FNZ and B1-B3 (table target,
  targets, both transitions, diphthong break points). Its header describes the output. To regenerate them (Windows,
  MSVC; Python 3 with matplotlib):

  ```bat
  formants_graphs\build_formantlog.bat
  python formants_graphs\make_formant_graphs.py
  python formants_graphs\make_formant_graphs.py --check
  ```

  The build writes `build\formantlog\formantlog.exe` (set `FORMANTLOG` to use another binary). The first Python
  command rewrites the nine PNGs. `--check` compares every frame word with the ROM's frame logs in
  `decomp/reference/`, which must be present locally, and checks that the logged terms add up to the words sent: it
  reports 9,015 frames, all matching.
- Background: MITalk Ch. 11 (Klatt's CV synthesis: vowels as straight-line segments, three vowel classes, the locus
  equation) and App. C (PHONET's `Bper`, `Tcf`, `Tcb`) in `docs/mitalk.html`; Klatt 1980 (`docs/klatt1980.pdf`) for the
  synthesizer; REFERENCE §15.17 for the first reading of `phsettar` and `phdraw`.
