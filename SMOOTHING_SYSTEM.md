# SMOOTHING_SYSTEM.md: forward and backward smoothing in the DTC-01 v1.8

This document describes how DECtalk I firmware v1.8 joins one phone's formants to the next: the **forward** and
**backward transitions** ("smoothing") that `phsettar` sets up at every phone boundary and `phdraw` adds to the
targets every 6.4 ms. It covers the frame-by-frame arithmetic, the events in a phone's life, the rules that choose a
boundary value and a ramp length from the phones on each side, the locus rule for plosives, fricatives and nasals
next to vowels (consonant-to-vowel and vowel-to-consonant), and what happens when the ramps meet. It goes deeper
than [FORMANT_SYSTEM.md](FORMANT_SYSTEM.md) §4.4-4.6, which summarizes the same rules; that document has the
target, diphthong and amplitude tables this one refers to.

| Stage | C (`src/speech/ph_frame.c`) | ROM | dapi counterpart |
|---|---|---|---|
| per phone: boundary values and ramp lengths of all 15 tracks | `phsettar`, lines 383-532 | `0xaa08` | `ph_setar.c` |
| the locus rule | `setloc`, lines 99-114 | `0xc420` | `ph_setar.c` `setloc` |
| per frame: target + ramps | `phdraw`, lines 548-593 | `0xa804` | `ph_draw.c` (DK 1984) |
| the frame loop that calls them | `phclause_draw_frames`, lines 76-93 | `0xa782` | `ph_claus.c` |

Confidence tags as in AGENTS.md: **[V]** read from the checked C / ROM, **[I]** an interpretation. Everything below
is [V] unless tagged. The C is checked word for word against the ROM (REFERENCE §15.18). On top of that,
[`make_smoothing_graphs.py`](formants_graphs/make_smoothing_graphs.py) holds a Python copy of the F1-F3 rules of §4-§5
(`fwd_rule`, `bwd_rule`), and `--check` compares it with what the C set up: **1,653 phone tracks from 23 texts, all
identical**, boundary values and lengths. So the rule tables below are complete for F1-F3, not a summary.

---

## 1. The picture

Every phone has a target for each track (a flat value for consonants, straight lines between break points for
vowels). If the tracks jumped from target to target the speech would click and sound robotic, so each phone gets
two linear ramps per track:

```
             boundary value                                           boundary value
             (forward, "bouval")                                     (backward, the next phone's forward)
                  ▶ ·                                                        ◁
                      ·  ·                                              ·  ·
                            ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·         target (or the vowel's lines)
                  │◀──── forward ramp ───▶│                  │◀─ backward ─▶│
   frame:         0                       n_f              tbacktr        durfon = next phone's frame 0
                  ▲ phsettar runs here                       ▲ = durfon − n_b
```

- The **forward transition** starts the phone at a boundary value and fades linearly into the target over `n_f`
  frames: "where the track comes from".
- The **backward transition** leaves the target over the last `n_b` frames toward the next boundary value: "where
  the track is going".
- The **boundary value** is normally the midpoint of the two phones' targets. Rules move it and change the lengths
  according to the classes of the two phones: the locus of a consonant next to a vowel, 3/4 of the way toward a
  glide or liquid, the whole phone for stops and nasals, a jump for F1 at a nasal, and so on (§4-§5).
- Both sides of a boundary compute the value independently, from the same targets and the same rule, so the track is
  continuous at nearly every boundary (§8).

This is Klatt's synthesis-by-rule scheme (MITalk Ch. 11 and App. C: `Tcf`/`Tcb`, the forward and backward transition
durations, and `Bper`, "the percent of movement from locus toward target"; REFERENCE §14). The ROM's version is
dapi's `ph_setar.c`/`ph_draw.c` of 1984 with its own tables.

## 2. Frame-by-frame arithmetic (`phdraw`)

`phclause_draw_frames` counts frames within a phone (`tcum` = 0 … `durfon` − 1). When `tcum` wraps, it calls
`phsettar` for the new phone **before** drawing its first frame, then `phdraw` and `pht0draw` draw the frame
and it goes to the DSP. So a phone's frame 0 already has the new ramps.

Per track `phsettar` leaves (units: Hz or dB × 8, i.e. three fraction bits; `ph_frame.h`):

| Field | Meaning |
|---|---|
| `tarcur`, `tarend` | the phone's start and end target (equal unless it is a vowel line) |
| `tarlas`, `tarnex` | the last phone's end target, and a copy of the next phone's start target |
| `ftran`, `dftran` | forward term × 8 and its step: `dftran = q14(8·(bouval − tarcur), divtab[n_f])`, `ftran = dftran·n_f` |
| `btran`, `dbtran`, `tbacktr` | backward term × 8 (starts at 0), its step `q14(8·(bouval − tarend), divtab[n_b])`, first frame `durfon − n_b` |

