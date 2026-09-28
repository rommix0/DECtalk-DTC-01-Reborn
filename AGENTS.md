# AGENTS.md — DTC-01 (DECtalk I) firmware reverse engineering

Entry point for any agent continuing the decompilation of the DECtalk DTC-01 68000 program ROM.
This file is loaded into every session, so it is deliberately short: ground rules, repo map,
hardware identity, and a list of the facts that are most likely to trip you up.
**All detailed lookup material lives in [`REFERENCE.md`](REFERENCE.md)** (host protocol, SETUP,
number/dictionary rules, voice tables, phoneme codes, ROM data catalog, function inventory, corrections,
MITalk lineage, test corpus). Section numbers `§3` and up in this file refer to that document.
Read it (or the relevant section) *before* touching a subsystem — do not rely on memory of it.

Confidence tags used in both files: **[V]** verified in the ROM (decompile / raw bytes) ·
**[M]** stated by the manuals or the 1980s sample code (behavior only) · **[I]** inferred, verify first.

The former `FINDINGS.md` (evidence trail) and `BRIEF.md` (its summary) were merged into
`REFERENCE.md` §15 and deleted; some Ghidra plate comments still carry their old, wrong wording
(REFERENCE §12 lists what to fix).

---

## 0. Ground rules (durable user instructions + working conventions)

