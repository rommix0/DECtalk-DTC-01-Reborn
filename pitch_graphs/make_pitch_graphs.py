"""make_pitch_graphs.py - the graphs of PITCH_SYSTEM.md, written as PNGs next to this script.

Every curve comes from pitchlog (pitchlog.c, built by build_pitchlog.bat into build/pitchlog/): the speech engine in
src/, checked against the ROM, run after the power-up greeting so that the state it carries between clauses is the
ROM's. Needs Python 3 with matplotlib.

usage: python pitch_graphs/make_pitch_graphs.py            write the eight PNGs
       python pitch_graphs/make_pitch_graphs.py --check    compare pitchlog's T0 with the ROM frame logs in
                                                            decomp/reference/ (plain local-terminal corpus entries)
Set PITCHLOG to use another pitchlog binary.
"""
import os, subprocess, sys, tempfile
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = HERE
EXE = os.environ.get('PITCHLOG') or os.path.join(ROOT, 'build', 'pitchlog',
                                                 'pitchlog.exe' if os.name == 'nt' else 'pitchlog')
TSV = os.path.join(tempfile.gettempdir(), 'pitchlog_run.tsv')
GREETING = 'DECtalk version 1.8 is running.'
FRAME = 0.0064

# colours: a categorical palette checked for colour-blind separation, light mode
SURF, INK, INK2, MUTED, GRID = '#fcfcfb', '#0b0b0b', '#52514e', '#8a8984', '#e4e3df'
S = ['#2a78d6', '#eb6834', '#1baf7a', '#eda100', '#e87ba4', '#008300', '#4a3aa7', '#e34948']

plt.rcParams.update({
    'figure.facecolor': SURF, 'axes.facecolor': SURF, 'savefig.facecolor': SURF,
    'axes.edgecolor': MUTED, 'axes.labelcolor': INK2, 'xtick.color': INK2, 'ytick.color': INK2,
    'text.color': INK, 'font.size': 9.5, 'axes.titlesize': 11, 'axes.titleweight': 'bold',
    'axes.titlelocation': 'left', 'axes.grid': True, 'grid.color': GRID, 'grid.linewidth': 0.6,
    'axes.spines.top': False, 'axes.spines.right': False, 'legend.frameon': False,
    'lines.linewidth': 2, 'font.family': 'DejaVu Sans',
})

_cache = {}
def run(text, lead=GREETING, trim=True):
    key = (text, lead, trim)
    if key in _cache:
        return _cache[key]
    tsv = TSV
    subprocess.run([EXE, tsv, text, lead], check=True)
    r = {'f': [], 'ev': [], 'ph': [], 'cl': []}
    for line in open(tsv):
        p = line.rstrip('\n').split('\t')
        if p[0] == 'F':
            v = list(map(int, p[1:]))
            r['f'].append(dict(n=v[0], hz=v[1] / 10, f0=v[2], base=v[3], hat=v[4], imp=v[5], seg=v[6], fin=v[7],
                               phone=v[8], stress=v[9], mode=v[10], t0=v[11], av=v[12], dip=v[13], smooth=v[14]))
        elif p[0] == 'E':
            r['ev'].append((int(p[1]), int(p[2])))
        elif p[0] == 'P':
            r['ph'].append((int(p[1]), p[2], int(p[3]), int(p[4])))
        elif p[0] == 'C':
            r['cl'].append(int(p[1]))
    voiced = [f['n'] for f in r['f'] if f['av'] > 0]
    if voiced and trim:
        end = voiced[-1] + 30
        r['f'] = [f for f in r['f'] if f['n'] <= end]
        r['ev'] = [e for e in r['ev'] if e[0] <= end]
        r['ph'] = [p for p in r['ph'] if p[0] <= end]
        r['cl'] = [c for c in r['cl'] if c <= end]
    _cache[key] = r
    return r

def t(n):
    return n * FRAME

def col(r, k):
    return [x[k] for x in r['f']]

