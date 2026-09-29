"""make_smoothing_graphs.py - the graphs of SMOOTHING_SYSTEM.md (graphs 10-15), written as PNGs next to this script.

The forward and backward transitions ("smoothing") of phsettar and phdraw. The curves come from formantlog, as in
make_formant_graphs.py (whose helpers, palette and ROM tables this script uses); the rule names come from
fwd_rule/bwd_rule below, a Python copy of phsettar's transition rules for F1-F3, which --check compares with
what the C code (checked against the ROM) set up.

usage: python formants_graphs/make_smoothing_graphs.py            write the PNGs
       python formants_graphs/make_smoothing_graphs.py --check    fwd_rule/bwd_rule against formantlog's phones
"""
import os, re, sys
sys.dont_write_bytecode = True                  # no __pycache__ in the repo from the import below
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import make_formant_graphs as fg
from make_formant_graphs import (plt, t, xs, col, voiced, header, finish, draw_phones, line_legend, target_line,
                                 S, SURF, INK, INK2, MUTED, GRID, FCOL, NOTE, PHONES, code)

# ---- the ROM tables the rules read, from the generated source ----
def _tables():
    src = open(os.path.join(fg.ROOT, 'src', 'speech', 'ph_rom.c')).read()
    out = {}
    for name in ('featb', 'begtyp', 'endtyp', 'plocu', 'maleloc', 'femloc', 'divtab_ext', 'inhdr'):
        m = re.search(r'\b%s\[[^=]*=\s*\{(.*?)\n\};' % name, src, re.S)
        out[name] = [int(x, 0) for x in re.findall(r'-?(?:0x[0-9a-fA-F]+|\d+)', m.group(1))]
    return out
TB = _tables()
FEATB, BEGTYP, ENDTYP, PLOCU = TB['featb'], TB['begtyp'], TB['endtyp'], TB['plocu']
DIVTAB = TB['divtab_ext'][20:]
NPH = 56
F_VOICED, F_SON, F_OBST, F_PLOS, F_NASAL, F_SONCONS, F_PAL = 0x02, 0x10, 0x20, 0x40, 0x80, 0x200, 0x800

def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v

def q14(a, b):
    return s16((s16(a) * s16(b)) >> 14)

def feat(name):
    return FEATB[code(name)]

# ---- phsettar's transition rules for F1-F3 (partyp 3), step by step ----
def setloc(cons, typ, typ_other, k, target, male):
    """setloc 0xc420: (bouval, durtran, locus, pct) or None when the rule does not apply"""
    if k >= 3 or typ != 4 or typ_other == 4:
        return None
    tt = 3 if typ_other == 5 else typ_other
    s = PLOCU[(tt - 1) * NPH + cons]
    if s == 0:
        return None
    tab = TB['maleloc' if male else 'femloc']
    L, pct, dur = tab[(s - 1) * 9 + 3 * k: (s - 1) * 9 + 3 * k + 3]
    if typ_other != tt and k != 0 and FEATB[cons] & F_PAL == 0:
        pct = s16((pct >> 1) + 0x2000)
    return s16(q14(pct, target - L) + L), dur, L, pct

def fwd_rule(p, k):
    """the forward transition of track k (0-2 = F1-F3) of phone record p: (bouval, durtran, bou_rule, dur_rule)"""
    cur, las = code(p['name']), code(p['las'])
    fc, fl = FEATB[cur], FEATB[las]
    d = p['F%d' % (k + 1)]
    b, n, rb, rn = (d['tarcur'] + d['tarlas']) >> 1, 5, 'midpoint', 'default 5'
    if fc & F_SON:
        if fc & F_SONCONS == 0:
            n, rn = 8, 'sonorant 8'
            if fl & F_SONCONS:
                b, n, rb, rn = (d['tarlas'] + b) >> 1, 9, '3/4 toward the glide/liquid', 'after w y r l: 9'
                if p['las'] == 'l' and k == 0:
                    b, rb = b + 80, '3/4 toward /l/, +80 Hz'
        else:
            n, rn = 5, 'glide/liquid 5'
            if fl & F_SONCONS == 0:
                b, n, rb, rn = (d['tarcur'] + b) >> 1, 3, '3/4 toward itself', 'glide/liquid 3'
    if cur == 0:
        b, n, rb, rn = d['tarlas'], p['dur'], 'silence: from the last value', 'silence: its length'
    else:
        for args, who in (((las, ENDTYP[las], BEGTYP[cur], k, d['tarcur']), 'locus of /%s/' % p['las']),
                          ((cur, BEGTYP[cur], ENDTYP[las], k, d['tarlas']), 'locus of /%s/' % p['name'])):
            r = setloc(*args, male=p['male'])
            if r:
                b, n, rb, rn = r[0], r[1], who, who
        if fc & F_OBST:
            n, rn = (p['dur'], 'plosive: its length') if fc & F_PLOS else (5, 'obstruent 5')
        if fc & F_NASAL:
            n, rn = (0, 'nasal F1: jump') if k == 0 else (p['dur'], 'nasal: its length')
        if k == 0 and fl & F_NASAL:
            n, rn = 16, 'F1 after a nasal: 16'
            if b < 425:
                b, rb = 425, 'F1 after a nasal: >= 425'
    if fc & F_OBST == 0 and n > 0:
        n = q14(n, p['fscale']) + 1
    n = min(n, p['dur'], 20)
    return max(b, 0), n, rb, rn