1. **Both ROMs are in scope.** The main 68000 ROM is worked in Ghidra. The TMS32010 DSP ROM
   (`merges/dsp_v1.8_A.bin`, 2048 words) was deferred earlier but **reopened by the user on 2026-09-23** ("Let's
   attempt to disassemble the DSP ROMs next"). Ghidra has no TMS320C1x module, so the DSP is disassembled with
   **`dsp/tmsdis.py`**, which writes **`dsp/dsp_v1.8.lst`** (labels and comments live in the script's tables; edit
   them and regenerate). Notes: REFERENCE §16. **The whole DSP program is C** (`src/speech/dsp_synth.c`,
   2026-09-27), plain C and sample-exact against the emulator (REFERENCE §16.10).
2. **Reference-source priority** (user-set). **`dapi/`, `klsyn/`, `xtras/`, `samples/` and the raw chip dumps `ROMs/`
   were removed from the tree on 2026-09-27 at the user's request** (to the Recycle Bin; the work that needed them is
   done, and `merges/` holds the assembled images). Citations of their files below and in REFERENCE stay as the
   record of where a fact came from; the folders are no longer there to open.
   - Synthesizer front end (`klsyn`): `klsyn/parwav.c` + `parwav.h` (Klatt 1982-88) first,
     fall back to `dapi/`.
   - Duration rules: `dapi/src/PH/p_us_tim.c` (exact match; unit/table check in REFERENCE §9.1, rule
     origins in MITalk Ch. 9, REFERENCE §14.3).
   - Algorithm background: `docs/mitalk.html` (REFERENCE §14) — explains *why*, not ROM addresses. For
     prosody/phonetics, DECtalk follows **Klattalk** (Klatt 1982; hat-pattern F0) more closely than MITalk; see the
     third-party notes in `docs/Klatt_1980_*/` and `docs/Klatt_1982_*/` and the papers themselves, `docs/klatt1980.pdf`
     (JASA, with the FORTRAN listing) and `docs/klatt1982.pdf` (REFERENCE §14.10). The notes are secondary: check
     their numbers against the papers, `klsyn/` or the ROM. The DSP follows `klsyn/parwav.c`, not the 1980 FORTRAN.
   - When the HTML manuals and the ROM disagree on a number, the ROM wins. When an HTML table looks
     garbled or a value seems off, **check the PDF scan** (`docs/*.pdf`, image-only pages; extractor and
     page offsets in REFERENCE App. A) — the scans are the ground truth for the manuals. (The earlier DjVu OCR,
     `docs/old/`, was deleted at the user's request; it had wrong digits.)
   - Letter-to-sound: **`docs/hunnicutt_lts.pdf`** (Hunnicutt 1976; the ROM's 373 rules descend from its rule
     listing, REFERENCE §14.9) and the decoded ROM table (REFERENCE §15.10), then `dapi/src/LTS/l_us_ru1.c`,
     `ls_rule.c`, `ls_rule2.c` (later, and laid out differently).
   - Memory map / hardware truth: `native/dtc01.c`, `native/dtc01.h` (working emulator).
   - **Behavior spec: `docs/` manuals** (+ the former `xtras/` samples, whose test vectors are in REFERENCE App. B)
     (REFERENCE §5–§9).
3. **Workflow per subsystem** (established pattern): find via self-identifying ROM strings
   → decompile → cross-reference a source/manual → `rename_function_by_address` (snake_case)
   → `set_plate_comment` with evidence → update `REFERENCE.md` (the §10 function inventory, an
   evidence entry in §15, and the §13 open items) → re-send it with `SendUserFile`. Keep the "N of 362
   functions named" count current (**305 as of this writing**; 362 since the 2026-09-26 kernel gap fill).
4. **Ghidra project:** `dtc01_disasm`, program `dectalk_v1.8_full.bin`, language
   `68000:BE:32:default`, base `0x00000000`. Access is through the `ghidra-mcp` MCP server
   (tools are deferred: load them with ToolSearch `select:mcp__ghidra-mcp__…`). The server
   can disconnect mid-session; after reconnecting confirm the program is still open
   (`list_open_programs` / `get_current_program_info`).
5. **Do not trust Ghidra auto-labels blindly.** `PTR_DAT_xxxxxxxx` means "looked like a
   pointer", not "is a pointer" — several are really byte-indexed tables (e.g. `0x13b48 +
   ascii`). `_DAT_` prefix = wider access than the label size. `undefined**` arithmetic is
   scaled by 4 in the decompiler output. Labels can be off by one (LTS rules really start
   at `0x13e10`, not the auto-labeled `0x13e11`).
6. **Windows/tooling pitfalls seen so far:**
   - Native Windows Python needs `C:/Users/...` paths; MSYS-style `/c/...` fails.
   - The Bash tool keeps its cwd between calls — use absolute paths; never rely on `cd`.
   - The Bash tool turns `\\` into `\` even inside quoted heredocs (a corpus text with `ESC \` became `ESC t`).
     Write scripts and escape-laden texts with the Write tool and run them from Python; use raw strings there.
   - `clear_flow_and_repair` can reset a function name back to `FUN_…`; re-apply name and
     plate comment afterwards.
   - `create_memory_block` needs decimal sizes.
   - The Grep tool defaults to `files_with_matches`; pass `output_mode: content` + `-n`.
   - `cmd` here has `NoDefaultCurrentDirectoryInExePath` set: it does not run a `.bat` or `.exe` from the current
     directory by its bare name. Give the full path (or `.\name`); otherwise the call silently does nothing.
   - WSL shuts down when idle and then clears `/tmp`. Keep what must survive between calls in `~/.cache/dtc01`
     (the WSL home), or do the whole job in one `wsl` call. Its apt lists are stale and there is no sudo: a private
     index (`apt-get -o Dir::State::Lists=… -o Dir::Cache=… -o Debug::NoLocking=1 update`, then `apt-get download`
     and `dpkg-deb -x`) gets headers such as GTK 3's without root (REFERENCE §17.12).
   - From Git Bash, `wsl -e bash /mnt/c/...` needs `MSYS_NO_PATHCONV=1`, or the path is rewritten.
   - Renaming an address *inside* a function renames the containing function (this
     happened once with `task_create`/`task_create_impl`); always check the
     function start with `get_function_by_address` first.
   - The ghidra-mcp server enforces a naming style (`g_` + Hungarian prefix for globals, PascalCase for
     functions). Function renames succeed with only a warning; **label/global renames are rejected** unless
     you pass `strict_mode: "off"` to `rename_or_label`. Keep the project's snake_case names.
7. **Scratch work** goes in the session scratchpad, never in the repo. New repo files only
   when asked.
8. **When Ghidra is disconnected, the raw ROM is still readable**: open
   `C:/Users/abart/Desktop/DTC-01/merges/dectalk_v1.8_full.bin` with Python and use
   `struct.unpack_from('>…', b, addr)` (big-endian; file offset == CPU address). This is how the
   voice records, duration arrays and phoneme-name table were verified without Ghidra. Use
   `re.finditer` on the bytes to hunt for strings/tables by known content.
   **Python `capstone` (5.0.7) disassembles 68000 code** (`Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN|CS_MODE_M68K_000)`);
   don't name the script `dis.py`, because that shadows the stdlib module capstone imports. This is how the LTS
   engine was decoded while Ghidra was down.
9. **PDF pages:** render with Ghostscript (`C:/Program Files/gs/gs10.02.0/bin/gswin64c.exe -q -dNOPAUSE -dBATCH
   -sDEVICE=pnggray -r100 -dFirstPage=N -dLastPage=N -sOutputFile=out.png <pdf>`) and view the PNG with the Read
   tool; details in REFERENCE App. A.

10. **Target architecture for the C decompilation (user, 2026-09-25).** The eventual goal is C source that
    compiles. Once decompilation is complete, the ROM is to be split into two separate parts:
    - **speech**: text pipeline, LTS/dictionary, phonetic component, and the synthesizer/DSP, **including the
      in-text `[…]` command system** (voice selection such as `[:np]`, voice design `[:dv …]`, the other `[:…]`
      commands, and phoneme streams, including singing);
    - **host terminal** (a simulator of the host side): everything else, i.e. the host command line
      (escape-sequence parser, SETUP, logging) and the phone/DTMF dialer tasks.

    The speech synthesizer should be able to run on its own, without the host terminal (if possible). The host
    terminal command-line program must still exist alongside the synth. A possible use case is connecting it to
    the internet/VoIP, since it has phone/modem capability. Details to be worked out later. Keep this boundary
    in mind when naming, typing and grouping functions (REFERENCE §13 item 15). The user started the C
    decompilation on 2026-09-26. Hand-written C lives in `src/<part>/` (moved from `decomp/src/` to the top of the
    tree on 2026-09-27), checked word for word against the ROM (REFERENCE §15.18); never hand-edit `decomp/raw/` or
    `decomp/draft/`.
    **The finished project must build without the ROMs (user, 2026-09-27):** all ROM data the firmware uses (68000
    and DSP) must end up extracted into the source tree, as `ph_rom.c` / `tx_rom*.c` already are (generated once,
    kept as source). The ROMs stay needed only for verification (emulator, captures). **No part of the product (the
    library, the host terminal, SAY, speak) may load the ROMs, not even as a temporary stand-in** (user, 2026-09-27;
    an emulator-backed `DECtalk.dll` was proposed and dropped). Once a table is understood,
    convert it from a ROM-address byte block into a **named, typed C table** (user, 2026-09-27), and check that all
    modes still pass (and an AddressSanitizer build, which catches the ROM's reads outside a table). **All the speech
    tables are converted** (REFERENCE §15.31). Status, rules and what remains: REFERENCE §13 item 15.
    **The speech synth is to become a DLL / shared object (user, 2026-09-27),** and the host terminal emulator a
    program that uses it. Its exports should follow dapi's (`dapi/src/dectalk.def`, `dapi/src/API/TTSAPI.H`:
    `TextToSpeechStartup`, `…Speak`, `…Sync`, `…Reset`, …). **Draft the API before building the library**, and give it
    an index callback, as dapi's `DtCallbackRoutine` with `TTS_MSG_INDEX_MARK` (REFERENCE §13 item 15). **The draft:
    `src/api/ttsapi.h` + `dectalk.def` (dapi's names: `DECtalk.dll`, `libtts_us.so`), explained in REFERENCE
    §17; the user's answers are §17.6.** The library outputs to the audio device, a wave file or memory, at 10 kHz only
    (never resampled); `TextToSpeechVersion` says "DECtalk v1.8". **dapi's SAY and speak samples (the former `samples/`) are
    the project's own speaking programs, on Windows and Linux** (user, 2026-09-27; plan REFERENCE §17.8, **done
    §17.12**): SAY as portable C (`src/apps/say/`); speak as a Win32 front end and a GTK 3 front end over one portable
    core (`src/apps/speak/`); one CMake build, `CMakeLists.txt` + `CMakePresets.json` at the top of the project, into `build/`, 64-bit by
    default (presets `x64`, `x86`, `linux`; MSVC, gcc; WSL Ubuntu is on the machine).
    `samples/` is gone, so `make_speak_pics.py` can no longer run: `speak_pics.c` is kept as source.

---

## 1. Repository map

| Path | What it is | Use |
|---|---|---|
| `merges/dectalk_v1.8_full.bin` | correctly assembled 256 KB 68000 image | **the file loaded in Ghidra** |
| `merges/dsp_v1.8_A.bin` | TMS32010 program ROM (E70 = high byte, E69 = low byte) | disassembled (§0.1) |
| `src/` | the hand-written C, the product (moved here from `decomp/src/` on 2026-09-27): **`src/speech/`** = hand-written C (klsyn work item to DSP frames: `klsyn_task_main`, `parse_phoneme_param_stream`, `klclause`, `[:dv]`, `phalloph`, `phtiming`, frame path; REFERENCE §15.18-15.23; the DSP program, `dsp_synth.c` + `dsp_rom.c`, §16.10; the DSP link and DAC clock, `dsp_link.c`, §16.11; the text pipeline, `tx_*.c`: LTS §15.24, dictionaries §15.25, words §15.26, tokens and numbers §15.27, the clause buffer §15.28, phonemic text §15.29, the clause scanner and `dttask_main` §15.30; `src/kernel/rtos.h` = the RTOS calls it uses, `src/kernel/console.c` = the ROM's `kprintf`, `src/kernel/stream.c` = its stdio, `src/kernel/kernel.c` = the speech side's kernel (the `rtos.h` calls on threads that take turns) and `src/speech/engine.c` = `dttask` + `klsyn` + the DSP link on it, what the library runs, §17.9); **`src/api/`** = the speech library (`ttsapi.h`, `dectalk.def`; `ttsapi.c` the thread and outputs, `tts_audio.c` the device, `tts_dict.c` the dictionaries, `tts_stubs.c` the rest; build `decomp/build_lib.bat` → `DECtalk.dll` or `CMakeLists.txt`; check `decomp/test/test_lib.c` + `decomp/scripts/check_lib.py`; §17, §17.10); **`src/term/`** = `dtc01term`, the host terminal emulator on the library (the host tasks on the kernel; `term_line.c`
the lines: console, stdio, TCP, COM/tty; `term_dev.c` the devices with the ROM's XOFF/XON points; `term_speech.c` the
speech half made of API calls; §17.14); **`src/apps/`** = the programs on the library: `say/say.c` (SAY) and `speak/` (speak: `speak_core.c` + `speak_win.c` / `speak_gtk.c`, pictures in `speak_pics.c`, generated once from the former `samples/`; §17.12); **`src/host/`** = the host task in C (escape parser, ESC/CSI/DCS commands, DT_PHONE and the dialer, DECTST, settings/NVRAM; §15.35), the phone task (`hs_phone.c`: `phtask_main`, the spoken DTMF menu; §15.36) and the main task (`hs_setup.c`: the local terminal and SETUP, with `hs_help.c`, its help texts; §15.37), and the host-timeout and stop tasks (`hs_tasks.c`, §15.38) | built by `CMakeLists.txt` and `decomp/build_*.bat`; checked by `decomp/test/` |
| `decomp/` | C decompilation work (the sources themselves are in `src/`): `raw/` (per-function Ghidra export + tables), `draft/` (grouped by speech/host/shared/kernel), `worklist.tsv`, `scripts/`, `test/` + `build_ph_test.bat` / `build_tx_test.bat` / `build_host_test.bat` / `build_dsp_test.bat` / `build_link_test.bat` / `build_engine_test.bat` = the harnesses (`test_frames`, `test_text`, `test_clause`, `test_dttask`, `test_dsp`, `test_link`, `test_engine`, `test_host`, with `--phone` / `--setup` / `--timeout` / `--stop` for the other host-side tasks), `reference/` = ROM frame logs; `scripts/check_term.py` = `dtc01term` against the ROM's host side (§17.14); see `decomp/README.md` | regenerate `raw/` after Ghidra changes (§15.15-15.16); `python decomp/scripts/check_frames.py` after C changes |
| `CMakeLists.txt`, `CMakePresets.json` | the product build (moved from `decomp/` on 2026-09-27): the library, SAY, speak, `dtc01term`, and the checks test_lib, test_kernel, test_line; no ROMs; presets `x64` (default), `x86` (32-bit, opt-in), `linux` | `cmake --preset x64` + `cmake --build --preset x64` from an x64 developer prompt (REFERENCE §17.12) |
| `build/` | the CMake build (`bin/`, `lib/`; the 32-bit one in `x86/`); generated, not source | delete freely |
| `dsp/` | `tmsdis.py` (TMS32010 disassembler, opcode map from `native/tms32010.c`) + `dsp_v1.8.lst` (annotated listing) | DSP work (REFERENCE §16) |
| `native/` | from-scratch emulator (Musashi 68000 + TMS32010 + SCN2681); `spclog.c` + `build_spclog.bat` = DSP frame logger (uses `dtc01_set_spc_tap`); `phcapture.c` + `build_phcapture.bat` = clause-state capture for the C harness (instruction hook via `phcap_hook.h`); `dspcapture.c` + `build_dspcapture.bat` = the DSP's port log for `test_dsp` (TMS32010 hook via `dspcap_hook.h`); `hostfeed.h` = their `-H` host-line feed and `-T` typed local-terminal feed | ground truth for memory map; can *run* the ROM; REFERENCE §16.9, §15.18, §15.23 |
| `docs/` | **Current:** `EK-DTC01-OM-002_Owners_Manual.html`, `EK-DTC01-RM-003.html` (ABBYY FineReader HTML; real `<table>`s, figures/tables that stayed raster are in `*_files/*.png`) and `mitalk.html` (+ `mitalk_files/`) — *From Text to Speech: The MITalk System* (Allen/Hunnicutt/Klatt, 1987). **`*.pdf`** = image-only scans of the same three books (ground truth when OCR is doubtful). **`hunnicutt_lts.pdf`** = Hunnicutt 1976, *Phonological Rules for a Text-to-Speech System* (full LTS rule listing, pp. 64-72; OCR text layer, but read the rule pages as images). **`Klatt_1980_CascadeParallelFormantSynthesizer/`**, **`Klatt_1982_KlattalkTTS/`** = third-party Markdown study notes on those papers; **`klatt1980.pdf`**, **`klatt1982.pdf`** = the papers themselves (REFERENCE §14.10) | behavior spec; MITalk lineage |
| `dtc01_disasm/` | Ghidra project directory | do not edit by hand |
| `AGENTS.md` / `REFERENCE.md` | our notes (short entry point / detailed reference + evidence trail), both at the top of the project (REFERENCE.md moved there from `docs/` on 2026-09-27) | keep REFERENCE current |

The HTML manuals must be converted to text before reading (script in REFERENCE App. A). The former `xtras`
text files were RMS variable-length records (decoder in REFERENCE App. B).

---

## 2. Hardware and firmware identity

| Fact | Value | Src |
|---|---|---|
| Product | DECtalk DTC01-AA ("DECtalk I"); DTC03 = "DECtalk III" (later) | [M] |
| This ROM | firmware **v1.8** ("one point eight", printed as *"DECtalk version %s is running."*) | [V] |
| Other firmware | 2.0 exists: adds silent `[+]` phoneme, Whispery Wendy `:nw`, Forte-voice `fo` param, better control-char logging | [M] |
| DA product id | **19** (reply `CSI ? 19 c`) | [M] |
| CPU | Motorola 68000, big-endian, reset SP `0x8c000`, PC `0x1f6` | [V] |
| Synth | TMS32010 DSP (program: `dsp/dsp_v1.8.lst`) + DAC. Parameter frame every **6.4 ms**; one waveform sample every **100 µs** (10 kHz, 64 samples/frame); audio lags parameter send by ≈6 ms | [M] |
| Serial | SCN2681 DUART: two ports — **host** (RS-232 to computer; default 1200 baud) and **local** (terminal; default 9600) | [M]/[V] |
| Phone | line interface (DTMF decode, ring detect, pulse/tone dial, off-hook) driven through the DSP handshake (SPC/TLC) | [V] |
| Power-up | ~7 s self-test, then speaks "DECtalk version xxx is running"; powers up **on-line** | [M] |

Memory map [V] (matches `native/dtc01.c`):

| Region | Base | Size |
|---|---|---|
| ROM | `0x000000` | `0x040000` |
| RAM | `0x080000` | `0x014000` |
| LED / NVRAM (X2212) | `0x094000` | `0x000400` |
| DUART | `0x098000` | `0x000020` |
| SPC/TLC (68000↔DSP) | `0x09C000` | `0x000008` |

ROM image: 16 chips byte-interleave in pairs (`En` = high byte, `E(n+14)` = low) into eight
32 KB banks. `merges/dectalk_v1.8_full.bin` is already the correct 256 KB image.

Emulator API (`native/dtc01.h`, flat C ABI): `dtc01_create(main_rom, len, dsp_words, n)`,
`dtc01_feed_text` (bytes into DUART channel B, which v1.8 uses as the **local terminal**), `dtc01_feed_host`
(channel A, the **host line**: escapes parsed, XOFF honoured), `dtc01_run_samples`,
`dtc01_read_host_tx` (channel B output) / `dtc01_read_host_line_tx` (host-line output), `dtc01_is_idle` /
`dtc01_input_idle`, `dtc01_dsp_active`, `dtc01_get_led`, `dtc01_read_ram32`,
`dtc01_state_save/restore`, `dtc01_unmapped_accesses` (non-zero ⇒ emulation diverges), `dtc01_set_selftest` (opt-in:
run the power-up self-test with its DTMF tones, REFERENCE §15.32; off by default so the reference logs stay valid),
`dtc01_phone_ring` / `_ringing` / `_offhook` / `_line_in` / `_keys` (the telephone line: rings, caller keys, hook state;
§15.34), `dtc01_feed_break` (a BREAK on the local line, which enters SETUP) and `dtc01_local_idle`. `hostfeed.h` (`-H`)
scripts calls with `\W` (wait 5 s), `\g` (ring) and `\kKEYS;` (caller keys), and takes any byte as `\xHH` (8-bit C1
codes, SO/SI). With `-T` it types the text on the local terminal instead, paced like a person, with `\B` for a
BREAK (§15.37).
`native/selftest.c` is a minimal harness (feeds `"[:np] Hello world.\r"`, drains host TX).
**Use it to test hypotheses dynamically** (test vectors: REFERENCE App. C and B.4).

---

## 3. Boot tasks (the map every session needs)

Small preemptive RTOS, syscalls by `TRAP #1` with the handler pointer in **A1**. Boot task table
`0x12bac` (14-byte records `{entry, handle_out_ptr, priority, name_ptr}`), spawned by
`spawn_system_tasks` `0x310a`:

| Task | Entry | Pri | Role |
|---|---|---|---|
| `phone` | `phtask_main` `0xf128` | 0 | phone line / DTMF events (REFERENCE §15.34) |
| `host` | `host_task_main` `0xd59e` | 0 | host-link escape parser |
| `klsyn` | `klsyn_task_main` `0x3c20` | 50 | Klatt front end → DSP |
| `dttask` | `dttask_main` `0xf946` | 10 | text pipeline |
| `host timeout` | `host_timeout_task_main` `0xf070` | −100 | 5-second speech timeout (emits CTRL-K) |
| `stop` | `stop_task_main` `0xfa7a` | 0 | waits for DT_STOP, aborts speech |

The first task, **`main`** (`main_task` `0x2700`, formerly `setup_mode_main_loop`), is created by `boot_init`
`0x1024`; it spawns the tasks above and then runs the SETUP/local-terminal loop (REFERENCE §12.35, §15.15).

Details (TCB layout, queues, ISRs, RAM globals): REFERENCE §3–§4. Function inventory (305 of 362
named): REFERENCE §10.

---

## 4. Facts that are easy to get wrong (read before editing anything)

These supersede older wording still present in some Ghidra plate comments (full list: REFERENCE §12).

- **`0x0B` is CTRL-K/VT = clause flush**, not an "attention marker". `0x1A` (SUB) is the substitute-for-bad-
  character *and* a clause terminator; `emit_sync_marker` also writes `0x1A` as the pipeline sync marker.
- **`DAT_000822ca` = `DT_LOG`, `DAT_000822ce` = `DT_MODE`** flag words (values in REFERENCE §5.4). The three
  logging helpers are LOG_ERROR (`0x20`), LOG_TRACE (`0x40`) and a `0x80` debug logger — **not** charset
  helpers. Renamed in Ghidra (2026-09-23): `log_trace` `0xd21e`, `log_debug` `0xd1f4`, `g_log_flags`,
  `g_mode_flags`.
- **`[:dv sex]`** changes **head size (∓18) and F4/F5 (×1.218 / ×0.821)**, *not* pitch/pitch range. The
  `parse_bracket_command` plate comment was corrected on 2026-09-23.
- **Voice records** are 28 big-endian shorts (stride `0x38`) at the addresses in REFERENCE §8.1; Paul's
  matches the manual's `[list]` example exactly. That example is `listall` output: v1.8's `list` hides the 8
  `kind & 8` parameters (REFERENCE §15.22).
- **Phoneme codes 0-55** (`_ iy ih ey … jh`, `em` = 35) come from the name table at `0x19704`; they equal
  dapi's `l_us_ph.h` except code 35 (REFERENCE §8.3). `inhdr[]`/`mindur[]` are **shorts in 6.4 ms frames**.
- **`Dr.`/`St.`**: default drive/street; a capitalized next word gives doctor/saint (resolved).
- **`token_dispatch` `0x4062` (formerly `numeric_token_dispatch`) gets every token of ordinary text**, not only
  numbers; non-numbers fall through to `out()`, which builds the words `pronounce_word_or_abbrev` sees (REFERENCE
  §15.27). A lone `0` is spoken "oh", and time fields are separated by ",", not "colon".
- **The host task acts on an escape at once; the text pipeline runs about as fast as speech.** A DT_LOG/DT_MODE
  change on the host line therefore applies to text sent *before* it that is still queued (REFERENCE §15.28).
- **`kprintf` is the ROM's own printf: only `%d` `%c` `%s`.** `0x%x` prints `0xx` (REFERENCE §12.49); the C uses
  `src/kernel/console.c`, never the C library's printf, for console text.
- **The power-up tones are the self-test's DTMF loopback** (`reset_entry` `0x1f6`): 16 digits `0-9 * # A-D` sent to
  the DSP (`0x8000|Hz`, `0x9000|Hz`) and read back from the phone chip's receiver. The emulator holds IP4 low (the
  "skip self-test" jumper) unless its self-test mode is on (`dtc01_set_selftest`, `spclog -t`; REFERENCE §15.32). Host-terminal I/O decision: serial lines
  through byte backends (COM/com0com, TCP, stdio), phone as a simulated line (REFERENCE §13 item 15).
- **SETUP is entered only from the local terminal:** by a BREAK or the SET INTERRUPT character. In the emulator the
  BREAK is `dtc01_feed_break` (`\B` in a `-T` text); a typed NUL is dropped. A keyword's abbreviation is its upper-case
  letters in the ROM (`SAve`, `LOCal`; REFERENCE §6, §15.37).
- **Scripting a call (`hostfeed.h`):** put a `\W` between DT_PHONE 10 and the ring (`\g`). Otherwise the reset
  event that DT_PHONE 10 posts cancels the answer (REFERENCE §15.36). A call made before the host's first byte gets
  the spoken DTMF menu (stand-alone mode).
- **Extended DSR request is `ESC [ n`**; firmware probe = extended DSR, `[+]`, extended DSR (v1.8 ⇒ error 25).
- **What v1.8's host side does not do** (REFERENCE §15.35):
  - There is no DT_MASK: P2 83 is DSR error 26.
  - Replies omit zero parameters (`ESC [ n`, `ESC P ; 70 ; z ESC \`).
  - Extended DSR never says `?21`.
  - DECTC1 is on at power-up, so 8-bit input loses bit 7 until DECAC1 (`ESC SP 7`).
- **Reset behavior (RM Tables 5-1/5-2, from the scan):** user dictionary deleted by power-up and **RIS only —
  DECSTR keeps it**; DECSTR/RIS/power-up hang up the phone; pending text flushed by power-up/RIS only; default
  `DT_TERMINAL` = 6, `DT_LOG` = 0; line speeds/formats only from power-up/DECNVR (REFERENCE §5.5).
- **LTS rules** start at `0x13e10`; rule order in the ROM table *is* priority (never reorder/dedupe). The table is
  **fully decoded** (REFERENCE §15.10, which includes a decoder script). Three things to remember:
  - Item codes use the phoneme/token numbering. The 16 "regular" letters enter as their phoneme code, while
    `A E I O U H X C J Q` are letter codes 70-79 (these were the "unexplained tokens").
  - `:--` is the suffix boundary `+` and `:++` the prefix boundary `=`.
  - The 373 rules run: suffix stripping → consonants → suffixes → prefixes → digraphs → vowels → stress/reduction →
    cleanup. This is Hunnicutt 1976 re-encoded with feature classes (§14.9).
- **Intonation is Klattalk's hat pattern**, not MITalk's O'Shaughnessy algorithm. The per-frame F0 routine is
  `pht0draw` `0xc722` (= dapi `ph_drwt01.c`), which uses `f0segtars` `0x169c4` (identical to dapi). `phsettar`
  `0xaa08` (= dapi `ph_setar.c`, [I]) reads `begtyp`/`endtyp`. Klatt's `amptable` (dB→linear) is in the **DSP** ROM,
  so the 68000 probably sends dB/Hz values (REFERENCE §14.10, §15.14).
- **DSP link (REFERENCE §16):**
  - **Confirmed from the emulator log (REFERENCE §16.9, §12.37):**
    - The **19-word `0x4000` frame is the speech frame**, sent every 6.4 ms: `T0(=40000/F0) F1 F2 F3 FNZ B1 B2 B3 AV
      AH A2 A3 A4 A5 A6 AB TLT`, then the trailer `0x43D4`.
    - The **24-word `0x6000` frame is the speaker definition**, sent on voice changes: f4/b4/f5/b5 scaled by head
      size, p4 p5, ap·10, la, g1-g5, ri/nf open phase, br, pr, the hs scale, gf gn gv gh, and a checksum
      `sum & 0x7FFF == 0` (`dsp_send_speech_frame` `0x122a4`).
    - There are also **bit-15 tone commands**.
    - `native/build/spclog.exe` logs these, and `decomp/reference/` holds the deterministic reference logs the C
      rebuild must reproduce.
    - **Builder (REFERENCE §15.17):** `klclause` `0x7a04` (dapi `phclause`) → frame loop `0xa782`: `phsettar` →
      `phdraw` `0xa804` → `pht0draw` → `dsp_post_frame` `0x7b56` (`parstochip` `0x822a4`, 18 words + trailer) →
      `dsp_queue`. The speaker packet comes from `setspdef` `0x81e6` (dapi `SP_CHIP` plus `f0minimum`/`f0scalefac`,
      words 7/17, which only the 68000 uses). The tracks are `ph_params` `0x8197e` (`ph_param_t[15]`, dapi
      `PARAMETER`).
  - The DSP converts dB amplitudes with Klatt's `amptable` and computes one 10 kHz sample per DAC interrupt,
    using `parwav.c`'s structure (4× glottal source, cascade, then a parallel branch with alternating signs). Its
    voicing is parwav's **natural glottal source**: the table at DSP `0x1C7` is parwav's `B0[224]`, byte-identical.
  - `kl3_push_event` is **not** part of this path: it is dapi's `make_f0_command` (F0 events for `pht0draw`).
  - **The DSP paces the 68000** (REFERENCE §16.10): it looks for a frame only every 64 samples, and after three
    frame times without one it stops producing samples until a frame comes (raising the semaphore with the error
    bit once, if it spoke: the "error" the 68000 sees after every utterance). The cascade runs nasal zero, nasal
    pole, F5 … F1; the parallel branch runs on the noise.
  - **The 68000 resets the DSP before every speaker frame, and after an error** (at the next post), and then sends
    the last speaker frame again (REFERENCE §16.11).
  - **Three flaws of the DSP program itself** (REFERENCE §16.12): a low thump when speech starts with aspiration after
    a reset (the cascade runs on 0 Hz, zero-bandwidth poles until the first glottal period), a small DC offset
    from truncation, and tones that start on a step and leave the DAC at half scale after each tone (the
    oscillators are cosines from phase 0). `dsp_t.fixes` (`DSP_FIX_FIRST_PERIOD`, `DSP_FIX_ROUND`, `DSP_FIX_TONE`)
    fixes them for the library; the tests run with them off. The library is for low latency (screen readers), not the ROM's timing (user, 2026-09-27).
    The 68000 side has library-only fixes too, also off in every ROM check (`engine_init`'s `ENGINE_FIX_MARKS` and
    `ENGINE_FIX_SPLIT`; `tx_fixes`, `ph_fixes`; REFERENCE §17.13): index marks that leave the speech as it is and come
    at their word's first phone, and no stale durations after a mid-text `[:n.]`/`[:ra]`. Any new fix goes the same
    way: a flag, off by default, the ROM path untouched.
- **DUART channel A is the host line, channel B the local terminal** (v1.8; REFERENCE §15.23, §12.42). The
  emulator's `dtc01_feed_text` types on the *local terminal*, where escapes are spoken, not parsed. For escape
  sequences (DT_SYNC, DT_STOP, index markers, …) use `dtc01_feed_host` / `spclog -H` / `phcapture -H` (`\e` = ESC,
  `\w` = wait 0.5 s; `native/hostfeed.h`) and read replies with `dtc01_read_host_line_tx`.
- **RTOS names changed on 2026-09-26** (REFERENCE §12.36, §15.16): `queue_send` → `event_wait`, `queue_receive` →
  `dev_getc`, `critical_section_enter_exit` → `dev_control`, `notify_sync_point` → `sem_wait` (DT_SYNC *waits*),
  `wait_for_stop_signal` → `task_suspend`. Old names in older text or plate comments mean these.
- **OCR:** numbers in the HTML manuals must still be checked against the ROM or the PDF scan (the deleted
  DjVu OCR had wrong digits; see REFERENCE §12.11, §12.21-27). The HTML prints the dial hook-flash `^` as `*`.

---

## 5. Next steps (top of the queue — full list in REFERENCE §13)

1. ~~`dcs_command_dispatch` `0xe152`: confirm the P2 constants, name the handlers, find the R2/R3 reply builders
   and the DT_MASK (P2 83) CR logic.~~ Done (REFERENCE §15.35): the constants are confirmed, and there is no DT_MASK
   in v1.8.
2. `DT_PHONE`: the handler is `dt_phone_command` `0xe5f6` (not `FUN_0000eecc`); dialing and the DSP tone path are
   read, and the emulator's self-test mode plays the power-up tones (REFERENCE §15.32). The phone task's side is
   read (device ops, TLC interrupt, ring counting, replies, stand-alone mode at power-up; §15.34), and the emulator
   models the line (rings, caller keys, hook state). The host side is C (§15.35; R3 = 3 is "text of 256+
   characters"), and so are the phone task (§15.36) and SETUP (§15.37).
3. ~~`csi_command_dispatch` `0xddd8` and the DSR error-flag word (`0x81f12`?).~~ Done: `dsr_reply` `0xdff2`, bits 0-5
   of `0x81f12` = errors 22-27 (§15.35).
4. ~~Clause scanner~~: done (`clause_readin` `0x3182`, REFERENCE §15.30).
5. Phonetic component: decompile `phsettar` `0xaa08` against dapi `ph_setar.c`, find its target tables, and follow
   its output to the queue items `dsp_send_speech_frame` sends. The hat-pattern command generator (dapi `phinton`)
   is the tail of `phtiming` (`0xa00c-0xa544`, the callers of `kl3_push_event` = `make_f0_command`); split or
   annotate it (REFERENCE §13.13-14, §15.14).
6. DSP: frame words are mapped, the FIFO log exists (REFERENCE §16.9) and the 68000 frame builder is traced
   (§15.17) and **rebuilt in C, word for word** (§15.18). **The DSP program itself is C** (`dsp_synth.c`, §16.10):
   every sample equals the ROM's DSP on the corpus and the self-test tones (`test_dsp`, mode `dsp` of
   `check_frames.py`), and it is plain C (named state, exact fixed-point arithmetic). **The link and the DAC clock
   are C too** (`dsp_link.c`, §16.11): the 68000's queue and semaphore handler, the SPC, the tone hooks and
   `dsp_link_run`, checked on the ROM's timing (`test_link`, mode `link`). **Index marks at audio time** work too
   (§16.13): `ph_mark_hook` defers a mark to the frame it goes with, the post carries it as a tag, and `dsp_link`
   reports it at that frame's first sample. **The speech side runs on its own** (§17.9): `kernel.c` runs `dttask`
   and `klsyn` as threads that take turns by priority, and `engine.c` ties them to the link; from power-up it posts
   every frame of the corpus as the ROM does (`test_engine`, mode `engine`; Windows and Linux). **The library
   speaks** (§17.10): `DECtalk.dll` / `libtts_us.so` with its thread, the device, wave files and memory buffers, index
   marks when heard, `Sync` (dapi's: until heard), `Reset`; `Speak` never waits (user, 2026-09-27). Checked by
   `test_lib` / `check_lib.py` (the same samples and marks through every output, Windows = Linux). **Every API call
   works** (§17.11): user and built-in dictionaries, `ConvertToPhonemes`, v1.8's voices, tones, console, log file,
   the phoneme array; only §17.4's stubs are left. **SAY and speak run on it** (§17.12), on Windows and Linux, built
   by `CMakeLists.txt`. v1.8's in-text `[:in n]` marks change the speech around them in five ways, and a
   mid-text voice/rate change reuses stale durations. The library fixes all six (`ENGINE_FIX_MARKS`,
   `ENGINE_FIX_SPLIT`; §17.13): with a mark before every word the speech is phone for phone the same, and each mark
   comes at its word's first phone. speak's highlighting is on by default with an on/off switch. **The host terminal
   emulator runs on the library** (§17.14): `dtc01term` (`src/term/`) runs the ROM-checked host tasks unchanged on
   the kernel, over console/stdio/TCP/COM lines, and speaks through `DECtalk.dll`; its host-line replies equal the
   ROM's on the corpus (`check_term.py`). Next: the options for the phone line (user, 2026-09-27: to be worked out
   before its design), then a second instance.
7. C rebuild: the frame path (REFERENCE §15.18), `phtiming` with the F0 commands (§15.19), `phalloph` (§15.20) and
   `klclause` + `parse_phoneme_param_stream` (§15.21), `parse_bracket_command` (§15.22) and the klsyn task loop
   with DT_SYNC, DT_STOP and index markers (§15.23) are done and match the ROM word for word. The whole klsyn task
   now runs in C, from its mailbox to the DSP posts. **The text pipeline (`dttask`) is done too** (§15.24-15.30): LTS
   (`tx_lts.c`), dictionaries (`tx_dict.c`), words (`tx_word.c`), tokens and numbers (`tx_num.c`), the clause buffer
   (`tx_clause.c`), phonemic text (`tx_phon.c`) and the clause scanner with `dttask_main` (`tx_scan.c`), each checked
   per call (`test_text`, `test_clause`) and the whole pipeline end to end (`test_dttask`: every character read,
   every klsyn message and console byte, from reset). Speech is C from the text pipe to the DSP words. **The host
   side is C, apart from the kernel: the `host`, `phone`, `main` (SETUP), `host timeout` and `stop` tasks**
   (`src/host/`, §15.35-15.38), checked end to end by `test_host` (`--phone`, `--setup`, `--timeout`, `--stop`).
   The speech library's API is drafted (REFERENCE §17) and the user has answered its questions (§17.6); the SAY/speak
   plan is settled (§17.8). The DSP program is C (§16.10), the speech side runs on its own kernel (§17.9), and the library
   speaks through its thread and outputs (§17.10), with the whole API (§17.11), and SAY and speak use it, built with CMake (§17.12). Host-line corpus entries test escapes. Plain local-terminal entries must stay short (no XOFF
   there); `'term'` entries, typed with `-T`, are paced. Host-line entries should stay under ~300 characters or be
   split with `\w` (REFERENCE §13, the caveats).

## 6. Where to look in `REFERENCE.md`

| Need | Section |
|---|---|
| RTOS, TCB, queues, ISRs, output streams | §3 |
| RAM globals | §4 |
| Host escape protocol: DCS P2 table, DT_PHONE, LOG/MODE/TERMINAL/MASK bits, CSI/DSR/RIS/DECNVR, control chars | §5 |
| SETUP mode commands | §6 |
| Pipeline model, word order, dictionaries, numbers, Tables A-1/A-2/B-1 | §7 |
| `[:…]` voice commands, per-voice ROM values, phoneme-code table, 1-char alphabet | §8 |
| ROM data/string catalog, duration arrays vs MITalk | §9 |
| Named-function inventory | §10 |
| dapi cross-reference map | §11 |
| Corrections list (what older notes/plate comments got wrong) | §12 |
| Prioritized next steps, emulator checks | §13 |
| MITalk lineage: FORMAT rules, durations, F0 constants, Klatt params, LTS design | §14 |
| Hunnicutt 1976 LTS paper (rule listing, notation, stress rules) vs the ROM rules | §14.9 |
| Klatt 1980 synthesizer / Klattalk 1982 notes vs the ROM (hat-pattern F0, DSP `amptable`) | §14.10 |
| Phonetic component evidence: `pht0draw`, `phsettar`, `f0segtars`/`begtyp`/`endtyp`/`notetab` | §15.14 |
| C decompilation step 1: bulk export, kernel gap fill, vector table, boot/.data, speech/host/shared classification | §15.15 |
| RTOS system-call API, C library, `/DTC01` types (TCB, device, mailbox, stream, voice, DSP message) | §15.16 |
| DSP (TMS32010): hardware interface, memory map, frame protocol, synthesis structure, open items | §16 |
| DSP frame word map (confirmed), emulator frame logger, reference corpus | §16.9 |
| **The DSP's thump and DC offset, and the library's fixes** (`dsp_t.fixes`) | §16.12 |
| **Index marks at audio time:** `ph_mark_hook`, `index_mark_spoken`, tags through the link, `mark()`, the checks | §16.13 |
| **The speech engine and its kernel:** tasks as threads taking turns, `engine_init` (= `speech_init`'s speech part), klsyn's wait for room and `yield`, `test_engine`, speed and latency | §17.9 |
| **The library's thread and outputs:** `ttsapi.c`, the device (waveOut / ALSA by dlopen), wave file, memory buffers, events at their sample, `Speak`/`Sync`/`Reset` semantics, `test_lib`, `check_lib.py`, device timings | §17.10 |
| **The rest of the API:** user dictionary (`tts_dict.c`, files), built-in dictionary (`DictionaryHit`, `DumpDictionary`), `ConvertToPhonemes`, voices, tones and `DSP_FIX_TONE`, console, log file, phoneme array | §17.11 |
| **SAY, speak and the CMake build:** the ports (`src/apps/`), what changed for v1.8, CTRL-C, the GTK front end, Val's picture, what `[:in n]` marks do to v1.8's timing, the memory-output `Sync` fix, the targets and how they were checked | §17.12 |
| **`dtc01term`, the host terminal emulator:** the design and the user's answers, the lines and the escape key, the speech hooks as API calls, the host line's XOFF/XON points from the ROM, the checks (`check_term.py`, `test_kernel`, `test_line`), the library's `Sync` fix | §17.14 |
| **Index marks that leave the speech as it is:** v1.8's five mark effects and the stale-duration flaw, the library's fixes (`ENGINE_FIX_MARKS`, `ENGINE_FIX_SPLIT`), marks out of the clause (message trailer), the checks; speak's highlight switch | §17.13 |
| **The DSP link and DAC clock in C:** `dsp_link.c`, `test_link` (posts on the ROM's timing, `--wav`), when the 68000 answers, resets and re-sent speaker frames, tones, the ROM's late frames | §16.11 |
| **The DSP program in C:** `dsp_synth.c`, its tables, the port log (`dspcapture`) and `test_dsp`; frame pacing, idle, the signal chain, tone mode | §16.10 |
| 68000 frame builder: `klclause` → `phsettar`/`phdraw`/`pht0draw` → `dsp_post_frame`; `setspdef`; track type | §15.17 |
| The frame path in C: files, capture/harness, 68000 arithmetic details, coverage | §15.18 |
| `phtiming` in C: duration rules, hat-pattern F0 commands, `--timing` test | §15.19 |
| `phalloph` in C: input symbol codes, r-colouring, stress/boundary/hat bits, `--alloph` test | §15.20 |
| klsyn work items in C: `parse_phoneme_param_stream`, `klclause`, pseudo-phonemes, voices/Val, `--stream` test | §15.21 |
| `[:dv …]` in C: `parse_bracket_command`, name/value syntax, `list`/`listall`, console capture, `--bracket` test | §15.22 |
| klsyn task loop in C, DT_SYNC/DT_STOP/index markers, the emulator's host line (`dtc01_feed_host`, `-H`), `--task` test | §15.23 |
| Text pipeline in C: stage plan, LTS engine (`tx_lts.c`, node pool, quirks), `tx_rom`, `test_text --lts` | §15.24 |
| Dictionaries in C: built-in trie layout (decoded), user hash table, suffix stripping and tails, `)` verb marker, `--dict`/`--udict` | §15.25 |
| Words in C: `pronounce_word` steps, `char_class` bits, spelling, punctuation symbols, wh- questions, `Dr.`/`St.`, `--word` | §15.26 |
| Tokens and numbers in C: `token_dispatch`, the number patterns (decoded), what the ROM says for each class, `out` flags, `--token` | §15.27 |
| The clause buffer in C: `clause_putsym` mark merging and its quirk, what ends a clause, the DT_LOG phoneme log, `test_clause` | §15.28 |
| Phonemic text in C: comments, `:dv` text, names and letter classes, `<dur,f0>`, ASKY input, `--phonemic` | §15.29 |
| The clause scanner and `dttask` in C: character classes, backspace, brackets, spoken control characters, the ROM's `kprintf`, `test_dttask` | §15.30 |
| The speech tables as named, typed C tables: layouts, re-encoded offsets, reads outside a table, the test-only address map, AddressSanitizer | §15.31 |
| DTMF tones: the DSP tone command, DT_PHONE dialing, the power-up self-test and its DTMF loopback, the emulator's self-test mode | §15.32 |
| The NVRAM settings record (layout, version, checksum) and why the emulator's boot says "NVR fault" | §15.33 |
| The phone task's side of DT_PHONE: TLC registers, `phone_dev` ops, ring counting, `phtask_main`, stand-alone mode | §15.34 |
| The host task in C: files, the `host.tsv` capture and `test_host`, the boundary, what v1.8 does (no DT_MASK, DSR, DECTC1, supplemental set) | §15.35 |
| The phone task in C: `phtask_main`, the spoken DTMF menu, `phone.tsv` and `test_host --phone`, the `host_phone` fix, stand-alone mode | §15.36 |
| SETUP and the local terminal in C: `main_task`, the line editor, `tdparse` and its command tree, `main.tsv` and `test_host --setup`, BREAK and `-T` in the emulator | §15.37 |
| The host-timeout and stop tasks in C; kernel-written variables in the captures; where they belong in the library API | §15.38 |
| **The speech library's API (draft):** what follows dapi, the model, the firmware → API mapping, the stubs, prerequisites, the user's answers | §17 |
| The Linux counterpart of window messages (event queue, waitable handle); dapi's SAY and speak samples, what they need, the port plan | §17.7, §17.8 |
| LTS rule-table byte format, feature classes, rule sections, decoder script | §15.10 |
| Per-function decompilation evidence (RTOS, host parser, klsyn, dictionary, LTS rule byte layout, `out`/`outn`, number engine) — the former FINDINGS.md | §15 |
| Docs/OCR caveats, HTML→text converter, table status | App. A |
| RMS decoder + wire-level test vectors from the former `xtras/` | App. B |
| Behavior test corpus for the emulator | App. C |