`divtab[n]` is 16384/n (Q14), so `dftran` is (bouval − target)·8/n rounded toward −∞. Each frame (F1-F3, B1-B3; the
other tracks the same without the vowel line):

```
value  = tarcur + dipcum/8            the target, moving along the vowel's line
       + ftran/8                      forward term;   then ftran −= dftran until it is 0
       + btran/8   if tcum ≥ tbacktr  backward term;  then btran += dbtran
if tcum < tspesh: value = pspesh      a special value (burst silence, aspiration bandwidths; FORMANT_SYSTEM §4.8)
F2 ≤ f2max, F3 ≤ f3max                the per-sex ceilings
```

Consequences worth knowing:

- **Frame 0 is on the forward boundary value** (to within the rounding of `dftran`: `ftran` is reset to a whole
  number of steps, so the start can differ from `bouval` by up to `n_f`/8 Hz). The term then falls by `dftran/8` a
  frame and is exactly 0 at frame `n_f`.
- **The backward boundary value is never drawn by the phone itself.** At `tbacktr` the term is 0; on the last frame it
  is (n_b − 1)/n_b of the way. The value is reached one frame later, on the next phone's frame 0, by *its* forward
  ramp. So the two ramps meet at a knot exactly on the boundary frame, and the track is piecewise linear.
- **The ramps ride on the vowel's lines.** `dipcum` keeps moving under both ramps, so a diphthong's movement starts
  while the forward transition is still fading ([02](formants_graphs/02_track_anatomy.png)). The backward ramp is
  measured from `tarend`, the value the last line reaches on the phone's last frame, so the sum still ends at the
  boundary.
- **Ramps add when they overlap.** If `n_f + n_b > durfon`, both terms are active at once (§7).
- **Everything is in frames.** A ramp's slope is (boundary − target)/n per 6.4 ms frame; nothing is interpolated
  inside a frame (the DSP loads the cascade formants once per glottal period, FORMANT_SYSTEM §8).

### 2.1 The events in one phone ([10](formants_graphs/10_smoothing_frame_by_frame.png))

F2 of `aa` in `[ax d'aag ax]` (Paul): 32 frames, frames 35-66.