def bwd_rule(p, k):
    """the backward transition of track k of phone record p: (bouval, durtran, bou_rule, dur_rule)"""
    cur, nex = code(p['name']), code(p['nex'])
    fc, fn = FEATB[cur], FEATB[nex]
    d = p['F%d' % (k + 1)]
    b, n, rb, rn = (d['tarnex'] + d['tarend']) >> 1, 4, 'midpoint', 'default 4'
    if fc & F_SON:
        n, rn = 8, 'sonorant 8'
        if fc & F_SONCONS == 0:
            if fn & F_SONCONS:
                b, n, rb, rn = (d['tarnex'] + b) >> 1, 9, '3/4 toward the glide/liquid', 'before w y r l: 9'
                if p['nex'] == 'l' and k == 0:
                    b, rb = b + 80, '3/4 toward /l/, +80 Hz'
        else:
            n, rn = 6, 'glide/liquid 6'
            if fn & F_SONCONS == 0:
                b, n, rb, rn = (d['tarend'] + b) >> 1, 3, '3/4 toward itself', 'glide/liquid 3'
    if nex == 0:
        n, rn = 0, 'before silence: none'
    else:
        for args, who in (((nex, BEGTYP[nex], ENDTYP[cur], k, d['tarend']), 'locus of /%s/' % p['nex']),
                          ((cur, ENDTYP[cur], BEGTYP[nex], k, d['tarnex']), 'locus of /%s/' % p['name'])):
            r = setloc(*args, male=p['male'])
            if r:
                b, n, rb, rn = r[0], r[1], who, who
        if fc & F_OBST:
            n, rn = (p['dur'], 'plosive: its length') if fc & F_PLOS else (5, 'obstruent 5')
        if fc & F_NASAL:
            n, rn = (0, 'nasal F1: jump') if k == 0 else (p['dur'], 'nasal: its length')
        if k == 0 and fn & F_NASAL:
            n, rn = 20, 'F1 before a nasal: 20'
            if b < 480:
                b, rb = 480, 'F1 before a nasal: >= 480'
    if fc & F_OBST == 0 and n > 0:
        n = q14(n, p['bscale']) + 1
    n = min(n, 20, p['dur'])
    return max(b, 0), n, rb, rn

def fwd_sent(p, k):
    """what phdraw does with the forward rule: the value it starts the phone at, and the frames (ftran = dftran x n)"""
    b, n = fwd_rule(p, k)[:2]
    tc = p['F%d' % (k + 1)]['tarcur']
    if n <= 0 or b == tc:
        return tc, 0
    df = q14(s16((b - tc) * 8), DIVTAB[n])
    if df == 0:
        return tc, 0
    return tc + (s16(df * n) >> 3), n

def bwd_sent(p, k):
    """the value the backward transition heads for (reached one frame after the phone), and its frames"""
    b, n = bwd_rule(p, k)[:2]
    te = p['F%d' % (k + 1)]['tarend']
    df = q14(s16((b - te) * 8), DIVTAB[n]) if n > 0 else 0
    return te + ((df * n) >> 3), n

# ---- running formantlog, with the context fields ----
def run(text, lead=fg.GREETING, trim=True):
    r = fg.run(text, lead, trim)
    if r['ph'] and 'las' not in r['ph'][0]:
        rows = [l.rstrip('\n').split('\t') for l in open(fg.TSV) if l.startswith('P\t')]
        by_n = {int(x[1]): x for x in rows}
        for p in r['ph']:
            x = by_n[p['n']]
            p['las'], p['nex'], p['nex2'] = x[13], x[14], x[15]
            p['fscale'], p['bscale'] = int(x[16]), int(x[17])
    return r

CHECK_TEXTS = [
    "[:np] [ax b'iy b ax] [ax d'iy d ax] [ax g'iy g ax] [ax b'aa b ax] [ax d'aa d ax] [ax g'aa g ax].",
    "[:np] [ax b'uw b ax] [ax d'uw d ax] [ax g'uw g ax] [ax p'ae t ax] [ax k'ow k ax] [ax t'ay t ax].",
    "[:np] [ax w'aa] [ax y'aa] [ax r'aa] [ax l'aa] [aa w ax] [aa l ax] [ax b l'aa] [ax g w'aa].",
    "[:np] [m'aa m ax] [n'iy n ax] [s'aa s ax] [sh'uw sh ax] [ch'iy ch ax] [f'ow v ax] [th'ih dh ax].",
    "[:nb] [ax b'iy b ax] [ax g'uw g ax] [ax d'aa d ax].",
]

def check():
    """fwd_rule/bwd_rule against phsettar: every F1-F3 transition of the test texts and the plain corpus entries"""
    texts = list(CHECK_TEXTS)
    corpus = os.path.join(fg.ROOT, 'decomp', 'reference', 'corpus.tsv')
    if os.path.exists(corpus):
        for line in open(corpus, encoding='utf-8'):
            f = line.rstrip('\n').split('\t')
            if len(f) >= 3 and f[2] == 'local' and '\\' not in f[1]:
                texts.append(f[1])
    n = bad = 0
    for txt in texts:
        r = run(txt, trim=False)
        for p in r['ph']:
            for k in range(3):
                d = p['F%d' % (k + 1)]
                fb, fn = fwd_sent(p, k)
                bb, bn = bwd_sent(p, k)
                ok = (fb, fn) == (d['fbou'], d['fdur']) and (bb, bn) == (d['bbou'], d['bdur'])
                n += 1
                if not ok:
                    bad += 1
                    if bad <= 20:
                        print('%-40.40s %-3s F%d  fwd %s/%s want %s/%s   bwd %s/%s want %s/%s' % (
                            txt, p['name'], k + 1, fb, fn, d['fbou'], d['fdur'], bb, bn, d['bbou'], d['bdur']))
    print('%d texts, %d phone tracks (F1-F3): %s' % (len(texts), n, 'all match' if bad == 0 else '%d differ' % bad))
    return bad == 0

# =====================================================================================================
# drawing helpers
FWD_C, BWD_C, DIP_C = S[0], S[7], S[3]

def phone_at(r, name, nth=0):
    return [p for p in r['ph'] if p['name'] == name][nth]

def frame(r, n):
    return next(f for f in r['f'] if f['n'] == n)

