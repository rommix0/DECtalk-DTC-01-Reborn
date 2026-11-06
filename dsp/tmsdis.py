"""TMS32010 disassembler for the DTC-01 DSP ROM (DECtalk v1.8, and v2.0 with the argument 2.0).

Input : merges/dsp_v1.8_A.bin = 2048 big-endian 16-bit words (E70_23-166F4 = high byte,
        E69_23-165F4 = low byte, interleaved); merges/dsp_v2.0_A.bin likewise (E70_23-410F4, E69_23-409F4).
Output: an annotated listing (stdout), e.g.  python dsp/tmsdis.py > dsp/dsp_v1.8.lst
                                          python dsp/tmsdis.py 2.0 > dsp/dsp_v2.0.lst

Opcode map and semantics follow native/tms32010.c (a 1:1 port of MAME's tms320c1x core, the
same core the firmware runs on in the emulator). Code is found by recursive descent from the
reset vector (0x000) and the interrupt vector (0x002); every word never reached is dumped as
data. Direct operands are data-RAM addresses (page = DP bit, so "08h" after LDPK 1 is 0x88).
Labels and comments below come from reading the code; see REFERENCE.md section 16.
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = sys.argv[1] if len(sys.argv) > 1 else '1.8'
W = struct.unpack('>2048H', open(os.path.join(HERE, '..', 'merges', 'dsp_v%s_A.bin' % VERSION), 'rb').read())

# ---- names ---------------------------------------------------------------------------------
LABELS = {
    0x000: 'RESET', 0x002: 'INTVEC',
    0x2A7: 'COLD_START', 0x2D9: 'WAIT_68K_READY', 0x2DF: 'FRAME_TIMEOUT_CHECK',
    0x2E7: 'IDLE_DECAY', 0x2ED: 'SIGNAL_ERROR', 0x2EE: 'CLEAR_STATE',
    0x2F9: 'MAIN_LOOP', 0x3D3: 'READ_FRAME_19', 0x456: 'FRAME_COEFFS',
    0x49E: 'NO_FRAME', 0x4DF: 'PITCH_SYNC_RESET', 0x4A5: 'SAMPLE', 0x4C2: 'GLOTTAL_4X_LOOP', 0x5BA: 'GLOTTAL_LOWPASS',
    0x5CD: 'NOISE_AND_MIX', 0x5E8: 'CASCADE_BRANCH', 0x632: 'PARALLEL_BRANCH',
    0x67C: 'WAIT_DAC_TICK', 0x685: 'DAC_TIMEOUT', 0x68F: 'SAMPLE_DONE',
    0x694: 'DAC_ISR', 0x698: 'TONE_MODE', 0x6A9: 'TONE_LOOP', 0x6D2: 'TONE_SAMPLE',
    0x709: 'COS_LOOKUP_A', 0x721: 'COS_LOOKUP_B', 0x72A: 'COS_LOOKUP_C',
}
DATA_NAMES = [   # (start, end_inclusive, name, description)
    (0x004, 0x013, 'INIT_CONSTS', 'copied to data RAM 0x00-0x10 at reset (data[n] = prog[n+3]); see RAM_CONSTS'),
    (0x014, 0x020, 'RESET_COEFFS', 'loaded at reset: 0x14 -> data 25 (noise seed), 0x15/0x16 -> 58/59 (parallel F6 B/C), '
                                   '0x17/0x18 -> 6A/6B (nasal pole B/C), 0x19-0x20 -> data 80-87: 81 = 31 (FNZ index '
                                   'offset), 84/85 = the 4x low-pass C/B; 80 82 83 86 87 are never read (REFERENCE s16.10)'),
    (0x021, 0x043, 'FNZ_TAB_A', 'nasal anti-resonator coefficient a (x gain), 35 steps: FNZ ~= 240 + 8k Hz, BW ~80 Hz'),
    (0x044, 0x066, 'FNZ_TAB_B', 'nasal anti-resonator coefficient b (same index; see FNZ_TAB_A)'),
    (0x067, 0x089, 'FNZ_TAB_C', 'nasal anti-resonator coefficient c (same index; see FNZ_TAB_A)'),
    (0x08A, 0x0E1, 'AMPTABLE', "Klatt's dB->linear amptable[88] (klsyn/parwav.h), indexed as 0x8A + dB"),
    (0x0E2, 0x1C6, 'COS_TABLE', 'cosine table, 8191*cos(theta) from 0x1FBF down to 0xE000 (-8192); '
                                'indexed by the COS_LOOKUP_* routines with piecewise resolution'),
    (0x1C7, 0x2A6, 'B0_TABLE', 'klsyn/parwav.h B0[224] = 1920000/nopen^2 (natural glottal source), '
                              'indexed by nopen-40; byte-identical'),
    (0x73F, 0x7FF, 'UNUSED_TAIL', 'never reached by code; looks like leftover/random EPROM contents [I]'),
]
RAM_CONSTS = {  # data RAM filled at reset from INIT_CONSTS (value -> use)
    0x01: '0x0118 = 280 (parallel F3 bandwidth term; 4*280 - 26 = the tilt slope)', 0x02: '0x1000 (1.0 Q12)',
    0x03: '0x0641 (noise generator feedback)', 0x04: '0x2AAB (1/3 in Q15: a = b*nopen/3)', 0x05: '0x7FFF (checksum mask)',
    0x06: '0x6000 (speaker-frame header)', 0x07: '0x0032 = 50 (frequency step for COS_LOOKUP ranges)',
    0x08: '0x21EA (x2 = 0x43D4, 19-word frame trailer)', 0x09: '0x00D2 (0xF5 + 0xD2 = B0_TABLE base; x2 = parallel F2 bandwidth term)',
    0x0A: '0x21FC (never read)', 0x0B: '0x7850 (never read)',
    0x0C: '0x1000 (1.0 Q12)', 0x0D: '0x000C (subtracted from the tilt word; 5000 in tone mode)',
    0x0E: '0x1FFF (header must-be-zero mask; noise mask)',
    0x0F: '0xE000 (noise: the sign bits of a negative value)', 0x10: '0x0001 (one)',
}
COMMENTS = {
    0x2A7: 'saturation mode on; DP=0',
    0x2AB: 'port 0: raise semaphore to the 68000 (value 0 = no error)',
    0x2B3: 'clear data RAM 0x00-0x80',
    0x2B9: 'copy INIT_CONSTS -> data 0x00-0x10',
    0x2D9: 'spin until BIO (68000 has acknowledged)',
    0x2ED: 'port 0 write with data[10]=1 -> error latch set, 68000 IRQ5',
    0x2F9: 'BIO clear -> no frame pending, go synthesize a sample',
    0x2FE: 'frame header -> data[2C]',
    0x2FF: 'bit 15 set -> TONE_MODE',
    0x303: 'header & 0x6000 must be non-zero',
    0x307: 'header & 0x1FFF must be zero',
    0x2DF: 'every 64 samples (data[2D]) back to MAIN_LOOP to look for a frame: the DSP paces the 68000',
    0x2E7: 'three frame times without a frame: stop making samples; signal "error" once if speech was spoken (8C)',
    0x30D: 'header 0x6000 = 24-word speaker frame; 0x2000/0x4000 = 19-word speech frame',
    0x316: 'speaker frame word 1 (running sum in data[73])',
    0x320: 'frequency -> cosine coefficient',
    0x378: 'dB -> linear via AMPTABLE',
    0x3CA: 'word 23: checksum; (sum & 0x7FFF) must be 0',
    0x3D0: 'port 0: semaphore, no error (frame accepted)',
    0x3D3: 'speech frame: T0 F1 F2 F3 FNZ B1 B2 B3 AV, then AH A2-A6 AB through AMPTABLE, TLT, the trailer',
    0x44C: 'last word must equal 2*data[08] = 0x43D4',
    0x455: 'frame accepted',
    0x4BA: 'nper > nmod (data[31]) -> noise halved (parwav: amplitude-modulated noise)',
    0x4C0: 'glottal source runs 4x per output sample (as in klsyn parwav.c)',
    0x4C7: 'nper (14) vs nopen (15): open phase -> natural source',
    0x4CB: 'parwav natural_source(): a -= b; vwave += a  (a = 13, b = 2E, vwave = 78)',
    0x4D1: 'glotout = vwave * amp_voice (76)',
    0x4D7: 'closed phase: vwave = 0 until nper reaches T0 (data[30])',
    0x4DF: 'parwav pitch_synch_par_reset(): nper = 0, vwave = 0',
    0x4E3: 'AV latched once per period: data[19] = AMPTABLE[1B + 4]',
    0x4F3: 'T0 += 7F*T0, then 7F = -7F (alternating skew, parwav "skew = -skew")',
    0x50B: 'nmod = T0, halved when voiced (parwav)',
    0x512: 'nopen = 28*T0 + 27 (open phase as a fraction of the period plus an offset)',
    0x519: 'clamp nopen >= 40',
    0x520: 'clamp nopen <= 0xD5 + data[07] = 263 (parwav limits 40..263)',
    0x528: 'nopen < T0',
    0x52F: 'b = B0_TABLE[nopen - 40]',
    0x537: 'a = b * nopen / 3 (two orderings to avoid overflow)',
    0x5A0: 'FNZ (data[1F], Hz) -> index 1F/8 - data[81](=31) into FNZ_TAB_A/B/C',
    0x4A5: 'noise generator: data[24] shifts left, XOR 0x0641 when it was <= 0; noise = its low 13 bits (sign-filled)',
    0x5CD: 'tilt filter on the voicing (7B, 18), breath noise in the open phase, then source = AV*voicing + AH*noise',
    0x5E8: 'cascade: states from data 0x4A down (AR1), coefficients C,B,A from 0x6E down (AR0): nasal zero (FIR), '
           'nasal pole, F5, F4, F3, F2, F1',
    0x632: 'parallel on the noise: F6 (fixed), F5, F4, F3, F2, summed with alternating signs (Klatt 1980)',
    0x673: 'bypass path: subtract data[17] * frication source',
    0x67A: 'sample ready in data[11]; wait up to 20 loops for the DAC interrupt',
    0x685: 'no DAC tick in time: wait for BIO, then flag an error to the 68000',
    0x694: 'output FIFO has room: write data[11] to the DAC',
    0x698: 'two-sinusoid generator (phase accumulators 2F/30, steps 2D/2E) - DTMF/tones [I]',
    0x6AC: 'a positive (bit 15 clear) header ends tone mode -> cold start',
    0x709: 'data[12] = frequency (Hz, piecewise ranges 200/400/800/1600/3200) -> data[12] = COS_TABLE entry [I]',
}

# ---- v2.0 (REFERENCE s18.5): its own names; v1.8's above ----------------------------------
if VERSION == '2.0':
    # v1.8's labels at the v2.0 instruction they align with (instruction shapes, RAM addresses hidden: 86% align)
    LABELS = {0x000: 'RESET', 0x002: 'INTVEC', 0x0D9: 'COLD_START', 0x0F6: 'WAIT_68K_READY',
              0x0FC: 'FRAME_TIMEOUT_CHECK', 0x104: 'IDLE_DECAY', 0x110: 'SIGNAL_ERROR', 0x111: 'CLEAR_STATE',
              0x11C: 'MAIN_LOOP', 0x1E6: 'READ_FRAME_19', 0x27F: 'FRAME_COEFFS', 0x2C7: 'NO_FRAME', 0x2CE: 'SAMPLE',
              0x2EB: 'GLOTTAL_4X_LOOP', 0x308: 'PITCH_SYNC_RESET', 0x3E2: 'GLOTTAL_LOWPASS', 0x3F3: 'NOISE_AND_MIX',
              0x413: 'CASCADE_BRANCH', 0x45D: 'PARALLEL_BRANCH', 0x4B2: 'SAMPLE_DONE', 0x4B7: 'QUEUE_SAMPLE',
              0x4DF: 'DAC_ISR', 0x4ED: 'TONE_MODE', 0x4FE: 'TONE_LOOP', 0x559: 'COS_COEFF'}
    COMMENTS = {
        0x0E3: 'copy INIT_CONSTS (prog 0x04-0x12) -> data 0x01-0x0F (data[0] gets prog[3])',
        0x0E9: 'RESET_COEFFS: prog 0x17/0x16 -> 6A/69, 0x15/0x14 -> 58/57, 0x13 -> 24 (noise seed); v1.8 also loaded data 80-87',
        0x357: 'replaces v1.8 b = B0_TABLE[nopen-40]: data[0] = nopen^2/8, then 50*600 = 30000 divided by it in 19 '
               'SUBC steps = 30000*64/nopen^2 = 1920000/nopen^2, parwav\'s B0 formula computed, not looked up [I]',
        0x559: 'replaces COS_LOOKUP_A-C and COS_TABLE: cosine of the frequency by range reduction and a polynomial '
               '(MPYK 1318, 316) [I]',
        0x4B0: "replaces v1.8's WAIT_DAC_TICK / DAC_TIMEOUT: no 20-loop limit and no error path",
        0x10A: 'new: after an utterance, wait for the 68000\'s next word; a tone word starts TONE_MODE, anything else '
               'gets the error (and is lost)',
        0x118: 'BUG: SACH *- runs on the current AR, which QUEUE_SAMPLE left as AR0 (at the last queue slot), not AR1: '
               'clears the 24 words below the slot instead of the histories (dsp_synth.c v20_clear)',
        0x135: 'the same AR0 bug for the speaker frame\'s 12 words',
        0x2DB: "data[02] was v1.8's 0x1000 (y/8); in v2.0 it is the LFSR mask 0x0641, so noise += 1601/32768 of y",
        0x269: 'new: AV (latched) + AH + A2-A6 + AB, linear, at most 4 starts the fade-out (data[7F] = 0x0FF0); more '
               'sets full gain (0x1000). A2/A3 are still the last frame\'s here',
        0x4A2: 'new: the fade, data[7F] -= 16 a sample while 0 < it < 0x1000; out = (fade x out - bypass) >> 12',
        0x407: 'new: breath noise in the closed phase too, at half weight',
        0x4B7: 'put the sample (| 1, so a queued 0 is non-zero) in the RAM queue data 0x86-0x8F: wait while 0x88 is '
               'full, then store it behind the last full slot, interrupts off [I]',
        0x4DF: 'DAC interrupt: write the oldest sample (0x8F) to the DAC and shift the queue 0x87-0x8E up by one',
    }
    DATA_NAMES = [
        (0x004, 0x012, 'INIT_CONSTS', "copied to data RAM 0x01-0x0F at reset; v1.8's set without its data[02] = 0x1000, "
                                      "and with F4B0 170F where v1.8 had two unread words; see RAM_CONSTS"),
        (0x013, 0x017, 'RESET_COEFFS', 'loaded at reset (see the comment at 0E9)'),
        (0x018, 0x03A, 'FNZ_TAB_A', 'identical to v1.8 FNZ_TAB_A'),
        (0x03B, 0x05D, 'FNZ_TAB_B', 'identical to v1.8 FNZ_TAB_B'),
        (0x05E, 0x080, 'FNZ_TAB_C', 'identical to v1.8 FNZ_TAB_C'),
        (0x081, 0x0D8, 'AMPTABLE', "identical to v1.8 AMPTABLE (Klatt's amptable[88]), indexed as 0x81 + dB"),
        (0x58E, 0x7FF, 'UNUSED_TAIL', 'zeros, never reached'),
    ]
    RAM_CONSTS = {  # v1.8's data[n+1] for n >= 2 (its data[02] = 0x1000 was dropped)
        0x01: '0x0118', 0x02: '0x0641', 0x03: '0x2AAB', 0x04: '0x7FFF', 0x05: '0x6000', 0x06: '0x0032 = 50',
        0x07: '0x21EA (x2 = 0x43D4 trailer)', 0x08: '0x00D2', 0x09: '0xF4B0 (new)', 0x0A: '0x170F (new)',
        0x0B: '0x1000 (1.0 Q12)', 0x0C: '0x000C', 0x0D: '0x1FFF', 0x0E: '0xE000', 0x0F: '0x0001',
    }

# ---- decoder -------------------------------------------------------------------------------
def mem(op):
    lo = op & 0xFF
    if lo & 0x80:
        s = '*+' if lo & 0x20 else '*-' if lo & 0x10 else '*'
        if not lo & 0x08: s += ',%d' % (lo & 1)          # new ARP
        return s
    return '%02Xh' % (lo & 0x7F)

BR = {0xF4: 'BANZ', 0xF5: 'BV', 0xF6: 'BIOZ', 0xF8: 'CALL', 0xF9: 'B', 0xFA: 'BLZ', 0xFB: 'BLEZ',
      0xFC: 'BGZ', 0xFD: 'BGEZ', 0xFE: 'BNZ', 0xFF: 'BZ'}
G7F = {0x00: 'NOP', 0x01: 'DINT', 0x02: 'EINT', 0x08: 'ABS', 0x09: 'ZAC', 0x0A: 'ROVM', 0x0B: 'SOVM',
       0x0C: 'CALA', 0x0D: 'RET', 0x0E: 'PAC', 0x0F: 'APAC', 0x10: 'SPAC', 0x1C: 'PUSH', 0x1D: 'POP'}
SIMPLE = {0x50: 'SACL', 0x60: 'ADDH', 0x61: 'ADDS', 0x62: 'SUBH', 0x63: 'SUBS', 0x64: 'SUBC',
          0x65: 'ZALH', 0x66: 'ZALS', 0x67: 'TBLR', 0x69: 'DMOV', 0x6A: 'LT', 0x6B: 'LTD', 0x6C: 'LTA',
          0x6D: 'MPY', 0x6F: 'LDP', 0x78: 'XOR', 0x79: 'AND', 0x7A: 'OR', 0x7B: 'LST', 0x7C: 'SST',
          0x7D: 'TBLW'}

def decode(pc):
    """-> (text, length, flow); flow is None, 'ret', 'stop', 'cala' or (kind, target)."""
    op = W[pc]; h = op >> 8
    if h in BR:
        t = W[(pc + 1) & 0x7FF] & 0xFFF
        m = BR[h]
        return '%-5s @%03X' % (m, t), 2, ('jmp' if m == 'B' else 'call' if m == 'CALL' else 'br', t)
    sh = lambda n: ',%d' % n if n else ''
    if h < 0x10: return 'ADD   %s%s' % (mem(op), sh(h)), 1, None
    if h < 0x20: return 'SUB   %s%s' % (mem(op), sh(h & 15)), 1, None
    if h < 0x30: return 'LAC   %s%s' % (mem(op), sh(h & 15)), 1, None
    if h in (0x30, 0x31): return 'SAR   AR%d,%s' % (h & 1, mem(op)), 1, None
    if h in (0x38, 0x39): return 'LAR   AR%d,%s' % (h & 1, mem(op)), 1, None
    if 0x40 <= h < 0x48: return 'IN    %s,PA%d' % (mem(op), h & 7), 1, None
    if 0x48 <= h < 0x50: return 'OUT   %s,PA%d' % (mem(op), h & 7), 1, None
    if 0x58 <= h < 0x60: return 'SACH  %s%s' % (mem(op), sh(h & 7)), 1, None
    if h == 0x68:
        if (op & 0xFE) == 0x80: return 'LARP  %d' % (op & 1), 1, None
        return 'MAR   %s' % mem(op), 1, None
    if h == 0x6E: return 'LDPK  %d' % (op & 1), 1, None
    if h in (0x70, 0x71): return 'LARK  AR%d,%02Xh' % (h & 1, op & 0xFF), 1, None
    if h == 0x7E: return 'LACK  %02Xh' % (op & 0xFF), 1, None
    if h == 0x7F:
        m = G7F.get(op & 0x1F)
        if m is None: return 'DW    %04Xh    ; illegal 7F-group opcode' % op, 1, 'stop'
        return m, 1, 'ret' if m == 'RET' else 'cala' if m == 'CALA' else None
    if 0x80 <= h < 0xA0:
        k = op & 0x1FFF
        return 'MPYK  %d' % (k - 0x2000 if k & 0x1000 else k), 1, None
    if h in SIMPLE: return '%-5s %s' % (SIMPLE[h], mem(op)), 1, None
    return 'DW    %04Xh    ; illegal opcode' % op, 1, 'stop'

def trace():
    code, labels, todo = {}, dict(LABELS), [0, 2]
    while todo:
        pc = todo.pop()
        while 0 <= pc < 2048 and pc not in code:
            txt, n, fl = decode(pc)
            code[pc] = (txt, n)
            if n == 2: code[pc + 1] = None
            if fl in (None, 'cala'): pc += n; continue
            if fl in ('ret', 'stop'): break
            kind, t = fl
            labels.setdefault(t, ('SUB_%03X' if kind == 'call' else 'L%03X') % t)
            todo.append(t)
            if kind == 'jmp': break
            pc += n
    return code, labels

def main():
    code, labels = trace()
    ncode = len(code)
    out = ['; DTC-01 (DECtalk v%s) TMS32010 DSP program - generated by dsp/tmsdis.py' % VERSION,
           '; %d code words, %d data words; data-RAM constants set at reset:' % (ncode, 2048 - ncode)]
    out += [';   data[%02X] = %s' % (k, v) for k, v in sorted(RAM_CONSTS.items())]
    out.append(';')
    dnames = {s: (n, d) for s, e, n, d in DATA_NAMES}
    pc = 0
    while pc < 2048:
        if pc in code and code[pc] is not None:
            txt, n = code[pc]
            if '@' in txt:
                t = int(txt.split('@')[1], 16); txt = txt.split('@')[0] + labels[t]
            if pc in LABELS: out.append('')
            lab = labels.get(pc, '')
            raw = ' '.join('%04X' % W[pc + i] for i in range(n))
            line = '%03X  %-9s  %-20s %s' % (pc, raw, lab + ':' if lab else '', txt)
            if pc in COMMENTS: line = '%-62s ; %s' % (line, COMMENTS[pc])
            out.append(line)
            pc += n
        else:
            s = pc
            while pc < 2048 and pc not in code: pc += 1
            a = s
            while a < pc:
                if a in dnames:
                    out.append(''); out.append('; %s: %s' % dnames[a])
                seg_end = min(a + 8, pc)
                nxt = [x for x in dnames if a < x < seg_end]
                if nxt: seg_end = min(nxt)
                out.append('%03X  %-9s  %-20s DW %s' % (a, 'DATA', (dnames[a][0] + ':') if a in dnames else '',
                                                       ' '.join('%04X' % v for v in W[a:seg_end])))
                a = seg_end
    print('\n'.join(out))

if __name__ == '__main__':
    main()