| # | Frame | Event | F2 |
|---|---|---|---|
| 1 | 35 (tcum 0) | `phsettar` runs; the forward ramp starts at the boundary value from `/d/`'s locus: 1500 Hz, pct 0 (before a class-2 vowel) | 1499, +247 from the target |
| 2 | 47 (tcum 12) | forward term reaches 0 (the locus set's 12 frames × `ftran_scale` 0.92, + 1): on the target line, which rises slowly (`aa` is a line, 1252 → 1288) | 1263 |
| 3 | 59 (tcum 24) | `tbacktr` = 32 − 8: the backward ramp starts (`/g/`'s set: 10 frames × `btran_scale` 0.78, + 1 = 8), +49 Hz a frame from the next frame on | 1279 |
| 4 | 66 (tcum 31) | the last frame of `aa`: 7/8 of the way | 1629 |
| 5 | 67 | `/g/` starts: its own forward boundary value, computed from `/g/`'s F2 locus 1680 (pct 0 after a class-2 vowel) | 1679 |

Inside `/d/` and `/g/`, both ramps span the closure, so F2 runs straight between the two boundary values and the stop's
own target (1548, 1712) never shows (§7).

## 3. What the rules look at

At a phone boundary `phsettar` knows the current phone (`phcur`), the last one (`pholas`), the next two (`phonex`,
`phonex2`), their feature bits (`featb`: `fealas`, `feacur`, `feanex`), their classes (`endtyp` of the last,
`begtyp`/`endtyp` of the current, `begtyp` of the next), the duration `durfon` and the stress. There is no look-ahead
beyond the next two phones and no look-back beyond the last one.

**Feature bits** (`featb`, `ph_rom.c`):

| Bit | Name (dapi) | Phones |
|---|---|---|
| `0x02` | voiced | vowels, `w y r l rx lx`, nasals, `el`, voiced obstruents, `q` |
| `0x10` | sonorant | vowels, `w y r l rx lx`, `hx`, nasals, `el`, **and silence `_`** |
| `0x20` | obstruent | `f v th dh s z sh zh p b t d k g dx tx ch jh` |
| `0x40` | plosive | `p b t d k g tx` (not `dx`, not `ch jh`) |
| `0x80` | nasal | `m n nx em en` |
| `0x200` | sonorant consonant ("glide/liquid" below) | `w y r l rx lx` (not `el`) |
| `0x800` | palatal | `sh zh ch jh` |

`q` (glottal stop, `0x2002`) is neither sonorant nor obstruent, and `hx` (`0x110`) is a sonorant but not voiced.

**Classes** (`begtyp` / `endtyp`, the class of a phone's start and end; FORMANT_SYSTEM §3.3):

| Class | begtyp | endtyp differs |
|---|---|---|
| 1 front | `iy ih ey eh ae yu ix ir er y` | `ay oy` end front |
| 2 back unrounded | `aa ay aw ah ax ar` | |
| 3 rounded | `ao ow oy uh uw rr or ur rx` | `yu ir er ar` end rounded |
| 4 consonant | silence, `hx`, `q`, nasals, obstruents | |
| 5 sonorant consonant | `w r l lx el` | `aw ow uw` end in class 5 |

## 4. F1-F3: the forward transition (into a phone)

Lines 383-455. The rules run in this order; a later one overrides an earlier one (the value, the length, or both):

| Step | Condition | Boundary value | Frames |
|---|---|---|---|
| 1 | always | midpoint of `tarlas` and `tarcur` | 5 |
| 2a | the phone is a sonorant but not a glide/liquid (vowels, `hx`, nasals, `el`, silence) | | 8 |
| 2b | … and the last phone is a glide/liquid | `(tarlas + midpoint)/2`: 3/4 of the way toward the glide | 9 |
| 2c | … … and it is `l`, F1 only | + 80 Hz | |
| 2d | the phone is a glide/liquid | | 5 |
| 2e | … and the last phone is not one | `(tarcur + midpoint)/2`: 3/4 of the way toward itself | 3 |
| 3 | **the phone is silence** | `tarlas` (start from the last value) | `durfon` (then capped, below); steps 4-7 are skipped |
| 4 | the last phone is class 4 and has a locus set for this phone's class (`setloc`, §6) | locus + pct·(`tarcur` − locus) | the set's |
| 5 | this phone is class 4 and has a locus set for the last phone's class (`setloc`) | locus + pct·(`tarlas` − locus) | the set's |
| 6a | the phone is an obstruent | | 5 |
| 6b | … a plosive (`p b t d k g tx`) | | `durfon` |
| 6c | the phone is a nasal | | `durfon`; **F1: 0** (a jump) |
| 7 | F1 only, the last phone is a nasal | at least 425 Hz | 16 |
| 8 | the phone is not an obstruent and the length > 0 | | `q14(n, ftran_scale) + 1` |
| 9 | always | at least 0 | at most `durfon`, at most 20 |

Only one of steps 4 and 5 can apply (one side must be class 4 and the other not). Step 6 keeps the locus value but
replaces its length: a fricative's side always moves in 5 frames and a plosive's or nasal's through its whole length,
whatever the locus set says. The locus set's own length is used on the *vowel* side.

## 5. F1-F3: the backward transition (out of a phone)

Lines 457-532, the mirror image, with the next phone in place of the last:

| Step | Condition | Boundary value | Frames |
|---|---|---|---|
| 1 | always | midpoint of `tarend` and `tarnex` | 4 |
| 2a | the phone is a sonorant (not a glide/liquid) | | 8 |
| 2b | … and the next phone is a glide/liquid | `(tarnex + midpoint)/2`: 3/4 toward the glide | 9 |
| 2c | … … and it is `l`, F1 only | + 80 Hz | |
| 2d | the phone is a glide/liquid | | 6 |
| 2e | … and the next phone is not one | `(tarend + midpoint)/2`: 3/4 toward itself | 3 |
| 3 | **the next phone is silence** | | **0: no backward transition**; steps 4-7 are skipped |
| 4 | the next phone is class 4 with a locus set for this phone's end class (`setloc`) | locus + pct·(`tarend` − locus) | the set's |
| 5 | this phone is class 4 with a locus set for the next phone's class (`setloc`) | locus + pct·(`tarnex` − locus) | the set's |
| 6a-c | obstruent / plosive / nasal (F1: 0) | | 5 / `durfon` / `durfon` |
| 7 | F1 only, the next phone is a nasal | at least 480 Hz | 20 |
| 8 | the phone is not an obstruent and the length > 0 | | `q14(n, btran_scale) + 1` |
| 9 | always | at least 0 | at most 20, at most `durfon`; `tbacktr = durfon − n` |

The differences from the forward side: default 4 frames instead of 5, a glide/liquid next to another glide/liquid
takes 6 instead of 5, the nasal floor is 480 Hz over 20 frames instead of 425 over 16, and the length scale is
0.36 + r/2 instead of 0.5 + r/2 (§7).

## 6. The locus rule (`setloc`): plosives, fricatives and nasals next to vowels

`setloc(consonant, its class, the other phone's class, track, the other phone's target)` (lines 99-114) applies when

- the track is F1, F2 or F3;
- the consonant's class is **4** (silence, `hx`, `q`, nasals, obstruents), and the other phone's is **not 4** (a
  vowel, `y`, `w r l lx el`, `rx`);
- `plocu[class − 1][consonant]` names a locus set. `hx`, `q`, silence and `el` have none, so they always take the
  midpoint.

Then, with the set `{locus, pct, frames}` for this formant,

```
boundary value = locus + pct × (other target − locus)          other target: the vowel's value at the boundary
frames         = the set's
```

The set is chosen by the other phone's class **at that boundary**: its `begtyp` when the consonant comes first (CV),
its `endtyp` when the consonant comes second (VC). Class 5 uses the class-3 set, with the pull on F2 and F3
halved: pct becomes pct/2 + 0.5, unless the consonant is palatal (`sh zh ch jh`). So `ow uw aw`, which end in class 5,
have a VC boundary nearer their own target than their CV one: `d` + `uw`: CV F2 pct 0, VC F2 pct 0.5
([11](formants_graphs/11_locus_cv_vc.png)).

How to read pct:

- **pct 0**: the boundary is the locus, whatever the vowel. `t d` and `n` before back vowels, `k g` before back
  unrounded ones: every vowel's F2 starts from the same place.
- **0 < pct < 1**: the boundary lies between the locus and the vowel target, pct of the way toward the target
  (Klatt's `Bper`).
- **pct > 1**: the boundary overshoots the target, away from the locus. `k g nx` F3 before front vowels (1.05-1.17):
  F3 starts above the vowel's (the velar pinch with F2, [04](formants_graphs/04_consonant_loci.png)).
- **pct < 0**: the boundary is beyond the locus, away from the target: `th dh` F3 before front vowels (−0.35).
- **locus 1**: a proportional rule, boundary ≈ pct × target: `m em f v` F2 (and `m em` F3 before back vowels).

### 6.1 The male locus sets

Each cell is F1 / F2 / F3 as `locus pct frames` (`x0.85 6` = the proportional rule). "as 2" = the same set as
class 2. The female sets (`femloc`) have the same layout with other values. `dx` and `tx` use `t`'s sets, `em en` use
`m n`'s. Frames are before the duration scaling of the vowel side (§7).

| | front (1) | back unrounded (2) | rounded (3; class 5 halves F2/F3 pct) |
|---|---|---|---|
| `p` | 360 .50 3 / 950 .66 5 / 2300 .15 7 | 360 .50 4 / 1020 .66 6 / 2250 .58 8 | as 2 |
| `b` | 340 .50 3 / 900 .66 5 / 2300 .15 7 | 340 .50 4 / 1020 .66 6 / 2250 .58 8 | as 2 |
| `t` `dx` `tx` | 350 .43 6 / 1700 .75 6 / 2600 .30 7 | 350 .43 7 / 1500 .00 12 / 2600 .00 8 | 350 .43 6 / 1500 .00 15 / 2300 .00 15 |
| `d` | 250 .43 6 / 1700 .75 6 / 2550 .30 7 | 250 .43 7 / 1500 .00 12 / 2600 .00 8 | 250 .43 6 / 1500 .00 15 / 2300 .00 15 |
| `k` | 350 .33 7 / 1990 .20 8 / 3000 1.17 8 | 350 .33 8 / 1700 .00 10 / 2150 .00 14 | 350 .33 6 / 1700 .42 10 / 1920 .15 13 |
| `g` | 250 .33 7 / 1990 .20 8 / 3000 1.13 8 | 300 .33 8 / 1680 .00 10 / 2150 .00 14 | 290 .45 6 / 1700 .42 10 / 1920 .15 13 |
| `ch` | 350 .54 9 / 1750 .25 11 / 2600 .19 11 | 350 .54 9 / 1730 .10 11 / 2350 .10 11 | 350 .54 9 / 1730 .10 14 / 2350 .10 16 |
| `jh` | 240 .32 9 / 1750 .25 11 / 2600 .19 11 | 245 .32 9 / 1730 .10 11 / 2350 .10 11 | 245 .32 9 / 1730 .10 14 / 2350 .10 16 |
| `f` | 320 .60 5 / x0.85 6 / 2080 .35 5 | 320 .60 5 / x0.91 6 / 2100 .65 6 | as 2 |
| `v` | 320 .60 5 / x0.85 6 / 2080 .35 6 | 320 .60 5 / x0.91 6 / 2100 .65 6 | as 2 |
| `th` | 340 .10 7 / 1000 .40 10 / 2550 −.35 10 | 360 .10 8 / 1270 .12 11 / 2620 .11 11 | as 2 |
| `dh` | 340 .10 7 / 1000 .35 10 / 2550 −.35 10 | 360 .10 8 / 1270 .12 11 / 2620 .11 11 | as 2 |
| `s` | 310 .40 6 / 1240 .40 8 / 2550 .00 11 | 310 .40 6 / 1240 .40 8 / 2630 .00 11 | 310 .40 6 / 1320 .15 10 / 2460 .00 10 |
| `z` | 310 .40 6 / 1240 .35 8 / 2550 .00 11 | 310 .40 6 / 1240 .35 10 / 2630 .00 11 | 310 .40 6 / 1320 .15 10 / 2460 .00 10 |
| `sh` `zh` | 285 .32 9 / 1830 .30 11 / 2640 .51 11 | 285 .32 9 / 1600 .07 11 / 2270 .00 13 | 340 .32 9 / 1530 .07 14 / 2100 .20 17 |
| `m` `em` | 520 .40 5 / x0.85 5 / 2100 .32 7 | 480 .30 5 / x0.90 10 / x0.97 8 | as 2 |
| `n` `en` | 480 .24 6 / x0.99 6 / 2800 .30 7 | 480 .22 5 / 1480 .00 12 / 2600 .00 10 | 480 .24 5 / 1450 .00 15 / 2320 .00 15 |
| `nx` | 480 .20 6 / 2300 .20 10 / 3000 1.05 10 | 480 .20 6 / 1700 .20 11 / 2150 .20 11 | 480 .20 6 / 1700 .42 11 / 1920 .25 11 |

Patterns [I, from the numbers]: labials (`p b f v m`) have low F2 loci with a large pct, so F2 rises into and falls
out of every vowel; alveolars (`t d s z n`) have one F2 locus near 1500-1700 Hz with pct 0 before back vowels; velars
(`k g nx`) have a locus per vowel class, high before front vowels, which is the MITalk observation that the velar
locus depends on the vowel (MITalk computes it from F1/F2; the ROM tabulates it).

### 6.2 Plosive to vowel and vowel to plosive ([11](formants_graphs/11_locus_cv_vc.png))

Both sides of a CV or VC boundary run `setloc`; one of the two calls applies. With `[ax C'VC]`:

| Boundary | Consonant side | Vowel side |
|---|---|---|
| CV (stop, then vowel) | the stop's **backward** ramp (step 5 of §5: this phone is class 4): value from the vowel's copied start target `tarnex`, over the whole closure | the vowel's **forward** ramp (step 4 of §4: the last phone is class 4): value from its own start target, over the set's frames × `ftran_scale` + 1 |
| VC (vowel, then stop) | the stop's **forward** ramp (step 5 of §4): value from `tarlas`, the vowel's end target, over the whole closure | the vowel's **backward** ramp (step 4 of §5): value from its end target, over the set's frames × `btran_scale` + 1 |

The consonant side of a plosive always spans the closure (step 6b), so **a stop's closure is a straight line from
its VC boundary value to its CV boundary value**, and its table target never appears
([14](formants_graphs/14_ramps_overlap.png), left). A fricative moves only in its first and last 5 frames and sits on
its (coarticulated) target in between; a nasal moves F2/F3 through its whole murmur and keeps F1 flat on its target
(its F1 ramps are 0 frames long).

Voicing and release do not enter these rules: `p` and `b` differ only in their F1 locus, and the burst and aspiration
(FORMANT_SYSTEM §4.8) overwrite amplitudes and bandwidths, not F1-F3. During aspiration the formants are already
moving along the vowel's forward ramp, which is how the aspiration gets its "locus-to-vowel" colour.

A clause-final `p b t d k g` is followed by a 4-frame `ax` (`ix` for `t d`, or after `iy ih ey eh ae`) that
`phtiming` adds as its release (`ph_timing.c`, lines 308-319); so a final stop's backward ramp goes to that vowel, not
to silence.

### 6.3 Consonant clusters, glides and the rest

| Boundary | Rule | Example |
|---|---|---|
| consonant + consonant (both class 4) | no locus: the midpoint, 5 frames (4 backward) for fricatives, the whole phone for plosives and nasals | `s t`, `n d`, `k s` |
| obstruent/nasal + `y` | locus, class 1 set, full pull (`y` is class 1, not 5) | |
| obstruent/nasal + `w r l lx el` | locus, class 3 set, F2/F3 pull halved (unless `sh zh ch jh`) | `b l`, `g w`, `t r`, `p el` |
| vowel + glide/liquid | 3/4 toward the glide; the vowel moves in 9 frames (scaled), the glide in 3 | [05](formants_graphs/05_glides_liquids_h.png), [12](formants_graphs/12_rules_by_context.png) |
| glide + glide | midpoint; 5 frames forward, 6 backward (scaled) | `rr lx` |
| vowel + vowel | midpoint, 8 frames each side (scaled) | `iy aa` |
| anything + `hx` | midpoint (no locus set); `hx` has no formant targets and takes the next phone's (FORMANT_SYSTEM §7), so F1-F3 glide straight through it | `aa hx aa` |
| anything + `q` | midpoint; `q` takes the next phone's targets, is neither sonorant nor obstruent, so it keeps the default 5/4 frames, scaled | `ah q ow` |
| a phone + a nasal | locus of the nasal for F2/F3 and F1's value, but F1 of the nasal jumps (0 frames) and the vowel's F1 stays ≥ 480 Hz over 20 frames before it and ≥ 425 Hz over 16 after it | [06](formants_graphs/06_nasals.png) |
| `l` + vowel or vowel + `l` | 3/4 rule, and F1 80 Hz higher on the vowel's side | `l aa` |

### 6.4 Pauses

- **Into a pause** there is no backward transition (§5 step 3): the phone ends on its target.
- **The pause itself** has no formant targets (−1: it takes the next phone's). Its forward ramp starts at `tarlas`,
  the last phone's end value, and glides toward the next phone's targets over its length, scaled and **capped at 20
  frames** (128 ms): a longer pause reaches the next phone's values early and then stays there. Nothing is heard
  (AV = 0), but the tracks are sent, and the next phone starts from values already close to its own.
- **Out of a pause** the phone's forward ramp uses the midpoint of the pause's end value (≈ its own target) and its
  target: almost no movement. There is no locus at a pause (silence has no locus sets).
- The very first phone after power-up starts from `reset_tarend` = 600 / 1600 / 2600 Hz.

## 7. Ramp lengths and duration ([14](formants_graphs/14_ramps_overlap.png))

For a phone that is not an obstruent, `phsettar` computes (lines 216-220)

```
r           = durfon / inhdr[phone]          (Q14, capped just under 2)
ftran_scale = 0.5  + r/2                     forward
btran_scale = 0.36 + r/2                     backward
n           = q14(n_rule, scale) + 1         then ≤ 20 and ≤ durfon
```

so a phone at its inherent length (r = 1) uses its rule's frames + 1 forward and about 0.86× + 1 backward, a
lengthened phone longer ramps (never beyond 20 frames), a shortened one shorter ones. **Obstruents are not scaled**:
5 frames, or the whole phone for plosives. Bandwidths, FNZ and amplitudes are not scaled either.

Because the rule's frames are only partly scaled, a vowel's ramps keep roughly the same length when the speaking
rate changes, and it is the middle of the vowel that shrinks (`eh` in `[ax d'ehg ax]`: 30 frames with ramps 7 + 8
at `:ra 100`, 13 frames with 5 + 6 at `:ra 450`).

**When the ramps overlap** (`n_f + n_b > durfon`), both terms are active at once and the track turns back before it
reaches the target: **undershoot**, the way a real short vowel does not reach its target. In the extreme case
`n_f = n_b = durfon` the two terms add up to a straight line from one boundary value to the other and the target has
no effect at all. That is always the case for a plosive's closure and a nasal's F2/F3, and it happens to short
vowels too: `ax` held to 30 ms between two `g`s ([14](formants_graphs/14_ramps_overlap.png), right) is 5 frames with
ramps 5 + 5, so F2 stays on the `/g/` locus (1680 Hz) instead of dipping toward its target (1330 Hz); at 50 ms it gets
down to 1592 Hz, at 80 ms 1486 Hz, and at 130 ms it touches the target for one frame. Unstressed vowels in running
speech are often in this regime ("Bob put a big cake…": `ax` 10 frames with ramps 10 + 4, `uh` 15 frames with 8 + 10).

## 8. Continuity: the two sides of a boundary ([13](formants_graphs/13_boundary_continuity.png))

Each boundary value is computed twice: once by the phone before it (backward: from its `tarend` and its copy
`tarnex` of the next target) and once by the phone after it (forward: from its `tarcur` and `tarlas`). Over 909
boundary-formant pairs from 14 test texts:

| Boundary | Pairs | Within 5 Hz | Why they differ |
|---|---|---|---|
| vowel to consonant, locus (VC) | 270 | 100 % | both sides use the vowel's end target: identical |
| vowel to vowel, midpoint | 78 | 100 % | `tarnex` = the next vowel's first value coarticulated with this `tarend`, as the next vowel computes it |
| consonant to vowel, locus (CV) | 285 | 96 % | identical for vowels; a glide or liquid's own target is coarticulated with *both* its neighbours, the copy only with the consonant |
| vowel and glide/liquid (3/4 rule) | 118 | 69 % | the same: the glide's target differs from its copy by up to ~90 Hz |
| consonant cluster, midpoint | 64 | 41 % | a consonant's target is coarticulated with the mean of both neighbours, the copy only with the last phone |
| pause to phone, midpoint | 43 | 56 % | the same |
| F1 at `/l/` | 11 | 0 % | on purpose: +80 Hz on the vowel side only (a jump of ~80 Hz, down into `l`, up out of it) |
| F1 into a nasal | 21 | 0 % | on purpose: the nasal's F1 jumps to its target (the vowel was heading for ≥ 480 Hz) |
| F1 out of a nasal | 19 | 16 % | on purpose: the nasal ends on its target, the vowel starts at ≥ 425 Hz |

The copy `tarnex` is `0.85·(next table target) + 0.15·tarend` (FORMANT_SYSTEM §4.2), while a consonant's own target
is `0.85·table + 0.15·(mean of its last and next)`. For a vowel the two agree, since its first line value is
coarticulated with the last phone only. Rounding adds 1-2 Hz (the `>> 3` of the × 8 terms and the Q14 step).

## 9. The other tracks

The same two ramps are used for all 15 tracks, with rules of their own. None of these lengths are scaled by duration.

### 9.1 Bandwidths B1-B3 (lines 414-420, 487-493)

| Forward (into the phone) | Value | Frames |
|---|---|---|
| default | midpoint | 6; 3 if the phone is voiceless |
| B1, a voiced phone after a voiceless one (not a pause) | midpoint + 20 Hz | 8 |
| after a pause | target + 100 (B1) / + 50 (B2) / + 0 (B3) | 8 |
| the phone is a pause | `tarlas` + 100 / 50 / 0 | 8 |
| B1 after a nasal | + 70 Hz | 16 |

| Backward (out of the phone) | Value | Frames |
|---|---|---|
| default | midpoint | 6; 3 if the phone is voiceless |
| B1, a voiced phone before a voiceless one | midpoint | 8 |
| before a pause | `tarend` + 100 / 50 / 0 | 8 |
| the phone is a pause | `tarnex` + 100 / 50 / 0 | 8 |
| B1 before a nasal | + 70 Hz | 16 |

So bandwidths widen toward pauses and nasals and around voiceless phones. During aspiration B1/B2 are replaced by
target + 250 / + 80 (special values, FORMANT_SYSTEM §4.8).

### 9.2 The nasal zero FNZ (lines 441-444, 519-522)

No ramps at all (0 frames: FNZ jumps between 300 and 527 Hz), except before a nasal, where it rises to 350 Hz over
the last 20 frames, and after one, where it starts at 350 Hz and returns over 16
([06](formants_graphs/06_nasals.png)).

### 9.3 Amplitudes AV, AH, A2-A6, AB (lines 421-440, 494-518)

Forward, in order:

| Condition | Value | Frames |
|---|---|---|
| default | midpoint of `tarlas` and `tarcur` | 5 |
| the midpoint is more than 12 dB below the target (an onset) | target − 12 | AV: 2 |
| … AV after a pause, on `w y r l` or a stressed phone | target − 20 | 7 |
| … AV after an obstruent | target − 6 | |
| after a nasal, rising | | 0 (a jump) |
| the value is more than 12 dB below `tarlas` (an offset) | `tarlas` − 15 | pause: 11; AV: 0 (a drop) |
| A4 of `ch jh` | target − 40 | `durfon` − 2 |

Backward, in order:

| Condition | Value | Frames |
|---|---|---|
| default | midpoint of `tarend` and `tarnex` | 4 |
| the midpoint is more than 12 dB below the next target | `tarnex` − 12 | pause: 11 |
| AV, the next phone is louder and is not `z` | | 0; and then for a plosive or `ch`: voiceless 0 dB, voiced **20 dB over 7 frames** (the voice bar's decay) |
| otherwise: a nasal before a louder phone | | 0 |
| a stop or affricate (`p` … `jh`) | | 2; for `p` … `q` the floor below is the phone's own target |
| the value is below the floor (`tarend` − 12) | floor − 3; AV: floor + 4, before a pause floor − 2 over 12 frames; AH: its own target | |
| the next phone is a stop or affricate | | 0 (cut at once) |

The special values override these ramps: the burst (A2-AB held at 0 until the last 1-13 frames of a stop or
affricate), the aspiration (AV 0, AH 55-58 dB for the VOT), the second burst frame 10 dB weaker and the AV onset ramp
of `phdraw` (FORMANT_SYSTEM §4.8, [07](formants_graphs/07_stops_amplitudes.png)).

## 10. v1.8 behaviour worth knowing

- **Smoothing is local and linear.** Each boundary sees its two phones (plus `phonex2` for −1 targets); ramps are
  straight lines in 6.4 ms steps; there is no filtering of the tracks.
- **The boundary value is drawn once**, on the first frame of the phone after the boundary; the phone before it stops
  one step short.
- **Stops and nasals are straight lines** between their boundary values (F1 of nasals: flat on the target, with jumps).
- **A consonant's locus decides its vowel's first and last frames** for F1-F3, with one set per vowel class and a
  class-5 variant for `ow uw aw` endings and for glides; the consonant's own target matters only next to other
  consonants and pauses.
- **Only F1-F3 ramps are scaled by duration**, and only for non-obstruents; everything else is in fixed frames.
- **Short phones undershoot**: overlapping ramps add, so the target may never be reached.
- **Pauses glide**: at most 20 frames from the last value toward the next phone, silently.
- **Two-sided computation leaves small steps** (a few Hz, up to ~90 Hz next to glides and in clusters) and deliberate
  ones at nasals (F1) and `l` (F1 +80).

## 11. The graphs

150 dpi PNGs in [`formants_graphs/`](formants_graphs/), numbered after FORMANT_SYSTEM.md's nine. Thick lines are
voiced frames (AV > 0), thin pale lines values sent while AV = 0; ▶ marks a forward boundary value (the phone's first
frame), ◁ a backward one (reached on the next phone's first frame); dashed lines are `phdraw`'s target.

| File | Shows |
|---|---|
| [10_smoothing_frame_by_frame.png](formants_graphs/10_smoothing_frame_by_frame.png) | F2 of `[ax d'aag ax]` one dot per frame, with the ramps' spans, the five events of §2.1, and below, the three terms `phdraw` adds |
| [11_locus_cv_vc.png](formants_graphs/11_locus_cv_vc.png) | F2 and F3 of `[ax C'VC]` for `b d g` × `iy aa uw`: the CV and VC boundary values, the loci of the sets used, pct and ramp lengths; `uw`'s class-5 ending |
| [12_rules_by_context.png](formants_graphs/12_rules_by_context.png) | `aa` next to a pause, `g`, `s`, `n`, `w`, `l`, `hx` and `iy`, with the rule and frames that decided each side |
| [13_boundary_continuity.png](formants_graphs/13_boundary_continuity.png) | every boundary of 14 texts: the next phone's first value minus the backward boundary value, by kind of boundary (§8) |
| [14_ramps_overlap.png](formants_graphs/14_ramps_overlap.png) | a closure as the sum of its two ramps; a stressed vowel at four rates; a short vowel at four durations (undershoot) |

To regenerate them (Windows, MSVC; Python 3 with matplotlib):

```bat
formants_graphs\build_formantlog.bat
python formants_graphs\make_smoothing_graphs.py
python formants_graphs\make_smoothing_graphs.py --check
```

`formantlog` (FORMANT_SYSTEM §12) also logs, per phone, the context `phsettar` used (`pholas`, `phonex`, `phonex2`,
`ftran_scale`, `btran_scale`). `make_smoothing_graphs.py` uses `make_formant_graphs.py`'s helpers and palette; its
`--check` runs the test texts (and the plain corpus entries of `decomp/reference/corpus.tsv`, when present) and
compares `fwd_rule`/`bwd_rule` with the logged boundary values and lengths; `make_formant_graphs.py --check` still
compares every frame with the ROM's frame logs. Background: MITalk Ch. 11 and App. C (`docs/mitalk.html`),
Klatt 1980 (`docs/klatt1980.pdf`), REFERENCE §15.17.
