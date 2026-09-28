"""make_formant_graphs.py - the graphs of FORMANT_SYSTEM.md, written as PNGs next to this script.

Every curve comes from formantlog (formantlog.c, built by build_formantlog.bat into build/formantlog/): the speech
engine in src/, checked against the ROM, run after the power-up greeting so that the state it carries between clauses
is the ROM's. The table values (targets, loci) are read from src/speech/ph_rom.c. Needs Python 3 with matplotlib.

usage: python formants_graphs/make_formant_graphs.py            write the PNGs
       python formants_graphs/make_formant_graphs.py --check    compare formantlog's frames with the ROM frame logs in
                                                                 decomp/reference/ (plain local-terminal corpus
                                                                 entries), and phdraw's terms with the words sent
Set FORMANTLOG to use another formantlog binary.
"""
import os, re, subprocess, sys, tempfile, textwrap
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = HERE
EXE = os.environ.get('FORMANTLOG') or os.path.join(ROOT, 'build', 'formantlog',
                                                   'formantlog.exe' if os.name == 'nt' else 'formantlog')
TSV = os.path.join(tempfile.gettempdir(), 'formantlog_run.tsv')
GREETING = 'DECtalk version 1.8 is running.'
FRAME = 0.0064

# colours: the categorical palette of pitch_graphs/, checked for colour-blind separation, light mode
SURF, INK, INK2, MUTED, GRID = '#fcfcfb', '#0b0b0b', '#52514e', '#8a8984', '#e4e3df'
S = ['#2a78d6', '#eb6834', '#1baf7a', '#eda100', '#e87ba4', '#008300', '#4a3aa7', '#e34948']
FCOL = {'F1': S[0], 'F2': S[1], 'F3': S[2], 'FNZ': S[6]}

plt.rcParams.update({
    'figure.facecolor': SURF, 'axes.facecolor': SURF, 'savefig.facecolor': SURF,
    'axes.edgecolor': MUTED, 'axes.labelcolor': INK2, 'xtick.color': INK2, 'ytick.color': INK2,
    'text.color': INK, 'font.size': 9.5, 'axes.titlesize': 11, 'axes.titleweight': 'bold',
    'axes.titlelocation': 'left', 'axes.grid': True, 'grid.color': GRID, 'grid.linewidth': 0.6,
    'axes.spines.top': False, 'axes.spines.right': False, 'legend.frameon': False,
    'lines.linewidth': 2, 'font.family': 'DejaVu Sans',
})

WORDS = ['T0', 'F1', 'F2', 'F3', 'FNZ', 'B1', 'B2', 'B3', 'AV', 'AH', 'A2', 'A3', 'A4', 'A5', 'A6', 'AB', 'TLT']
TRACKS = ['F1', 'F2', 'F3', 'FNZ', 'B1', 'B2', 'B3']
PFIELDS = ['table', 'tarlas', 'tarcur', 'tarend', 'tarnex', 'fbou', 'fdur', 'bbou', 'bdur']

# ---- the ROM tables, from the generated source ----
def _rom_tables():
    src = open(os.path.join(ROOT, 'src', 'speech', 'ph_rom.c')).read()
    out = {}
    for name in ('maltar', 'femtar', 'maleloc', 'femloc', 'plocu', 'begtyp', 'endtyp', 'maldip', 'femdip'):
        m = re.search(r'\b%s\[[^=]*=\s*\{(.*?)\n\};' % name, src, re.S)
        out[name] = [int(x) for x in re.findall(r'-?\d+', m.group(1))]
    return out
ROM = _rom_tables()
NPH = 56
PHONES = ('_ iy ih ey eh ae aa ay aw ah ao ow oy uh uw rr yu ax ix ir er ar or ur w y r l hx rx lx m n nx el em en '
          'f v th dh s z sh zh p b t d k g dx tx q ch jh').split()
def code(name):
    return PHONES.index(name)
def locus(cons, vtype, male=True):
    """setloc's locus set for consonant cons before/after a vowel of type vtype (1-3, 5 -> 3): {F1..F3: (locus,
    pct, durtran)}"""
    t = 3 if vtype == 5 else vtype
    s = ROM['plocu'][(t - 1) * NPH + code(cons)]
    if s == 0:
        return None
    tab = ROM['maleloc' if male else 'femloc']
    return [tuple(tab[(s - 1) * 9 + 3 * k: (s - 1) * 9 + 3 * k + 3]) for k in range(3)]
def table_target(row, name, male=True):
    return ROM['maltar' if male else 'femtar'][row * NPH + code(name)]