def short(rule):
    return {'midpoint': 'midpoint', 'silence: from the last value': 'from the last value',
            '3/4 toward the glide/liquid': '3/4 toward the consonant', '3/4 toward /l/, +80 Hz': '3/4 toward /l/ +80 Hz',
            '3/4 toward itself': '3/4 toward itself', 'F1 after a nasal: >= 425': 'at least 425 Hz',
            'F1 before a nasal: >= 480': 'at least 480 Hz'}.get(rule, rule)

def rule_line(p, side):
    """one line for a phone's forward ('fwd') or backward ('bwd') transitions of F1-F3: rule(s) and frames"""
    rules, frames = [], []
    for k in range(3):
        b, n, rb, rn = (fwd_rule if side == 'fwd' else bwd_rule)(p, k)
        rules.append(short(rb))
        frames.append('jump' if n == 0 and 'nasal' in rn else str(n))
    if len(set(rules)) == 1:
        txt = rules[0]
    elif rules[1] == rules[2]:
        txt = 'F1 %s, F2-F3 %s' % (rules[0], rules[1])
    else:
        txt = ', '.join('F%d %s' % (k + 1, x) for k, x in enumerate(rules))
    return '%s; %s frames' % (txt, '/'.join(frames))

def event(ax, x, y, num, dx=0, dy=0):
    ax.annotate(str(num), xy=(x, y), xytext=(x + dx, y + dy), fontsize=8, fontweight='bold', color=SURF, ha='center',
                va='center', zorder=9, bbox=dict(boxstyle='circle,pad=0.25', fc=INK, ec='none'),
                arrowprops=dict(arrowstyle='-', color=INK, lw=0.8) if dx or dy else None)