def voiced_hz(r):
    """F0 in Hz where the frame is voiced (AV > 0), None elsewhere: the pitch you hear."""
    return [x['hz'] if x['av'] > 0 else None for x in r['f']]

# ---- F0 command classification (ph_timing.c phtiming, ph_frame.c pht0draw) ----
KINDS = {
    'rise':  ('^', S[0], 'hat rise to the top (step up)'),
    'fall':  ('v', S[0], 'hat fall (step down)'),
    'ret':   ('>', S[0], 'hat back to the baseline (step up)'),
    'imp':   ('o', S[1], 'stress impulse (16 frames)'),
    'cont':  ('D', S[2], 'continuation / question rise impulse'),
    'low':   ('s', S[7], 'final lowering impulse'),
    'reset': ('X', INK2, 'sentence reset (0)'),
    'note':  ('*', S[6], 'sung note / F0 target'),
}
NOTES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
NOTETAB = [640, 678, 718, 761, 806, 854, 905, 959, 1016, 1076, 1140, 1208, 1280, 1356, 1437, 1522, 1613, 1709,
           1810, 1918, 2032, 2152, 2280, 2416, 2560, 2712, 2874, 3044, 3226, 3418, 3620, 3836, 4064, 4304, 4560,
           4832, 5120]      # ph_rom.c notetab (ROM 0x16172)