# ---- running formantlog ----
_cache = {}
def run(text, lead=GREETING, trim=True):
    key = (text, lead, trim)
    if key in _cache:
        return _cache[key]
    subprocess.run([EXE, TSV, text, lead], check=True)
    r = {'f': [], 'ph': [], 'cl': []}
    for line in open(TSV):
        p = line.rstrip('\n').split('\t')
        if p[0] == 'F':
            f = dict(n=int(p[1]))
            f.update(zip(WORDS, map(int, p[2:19])))
            for k, tr in zip(('F1', 'F2', 'F3'), p[19:22]):
                v = list(map(int, tr.split(',')))
                f['t' + k] = dict(tar=v[0], dip=v[1], fwd=v[2], bwd=v[3], special=v[4])
            r['f'].append(f)
        elif p[0] == 'P':
            ph = dict(n=int(p[1]), name=p[2], dur=int(p[3]), feats=int(p[4]), male=int(p[5]) == 1)
            for k, tr in zip(TRACKS, p[6:13]):
                v = tr.split(',')
                d = dict(zip(PFIELDS, map(int, v[:9])))
                d['breaks'] = [] if v[9] == '-' else list(map(int, v[9].split(':')))
                ph[k] = d
            r['ph'].append(ph)
        elif p[0] == 'C':
            r['cl'].append(int(p[1]))
    voiced = [f['n'] for f in r['f'] if f['AV'] > 0]
    if voiced and trim:
        end = voiced[-1] + 12
        r['f'] = [f for f in r['f'] if f['n'] <= end]
        r['ph'] = [p for p in r['ph'] if p['n'] <= end]
        r['cl'] = [c for c in r['cl'] if c <= end]
    _cache[key] = r
    return r

def t(n):
    return n * FRAME

def xs(r):
    return [t(f['n']) for f in r['f']]

def col(r, k):
    return [f[k] for f in r['f']]

def voiced(r, k):
    return [f[k] if f['AV'] > 0 else None for f in r['f']]

def phone_at(r, name, nth=0):
    hits = [p for p in r['ph'] if p['name'] == name]
    return hits[nth]

# ---- drawing helpers ----
def header(fig, title, sub, left=0.07, size=13):
    fig.text(left, 0.985, title, fontsize=size, fontweight='bold', va='top', ha='left')
    w = int(fig.get_figwidth() * 12.0)
    fig.text(left, 0.985 - 0.33 / fig.get_figheight(), textwrap.fill(sub, w), fontsize=9.5, color=INK2, va='top',
             ha='left', linespacing=1.35)

def finish(fig, name, note=None):
    if note:
        fig.text(0.01, 0.005, note, fontsize=7.5, color=MUTED, ha='left', va='bottom')
    fig.savefig(os.path.join(OUT, name), dpi=150)
    plt.close(fig)
    print('wrote', name)

NOTE = 'DECtalk DTC-01 v1.8. Data: the C rebuild of the ROM\'s speech code (every frame word identical to the ROM\'s), ' \
       'one point per 6.4 ms frame.'

def draw_phones(ax, r, y=0.015, size=7.5, lines=True, x0=0.0):
    ph = r['ph']
    xl = ax.get_xlim()
    for i, p in enumerate(ph):
        a = t(p['n']) - x0
        b = (t(ph[i + 1]['n']) if i + 1 < len(ph) else t(r['f'][-1]['n'] + 1)) - x0
        if lines and p['name'] != '_':
            ax.axvline(a, color=GRID, lw=0.7, zorder=0)
        if p['name'] == '_' or (b - a) * ax.bbox.width / (xl[1] - xl[0]) < 5.5 * len(p['name']) + 3:
            continue
        if b < xl[0] or a > xl[1]:
            continue
        st = p['feats'] & 7
        ax.text((a + b) / 2, y, p['name'], transform=ax.get_xaxis_transform(), ha='center', va='bottom',
                fontsize=size, color=INK if st >= 2 else MUTED, fontweight='bold' if st >= 5 else 'normal', zorder=6)

def formant_lines(ax, r, tracks=('F1', 'F2', 'F3'), x0=0.0, label=True, lw=2.2, sent=True, **kw):
    """the voiced part solid (what is heard), every frame's value thin and pale"""
    x = [v - x0 for v in xs(r)]
    for k in tracks:
        c = kw.get('color', FCOL.get(k, INK))
        if sent:
            ax.plot(x, col(r, k), color=c, lw=0.9, alpha=0.35)
        ax.plot(x, voiced(r, k), color=c, lw=lw, label=k if label else None)


def check():
    """formantlog's frames against the ROM's frame logs (native/spclog via decomp/scripts/make_reference.py), and
    phdraw's logged terms against the words it sent."""
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
        want = [[int(w, 16) for w in x[6].split()[1:18]] for x in ref if x[3] == 'frame19']
        r = run(f[1], trim=False)
        mine = [[f_[k] & 0xffff for k in WORDS] for f_ in r['f']]
        diff = sum(a != b for a, b in zip(want, mine)) + abs(len(want) - len(mine))
        terms = 0
        for fr in r['f']:
            for k in ('F1', 'F2', 'F3'):
                d = fr['t' + k]
                s = d['tar'] + d['dip'] + d['fwd'] + d['bwd']
                if not d['special'] and s != fr[k] and not (k != 'F1' and s > fr[k]):   # F2/F3: f2max, f3max
                    terms += 1
        print('%-14s %5d frames  %s%s' % (f[0], len(want), 'ok' if diff == 0 else '%d differ' % diff,
                                           '' if terms == 0 else ', %d frames whose terms do not add up' % terms))
        bad += diff != 0 or terms != 0
        total += len(want)
    print('%d frames; %s' % (total, 'all match' if bad == 0 else '%d entries differ' % bad))
    return bad == 0

def target_line(r, k, x0=0.0):
    """phdraw's target: tarcur plus the diphthong line (coarticulated), per frame"""
    return [v - x0 for v in xs(r)], [f['t' + k]['tar'] + f['t' + k]['dip'] for f in r['f']]