# =====================================================================================================
# 10. the mechanism, frame by frame, with its events
def g10():
    txt = "[:np] [ax d'aag ax]."
    r = run(txt)
    fig, (a, b) = plt.subplots(2, 1, figsize=(13.5, 9.6), sharex=True, gridspec_kw=dict(height_ratios=[1.7, 1]))
    fig.subplots_adjust(left=0.065, right=0.70, top=0.86, bottom=0.12, hspace=0.07)
    header(fig, 'Forward and backward smoothing, frame by frame: F2 of [ax d\'aag ax] (Perfect Paul)',
           'At each phone boundary phsettar gives every track two ramps: a forward transition that starts at a boundary '
           'value and fades into the target, and a backward transition that leaves the target toward the next boundary '
           'value. phdraw adds them to the target each 6.4 ms frame. Between a stop and a vowel the boundary value is the '
           'stop\'s locus rule (here /d/ and /g/ before and after a back unrounded vowel: their F2 loci, pct 0).',
           left=0.065, size=13)
    x = [f['n'] for f in r['f']]
    a.plot(x, [f['F2'] for f in r['f']], color=FCOL['F2'], lw=0.9, alpha=0.4)
    a.plot(x, voiced(r, 'F2'), color=FCOL['F2'], lw=2.2)
    a.plot(x, [f['F2'] for f in r['f']], ls='none', marker='o', ms=3.2, color=FCOL['F2'], mec=SURF, mew=0.5, zorder=5)
    xt = x
    a.plot(xt, [f['tF2']['tar'] + f['tF2']['dip'] for f in r['f']], color=INK2, lw=1.1, ls=(0, (4, 2.5)), zorder=4)
    for p in r['ph']:
        if p['name'] == '_':
            continue
        d = p['F2']
        a.axvline(p['n'] - 0.5, color=MUTED, lw=0.7, zorder=0)
        a.plot([p['n']], [d['fbou']], marker='>', color=FWD_C, ms=8.5, mec=SURF, zorder=7)
        if d['bdur'] > 0:
            a.plot([p['n'] + p['dur']], [d['bbou']], marker='<', color=BWD_C, ms=8.5, mfc=SURF, mew=1.6, zorder=6)
        yb = 0.115
        if d['fdur'] > 0:
            a.plot([p['n'] - 0.35, p['n'] + d['fdur'] - 0.65], [yb, yb], color=FWD_C, lw=4.5, solid_capstyle='butt',
                   transform=a.get_xaxis_transform())
        if d['bdur'] > 0:
            a.plot([p['n'] + p['dur'] - d['bdur'] - 0.35, p['n'] + p['dur'] - 0.65], [yb - 0.035, yb - 0.035],
                   color=BWD_C, lw=4.5, solid_capstyle='butt', transform=a.get_xaxis_transform())
    aa, g = phone_at(r, 'aa'), phone_at(r, 'g')
    d = aa['F2']
    n0, n1 = aa['n'], aa['n'] + aa['dur']
    f_last = frame(r, n1 - 1)
    event(a, n0, d['fbou'], 1, -3, 90)
    event(a, n0 + d['fdur'], frame(r, n0 + d['fdur'])['F2'], 2, 0, -95)
    event(a, n1 - d['bdur'], frame(r, n1 - d['bdur'])['F2'], 3, 0, -95)
    event(a, n1 - 1, f_last['F2'], 4, -4, 80)
    event(a, g['n'], g['F2']['fbou'], 5, 3, 80)
    dph = phone_at(r, 'd')
    a.annotate('/d/ and /g/ closures: both ramps span the phone, so the\ntrack runs straight between its two '
               'boundary values', xy=(dph['n'] + 6, frame(r, dph['n'] + 6)['F2'] + 8), xytext=(n0 + 1, 1790),
               fontsize=8, color=INK2, arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
    a.annotate('', xy=(g['n'] + 5, frame(r, g['n'] + 5)['F2'] + 8), xytext=(n0 + 20, 1790),
               arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
    a.set_ylabel('F2 (Hz)')
    a.set_ylim(1050, 1950)
    a.set_xlim(4, phone_at(r, 'ax', 1)['n'] + 12)
    for p in r['ph']:
        if p['name'] != '_':
            st = p['feats'] & 7
            a.text(p['n'] + p['dur'] / 2 - 0.5, 0.955, p['name'], transform=a.get_xaxis_transform(), ha='center',
                   fontsize=10, fontweight='bold' if st >= 5 else 'normal', color=INK if st >= 2 else MUTED)
    fw = [f['tF2']['fwd'] for f in r['f']]
    bw = [f['tF2']['bwd'] for f in r['f']]
    dp = [f['tF2']['dip'] for f in r['f']]
    b.axhline(0, color=MUTED, lw=0.8)
    b.plot(x, fw, color=FWD_C, lw=1.8, marker='o', ms=3, label='forward term ftran/8')
    b.plot(x, bw, color=BWD_C, lw=1.8, marker='o', ms=3, label='backward term btran/8')
    b.plot(x, dp, color=DIP_C, lw=1.8, marker='o', ms=3, label='diphthong line dipcum/8')
    for p in r['ph']:
        if p['name'] != '_':
            b.axvline(p['n'] - 0.5, color=MUTED, lw=0.7, zorder=0)
    b.set_ylabel('term added to the target (Hz)')
    b.set_xlabel('frame (6.4 ms each)')
    b.set_ylim(-330, 470)
    b.legend(loc='upper left', fontsize=8.5, ncol=3)
    s_f = q14(s16((fwd_rule(aa, 1)[0] - d['tarcur']) * 8), DIVTAB[d['fdur']]) / 8
    s_b = q14(s16((bwd_rule(aa, 1)[0] - d['tarend']) * 8), DIVTAB[d['bdur']]) / 8
    b.annotate('forward: starts at bouval - target\n= %+d Hz, steps %+.1f Hz a frame' % (d['fbou'] - d['tarcur'], -s_f),
               xy=(n0 + 1, fw[x.index(n0 + 1)]), xytext=(n0 + 6, -280), fontsize=8, color=INK2,
               arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
    b.annotate('backward: starts at 0, grows %+.1f Hz a frame;\nthe last frame is one step short' % s_b,
               xy=(n1 - 2, bw[x.index(n1 - 2)]), xytext=(n1 - 36, 330), fontsize=8, color=INK2,
               arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
    ev = [
        (1, 'Frame %d: /aa/ starts. phsettar has run: the forward transition begins at the boundary value %d Hz '
            '(the /d/ F2 locus 1500 with pct 0, before a class-2 vowel), %+d Hz from the target.'
            % (n0, d['fbou'], d['fbou'] - d['tarcur'])),
        (2, 'Frame %d: after %d frames (the locus set\'s %d x ftran_scale %.2f, + 1) the forward term is 0: the track '
            'is on its target line (dashed), which moves a little: /aa/ is a line in maldip.'
            % (n0 + d['fdur'], d['fdur'], eff_locus('d', BEGTYP[code('aa')], 1)[2], aa['fscale'] / 16384)),
        (3, 'Frame %d = tbacktr (duration - %d; the /g/ set\'s %d frames x btran_scale %.2f, + 1): the backward term '
            'starts growing toward the next boundary value %d Hz (the /g/ F2 locus 1680, pct 0 after a class-2 vowel).'
            % (n1 - d['bdur'], d['bdur'], eff_locus('g', ENDTYP[code('aa')], 1)[2], aa['bscale'] / 16384, d['bbou'])),
        (4, 'Frame %d, the last of /aa/: %d Hz, one step (1/%d) short of the boundary value.'
            % (n1 - 1, f_last['F2'], d['bdur'])),
        (5, 'Frame %d: /g/ starts at its own forward boundary value, %d Hz, computed from the other side of the same '
            'boundary with the same locus rule, so the track is continuous.' % (g['n'], g['F2']['fbou'])),
    ]
    yy = 0.85
    fig.text(0.72, yy + 0.012, 'Events at the /aa/ phone', fontsize=10.5, fontweight='bold', va='bottom')
    import textwrap
    for num, s in ev:
        fig.text(0.72, yy, str(num), fontsize=8, fontweight='bold', color=SURF, va='top', ha='center',
                 bbox=dict(boxstyle='circle,pad=0.25', fc=INK, ec='none'))
        wrapped = textwrap.fill(s, 52)
        fig.text(0.735, yy + 0.002, wrapped, fontsize=8.5, color=INK2, va='top', linespacing=1.3)
        yy -= 0.028 * (wrapped.count('\n') + 1) + 0.022
    fig.text(0.72, yy - 0.01, textwrap.fill(
        'Per frame (phdraw): F2 = target + diphthong line + forward term + backward term (after tbacktr). The '
        'terms are kept x 8 (3 fraction bits); the forward one is rounded to a whole number of steps.', 58),
        fontsize=8.5, color=INK2, va='top', linespacing=1.3, style='italic')
    items = [dict(color=FCOL['F2'], lw=2.2, marker='o', ms=3.2, label='F2 sent, one dot per frame (pale: AV = 0)'),
             dict(color=INK2, lw=1.1, ls=(0, (4, 2.5)), label='target (tarcur + diphthong line)'),
             dict(color=FWD_C, marker='>', ls='none', ms=8.5, mec=SURF, label='forward boundary value (bar: its frames)'),
             dict(color=BWD_C, marker='<', ls='none', ms=8.5, mfc=SURF, mew=1.6,
                  label='backward boundary value (bar: its frames)')]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=4, fontsize=8.5)
    finish(fig, '10_smoothing_frame_by_frame.png', NOTE)

# =====================================================================================================
# 11. locus transitions: stop to vowel (CV) and vowel to stop (VC)
def eff_locus(cons, vclass, k, male=True):
    """setloc's (locus, pct) for consonant cons next to a vowel side of class vclass, with class 5's halving"""
    tt = 3 if vclass == 5 else vclass
    lo = fg.locus(cons, tt, male)
    if lo is None:
        return None
    L, pct, dur = lo[k]
    if vclass == 5 and k != 0 and feat(cons) & F_PAL == 0:
        pct = (pct >> 1) + 0x2000
    return L, pct / 16384, dur

def g11():
    stops = ['b', 'd', 'g']
    vowels = [('iy', 'front'), ('aa', 'back unrounded'), ('uw', 'rounded; ends as class 5')]
    fig, axs = plt.subplots(3, 3, figsize=(14, 12.5), sharey=True, sharex=True)
    fig.subplots_adjust(left=0.07, right=0.99, top=0.855, bottom=0.08, hspace=0.34, wspace=0.06)
    header(fig, 'Locus transitions: plosive to vowel (CV) and vowel to plosive (VC), [ax C\'VC] (Perfect Paul)',
           'setloc runs on both sides of every boundary. When one phone is a consonant (class 4) and the other a vowel, '
           'glide or liquid, the boundary value of F1-F3 is locus + pct x (vowel target - locus), with locus, pct and the '
           'ramp length from the consonant\'s set for the vowel\'s class: begtyp (how the vowel starts) for CV, endtyp '
           '(how it ends) for VC. pct 0 pins the boundary on the locus; pct 1 would put it on the vowel\'s own target. '
           '/uw/ ends in class 5, which uses the rounded set with half the pull on F2 and F3 (pct/2 + 0.5), so its VC '
           'differs from its CV. Aligned at the vowel\'s start; grey bands: the closures (the final stop is followed by the '
           '4-frame release vowel phtiming adds to a clause-final stop).', left=0.07)
    for i, c in enumerate(stops):
        for j, (v, vname) in enumerate(vowels):
            ax = axs[i][j]
            r = run("[:np] [ax %s'%s%s]." % (c, v, c))
            p = phone_at(r, v)
            c1, c2 = phone_at(r, c, 0), phone_at(r, c, 1)
            x0 = p['n']
            x = [f['n'] - x0 for f in r['f']]
            for q in (c1, c2):
                ax.axvspan(q['n'] - x0 - 0.5, q['n'] + q['dur'] - x0 - 0.5, color=MUTED, alpha=0.12, lw=0)
            lines = []
            for k, ls in ((1, '-'), (2, '--')):
                name = 'F%d' % (k + 1)
                colr = FCOL[name]
                ax.plot(x, [f[name] for f in r['f']], color=colr, lw=0.9, alpha=0.35, ls=ls)
                ax.plot(x, voiced(r, name), color=colr, lw=2.1, ls=ls)
                ax.plot([0], [p[name]['fbou']], marker='>', color=colr, ms=8, mec=SURF, zorder=7)
                ax.plot([p['dur']], [p[name]['bbou']], marker='<', color=colr, ms=8, mfc=SURF, mew=1.6, zorder=7)
                for q, vc, side in ((c1, BEGTYP[code(v)], 'CV'), (c2, ENDTYP[code(v)], 'VC')):
                    L, pct, dur = eff_locus(c, vc, k)
                    ax.plot([q['n'] - x0 - 0.5, q['n'] + q['dur'] - x0 - 0.5], [L, L], color=colr, lw=3, alpha=0.45,
                            solid_capstyle='butt')
                cv, vc_ = eff_locus(c, BEGTYP[code(v)], k), eff_locus(c, ENDTYP[code(v)], k)
                lines.append('%s  CV: locus %4d pct %.2f %2d fr   VC: locus %4d pct %.2f %2d fr' % (
                    name, cv[0], cv[1], p[name]['fdur'], vc_[0], vc_[1], p[name]['bdur']))
            ax.text(0.02, 0.03, '\n'.join(lines), transform=ax.transAxes, fontsize=7.2, color=INK2, va='bottom',
                    family='DejaVu Sans Mono', bbox=dict(fc=SURF, ec='none', alpha=0.85, pad=1.5))
            ax.set_xlim(-22, p['dur'] + 20)
            ax.set_ylim(400, 3100)
            ax.set_title('/%s/ + /%s/ (%s) + /%s/' % (c, v, vname, c), fontsize=10.5)
            for q in r['ph']:
                if q['name'] != '_' and -22 < q['n'] - x0 + q['dur'] / 2 < p['dur'] + 20:
                    ax.text(q['n'] - x0 + q['dur'] / 2 - 0.5, 0.94, q['name'], transform=ax.get_xaxis_transform(),
                            ha='center', fontsize=8.5, color=INK if q['feats'] & 7 >= 2 else MUTED,
                            fontweight='bold' if q['feats'] & 7 >= 5 else 'normal')
    for ax in axs[:, 0]:
        ax.set_ylabel('frequency (Hz)')
    for ax in axs[-1]:
        ax.set_xlabel('frames from the vowel\'s start (6.4 ms each)')
    items = [dict(color=FCOL['F2'], lw=2.1, label='F2'), dict(color=FCOL['F3'], lw=2.1, ls='--', label='F3'),
             dict(color=INK2, lw=3, alpha=0.45, label='locus of the set used (on the closure)'),
             dict(color=INK2, marker='>', ls='none', ms=8, mec=SURF, label='CV boundary value (vowel\'s forward)'),
             dict(color=INK2, marker='<', ls='none', ms=8, mfc=SURF, mew=1.6, label='VC boundary value (vowel\'s backward)')]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.012), ncol=5, fontsize=8.5)
    finish(fig, '11_locus_cv_vc.png', NOTE + ' "fr": the ramp\'s length in frames after scaling by the vowel\'s duration.')

# =====================================================================================================
# 12. the rules by context: one vowel, eight neighbours
def g12():
    ctx = [("['aa].", 'silence', '_'), ("[ax g'aa g ax].", 'plosive /g/', 'g'),
           ("[ax s'aa s ax].", 'fricative /s/', 's'), ("[ax n'aa n ax].", 'nasal /n/', 'n'),
           ("[ax w'aa w ax].", 'glide /w/', 'w'), ("[ax l'aa l ax].", 'liquid /l/', 'l'),
           ("[ax hx'aa hx ax].", '/hx/ (no targets of its own)', 'hx'), ("[iy 'aa iy].", 'vowel /iy/', 'iy')]
    fig, axs = plt.subplots(2, 4, figsize=(15, 10.8), sharey=True, sharex=True)
    fig.subplots_adjust(left=0.055, right=0.99, top=0.845, bottom=0.15, hspace=0.5, wspace=0.06)
    header(fig, 'Transition rules by context: /aa/ between eight kinds of neighbour (Perfect Paul)',
           'The same vowel, with the neighbour on both sides. The boundary value is the midpoint of the two targets '
           'unless a rule applies: the locus of an obstruent or nasal; 3/4 of the way toward a glide or liquid (w y r l), '
           'which moves fast (3 frames) while the vowel moves slowly (9); F1 jumps at a nasal, which keeps F1 flat, and '
           'the vowel\'s F1 stays at or above 425/480 Hz beside it; F1 is 80 Hz higher at /l/. Before a pause there is no '
           'backward transition; the pause itself glides from the last value. /hx/ takes the vowel\'s formants. Ramp '
           'lengths are scaled by the phone\'s duration (non-obstruents) and capped at 20 frames.', left=0.055)
    for ax, (txt, name, cname) in zip(axs.flat, ctx):
        r = run('[:np] ' + txt)
        p = phone_at(r, 'aa')
        x0 = p['n']
        x = [f['n'] - x0 for f in r['f']]
        for q in r['ph']:
            if q['name'] == cname and cname != '_':
                ax.axvspan(q['n'] - x0 - 0.5, q['n'] + q['dur'] - x0 - 0.5, color=MUTED, alpha=0.12, lw=0)
        for k in ('F1', 'F2', 'F3'):
            ax.plot(x, [f['t' + k]['tar'] + f['t' + k]['dip'] for f in r['f']], color=FCOL[k], lw=0.9, ls=(0, (4, 2.5)))
            ax.plot(x, [f[k] for f in r['f']], color=FCOL[k], lw=0.9, alpha=0.35)
            ax.plot(x, voiced(r, k), color=FCOL[k], lw=2.1)
            if p[k]['fdur'] > 0:
                ax.plot([0], [p[k]['fbou']], marker='>', color=FCOL[k], ms=7.5, mec=SURF, zorder=7)
            if p[k]['bdur'] > 0:
                ax.plot([p['dur']], [p[k]['bbou']], marker='<', color=FCOL[k], ms=7.5, mfc=SURF, mew=1.5, zorder=7)
        ax.set_title('next to %s' % name, fontsize=10.5)
        ax.set_xlim(-25, 60)
        ax.set_ylim(0, 3300)
        nex = r['ph'][r['ph'].index(p) + 1]
        import textwrap
        note = [textwrap.fill('into /aa/: ' + rule_line(p, 'fwd'), 58, subsequent_indent='    '),
                textwrap.fill('out of /aa/: ' + (rule_line(p, 'bwd') if nex['name'] != '_' else 'none before a pause'),
                              58, subsequent_indent='    ')]
        ax.text(0.0, -0.11, '\n'.join(note), transform=ax.transAxes, fontsize=7.6, color=INK2, va='top',
                linespacing=1.35)
        for q in r['ph']:
            if q['name'] != '_' and -25 < q['n'] - x0 + q['dur'] / 2 < 60:
                ax.text(q['n'] - x0 + q['dur'] / 2 - 0.5, 0.94, q['name'], transform=ax.get_xaxis_transform(),
                        ha='center', fontsize=8.5, color=INK if q['feats'] & 7 >= 2 else MUTED,
                        fontweight='bold' if q['feats'] & 7 >= 5 else 'normal')
    for ax in axs[:, 0]:
        ax.set_ylabel('frequency (Hz)')
    for ax in axs[0]:
        ax.tick_params(labelbottom=True)
    items = [dict(color=FCOL[k], lw=2.1, label=k) for k in ('F1', 'F2', 'F3')]
    items += [dict(color=INK2, lw=0.9, ls=(0, (4, 2.5)), label='target'),
              dict(color=INK2, marker='>', ls='none', ms=7.5, mec=SURF, label='forward boundary value of /aa/'),
              dict(color=INK2, marker='<', ls='none', ms=7.5, mfc=SURF, mew=1.5, label='backward boundary value of /aa/'),
              dict(color=MUTED, lw=8, alpha=0.3, label='the neighbour')]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.5, 0.012), ncol=7, fontsize=8.5)
    finish(fig, '12_rules_by_context.png', NOTE + ' x axis: frames from the vowel\'s start. Frames listed F1/F2/F3.')