def classify(events):
    out, hat = [], 0
    for n, v in events:
        if v == 0:
            k, hat = 'reset', 0
            lab = 'reset'
        elif v >= 2000:
            nn = v - 2000
            if nn < 50:
                hz = NOTETAB[min(nn, 37) - 1] / 10
                lab = 'note %d = %s%d, %.1f Hz' % (nn, NOTES[(nn - 1) % 12], 2 + (nn - 1) // 12, hz)
            else:
                lab = 'target %d Hz' % nn
            k = 'note'
        elif v % 2 == 0:
            if v < 0:
                k = 'fall'
            else:
                k = 'rise' if hat + v > 0 else 'ret'
            hat += v
            lab = '%+g Hz' % (v / 10)
        else:
            if v < 0:
                k = 'low'
            elif v in (51, 251):
                k = 'cont'
            else:
                k = 'imp'
            lab = '%+g Hz' % (2 * v / 10)
        out.append((n, v, k, lab))
    return out

def draw_events(ax, r, y=0.965, labels=True, only=None):
    ev = classify(r['ev'])
    x0, x1 = ax.get_xlim()
    px = ax.bbox.width / (x1 - x0)          # pixels per second
    lanes = [-1e9] * 4
    for n, v, k, lab in ev:
        if only and k not in only:
            continue
        m, c, _ = KINDS[k]
        x = t(n)
        ax.axvline(x, color=c, lw=0.7, ls=(0, (2, 3)), alpha=0.55, zorder=1)
        need = (14 + 6.2 * len(lab)) / px if labels else 12 / px
        lane = next((i for i, e in enumerate(lanes) if e < x), len(lanes) - 1)
        lanes[lane] = x + need
        yy = y - 0.062 * lane
        ax.plot([x], [yy], marker=m, color=c, ms=7, mec=SURF, mew=1.2, transform=ax.get_xaxis_transform(),
                clip_on=False, zorder=5)
        if labels:
            ax.text(x + 0.008, yy, lab, transform=ax.get_xaxis_transform(), fontsize=7.5, color=INK2,
                    va='center', ha='left', zorder=6)

def draw_phones(ax, r, y=0.03, size=7.5):
    ph = r['ph']
    for i, (n, name, dur, feats) in enumerate(ph):
        x0 = t(n)
        x1 = t(ph[i + 1][0]) if i + 1 < len(ph) else t(r['f'][-1]['n'] + 1)
        if name != '_':
            ax.axvline(x0, color=GRID, lw=0.6, zorder=0)
        stressed = (feats & 7) >= 2 and name not in ('_',)
        w = (x1 - x0)
        xl = ax.get_xlim()
        if name == '_' or w * ax.bbox.width / (xl[1] - xl[0]) < 5.5 * len(name) + 3:
            continue
        ax.text((x0 + x1) / 2, y, name, transform=ax.get_xaxis_transform(), ha='center',
                va='bottom', fontsize=size, color=INK if stressed else MUTED,
                fontweight='bold' if (feats & 7) >= 5 else 'normal', zorder=6)

def event_legend(ax, kinds, loc='lower right', **kw):
    h = [Line2D([], [], marker=KINDS[k][0], color=KINDS[k][1], ls='none', ms=7, mec=SURF, label=KINDS[k][2])
         for k in kinds]
    return ax.legend(handles=h, loc=loc, fontsize=8, **kw)

def used_kinds(r):
    seen = []
    for _, _, k, _ in classify(r['ev']):
        if k not in seen:
            seen.append(k)
    order = list(KINDS)
    return sorted(seen, key=order.index)

import textwrap
def header(fig, title, sub, left=0.07, size=13):
    fig.text(left, 0.985, title, fontsize=size, fontweight='bold', va='top', ha='left')
    w = int(fig.get_figwidth() * 12.0)
    fig.text(left, 0.985 - 0.33 / fig.get_figheight(), textwrap.fill(sub, w), fontsize=9.5, color=INK2, va='top',
             ha='left', linespacing=1.35)

LINES = {
    'voiced': dict(color=INK, lw=2.4, label='F0 where voiced (AV > 0)'),
    'sent':   dict(color=S[6], lw=1.4, alpha=0.5, label='F0 sent to the DSP (every frame)'),
    'base':   dict(color=MUTED, lw=1.2, ls='--', label='baseline'),
    'hat':    dict(color=S[0], lw=1.4, drawstyle='steps-post', label='baseline + hat'),
}
def bottom_legend(fig, lines, kinds, y=0.035, ncol=None):
    h = [Line2D([], [], **{k: v for k, v in LINES[l].items() if k != 'drawstyle'}) for l in lines]
    h += [Line2D([], [], marker=KINDS[k][0], color=KINDS[k][1], ls='none', ms=7, mec=SURF, label=KINDS[k][2])
          for k in sorted(kinds, key=list(KINDS).index)]
    fig.legend(handles=h, loc='lower center', bbox_to_anchor=(0.5, y), ncol=ncol or min(len(h), 5), fontsize=8.5,
               columnspacing=1.8, handlelength=2.2)

def finish(fig, name, note=None):
    if note:
        fig.text(0.01, 0.005, note, fontsize=7.5, color=MUTED, ha='left', va='bottom')
    fig.savefig(os.path.join(OUT, name), dpi=150)
    plt.close(fig)
    print('wrote', name)

NOTE = 'DECtalk DTC-01 v1.8. Data: the C rebuild of the ROM\'s speech code (T0 identical to the ROM frame for frame), ' \
       'one point per 6.4 ms frame.'


def xs(r):
    return [t(f['n']) for f in r['f']]

def std_lines(ax, r, which=('base', 'sent', 'voiced')):
    x = xs(r)
    if 'base' in which:
        ax.plot(x, [f['base'] / 10 for f in r['f']], **LINES['base'])
    if 'hat' in which:
        ax.plot(x, [(f['base'] + f['hat']) / 10 for f in r['f']], **LINES['hat'])
    if 'sent' in which:
        ax.plot(x, col(r, 'hz'), **LINES['sent'])
    if 'voiced' in which:
        ax.plot(x, voiced_hz(r), **LINES['voiced'])

# =====================================================================================================
# 1. anatomy: the terms pht0draw sums, the smoother, the output
def g1():
    txt = '[:np] The old man sat in a rocker.'
    r = run(txt)
    x = xs(r)
    fig, (a, b) = plt.subplots(2, 1, figsize=(12, 9), sharex=True, gridspec_kw=dict(height_ratios=[1.35, 1]))
    fig.subplots_adjust(left=0.07, right=0.985, top=0.87, bottom=0.15, hspace=0.1)
    header(fig, 'Anatomy of a hat pattern: "The old man sat in a rocker." (Perfect Paul)',
           'Every 6.4 ms pht0draw adds four terms (baseline, hat, stress impulse, segmental) into a target, smooths it '
           'with a two-pole low-pass, subtracts the glottal-stop dip, and scales the result for the voice (Paul, '
           'ap 120 and pr 100, is scaled 1:1). The markers along the top are the F0 commands phtiming wrote, at the '
           'frame pht0draw reads each one. Phone labels: bold = primary stress, black = secondary, grey = unstressed.')
    a.plot(x, [f['base'] / 10 for f in r['f']], **LINES['base'])
    a.plot(x, [(f['base'] + f['hat']) / 10 for f in r['f']], **LINES['hat'])
    a.plot(x, [f['fin'] / 10 for f in r['f']], color=S[3], lw=1.0, drawstyle='steps-post')
    a.plot(x, col(r, 'hz'), **LINES['sent'])
    a.plot(x, voiced_hz(r), **LINES['voiced'])
    a.set_ylabel('F0 (Hz)')
    a.set_ylim(50, 212)
    draw_events(a, r)
    draw_phones(a, r)
    dips = [f for f in r['f'] if f['dip'] and f['n'] < 60]
    if dips:
        d = min(dips, key=lambda f: f['hz'])
        a.annotate('glottal-stop dip before\nthe vowel-initial "old"', xy=(t(d['n']) + 0.01, d['hz'] + 3),
                   xytext=(t(d['n']) + 0.07, 72), fontsize=8, color=INK2,
                   arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
    a.annotate('sentence reset: the baseline\nrestarts at bf and falls again', xy=(1.69, 115),
               xytext=(1.47, 165), fontsize=8, color=INK2, arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
    b.axhline(0, color=MUTED, lw=0.8)
    b.plot(x, [f['hat'] / 10 for f in r['f']], color=S[0], lw=1.8, drawstyle='steps-post', label='hat (tarhat)')
    b.plot(x, [f['imp'] / 10 for f in r['f']], color=S[1], lw=1.8, drawstyle='steps-post',
           label='impulses (tarimp = 2 × command, 16 frames): stress, final lowering')
    b.plot(x, [f['seg'] / 10 for f in r['f']], color=S[2], lw=1.8, drawstyle='steps-post',
           label='segmental (f0segtars, ¼ when unstressed)')
    b.plot(x, [f['dip'] / 10 for f in r['f']], color=S[7], lw=1.8, label='glottal-stop dip (applied after the smoother)')
    b.set_ylabel('term (Hz above the baseline)')
    b.set_xlabel('time (s)')
    b.legend(loc='lower left', ncol=2, fontsize=8)
    b.set_ylim(-68, 64)
    h = [Line2D([], [], color=S[3], lw=1.0, label='target f0in (sum of the terms)')]
    bl = [Line2D([], [], **{k: v for k, v in LINES[l].items() if k != 'drawstyle'}) for l in ('base', 'hat')]
    ln = [Line2D([], [], **LINES[l]) for l in ('sent', 'voiced')]
    ev = [Line2D([], [], marker=KINDS[k][0], color=KINDS[k][1], ls='none', ms=7, mec=SURF, label=KINDS[k][2])
          for k in used_kinds(r)]
    fig.legend(handles=bl + h + ln + ev, loc='lower center', bbox_to_anchor=(0.5, 0.025), ncol=4, fontsize=8.5,
               columnspacing=1.8)
    finish(fig, '01_hat_pattern_anatomy.png', NOTE)

# =====================================================================================================
# 2. the smoother: step and impulse responses, with the ROM's Q14 arithmetic
def q14(a, b):
    return (a * b) >> 14

def g2():
    def filt(inp):
        l1 = l2 = 1150 << 3
        out = []
        for v in inp:
            l1 = q14(0x3000, v) + q14(0x3a00, l1)
            l2 = q14(0x600, l1) + q14(0x3a00, l2)
            out.append((l2 >> 3) / 10)
        return out
    N = 70
    base = 1150
    step = [base + (200 if i >= 10 else 0) for i in range(N)]
    fall = [base + (-300 if i >= 10 else 0) for i in range(N)]
    imp = [base + (562 if 10 <= i < 26 else 0) for i in range(N)]
    imp2 = [base + (142 if 10 <= i < 26 else 0) for i in range(N)]
    x = [t(i) * 1000 for i in range(N)]
    fig, (a, b) = plt.subplots(1, 2, figsize=(12, 5))
    fig.subplots_adjust(left=0.07, right=0.985, top=0.8, bottom=0.14, wspace=0.18)
    header(fig, 'The F0 smoother: what one command does',
           'Two one-pole low-passes in series, both with pole 0x3A00/0x4000 = 0.906 (time constant about 10 frames, '
           '64 ms); the gains 0.75 and 0.094 make the DC gain 8, undone by >> 3. Computed here with the ROM\'s Q14 '
           'arithmetic from a 115 Hz rest; dashed = the command as pht0draw adds it, solid = the F0 that comes out.')
    a.plot(x, [v / 10 for v in step], color=S[0], lw=1.2, drawstyle='steps-post', ls='--', label='input: hat rise +20 Hz')
    a.plot(x, filt(step), color=S[0], lw=2.2, label='output')
    a.plot(x, [v / 10 for v in fall], color=S[7], lw=1.2, drawstyle='steps-post', ls='--',
           label='input: hat fall −30 Hz')
    a.plot(x, filt(fall), color=S[7], lw=2.2, label='output')
    a.set_title('Steps (hat rise and fall)')
    a.set_xlabel('time (ms)'); a.set_ylabel('F0 (Hz)')
    a.legend(fontsize=8, loc='center right')
    pk1, pk2 = max(filt(imp)) - 115, max(filt(imp2)) - 115
    b.plot(x, [v / 10 for v in imp], color=S[1], lw=1.2, drawstyle='steps-post', ls='--',
           label='input: 1st primary stress, command 281 = +56.2 Hz for 16 frames')
    b.plot(x, filt(imp), color=S[1], lw=2.2, label='output: peak +%.1f Hz' % pk1)
    b.plot(x, [v / 10 for v in imp2], color=S[2], lw=1.2, drawstyle='steps-post', ls='--',
           label='input: 4th primary stress, command 71 = +14.2 Hz')
    b.plot(x, filt(imp2), color=S[2], lw=2.2, label='output: peak +%.1f Hz' % pk2)
    b.set_title('Impulses (stress)')
    b.set_xlabel('time (ms)')
    b.set_ylim(110, 192)
    b.legend(fontsize=8, loc='upper right')
    for axx in (a, b):
        axx.axvline(x[10], color=MUTED, lw=0.8, ls=':')
        axx.text(x[10] + 1, 0.02, 'command read', transform=axx.get_xaxis_transform(), fontsize=7.5, color=INK2)
    finish(fig, '02_smoother_response.png', 'f0las1 = 0.75·in + 0.906·f0las1;   f0las2 = 0.094·f0las1 + 0.906·f0las2;'
           '   F0 = f0las2 / 8   (pht0draw, src/speech/ph_frame.c).')

# =====================================================================================================
# 3. sentence types
def g3():
    rows = [('Statement', '[:np] You are going home.'),
            ('Yes/no question', '[:np] You are going home?'),
            ('Exclamation', '[:np] You are going home!')]
    fig, axs = plt.subplots(3, 1, figsize=(12, 10.5), sharex=True, sharey=True)
    fig.subplots_adjust(left=0.07, right=0.985, top=0.885, bottom=0.12, hspace=0.2)
    header(fig, 'One sentence, three terminators',
           'The terminator sets the boundary strength: "." and "!" 0x200, "?" 0x380. In a statement the hat falls 51.2 Hz, '
           'to 31.2 Hz under the baseline. In a question it falls 32 Hz, to 12 Hz under it, then gets a +50.2 Hz impulse and returns to the baseline: in '
           'v1.8 that gives a bump, not a high final rise. "!" makes the syllable that carries the hat fall emphatic: '
           'its impulse grows from +18.2 to +56.2 Hz and comes without the usual 4-frame delay, and the syllable is '
           'longer.')
    kinds = []
    for ax, (name, txt) in zip(axs, rows):
        r = run(txt)
        std_lines(ax, r)
        ax.set_title('%s: "%s"' % (name, txt[6:]), fontsize=10.5)
        ax.set_ylabel('F0 (Hz)')
        ax.set_ylim(50, 222)
        draw_events(ax, r)
        draw_phones(ax, r)
        kinds += [k for k in used_kinds(r) if k not in kinds]
    axs[-1].set_xlabel('time (s)')
    bottom_legend(fig, ['base', 'sent', 'voiced'], kinds, y=0.02, ncol=5)
    finish(fig, '03_sentence_types.png', NOTE)

# =====================================================================================================
# 4. phrases, continuation, declination and the reset
def g4():
    txt = '[:np] When the sun went down behind the hills, the air grew cold. We went inside.'
    r = run(txt)
    fig, ax = plt.subplots(figsize=(15, 6.6))
    fig.subplots_adjust(left=0.055, right=0.99, top=0.82, bottom=0.2)
    header(fig, 'Clauses and sentences: "When the sun went down behind the hills, the air grew cold. We went '
           'inside."',
           'Each clause (dttask sends a clause at the comma and at each period) gets its own hat. The comma clause '
           'ends with a fall to 12 Hz under the baseline, a +10.2 Hz continuation impulse and a return to the baseline. The baseline is not '
           'reset at the comma: it has fallen to ef (100 Hz) and stays there. The period\'s command 0 resets it to bf '
           '(115 Hz) at the start of the pause, so the next sentence starts from wherever it has fallen to by then. '
           'Within a hat the stress impulses shrink: 281, 141, 91, 71.', left=0.055)
    std_lines(ax, r, ('base', 'hat', 'sent', 'voiced'))
    for c in r['cl'][1:]:
        ax.axvline(t(c), color=INK2, lw=1.0)
        ax.text(t(c) + 0.012, 0.3, 'next clause', transform=ax.get_xaxis_transform(), rotation=90, fontsize=7.5,
                color=INK2, va='bottom')
    ax.set_ylim(50, 222)
    ax.set_ylabel('F0 (Hz)'); ax.set_xlabel('time (s)')
    draw_events(ax, r)
    draw_phones(ax, r, size=7)
    bottom_legend(fig, ['base', 'hat', 'sent', 'voiced'], used_kinds(r), y=0.03, ncol=5)
    finish(fig, '04_clauses_and_reset.png', NOTE)

# =====================================================================================================
# 5. voices
def g5():
    txt = 'The old man sat in a rocker.'
    voices = [('np', 'Paul', 'ap 120, pr 100, as 100'), ('nh', 'Harry', 'ap 78, pr 50, as 100'),
              ('nb', 'Betty', 'ap 222, pr 160, as 50'), ('nk', 'Kit', 'ap 306, pr 180, as 40')]
    fig, ax = plt.subplots(figsize=(12, 6.4))
    fig.subplots_adjust(left=0.07, right=0.74, top=0.8, bottom=0.1)
    header(fig, 'The same sentence in four voices',
           'The rules make one contour around a 120 Hz reference; the voice then maps it: F0 = (F0 − 120 Hz) × pr/100 '
           '+ ap. as scales the hat fall and final lowering inside the rules (Betty and Kit fall less). Only Paul has '
           'a falling baseline; the other voices have bf = ef = 107 Hz (Ursula 50/107), so their baseline is flat.')
    for i, (v, name, lab) in enumerate(voices):
        r = run('[:%s] %s' % (v, txt), lead=GREETING + ' [:%s] Ready.' % v)
        x = xs(r)
        y = voiced_hz(r)
        ax.plot(x, y, color=S[i], lw=2.2, label='%s (%s)' % (name, lab))
        pts = [(xx, yy) for xx, yy in zip(x, y) if yy is not None]
        ax.text(pts[-1][0] + 0.02, pts[-1][1], name, color=INK, fontsize=9, va='center')
    ax.set_yscale('log')
    ax.set_yticks([50, 60, 80, 100, 120, 150, 200, 250, 300, 400])
    ax.get_yaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
    ax.minorticks_off()
    ax.set_ylabel('F0 (Hz, log scale), voiced frames'); ax.set_xlabel('time (s)')
    ax.legend(loc='upper left', bbox_to_anchor=(1.01, 1), fontsize=8.5)
    finish(fig, '05_voices.png', NOTE)

# =====================================================================================================
# 6. pitch range and assertiveness
def g6():
    txt = 'The old man sat in a rocker.'
    fig, (a, b) = plt.subplots(1, 2, figsize=(13, 5.6), sharey=True)
    fig.subplots_adjust(left=0.06, right=0.99, top=0.8, bottom=0.12, wspace=0.06)
    header(fig, 'Pitch range and assertiveness (Paul, "The old man sat in a rocker.")',
           'pr scales every excursion from 120 Hz after the rules, baseline included (at pr 0 the voice is a monotone '
           'at ap). as scales only the hat fall and the final lowering, inside the rules, so it changes the end of '
           'the sentence and nothing before it.', left=0.06)
    for i, pr in enumerate([0, 50, 100, 200]):
        r = run('[:np :dv pr %d] %s' % (pr, txt))
        a.plot(xs(r), voiced_hz(r), color=S[i], lw=2, label='pr %d%s' % (pr, ' (Paul)' if pr == 100 else ''))
    a.set_title('Pitch range  [:dv pr n]')
    a.set_ylabel('F0 (Hz), voiced frames'); a.set_xlabel('time (s)')
    a.legend(fontsize=8.5, loc='upper right')
    for i, asv in enumerate([0, 50, 100]):
        r = run('[:np :dv as %d] %s' % (asv, txt))
        b.plot(xs(r), voiced_hz(r), color=S[i], lw=2, label='as %d%s' % (asv, ' (Paul)' if asv == 100 else ''))
    b.set_title('Assertiveness  [:dv as n]')
    b.set_xlabel('time (s)')
    b.legend(fontsize=8.5, loc='upper right')
    finish(fig, '06_pitch_range_assertiveness.png', NOTE)

# =====================================================================================================
# 7. emphasis
def g7():
    rows = [('Plain', '[:np] I never said she stole my money.'),
            ('Emphatic stress on "she"', '[:np] I never said [sh"iy] stole my money.')]
    fig, axs = plt.subplots(2, 1, figsize=(12, 8), sharex=True, sharey=True)
    fig.subplots_adjust(left=0.07, right=0.985, top=0.86, bottom=0.15, hspace=0.2)
    header(fig, 'Emphatic stress (the phonemic " symbol)',
           'The emphatic syllable (stress 6) gets the count step plus 261, here +52.2 Hz, read at once instead of 4 '
           'frames after the vowel starts. Its presence sets f0_halfsteps for the clause, which halves every other '
           'stress impulse: 281 becomes 141, 141 becomes 71, 71 becomes 35.')
    kinds = []
    for ax, (name, txt) in zip(axs, rows):
        r = run(txt)
        std_lines(ax, r)
        ax.set_title('%s: "%s"' % (name, txt[6:]), fontsize=10.5)
        ax.set_ylabel('F0 (Hz)')
        ax.set_ylim(50, 235)
        draw_events(ax, r)
        draw_phones(ax, r)
        kinds += [k for k in used_kinds(r) if k not in kinds]
    axs[-1].set_xlabel('time (s)')
    bottom_legend(fig, ['base', 'sent', 'voiced'], kinds, y=0.025, ncol=5)
    finish(fig, '07_emphasis.png', NOTE)

# =====================================================================================================
# 8. singing and F0 targets
def g8():
    txt = '[:np] [_<,120>ah<500,13>ah<500,17>ah<500,20>ah<500,25>ah<300,200>ah<300,90>].'
    r = run(txt)
    x = xs(r)
    fig, ax = plt.subplots(figsize=(12, 6))
    fig.subplots_adjust(left=0.07, right=0.985, top=0.8, bottom=0.15)
    header(fig, 'Singing and F0 targets (f0mode 1)',
           'Input: [_<,120> ah<500,13> ah<500,17> ah<500,20> ah<500,25> ah<300,200> ah<300,90>]. A value 1-37 is a '
           'note (notetab: 1 = C2 = 64 Hz, one semitone per step): F0 moves by a fixed 1/16 of the interval per frame, '
           'then a vibrato of ±1.8 Hz with a period of 24 frames (6.5 Hz). A value 50-511 is a target in Hz, reached '
           'linearly over the phone. There is no hat, no smoother and no voice scaling in this mode.')
    ax.plot(x, col(r, 'hz'), **LINES['sent'])
    ax.plot(x, voiced_hz(r), **LINES['voiced'])
    for nn in (13, 17, 20, 25):
        ax.axhline(NOTETAB[nn - 1] / 10, color=MUTED, lw=0.6, ls=':')
    ax.set_ylim(80, 275)
    draw_events(ax, r)
    ax.set_ylabel('F0 (Hz)'); ax.set_xlabel('time (s)')
    ins = ax.inset_axes([0.035, 0.5, 0.3, 0.28])
    seg = [(xx, yy) for xx, yy in zip(x, col(r, 'hz')) if 0.75 <= xx <= 1.12]
    ins.plot([p[0] for p in seg], [p[1] for p in seg], color=INK, lw=1.3, marker='.', ms=3.5)
    ins.set_title('vibrato on note 17, one dot per frame', fontsize=8)
    ins.tick_params(labelsize=7)
    ins.set_facecolor(SURF)
    bottom_legend(fig, ['sent', 'voiced'], used_kinds(r), y=0.03, ncol=3)
    finish(fig, '08_singing_and_targets.png', NOTE)


def check():
    """pitchlog's T0 against the ROM's frame logs (native/spclog via decomp/scripts/make_reference.py)."""
    ref_dir = os.path.join(ROOT, 'decomp', 'reference')
    bad = total = 0
    for line in open(os.path.join(ref_dir, 'corpus.tsv'), encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if len(f) < 3 or f[2] != 'local' or '\\' in f[1]:    # host-line escapes need the host feed
            continue
        frames = os.path.join(ref_dir, f[0] + '.frames.tsv')
        if not os.path.exists(frames):
            continue
        ref = [x.split('\t') for x in open(frames)][1:]
        t0 = [int(x[6].split()[1], 16) for x in ref if x[3] == 'frame19']
        mine = [x['t0'] for x in run(f[1], trim=False)['f']]
        diff = sum(a != b for a, b in zip(t0, mine)) + abs(len(t0) - len(mine))
        print('%-14s %5d frames  %s' % (f[0], len(t0), 'ok' if diff == 0 else '%d differ' % diff))
        bad += diff != 0
        total += len(t0)
    print('%d frames; %s' % (total, 'all match' if bad == 0 else '%d entries differ' % bad))
    return bad == 0

if __name__ == '__main__':
    if '--check' in sys.argv[1:]:
        sys.exit(0 if check() else 1)
    for g in (g1, g2, g3, g4, g5, g6, g7, g8):
        g()