def table_poly(p, k):
    """the track's table values before coarticulation, as (frame offsets, values): a flat line, or a vowel's lines
    between its (rescaled) break points"""
    row = {'F1': 0, 'F2': 1, 'F3': 2, 'B1': 3, 'B2': 4, 'B3': 5}[k]
    tar = ROM['maltar' if p['male'] else 'femtar']
    dip = ROM['maldip' if p['male'] else 'femdip']
    v = tar[row * NPH + code(p['name'])]
    br = p[k]['breaks']
    if v >= -1 or not br:
        return [0, p['dur']], [p[k]['table']] * 2
    idx = -v
    xs_, ys_ = [0], [dip[idx]]
    for j, b in enumerate(br):
        xs_.append(b)
        ys_.append(dip[idx + 2 * j])
    return xs_, ys_

def line_legend(ax_or_fig, items, **kw):
    h = [Line2D([], [], **d) for d in items]
    return ax_or_fig.legend(handles=h, **kw)


# =====================================================================================================
# 1. overview: a sentence's F1-F3 against phdraw's targets
def g1():
    txt = '[:np] The old man sat in a rocker.'
    r = run(txt)
    fig, ax = plt.subplots(figsize=(13, 6.6))
    fig.subplots_adjust(left=0.065, right=0.985, top=0.82, bottom=0.17)
    header(fig, 'Formant tracks of a sentence: "The old man sat in a rocker." (Perfect Paul)',
           'Each phone has a target per formant (dashed: phdraw\'s target, the table value moved 15 % toward the '
           'neighbours, plus the diphthong lines of vowels). Each 6.4 ms frame, the value sent to the DSP is that target plus '
           'a forward transition that fades out after the phone starts and a backward transition that grows before '
           'it ends. The two meet at a boundary value, so the tracks are continuous. Thick = voiced frames (what you hear), '
           'thin = the value sent while AV = 0 (closures, silence, aspiration and frication use it too).', left=0.065)
    for k in ('F1', 'F2', 'F3'):
        x, y = target_line(r, k)
        ax.plot(x, y, color=FCOL[k], lw=1.0, ls=(0, (4, 2.5)), alpha=0.9)
    formant_lines(ax, r)
    ax.set_ylim(0, 3200)
    ax.set_ylabel('frequency (Hz)')
    ax.set_xlabel('time (s)')
    ax.set_xlim(0, t(r['f'][-1]['n']))
    draw_phones(ax, r, y=0.965)
    items = [dict(color=FCOL[k], lw=2.2, label='%s, voiced frames' % k) for k in ('F1', 'F2', 'F3')]
    items += [dict(color=INK2, lw=0.9, alpha=0.5, label='value sent while AV = 0'),
              dict(color=INK2, lw=1.0, ls=(0, (4, 2.5)), label='phdraw\'s target (tarcur + diphthong line)')]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.025), ncol=5, fontsize=8.5)
    finish(fig, '01_formant_tracks.png', NOTE)