# =====================================================================================================
# 13. the two sides of a boundary
GRAPH_TEXTS = CHECK_TEXTS + [
    '[:np] Hello world.', '[:np] She sells six thick fish; the zoo has five vases.',
    '[:np] Bob put a big cake at the good tip top.', '[:np] Many men and women sang a long song.',
    '[:np] The old man sat in a rocker.', '[:np] Take the apple to the street!', '[:np] We were away a year ago.',
    '[:np] Why go away?', '[:nb] Betty will read the letter tomorrow morning.']

VOWEL = lambda name: 1 <= code(name) <= 23

def boundaries():
    """(category, formant, gap) for every boundary of GRAPH_TEXTS with a backward transition: gap = the next phone's
    first value minus the value this phone's backward transition heads for"""
    out = []
    for txt in GRAPH_TEXTS:
        r = run(txt, trim=False)
        ph = r['ph']
        for a, b in zip(ph, ph[1:]):
            if b['n'] != a['n'] + a['dur'] or a['nex'] != b['name'] or b['name'] == '_':
                continue
            for k in range(3):
                bb, bn = bwd_sent(a, k)
                if bn == 0:                     # no backward ramp (a nasal's F1): the phone ends on its target
                    bb = a['F%d' % (k + 1)]['tarend']
                fb = fwd_sent(b, k)[0]
                ra, rb = bwd_rule(a, k)[2], fwd_rule(b, k)[2]
                fa, fbb = feat(a['name']), feat(b['name'])
                if k == 0 and fbb & F_NASAL:
                    cat = 'F1 into a nasal (it jumps)'
                elif k == 0 and fa & F_NASAL:
                    cat = 'F1 out of a nasal (at least 425 Hz)'
                elif '+80' in ra or '+80' in rb:
                    cat = 'F1 at /l/ (+80 Hz on the vowel side)'
                elif a['name'] == '_':
                    cat = 'pause to phone (midpoint)'
                elif 'locus' in ra:
                    cat = 'locus: consonant to vowel (CV)' if BEGTYP[code(a['name'])] == 4 and not fa & F_SONCONS \
                        else 'locus: vowel to consonant (VC)'
                elif '3/4' in ra:
                    cat = 'vowel and glide/liquid (3/4 rule)'
                elif VOWEL(a['name']) and VOWEL(b['name']):
                    cat = 'vowel to vowel (midpoint)'
                else:
                    cat = 'consonant cluster (midpoint)'
                out.append((cat, k, fb - bb))
    return out