# =====================================================================================================
# 2. anatomy of one track: target, coarticulation, forward and backward transitions, locus, diphthong lines
def g2():
    txt = "[:np] [ax d'ay]."
    r = run(txt)
    fig, (a, b) = plt.subplots(2, 1, figsize=(12.5, 9), sharex=True, gridspec_kw=dict(height_ratios=[1.6, 1]))
    fig.subplots_adjust(left=0.075, right=0.985, top=0.855, bottom=0.14, hspace=0.08)
    header(fig, 'Anatomy of a formant track: F2 of [ax d\'ay] ("a die", Perfect Paul)',
           'phsettar runs at each phone boundary and sets up every track; phdraw then draws the track frame by frame '
           'as target + diphthong line + forward transition + backward transition. The table target is moved 15 % '
           'toward the neighbours (coarticulation). Between /d/ and a vowel the boundary value comes from /d/\'s locus: '
           'locus + pct × (vowel target − locus). Before a class-2 vowel like /ay/ that is 1500 Hz with pct 0, the locus '
           'itself; the closure, whose two transitions both span the phone, runs straight between its boundary values. '
           '/ay/ is a table of straight lines whose break points are rescaled to the phone\'s duration.', left=0.075)
    x = xs(r)
    xt, yt = target_line(r, 'F2')
    ph = [p for p in r['ph'] if p['name'] != '_' or p['n'] == 0]
    for p in r['ph']:
        d = p['F2']
        x0, x1 = t(p['n']), t(p['n'] + p['dur'])
        if p['name'] == '_':
            continue
        tx, ty = table_poly(p, 'F2')
        a.plot([t(p['n'] + q) for q in tx], ty, color=MUTED, lw=2.5, alpha=0.6, solid_capstyle='butt')
        if d['fdur'] > 0:
            a.plot([x0, t(p['n'] + d['fdur'])], [960, 960], color=S[0], lw=4, solid_capstyle='butt')
            a.plot([x0], [d['fbou']], marker='>', color=S[0], ms=8, mec=SURF, zorder=6)
        if d['bdur'] > 0:
            a.plot([t(p['n'] + p['dur'] - d['bdur']), x1], [930, 930], color=S[7], lw=4, solid_capstyle='butt')
            a.plot([x1], [d['bbou']], marker='<', color=S[7], ms=8, mec=SURF, zorder=6)
        for bt in d['breaks'][:-1]:
            i = p['n'] + bt
            fr = next(f for f in r['f'] if f['n'] == i)
            a.plot([t(i)], [fr['tF2']['tar'] + fr['tF2']['dip']], marker='o', color=S[3], ms=7, mec=SURF, zorder=6)
    a.plot(x, col(r, 'F2'), color=FCOL['F2'], lw=0.9, alpha=0.35)
    a.plot(x, voiced(r, 'F2'), color=FCOL['F2'], lw=2.4)
    a.plot(xt, yt, color=INK2, lw=1.2, ls=(0, (4, 2.5)), zorder=5)
    dph = phone_at(r, 'd')
    lo = locus('d', ROM['begtyp'][code('ay')])[1]
    a.axhline(lo[0], color=S[6], lw=1.0, ls=':')
    a.text(0.565, lo[0] - 12, '/d/ F2 locus, class 2: %d Hz, pct %.2f' % (lo[0], lo[1] / 16384),
           color=INK2, fontsize=8, ha='right', va='top')
    ay = phone_at(r, 'ay')
    a.annotate('boundary value %d Hz:\nforward transition of /ay/ starts here' % ay['F2']['fbou'],
               xy=(t(ay['n']), ay['F2']['fbou']), xytext=(t(ay['n']) + 0.04, 1600), fontsize=8, color=INK2,
               arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
    a.annotate('/d/: a stop moves through its whole\nclosure (transitions span the phone)',
               xy=(t(dph['n'] + 6), 1505), xytext=(0.035, 1880), fontsize=8, color=INK2,
               arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
    a.set_ylabel('F2 (Hz)')
    a.set_ylim(900, 2100)
    a.set_xlim(0.03, 0.57)
    draw_phones(a, r, y=0.07, size=9)
    b.axhline(0, color=MUTED, lw=0.8)
    b.plot(x, [f['tF2']['fwd'] for f in r['f']], color=S[0], lw=1.8, drawstyle='steps-post',
           label='forward transition (ftran / 8): fades linearly to 0')
    b.plot(x, [f['tF2']['bwd'] for f in r['f']], color=S[7], lw=1.8, drawstyle='steps-post',
           label='backward transition (btran / 8): grows linearly from 0')
    b.plot(x, [f['tF2']['dip'] for f in r['f']], color=S[3], lw=1.8, drawstyle='steps-post',
           label='diphthong line (dipcum / 8), from the phone\'s first target')
    b.set_ylabel('term (Hz)')
    b.set_xlabel('time (s)')
    b.legend(loc='upper left', fontsize=8)
    b.set_ylim(-120, 700)
    items = [dict(color=FCOL['F2'], lw=2.4, label='F2 sent, voiced frames'),
             dict(color=FCOL['F2'], lw=0.9, alpha=0.4, label='F2 sent while AV = 0'),
             dict(color=MUTED, lw=2.5, alpha=0.6, label='table target (maltar, maldip)'),
             dict(color=INK2, lw=1.2, ls=(0, (4, 2.5)), label='phdraw\'s target (coarticulated + diphthong)'),
             dict(color=S[0], marker='>', ls='none', ms=8, mec=SURF, label='forward boundary value (bar: its frames)'),
             dict(color=S[7], marker='<', ls='none', ms=8, mec=SURF, label='backward boundary value (bar: its frames)'),
             dict(color=S[3], marker='o', ls='none', ms=7, mec=SURF, label='diphthong break point')]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=4, fontsize=8.5)
    finish(fig, '02_track_anatomy.png', NOTE)

# =====================================================================================================
# 3. diphthongs (and the "monophthongs", which are lines too)
def g3():
    words = [('iy', 'heed'), ('ey', 'hayed'), ('ay', 'hide'), ('oy', 'Hoyd'),
             ('uw', 'who\'d'), ('ow', 'hoed'), ('aw', 'how\'d'), ('yu', 'hued')]
    fig, axs = plt.subplots(2, 4, figsize=(14, 8.2), sharey=True)
    fig.subplots_adjust(left=0.06, right=0.99, top=0.83, bottom=0.12, hspace=0.3, wspace=0.07)
    header(fig, 'Vowels are drawn as straight lines between break points: [hx\'Vd] (Perfect Paul)',
           'Every vowel\'s F1-F3 (and some bandwidths) is a list of {value, time} points in maldip/femdip, joined by '
           'straight lines; the ROM has no single-target vowels. The times are for the vowel\'s inherent duration and are '
           'rescaled to the actual one (diph_time_scale). The first point '
           'is moved 15 % toward the previous phone and the others toward the next. /hx/ takes the vowel\'s own '
           'targets, so the vowel\'s formants are already in place during the aspiration.', left=0.06)
    for ax, (v, w) in zip(axs.flat, words):
        r = run("[:np] [hx'%sd]." % v)
        p = phone_at(r, v)
        x0 = t(p['n'])
        for k in ('F1', 'F2', 'F3'):
            x, y = target_line(r, k, x0)
            ax.plot(x, y, color=FCOL[k], lw=1.0, ls=(0, (4, 2.5)))
            for bt in p[k]['breaks'][:-1]:
                i = p['n'] + bt
                fr = next(f for f in r['f'] if f['n'] == i)
                ax.plot([t(i) - x0], [fr['t' + k]['tar'] + fr['t' + k]['dip']], marker='o', color=FCOL[k], ms=5.5,
                        mec=SURF, zorder=6)
        formant_lines(ax, r, x0=x0, label=False)
        ax.axvline(0, color=INK2, lw=0.8)
        ax.axvline(t(p['dur']), color=INK2, lw=0.8)
        ax.set_xlim(-0.12, t(p['dur']) + 0.14)
        ax.set_ylim(0, 3100)
        ax.set_title('%s  [hx\'%sd] "%s", %d ms' % (v, v, w, round(p['dur'] * 6.4)), fontsize=10)
        draw_phones(ax, r, lines=False, x0=x0, size=8)
    for ax in axs[:, 0]:
        ax.set_ylabel('frequency (Hz)')
    for ax in axs[1]:
        ax.set_xlabel('time from the vowel\'s start (s)')
    items = [dict(color=FCOL[k], lw=2.2, label=k) for k in ('F1', 'F2', 'F3')]
    items += [dict(color=INK2, lw=1.0, ls=(0, (4, 2.5)), label='phdraw\'s target line'),
              dict(color=INK2, marker='o', ls='none', ms=5.5, mec=SURF, label='break point (a line ends)'),
              dict(color=INK2, lw=0.8, label='vowel start / end')]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=6, fontsize=8.5)
    finish(fig, '03_diphthong_lines.png', NOTE)