def g13():
    import random
    rnd = random.Random(1)
    data = boundaries()
    cats = ['locus: consonant to vowel (CV)', 'locus: vowel to consonant (VC)', 'vowel and glide/liquid (3/4 rule)',
            'vowel to vowel (midpoint)', 'consonant cluster (midpoint)', 'pause to phone (midpoint)',
            'F1 at /l/ (+80 Hz on the vowel side)', 'F1 into a nasal (it jumps)', 'F1 out of a nasal (at least 425 Hz)']
    fig, ax = plt.subplots(figsize=(13, 7.6))
    fig.subplots_adjust(left=0.27, right=0.86, top=0.8, bottom=0.14)
    header(fig, 'Both sides of a boundary: is the track continuous?',
           'Each boundary value is computed twice: by the phone before it (its backward transition, from its end target '
           'and a copy of the next target) and by the phone after it (its forward transition, from its own start target '
           'and the last end target). Each dot is one boundary and formant: the next phone\'s first value minus the value '
           'the backward ramp was heading for. Most sit within a few Hz (the two copies of a consonant\'s target are '
           'coarticulated differently, plus rounding); the wide ones are the rules that make the sides differ on '
           'purpose.', left=0.03)
    for i, c in enumerate(cats):
        pts = [(k, g) for cc, k, g in data if cc == c]
        for k, g in pts:
            ax.plot([g], [i + rnd.uniform(-0.28, 0.28)], 'o', ms=4.2, color=FCOL['F%d' % (k + 1)], alpha=0.75,
                    mec=SURF, mew=0.4)
        if pts:
            small = sum(abs(g) <= 5 for _, g in pts)
            ax.text(1.01, i, '%d, %d%% within 5 Hz' % (len(pts), round(100 * small / len(pts))),
                    transform=ax.get_yaxis_transform(), va='center', fontsize=8, color=INK2)
    ax.axvline(0, color=INK2, lw=0.8)
    ax.set_yticks(range(len(cats)))
    ax.set_yticklabels(cats)
    ax.invert_yaxis()
    ax.set_xlim(-130, 130)
    ax.set_xlabel('next phone\'s first value - backward boundary value (Hz)')
    ax.grid(axis='y', visible=False)
    items = [dict(color=FCOL['F%d' % k], marker='o', ls='none', ms=5, label='F%d' % k) for k in (1, 2, 3)]
    line_legend(fig, items, loc='lower center', bbox_to_anchor=(0.55, 0.02), ncol=3, fontsize=8.5)
    finish(fig, '13_boundary_continuity.png', NOTE + ' %d boundaries x formants, %d texts; not shown: boundaries into a '
           'pause.' % (len(data), len(GRAPH_TEXTS)))