# =====================================================================================================
# 4. consonant loci
def g4():
    vowels = [('iy', 1, 'front'), ('aa', 2, 'back unrounded'), ('uw', 3, 'rounded')]
    cons = [('b', S[0]), ('d', S[1]), ('g', S[2])]
    fig, axs = plt.subplots(1, 3, figsize=(14, 6.6), sharey=True)
    fig.subplots_adjust(left=0.06, right=0.99, top=0.76, bottom=0.17, wspace=0.07)
    header(fig, 'Consonant loci: F2 and F3 of [ax C\'V] for /b d g/ before three vowels (Perfect Paul)',
           'At a consonant-vowel boundary (setloc) the boundary value is not the midpoint of the two targets but '
           'locus + pct × (vowel target − locus), with locus, pct and the transition length from a table chosen by '
           'the consonant and the vowel\'s class (begtyp/endtyp 1 front, 2 back unrounded, 3 rounded). pct 0 puts the '
           'boundary on the locus itself. /g/ has one locus set per vowel class: its F2 and F3 start close together '
           '(the "velar pinch") and follow the vowel. Aligned at the vowel\'s start; before it, the /ax/ and the closure.',
           left=0.06)
    for ax, (v, vt, vname) in zip(axs, vowels):
        for c, colr in cons:
            r = run("[:np] [ax %s'%s]." % (c, v))
            p = phone_at(r, v)
            x0 = t(p['n'])
            x = [q - x0 for q in xs(r)]
            ax.plot(x, col(r, 'F2'), color=colr, lw=0.9, alpha=0.35)
            ax.plot(x, voiced(r, 'F2'), color=colr, lw=2.2)
            ax.plot(x, col(r, 'F3'), color=colr, lw=0.9, alpha=0.35, ls='--')
            ax.plot(x, voiced(r, 'F3'), color=colr, lw=1.6, ls='--')
            lo = locus(c, vt)
            for k, sym in ((1, 'F2'), (2, 'F3')):
                L, pct, dur = lo[k]
                if L > 1:
                    ax.plot([-0.045, 0], [L, L], color=colr, lw=2.5, alpha=0.55, solid_capstyle='butt')
                ax.plot([0], [p[sym]['fbou']], marker='>', color=colr, ms=8, mec=SURF, zorder=6)
            L2, pct2, _ = lo[1]
            yy = {'b': 0.095, 'd': 0.055, 'g': 0.015}[c]
            ax.plot([0.025], [yy + 0.012], marker='s', color=colr, ms=6, transform=ax.transAxes)
            ax.text(0.045, yy, '/%s/: F2 locus %s, pct %.2f' % (c, '%d Hz' % L2 if L2 > 1 else '—', pct2 / 16384),
                    transform=ax.transAxes, ha='left', fontsize=8, color=INK2)
        ax.axvline(0, color=INK2, lw=0.8)
        ax.set_xlim(-0.2, 0.3)
        ax.set_ylim(500, 3000)
        ax.set_title('before /%s/ (%s, type %d)' % (v, vname, vt), fontsize=10.5)
        ax.set_xlabel('time from the vowel\'s start (s)')
    axs[0].set_ylabel('frequency (Hz)')
    items = [dict(color=c, lw=2.2, label='/%s/' % n) for n, c in cons]
    items += [dict(color=INK2, lw=2.2, label='F2'), dict(color=INK2, lw=1.6, ls='--', label='F3'),
              dict(color=INK2, lw=2.5, alpha=0.55, label='locus (table value)'),
              dict(color=INK2, marker='>', ls='none', ms=8, mec=SURF, label='boundary value into the vowel')]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=7, fontsize=8.5)
    finish(fig, '04_consonant_loci.png', NOTE)

# =====================================================================================================
# 5. glides, liquids and /h/
def g5():
    rows = [["[ax w'aa]", "[ax y'aa]", "[ax r'aa]", "[ax l'aa]"],
            ["[hx'iy]", "[hx'aa]", "[hx'uw]", "[aa hx'aa]"]]
    names = ['w', 'y', 'r', 'l', 'hx', 'hx', 'hx', 'hx']
    fig, axs = plt.subplots(2, 4, figsize=(14, 8.2), sharey=True)
    fig.subplots_adjust(left=0.06, right=0.99, top=0.82, bottom=0.12, hspace=0.32, wspace=0.07)
    header(fig, 'Glides, liquids and /h/ (Perfect Paul)',
           'Top: between a vowel and a sonorant consonant (w y r l, featb 0x200) the boundary value lies 3/4 of the '
           'way toward the consonant, and the vowel side moves slowly (9 frames, scaled with the vowel\'s length) while '
           'the consonant side moves fast (3 frames): the consonant is a brief target the vowels glide into and out '
           'of. After /l/, F1 starts 80 Hz higher. Bottom: /hx/ has no targets of its own (-1 in the table); it takes '
           'the next phone\'s, so its aspiration already has the vowel\'s formants, and between vowels F1-F3 glide '
           'straight through it.', left=0.06)
    for i, (ax, txt) in enumerate(zip(axs.flat, rows[0] + rows[1])):
        r = run('[:np] %s.' % txt)
        c = phone_at(r, names[i], -1 if i == 7 else 0)
        x0 = t(c['n'])
        for k in ('F1', 'F2', 'F3'):
            x, y = target_line(r, k, x0)
            ax.plot(x, y, color=FCOL[k], lw=1.0, ls=(0, (4, 2.5)))
            for p in r['ph']:
                if p['name'] == '_':
                    continue
                if p['F1']['fdur'] > 0 or k != 'F1':
                    ax.plot([t(p['n']) - x0], [p[k]['fbou']], marker='>', color=FCOL[k], ms=6.5, mec=SURF, zorder=6)
        formant_lines(ax, r, x0=x0, label=False)
        ax.axvspan(0, t(c['dur']), color=MUTED, alpha=0.1, lw=0)
        ax.set_xlim(-0.12, 0.33)
        ax.set_ylim(0, 3100)
        ax.set_title('%s' % txt, fontsize=10.5)
        draw_phones(ax, r, lines=False, x0=x0, size=8)
    for ax in axs[:, 0]:
        ax.set_ylabel('frequency (Hz)')
    for ax in axs[1]:
        ax.set_xlabel('time from the consonant\'s start (s)')
    items = [dict(color=FCOL[k], lw=2.2, label=k) for k in ('F1', 'F2', 'F3')]
    items += [dict(color=INK2, lw=1.0, ls=(0, (4, 2.5)), label='phdraw\'s target line'),
              dict(color=INK2, marker='>', ls='none', ms=6.5, mec=SURF, label='boundary value at a phone start'),
              dict(color=MUTED, lw=8, alpha=0.25, label='the consonant')]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=6, fontsize=8.5)
    finish(fig, '05_glides_liquids_h.png', NOTE)

# =====================================================================================================
# 6. nasals
def g6():
    txt = '[:np] Many men.'
    r = run(txt)
    x = xs(r)
    fig, (a, b, c) = plt.subplots(3, 1, figsize=(12.5, 10), sharex=True,
                                  gridspec_kw=dict(height_ratios=[1.6, 0.9, 0.6]))
    fig.subplots_adjust(left=0.075, right=0.985, top=0.87, bottom=0.12, hspace=0.1)
    header(fig, 'Nasals: "Many men." (Perfect Paul)',
           'The nasal zero FNZ sits at 300 Hz, where it cancels the DSP\'s fixed nasal pole (298 Hz), and jumps to 527 Hz in a nasal '
           'murmur. Before a nasal it rises to 350 Hz over up to 20 frames; after one it starts at 350 Hz and falls '
           'back over 16. F1 jumps into a nasal (no transition) and, out of one, starts at 425 Hz or more and moves '
           'over 16 frames. B1 widens by 70 Hz across the nasal-vowel boundary. F2 and F3 move through the whole murmur.',
           left=0.075)
    for p in r['ph']:
        if p['name'] in ('m', 'n'):
            for axx in (a, b, c):
                axx.axvspan(t(p['n']), t(p['n'] + p['dur']), color=S[6], alpha=0.07, lw=0)
    for k in ('F1', 'F2', 'F3'):
        xt, yt = target_line(r, k)
        a.plot(xt, yt, color=FCOL[k], lw=1.0, ls=(0, (4, 2.5)))
    formant_lines(a, r)
    a.plot(x, col(r, 'FNZ'), color=FCOL['FNZ'], lw=2.2, drawstyle='steps-post', label='FNZ')
    a.axhline(300, color=FCOL['FNZ'], lw=0.7, ls=':')
    a.text(x[-1], 300 - 25, 'fnz_default 300 Hz', color=INK2, fontsize=8, ha='right', va='top')
    a.set_ylabel('frequency (Hz)')
    a.set_ylim(0, 2800)
    draw_phones(a, r, y=0.965, size=9)
    a.legend(loc='center right', bbox_to_anchor=(1, 0.36), fontsize=8.5, ncol=4)
    b.plot(x, col(r, 'B1'), color=S[0], lw=2, drawstyle='steps-post', label='B1')
    b.plot(x, col(r, 'B2'), color=S[1], lw=1.4, drawstyle='steps-post', label='B2')
    b.set_ylabel('bandwidth (Hz)')
    b.set_ylim(0, 450)
    b.legend(loc='upper center', fontsize=8.5, ncol=2)
    c.plot(x, col(r, 'AV'), color=INK, lw=1.8, drawstyle='steps-post')
    c.set_ylim(0, 80)
    c.set_ylabel('AV (dB)')
    c.set_xlabel('time (s)')
    items = [dict(color=INK2, lw=1.0, ls=(0, (4, 2.5)), label='phdraw\'s target line'),
             dict(color=S[6], lw=8, alpha=0.15, label='nasal murmur (m, n)')]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=2, fontsize=8.5)
    finish(fig, '06_nasals.png', NOTE)