# =====================================================================================================
# 14. when ramps meet: straight closures, fixed-length ramps, undershoot
def g14():
    fig, (a, b, c) = plt.subplots(1, 3, figsize=(15, 6.4), gridspec_kw=dict(width_ratios=[1, 1.15, 1.15]))
    fig.subplots_adjust(left=0.05, right=0.99, top=0.76, bottom=0.2, wspace=0.2)
    header(fig, 'When the ramps meet: closures, speaking rate and undershoot (F2, Perfect Paul)',
           'A ramp\'s length is set in frames: fixed for obstruents (5 frames; a plosive\'s ramps span its whole '
           'closure), and for the other phones the rule\'s frames (vowels 8-9, glides 3-6, or the locus set\'s) x (0.5 + '
           'duration ratio/2), or (0.36 + ratio/2) backward, plus 1, at most 20 frames and the phone\'s length. Left: in a closure the two ramps add up to a '
           'straight line and the stop\'s own target never shows. Middle: a stressed vowel keeps ramps of about the same '
           'length at every rate, and only its middle shrinks. Right: when a short vowel\'s ramps add up to more than its '
           'length they overlap, and the track turns back before it reaches the target (undershoot); the vowel\'s '
           'duration is set with <ms>.', left=0.05)
    # left: the closure
    r = run("[:np] [ax b'iyg ax].")
    g = phone_at(r, 'g')
    x = [f['n'] - g['n'] for f in r['f']]
    tar = [f['tF2']['tar'] for f in r['f']]
    a.axvspan(-0.5, g['dur'] - 0.5, color=MUTED, alpha=0.12, lw=0)
    rng = [i for i, f in enumerate(r['f']) if 0 <= f['n'] - g['n'] < g['dur']]
    a.plot([x[i] for i in rng], [tar[i] for i in rng], color=INK2, lw=1.1, ls=(0, (4, 2.5)), label='/g/\'s target')
    a.plot([x[i] for i in rng], [tar[i] + r['f'][i]['tF2']['fwd'] for i in rng], color=FWD_C, lw=1.4,
           label='target + forward term')
    a.plot([x[i] for i in rng], [tar[i] + r['f'][i]['tF2']['bwd'] for i in rng], color=BWD_C, lw=1.4,
           label='target + backward term')
    a.plot(x, [f['F2'] for f in r['f']], color=FCOL['F2'], lw=0.9, alpha=0.4)
    a.plot(x, voiced(r, 'F2'), color=FCOL['F2'], lw=2.2)
    a.plot([x[i] for i in rng], [r['f'][i]['F2'] for i in rng], 'o', ms=3.5, color=FCOL['F2'], label='F2 sent (the sum)')
    a.set_xlim(-8, g['dur'] + 8)
    a.set_ylim(1550, 2250)
    a.set_title('/g/ in [ax b\'iyg ax]: a straight closure', fontsize=10.5)
    a.set_xlabel('frames from the closure\'s start')
    a.set_ylabel('F2 (Hz)')
    a.legend(loc='upper right', fontsize=8)
    for q in r['ph']:
        if q['name'] != '_' and -8 < q['n'] - g['n'] + q['dur'] / 2 < g['dur'] + 8:
            a.text(q['n'] - g['n'] + q['dur'] / 2 - 0.5, 0.02, q['name'], transform=a.get_xaxis_transform(),
                   ha='center', fontsize=9, color=INK)
    # middle: a stressed vowel at four rates
    for ra, colr in ((100, S[0]), (180, S[1]), (300, S[2]), (450, S[6])):
        r = run("[:np :ra %d] [ax d'ehg ax]." % ra)
        p = phone_at(r, 'eh')
        keep = [i for i, f in enumerate(r['f']) if -6 <= f['n'] - p['n'] <= p['dur']]
        x = [r['f'][i]['n'] - p['n'] for i in keep]
        vf = voiced(r, 'F2')
        b.plot(x, [r['f'][i]['F2'] for i in keep], color=colr, lw=0.9, alpha=0.3)
        b.plot(x, [vf[i] for i in keep], color=colr, lw=2, label=':ra %d: %d frames, ramps %d + %d' % (
            ra, p['dur'], p['F2']['fdur'], p['F2']['bdur']))
        b.plot([p['F2']['fdur']], [frame(r, p['n'] + p['F2']['fdur'])['F2']], marker='v', color=colr, ms=7, mec=SURF)
        b.plot([p['dur'] - p['F2']['bdur']], [frame(r, p['n'] + p['dur'] - p['F2']['bdur'])['F2']], marker='^',
               color=colr, ms=7, mec=SURF)
        b.plot([p['dur']], [p['F2']['bbou']], marker='<', color=colr, ms=7, mfc=SURF, mew=1.4)
    b.axvline(0, color=INK2, lw=0.8)
    b.set_xlim(-7, 33)
    b.set_ylim(1450, 2030)
    b.set_title('/eh/ in [ax d\'ehg ax] at four rates', fontsize=10.5)
    b.set_xlabel('frames from the vowel\'s start')
    b.legend(loc='upper left', fontsize=7.8)
    b.text(0.02, 0.03, 'v forward ramp ends   ^ backward ramp starts', transform=b.transAxes, fontsize=7.8, color=INK2)
    # right: undershoot
    for ms, colr in ((30, S[0]), (50, S[1]), (80, S[2]), (130, S[6])):
        r = run("[:np] [b'aa g ax<%d> g 'aa]." % ms)
        p = phone_at(r, 'ax')
        g1, g2 = phone_at(r, 'g', 0), phone_at(r, 'g', 1)
        x = [f['n'] - p['n'] if g1['n'] <= f['n'] < g2['n'] + g2['dur'] else None for f in r['f']]
        seg = [i for i, f in enumerate(r['f']) if -3 <= f['n'] - p['n'] < p['dur'] + 3]
        tgt = [f['tF2']['tar'] + f['tF2']['dip'] for f in r['f']]
        c.plot([x[i] for i in seg if 0 <= x[i] < p['dur']], [tgt[i] for i in seg if 0 <= x[i] < p['dur']], color=colr,
               lw=1.0, ls=(0, (4, 2.5)))
        c.plot(x, [f['F2'] if xx is not None else None for f, xx in zip(r['f'], x)], color=colr, lw=0.9, alpha=0.3)
        low = min(frame(r, p['n'] + j)['F2'] for j in range(p['dur']))
        c.plot(x, [v if xx is not None else None for v, xx in zip(voiced(r, 'F2'), x)], color=colr, lw=2, label='%d ms: %d frames, ramps %d + %d; lowest %d Hz' % (
            ms, p['dur'], p['F2']['fdur'], p['F2']['bdur'], low))
        c.plot([0, p['dur'] - 1], [frame(r, p['n'])['F2'], frame(r, p['n'] + p['dur'] - 1)['F2']], 'o', ms=4,
               color=colr)
    c.axvline(0, color=INK2, lw=0.8)
    c.set_xlim(-14, 34)
    c.set_ylim(1150, 1850)
    c.set_title('/ax/ in [b\'aa g ax<ms> g \'aa], four durations', fontsize=10.5)
    c.set_xlabel('frames from the vowel\'s start')
    c.legend(loc='lower left', fontsize=7.8)
    c.text(0.52, 0.2, 'dashed: /ax/\'s target', transform=c.transAxes, fontsize=7.8, color=INK2)
    c.annotate('5 frames: both ramps span the vowel,\nso it runs flat at the /g/ locus', xy=(3, 1679),
               xytext=(8, 1770), fontsize=7.8, color=INK2, arrowprops=dict(arrowstyle='-', color=MUTED, lw=0.8))
    finish(fig, '14_ramps_overlap.png', NOTE)

GRAPHS = [g10, g11, g12, g13, g14]

if __name__ == '__main__':
    if '--check' in sys.argv[1:]:
        sys.exit(0 if check() else 1)
    names = [a for a in sys.argv[1:] if not a.startswith('-')]
    for g in GRAPHS:
        if not names or g.__name__ in names:
            g()