# =====================================================================================================
# 7. stops: closure, burst, aspiration, voicing onset
AMPS = [('AV', INK, 2.2), ('AH', S[6], 1.8), ('A2', S[0], 1.4), ('A3', S[1], 1.4), ('A4', S[2], 1.4),
        ('A5', S[3], 1.4), ('A6', S[4], 1.4), ('AB', S[7], 1.8)]
def g7():
    words = [["[ax p'aa]", "[ax t'aa]", "[ax k'aa]"], ["[ax b'aa]", "[ax d'aa]", "[ax g'aa]"]]
    fig, axs = plt.subplots(2, 3, figsize=(14, 8.4), sharey=True, sharex=True)
    fig.subplots_adjust(left=0.06, right=0.99, top=0.82, bottom=0.13, hspace=0.25, wspace=0.07)
    header(fig, 'Stops: the amplitude tracks of [ax C\'aa] (Perfect Paul)',
           'A stop sends its burst in the last 1-3 frames of the closure (stop_burst_dur) as the parallel-branch '
           'amplitudes A2-A6 and the bypass AB, set per stop and following vowel class (malamp); a second burst frame '
           'is 10 dB weaker, and the vowel\'s forward transition fades the rest out from 15 dB below. A voiceless stop before a stressed vowel is followed by 6 frames of aspiration (AH 58 dB, '
           'AV 0, B1 +250 and B2 +80 Hz), 4 before an unstressed one; voicing then starts 8 dB low and rises 1 dB per '
           'frame. A voiced stop keeps a decaying voice bar (AV) through its closure. Aligned at the vowel\'s start.',
           left=0.06)
    for ax, txt in zip(axs.flat, words[0] + words[1]):
        r = run('[:np] %s.' % txt)
        p = phone_at(r, 'aa')
        c = r['ph'][[q['name'] for q in r['ph']].index('aa') - 1]
        x0 = t(p['n'])
        x = [q - x0 for q in xs(r)]
        ax.axvspan(t(c['n']) - x0, 0, color=MUTED, alpha=0.1, lw=0)
        for k, colr, lw in AMPS:
            y = col(r, k)
            if max(y[-60:] + y[:1] + [0]) == 0 and max(y) == 0:
                continue
            ax.plot(x, y, color=colr, lw=lw, drawstyle='steps-post', label=k)
        ax.set_xlim(-0.16, 0.17)
        ax.set_ylim(0, 80)
        ax.set_title('%s: closure %d ms' % (txt, round(c['dur'] * 6.4)), fontsize=10.5)
        ax.axvline(0, color=INK2, lw=0.8)
        vot = [f for f in r['f'] if f['n'] >= p['n'] and f['AH'] > 0 and f['AV'] == 0]
        if vot:
            ax.annotate('aspiration\n%d frames' % len(vot), xy=(t(vot[-1]['n']) - x0 + 0.003, 58),
                        xytext=(0.07, 73), fontsize=7.5, color=INK2, arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
        draw_phones(ax, r, lines=False, x0=x0, size=8, y=0.015)
    for ax in axs[:, 0]:
        ax.set_ylabel('amplitude (dB)')
    for ax in axs[1]:
        ax.set_xlabel('time from the vowel\'s start (s)')
    items = [dict(color=c, lw=lw, label=k + {'AV': ' voicing', 'AH': ' aspiration', 'AB': ' bypass'}.get(k, ''))
             for k, c, lw in AMPS]
    items.append(dict(color=MUTED, lw=8, alpha=0.25, label='closure'))
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=9, fontsize=8.5)
    finish(fig, '07_stops_amplitudes.png', NOTE)

# =====================================================================================================
# 8. voices: the male and female tables, the F2/F3 ceilings, and head size in the DSP
def head(f, hs, pivot):
    s = (200 - hs) * 41
    return (pivot * (4096 - s) + f * s) >> 12
def g8():
    txt = 'We were away a year ago.'
    fig, (a, b) = plt.subplots(1, 2, figsize=(14, 6.4), sharey=True)
    fig.subplots_adjust(left=0.06, right=0.99, top=0.78, bottom=0.17, wspace=0.06)
    header(fig, 'Voices: two target tables, and head size in the DSP',
           'Left: the 68000 has one set of tables for male voices (maltar, maldip, maleloc) and one for female voices '
           '(femtar, femdip, femloc), chosen by sex; the rules are the same. F2 and F3 are capped (female 3050/3350 '
           'Hz, male 2750/3050; lower if f4 is set low). Right: head size is applied by the DSP, not by the 68000: '
           'it scales F1 about 256 Hz, F2 about 512 Hz and F3 about 0 by (200 − hs)/100 (and F4/F5 in setspdef), so a '
           'larger head lowers the formants. Kit is female with hs 85, Harry male with hs 120.', left=0.06)
    for name, v, colr in (('Paul (male tables)', 'np', S[0]), ('Betty (female tables)', 'nb', S[1])):
        r = run('[:%s] %s' % (v, txt), lead=GREETING + ' [:%s] Ready.' % v)
        for k, ls in (('F1', '-'), ('F2', '-'), ('F3', '-')):
            a.plot(xs(r), voiced(r, k), color=colr, lw=1.9, ls=ls, label=name if k == 'F1' else None)
    for cap, lab in ((2750, 'male F2 cap 2750'), (3050, 'female F2 cap 3050 = male F3 cap'), (3350, 'female F3 cap 3350')):
        a.axhline(cap, color=MUTED, lw=0.8, ls=':')
        a.text(0.01, cap + 20, lab, color=INK2, fontsize=7.5, transform=a.get_yaxis_transform())
    a.set_title('Paul and Betty: F1, F2, F3 as sent (voiced frames)', fontsize=10.5)
    a.set_xlabel('time (s)'); a.set_ylabel('frequency (Hz)')
    a.set_ylim(0, 3800)
    a.legend(loc='upper center', fontsize=8.5, ncol=2)
    r = run('[:np] ' + txt)
    x = xs(r)
    for hs, colr in ((80, S[2]), (100, S[0]), (120, S[7])):
        for k, pivot in (('F1', 256), ('F2', 512), ('F3', 0)):
            y = [head(f[k], hs, pivot) if f['AV'] > 0 else None for f in r['f']]
            b.plot(x, y, color=colr, lw=1.9, label=('hs %d%s' % (hs, ' (Paul)' if hs == 100 else '')) if k == 'F1' else None)
    b.set_title('Paul with [:dv hs 80], 100 and 120: F1-F3 as the DSP uses them', fontsize=10.5)
    b.set_xlabel('time (s)')
    b.legend(loc='upper center', fontsize=8.5, ncol=3)
    draw_phones(a, run('[:np] ' + txt), y=0.015, size=7, lines=False)
    draw_phones(b, r, y=0.015, size=7, lines=False)
    finish(fig, '08_voices_head_size.png', NOTE + ' Right panel: the 68000\'s frames with the DSP\'s FRAME_COEFFS scaling '
           'applied (dsp_synth.c).')

# =====================================================================================================
# 9. speaking rate: durations scale, transitions partly, the frame grid not
def g9():
    rates = [(90, S[0]), (180, S[1]), (400, S[2])]
    fig, axs = plt.subplots(3, 1, figsize=(12.5, 9.4), sharex=True, sharey=True)
    fig.subplots_adjust(left=0.07, right=0.985, top=0.84, bottom=0.12, hspace=0.18)
    header(fig, 'Speaking rate: "Why go away?" at :ra 90, 180 and 400 (Perfect Paul)',
           'The rate changes the phones\' durations; the formant rules follow them. Diphthong break points are '
           'rescaled with the vowel (diph_time_scale). Transitions of vowels and sonorants are scaled by 0.5 + 0.5 × '
           '(duration / inherent duration) (0.36 + 0.5 × for the backward one), then capped at the phone\'s length and '
           '20 frames; those of obstruents are not. The frame grid stays at 6.4 ms, so at fast rates a vowel is mostly '
           'transition and the tracks rarely sit on a target.')
    for ax, (ra, colr) in zip(axs, rates):
        r = run('[:np :ra %d] Why go away?' % ra)
        for k in ('F1', 'F2', 'F3'):
            xt, yt = target_line(r, k)
            ax.plot(xt, yt, color=FCOL[k], lw=1.0, ls=(0, (4, 2.5)))
        formant_lines(ax, r, label=False)
        for p in r['ph']:
            for k in ('F1', 'F2', 'F3'):
                for bt in p[k]['breaks'][:-1]:
                    i = p['n'] + bt
                    fr = next((f for f in r['f'] if f['n'] == i), None)
                    if fr:
                        ax.plot([t(i)], [fr['t' + k]['tar'] + fr['t' + k]['dip']], marker='o', color=FCOL[k], ms=5,
                                mec=SURF, zorder=6)
        ax.set_title(':ra %d words/min, %.2f s' % (ra, t(r['f'][-1]['n'])), fontsize=10.5)
        ax.set_ylabel('frequency (Hz)')
        ax.set_ylim(0, 3000)
        draw_phones(ax, r, y=0.965, size=8)
    axs[-1].set_xlabel('time (s)')
    axs[0].set_xlim(0, max(t(run('[:np :ra 90] Why go away?')['f'][-1]['n']), 0.5))
    items = [dict(color=FCOL[k], lw=2.2, label=k) for k in ('F1', 'F2', 'F3')]
    items += [dict(color=INK2, lw=1.0, ls=(0, (4, 2.5)), label='phdraw\'s target line'),
              dict(color=INK2, marker='o', ls='none', ms=5, mec=SURF, label='diphthong break point')]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=5, fontsize=8.5)
    finish(fig, '09_speaking_rate.png', NOTE)

GRAPHS = [g1, g2, g3, g4, g5, g6, g7, g8, g9]

if __name__ == '__main__':
    if '--check' in sys.argv[1:]:
        sys.exit(0 if check() else 1)
    for g in GRAPHS:
        g()
