# REFERENCE.md — DTC-01 (DECtalk I) detailed lookup tables

Detailed reference for the DECtalk DTC-01 68000 ROM reverse-engineering project. It was split out of
`AGENTS.md` so that the always-loaded file stays small. **`AGENTS.md` holds §0–§2** (ground rules,
repository map, hardware identity, emulator API) — references to `§0.x`, `§1`, `§2` in the text below
point there; everything else (`§3`–`§14`, Appendices A–C) is in this file with the numbering unchanged.

Confidence tags: **[V]** verified in the ROM (decompile / raw bytes) · **[M]** stated by the manuals or
1980s sample code (behavior only) · **[I]** inferred, verify before relying on it.
The former `FINDINGS.md` (evidence trail) and `BRIEF.md` (its summary) were merged into §15 and deleted;
some Ghidra plate comments still carry their old wording — §12 lists what was wrong.

Contents: §3 ROM architecture · §4 RAM globals · §5 host protocol (DCS/CSI/flags/control chars) · §6 SETUP
mode · §7 TTS pipeline (dictionaries, numbers, Tables A-1/A-2/B-1) · §8 voices, phoneme codes, ASKY ·
§9 ROM data/string catalog (+§9.1 durations) · §10 function inventory · §11 dapi map · §12 corrections ·
§13 next steps · §14 MITalk lineage · §15 decompilation evidence trail (ex-FINDINGS) · App. A docs/OCR ·
App. B xtras RMS format · App. C test corpus.

**Tree changes on 2026-09-27 (user):** this file moved from `docs/` to the top of the project, next to `AGENTS.md`
(older text that says `docs/REFERENCE.md` means this file). The hand-written C moved from `decomp/src/` to `src/` at
the top of the project (older text that says `decomp/src/...` means `src/...`). The reference folders `dapi/`, `klsyn/`, `xtras/`,
`samples/` and the raw chip dumps `ROMs/` were removed (to the Recycle Bin): the facts taken from them are recorded
here, and their file names below stay as citations. `merges/` keeps the assembled 68000 and DSP images. The
capture cache `decomp/build/capture/` was cleared as well; `check_frames.py` remakes it when it is missing.

---

## 3. ROM architecture (established)

### 3.1 RTOS
Small preemptive kernel. Every system call is a user stub `lea handler,A1; trap #n; rts`: TRAP #0 passes no
arguments, #1 passes `A0`, #2 passes `D0-D1`, and the TRAP entry calls `(A1)` in supervisor mode. The full API
(semaphores, mailboxes, events and timers, mutexes, devices, heap, signals) is in **§15.16** (renamed
2026-09-26; §12.36 lists the old names).

- Global task list head/current/tail: `0x80000` / **`0x80004` (current task ptr)** / `0x80008`.
- `scheduler_context_switch` `0x112a`: `MOVEM.L` save/restore of `D2-D7/A2-A6/USP` at
  **TCB+0x9C…0xCC** ⇒ A4 (and the others) are genuine per-task state. Saved SP at TCB+0xB4.
- `task_create` `0x116e` (thunk) → `task_create_impl` `0x117c`; initial frame returns to `0x11fc`.
- Waits and devices: `event_wait` `0x986` (formerly `queue_send`), `dev_getc` `0xc00` (formerly `queue_receive`),
  `dev_control` `0xe04` (formerly `critical_section_enter_exit`; `0xffff0001`/`0xfffe0001` lock/unlock a device's
  output side). `event_wait(100,0,0)` is the timed-sleep idiom (tick unit unconfirmed; 5 counted periods = the manual's 5 s timeout only
  if each call is ≈1 s — see §12).

Boot task table `0x12bac`, 14-byte records `{entry:4, handle_out_ptr:4, priority:2, name_ptr:4}`,
read by `spawn_system_tasks` `0x310a`:

| Task | Entry | Pri | Role |
|---|---|---|---|
| `phone` | `phtask_main` `0xf128` (formerly `phtask_main_loop`) | 0 | phone line / DTMF events (§15.34) |
| `host` | `host_task_main` `0xd59e` | 0 | host-link escape parser |
| `klsyn` | `klsyn_task_main` `0x3c20` | 50 | Klatt front end → DSP |
| `dttask` | `dttask_main` `0xf946` | 10 | text pipeline |
| `host timeout` | `host_timeout_task_main` `0xf070` | −100 | **5-second speech timeout** (§12) |
| `stop` | `stop_task_main` `0xfa7a` | 0 | waits for DT_STOP, aborts speech |

Before these, `boot_init` `0x1024` creates the first task, **`main`** (`main_task` `0x2700`, descriptor
`0x268e`). It initialises, calls `spawn_system_tasks`, prints the banner and then runs the **SETUP /
local-terminal** loop. (Formerly `setup_mode_main_loop`, which is not called from `host_task_main`; §12.35.)

Boot: `reset_entry` `0x1f6` (DUART jumper bit `0x9801B & 0x10`: clear ⇒ straight to
crt0 `0x1024`; set ⇒ ROM checksum + RAM march `0x55555555/0xAAAAAAAA`, halt-blink at `0x1cc`
on failure) → crt0 `FUN_00001024` (zero RAM `0x80000-0x8C000`, copy init data from
`0x2646-0x265E`, module-init pointers `0x265E-0x268E`, start scheduler + `spawn_system_tasks`).

### 3.2 Interrupts / I/O
- `duart_isr` `0x16fe` (IRQ6) → `duart_isr_service` `0x1710`: RxRDY → `duart_rx_char` `0x1870`, TxRDY →
  `duart_tx_char` `0x17f8` (formerly `duart_tx_drain_queue` / `FUN_000017f8`). **Channel A is the host line** (host
  device `0x8011e`, which `read_host_byte` reads), **channel B the local terminal** (console device `0x80328`); the
  device's `+0x46` points at its channel (§15.23).
- `dsp_command_queue_isr` `0x12258` (IRQ5 path): drains command queue (head `0x82256`)
  into SPC regs `0x9c000/0x9c002` — carries the synthesizer frames to the DSP (24-word speech frames via
  `dsp_send_speech_frame` `0x122a4`, 19-word frames inline, 2-word tone commands; §16).
- The phone line is **not** on the DUART. It is the telephone interface (TLC) behind `0x9c004`/`0x9c006`, with
  its own level-4 interrupt `phone_tlc_isr` `0x214e`, and the `phone_dev` device `0x80552` whose ops set the hook
  relay and the interrupt enables. The `phone` task `phtask_main` reads its events: `0x81` ring, `0x82` off hook,
  `0x84`/`0x85`/`0x86` reset/hang-up/ringing stopped, and DTMF codes 0-15. The whole path is in §15.34 (it corrects
  the older names `phone_line_status_from_dsp` and `phone_line_state_enter2/3`, §12.53).

### 3.3 Output stream & console helpers
`kprintf` `0xd248` → `vformat_string` `0xd260`; `dev_putc` `0xc70` (formerly `console_queue_putc`);
`console_putchar_caret` `0xd3e4` (caret notation + `<ESC>/<DCS>/<CSI>` names). `vformat_string` is the ROM's own
printf: only `%d`/`%D`, `%c` and `%s`; any other letter after `%` is printed and takes no argument, so `0x%x` prints
`0xx` (§15.30). The four are C in `src/kernel/console.c`.
"Current output stream" pointer **`cur_stream` `0x81f1e`** (`stream_t *`, a small stdio; §15.16): `fprintf`
`0x11c88`, `stream_putc` `0xd99e` (putc), `_flsbuf` `0x11efe`, `fflush_if_pending` `0x11ec4`. `out`/`outn` (`0x556e`/`0x5614`) are **word tokenizers** feeding the
pronunciation pipeline (§7), not phoneme emitters.

---

## 4. Key RAM globals (addresses seen in decompiles)

| Address | Meaning | Tag |
|---|---|---|
| `0x80004` | current task pointer | [V] |
| `0x8011e` | host-line byte queue (host_task input) | [V] |
| `0x80328` | console / local-terminal output queue | [V] |
| `0x80552` | phone event queue | [V] |
| `0x807a2` | klsyn work queue (also posted by `dttask`/`newclause`) | [V] |
| `0x807b4`-`0x807d0` | **the clause buffer** (§15.28): `clause_msg` `0x807b4` (the `klsyn_free_pool` message being filled), `clause_wr` `0x807b8`, `clause_end` `0x807bc` (payload + 199 words), `clause_soft_end` `0x807c0` (25 words before it), `clause_last` `0x807c4` (the last plain symbol stored), `clause_mark` `0x807c8` (`clause_last` when the last `)` `,` `!` `?` `.` arrived), `clause_run` `0x807cc` (symbols stored since), `clause_bound` `0x807ce` (the last `*` word-boundary `(` `)` mark), `clause_silence` `0x807d0` (the last symbol was a silence) | [V] |
| `0x82120`-`0x82147` | **`phoneme_name`'s state** (§15.28): `name_nval` `0x82120`, `name_nvalues` `0x82122`, `name_in_frames` `0x82124`, `name_pseudo` `0x82126`, `name_buf` `0x82128`; shared by the text pipeline's log and klsyn's `phtiming` | [V] |
| `0x80732`-`0x8078e`, `0x81d6a` | **the clause scanner** (§15.30): `readin_buf` `0x80732` (the word being read, 82 bytes, cut at 79), `readin_depth` `0x80784` (`[ ]` nesting), `readin_in_dv` `0x80786` and `readin_in_comment` `0x8078c` (`parse_phonemic_text`'s two flags), `readin_hi` `0x80788` (the end of the word so far), `readin_after_bracket` `0x8078e` (the last `]` closed phonemic text); `dttask_eof` `0x81d6a` (the `0x1A` marker was read); `dttask_in` `0x81f1a` (the text pipe's reading end; `cur_stream` `0x81f1e` is its writing end), `dttask_pool` `0x8208c` (2 messages of 2 words, for the sync message) | [V] |
| `0x80886…0x808d4`, `0x82286` | **`out_buf`** / **`out_ptr`**: the word `out()` is building (79 chars at most); `out_sep` `0x808d8` = the last `out()` flag (§15.27) | [V] |
| `0x807d2`-`0x80885` | **number state** (§15.27): `money_num` `0x807d2` and `money_trail` `0x80824` (held-back money and its punctuation, 82 bytes each), `money_mode` `0x80876`, `currency` `0x80878` (1 lone `$`, 4 "dollar", 6 "dollars" due), `num_sign` `0x8087a`, `num_group` `0x8087e` (the last digit group's value), `unit_number` `0x80882` (the last token was a number: 1 one, 2 several), `num_count` `0x80884` | [V] |
| `0x808da`/`0x8092c`/`0x8097e` | **`tok_lead`/`tok_body`/`tok_trail`**: the token split by `split_token`, 82 bytes each, back to back (§15.27); `pat_start` `0x80f68` = where a number pattern must start | [V] |
| `0x80f82` `allophons[]`, `0x812a2` `allofeats[]`, `0x81112` `allodurs[]` | duration-engine arrays | [V] |
| `0x81c44` | **active speaker parameter block** (28 shorts) | [V] |
| `0x81c7c` | RAM "Val" voice slot (`[save]` target) | [V] |
| `0x81ccc` | last parsed bracket-command value | [V] |
| `0x81d72` | host idle counter (reset to 0 by `read_host_byte`; 5 = timeout) | [V] |
| `0x81d76` | **`wh_question`** (§15.26): −1 at a clause start (`newclause`, `.` `!` `?`); the clause's first spoken word sets 1 if it starts with "wh", else 0; a `?` after a wh- word is sent as `.` | [V] |
| `0x81f0a` | **`word_count`**: `pronounce_word` calls (never read) | [V] |
| `0x80f66` | **`abbrev_pending`**: a lone `Dr.` (1) or `St.` (2) waiting for the next word (`pronounce_word_or_abbrev`, §15.26) | [V] |
| `0x8228a` | **`spell_buf`**: `" c"`, the dictionary key `spell_char` looks up | [V] |
| `0x81f12` | **`dt_error_flags`**, error/status flag word; `\|= 8` on a bad bracket command, and on bad phonemic text (`parse_phonemic_text`, `parse_phoneme`, `lookup_phoneme`, §15.29) ⇒ very likely DSR error 25 "phonemic transcription" (bits 22…27 → 0x01…0x20) | [I] |
| `0x81f26` | **`sync_sem`** (`ksem_t`): `emit_sync_marker` (DT_SYNC) does `sem_wait`, `klsyn_task_main` does `sem_signal` when a sync message arrives | [V] |
| `0x809d4`-`0x80f65` | **LTS working list** (§15.24): `lts_hdr` `0x809d4` (pseudo node; `+6` = the head `0x809da`), `lts_pool` `0x809e2` (100 nodes of 14 bytes `{code.w, feat.l, next.l, prev.l}`), `lts_free` `0x80f5a`, `lts_nnodes` `0x80f5e`, `lts_rule` `0x80f60`, `lts_npos`/`lts_pos` `0x80f62`/`0x80f64`; `lts_pool_next` `0x8228e` | [V] |
| `0x81d74` | **`prev_word_stressed`** (was `clause_has_word`, §12.44): `pronounce_word` subtracts 1 at every word, a primary stress (dictionary or LTS) sets it to 1, `newclause` clears it; so during a word it is ≥ 0 when the previous word of the clause was stressed. The dictionary's `)` verb marker is kept only then (§15.25) | [V] |
| `0x81d70` | **`last_index`**: the last index marker spoken (`index_mark_reached`); `DT_INDEX_QUERY` replies with it (§15.23) | [V] |
| `0x81bec` | **`stop_pending`**: DT_STOP in progress (`stop_task_main`); cuts the clause, klsyn drops phoneme streams (§15.23) | [V] |
| `0x81f2c` | **host escape-parser state struct** (layout in `read_host_char_collect_csi` plate comment: `+0 mode, +2 final, +3 private marker, +4 param count, +6 params[16], +0x26 intermediate count, +0x27 intermediates`) | [V] |
| `0x820a0` | **`dict_user_table`**: the user dictionary (DT_DICT), 32 buckets of malloc'd entries `{next.l, len.b, name, NUL, substitution, NUL}`; `dict_user_count` `0x8209e`, `dict_user_text` `0x809d0` (the substitution of the last user lookup), `dict_hits` `0x81f0e` (built-in hits; nothing reads it) (§15.25) | [V] |
| `0x822ca` | **`DT_LOG` flag word** (Ghidra `g_log_flags`), bits identical to `LOG_*` (§5.4): `0x02` PHONEME (`newclause` logs each clause, §15.28), `0x04` RAWHOST (raw byte → console queue), `0x08` INHOST (caret echo), `0x20` ERROR, `0x40` TRACE, `0x80` DEBUG | [V]/[M] |
| `0x822ce` | **`DT_MODE` flag word** (Ghidra `g_mode_flags`): bit0 SQUARE (tested in `pronounce_word`, clause scanner), bit1 ASKY, bit2 MINUS (`spell_chars`: a spelled `-` is "minus", else "dash"); power-up value 1; C `dt_mode` (§15.26) | [V]/[I] |
| `0x822d2` | host-speak enabled (SET HOST SPEAK / DT_SPEAK); text chars are dropped when 0 | [V]/[I] |
| `0x822e6` / `0x822e8` | DECTC1(1)/DECAC1(0) "strip 8th bit" ; S7C1T(1)/S8C1T(0) | [V] |
| `0x822ea` | pending single-shift (SS2=2 / SS3=3) for next char | [V] |
| `0x822ee`, `0x822f2` | active GL / GR charset designator | [V] |
| `0x822f6/fa/fe`, `0x82302` | G0 / G1 / G2 / G3 designators (`'B'`=ASCII, `'<'`=DEC supplemental) | [V] |
| `0x80160` | inhibit flag for the host-timeout counter (probably "speaking") | [I] |
| `0x81d6c`, `0x81d6e`, `0x822cc`-`0x822e4` | **host side, C names (§15.35):** `phone_dialing` `0x81d6c` (byte), `keypad_ticks` `0x81d6e`, `dt_terminal` `0x822cc` (the LOCAL flags), `host_modem` `0x822d0` (SET HOST MODEM, record word 12), `speak_enabled` `0x822d2`, `phone_standalone` `0x822d4` (byte), `phone_offhook` `0x822d6`, `keypad_timeout` `0x822d8`, `answer_rings` `0x822da`, `line_speed[2]` `0x822dc`, `line_format[2]` `0x822e0`, `setup_interrupt_char` `0x822e4` (SET INTERRUPT, word 10; §15.37); the DCS text `dcs_text` `0x81f66` (256 bytes) and its pointer `0x82066`, DT_PHONE's parameters `0x8206a`/`0x8206c` | [V] |

---

## 5. Host protocol (what the ROM must implement) — from the Programmer Reference Manual

### 5.1 Framing
- **DCS commands:** `ESC P 0 ; P2 [; P3 [; P4…]] z [text] ESC \` (8-bit form: `0x90 … 0x9C`).
  P1 is always 0, final byte is lower-case `z`. Parameters are decimal ASCII; **empty = 0**;
  DECtalk **omits zero parameters in its own replies** (`ESC P ; 31 ; z ESC \` for index 0).
  Sequences with P1≠0 or a different final are ignored (later dapi `esc.h` says the same).
  A sequence ends when its last char is sent — do **not** add CR/LF. Invalid sequences are ignored.
  Escape sequences are honored **only on the host line**, not from the local terminal.
- ESC + `0x40-0x5F` is the 7-bit form of C1 (`ESC [` = CSI, `ESC P` = DCS, `ESC \` = ST, …).
  The ROM does exactly this in `read_host_char_collect_csi` (`+0x40`).
- Up to **16** numeric params and **2** intermediates per sequence (`SEQ_PARMAX 16`,
  `SEQ_INTMAX 2` in `dectlk.h` ⇒ matches the parser struct).
- Flow control: DECtalk sends **XOFF (0x13)** when its input buffer is nearly full and
  **XON (0x11)** when nearly empty. The buffer is sized so a host at 9600 baud can keep
  sending for **250 ms** after XOFF (≈240 bytes) without loss. Overflow is silent (garbled
  words); the host can detect it with DSR (error 23). **[M]** **[V] (2026-09-27, §17.14):** the host device's input
  ring holds 304 bytes (`0x130`, set up at `0x15d0`). `duart_rx_char` sends XOFF when a byte arrives and finds more
  than 64 waiting (`0x192c`), and the device's after-read hook `0x1996` sends XON when a read leaves fewer than 16.
  So 239 bytes of room remain after XOFF: 250 ms at 9600 baud, as the manual says.
- Speech is not started until a **clause boundary**: `. , ! ?` (period is checked for
  abbreviations), a nearly-full buffer (**≈12 words** per RM, "about 50" per OM), or the
  **5-second timeout** (speak buffered text "as if a comma were sent").
- Local terminal: `BREAK` key enters SETUP; `CTRL-R` repeats last line, `CTRL-U` kills line,
  `CTRL-K` ends clause/leaves phonemic mode (off-line editing features).

### 5.2 DCS command table (`P2` numbers — `dectlk.h` values)

ROM handler: `dcs_command_dispatch` `0xe152`; every branch has a self-identifying
`DT_*` bug string [V]. **The P2 constants are checked against the code and rebuilt in C** (2026-09-27, §15.35): 0, 10,
11, 12, 20, 21, 22, 40, 60, 80, 81 and 82. **83 (DT_MASK) is not in v1.8.**

| P2 | Mnemonic (manual / ROM string) | Wire | Meaning / reply |
|---|---|---|---|
| 0 | `DT_PHOTEXT` | `ESC P 0;0 z text ESC \` | speak phonemic text (≡ `[` … `]`); delimiters act like spaces; `/* comment */` allowed inside; ends at `ESC \` |
| 10 | `DT_STOP` | `…;10 z ESC \` | stop speech **immediately**, reinitialize all internal buffers ⇒ `stop_task_main` path [I] |
| 11 | `DT_SYNC` | `…;11 z ESC \` | finish speaking pending text before processing the next command; acts as a clause boundary; **no reply** (follow with INDEX_QUERY to get one) ⇒ `emit_sync_marker` |
| 12 | `DT_SPEAK` | `…;12;P3 z` | RM Table A-1: **P3=1 enable, P3=0 disable** speaking (stop passing host chars to TTS). Also resumed by DT_SYNC, DT_STOP, RIS, DECSTR, PH_ANSWER ⇒ flag `0x822d2` [I] |
| 20 | `DT_INDEX` (`DT_INDEX_TEXT`) | `…;20;P3 z` | insert index marker P3 (0…32767, overflow bits masked); remembered when spoken |
| 21 | `DT_INDEX_REPLY` | `…;21;P3 z` | as 20, but DECtalk replies **R2=31, R3=P3** when the mark is spoken |
| 22 | `DT_INDEX_QUERY` | `…;22 z` | immediate reply **R2=32, R3=last index spoken** (0 if none) |
| 40 | `DT_DICT` | `…;40 z name subst ESC \` | load user-dictionary entry → reply **R2=50, R3∈{0 loaded, 1 no room, 2 too long (256 max)}** |
| 60 | `DT_PHONE` (ROM: `DT_PHONE_HOME`) | `…;60;Pn;Pn… z [text] ESC \` | telephone control, list of sub-commands executed in order (§5.3) → reply **R2=70, R3=status** |
| 80 | `DT_MODE` | `…;80;P3 z` | P3 = sum of MODE flags (§5.4) |
| 81 | `DT_LOG` | `…;81;P3 z` | P3 = sum of LOG flags |
| 82 | `DT_TERMINAL` | `…;82;P3 z` | P3 = sum of TERM flags (local-terminal data paths) |
| 83 | `DT_MASK` (`P2_MASK`) | `…;83;P3 z` | 16-bit keypad-mask: for each set bit DECtalk appends **CR** after that keypad char; any bit set ⇒ CR after every DECtalk-*generated* reply sequence (after `ESC \`; not after terminal-generated characters). P3=0 turns it off. **Bit map (RM Table 5-6/A-8, recovered from the HTML): bit *n* (value 2ⁿ) for n=0-9 ⇒ digit `n`; bit 10 (1024) ⇒ `*`; bit 11 (2048) ⇒ `#`; bits 12-15 (4096/8192/16384/32768) ⇒ `A B C D`.** Example P3=3072 makes `*` and `#` terminators; then keypad `1 2 3 #` + timeout arrives as `123#<CR>` `ESC P ; 70 ; 2 z ESC \ <CR>`. The RM covers firmware 1.8 *and* 2.0 and lists `P2_MASK` in its DECTLK.H, **but v1.8 does not implement it [V]**: `dcs_command_dispatch` treats P2 83 like any unknown P2 (DSR error 26, `DT_XXX: P2 = 83`), so no CR is ever added (§15.35). |

Index markers matter to number/abbreviation rules: `$ 12.45` → "twelve dollars and forty-five
cents", but an index between `$` and `1` gives "dollar twelve point four five" — this is
what `token_dispatch`'s held-back money (`defer_money`, `flush_currency_suffix`, `currency` `0x80878`,
§15.27) implements. Anything that interleaves phonemic text or escape sequences blocks
multi-word rules; keep spoken text contiguous.

### 5.3 `DT_PHONE` sub-commands and replies
P3 sub-commands (`P4` = extra param / dial text follows `z`):

| P3 | Name | Behavior |
|---|---|---|
| 0 | `PH_STATUS` | report status |
| 10 | `PH_ANSWER` | answer after **P4 rings** (0 or 1 = first ring). If phone is off-hook it is hung up first. Two replies: command accepted (on-hook), then off-hook when actually answered. Waiting is cancelled by HANGUP, either dial, RIS, DECSTR. ROM string `"answer in %d ring"` |
| 11 | `PH_HANGUP` | hang up **and disable the keypad**; reply delayed until on-hook (host should wait for R3=0 before sending more). ROM `"hangup"`, `"phone on hook"` |
| 20 / 21 | `PH_KEYPAD` / `PH_NOKEYPAD` | enable keypad ("direct keypad decoding") / disable it *without* hanging up (both ignored, but an on-hook reply is sent, if the phone is inactive) |
| 30 | `PH_TIMEOUT` | start/restart keypad timer, **P4 seconds** (0 cancels). One-shot; expiry sends unsolicited **R3=2**. It is the *only* way to detect that the caller hung up. Set `PH_KEYPAD` on first, or the caller cannot answer |
| 40 | `PH_TONE_DIAL` | dial text; chars `0-9 * # A-D` (+ `!` `^`). ROM `"tone dial %s"` |
| 41 | `PH_PULSE_DIAL` | dial text; chars `0-9` (+ `!` `^`) only. ROM `"pulse dial %s"` |

Dial text extras: `!` = 1 s pause, `^` = 250 ms switch-hook flash (repeat for longer). *(The
HTML OCR of the RM prints the flash character as `*`; that is an OCR error — the **PDF scan shows `^`**
(RM printed p. 49), as does `xtras/dtlib/dtlib.rno` (`'_^'` = RUNOFF-quoted caret); `*` is a DTMF digit.)* If the phone is on-hook,
dialing first lifts the handset and inserts a **2 s delay**. No call-progress detection exists
on the DTC01 (busy / no-answer / dial-tone are invisible; hosts speak a message and wait for a
keypress). The public network may hang up on its own, so hosts must accept unsolicited R3=0
replies. `A B C D` generate the four extra military-handset tones (`A` = right of `3`, `B` below it…).

Replies: `ESC P ; 70 ; R3 z ESC \` with **R3: 0 on-hook, 1 off-hook, 2 keypad timeout, 3 dial text too long (> 256 chars)** (RM Table 4-2: `R3_PH_ONHOOK/OFFHOOK/TIMEOUT/TOOLONG`).
Keypad presses arrive as **plain characters** `0-9 * # A B C D` (not escape sequences), possibly
interleaved with replies. ROM digit table `"D1234567890*#ABC"` at `0x1899a` [V] is the
standard 16-code DTMF-receiver map (code 0 = `D`, 1-9 = digits, 10 = `0`, 11 = `*`, 12 = `#`,
13-15 = `A B C`) — all four extended keys `A-D` are present.

### 5.4 Flag words (`dectlk.h` — exact values)

| Word | Bits |
|---|---|
| `DT_MODE` | `MODE_SQUARE 0x01` (`[ ]` phonemic delimiters; also enables `)`-prefix alternate pronunciations), `MODE_ASKY 0x02` (1-char phoneme alphabet), `MODE_MINUS 0x04` (`-` spoken "minus" instead of "dash") |
| `DT_LOG` | `LOG_TEXT 0x01`, `LOG_PHONEME 0x02`, `LOG_RAWHOST 0x04` (all chars as received except NUL/XON/XOFF), `LOG_INHOST 0x08` (visible form), `LOG_OUTHOST 0x10`, `LOG_ERROR 0x20`, `LOG_TRACE 0x40` (escape sequences shown symbolically), `LOG_DEBUG 0x80` ("reserved for DECtalk internal use" — RM Table A-7 and the DTK$ header agree) |
| `DT_TERMINAL` | `TERM_HOST 0x01` (local typing → host), `TERM_SPEAK 0x02`, `TERM_EDITED 0x04` (line editing), `TERM_HARD 0x08` (hardcopy echo), `TERM_SETUP 0x10` (speak SETUP dialog), `TERM_FILTER 0x20` (RM Table A-4: "do not send DECtalk-specific escape sequences to the terminal"; the filter has to parse escapes — S7C1T/S8C1T, DECTC1/DECAC1 — so it only works with an ANSI terminal) |

### 5.5 CSI / ESC commands

| Sequence | Name | Behavior / reply |
|---|---|---|
| `ESC [ 0 c` or `ESC [ c` | DA primary | reply **`ESC [ ? 19 c`**; secondary DA is ignored |
| `ESC Z` (C1 `0x9A`) | DECID | identical to DA primary. ROM: `host_task_main` handles `0x9A` (`"OLDID"` string) [V] |
| `ESC [ 5 ; Pn y` | DECTST | Pn: 1 POWER (rerun power-up init; **everything resets, phone hangs up, dictionary deleted**), 2 HDATA, 3 HCONTROL (loopback connectors needed), 4 LDATA, 5 SPEAK (canned message incl. firmware version) ⇒ `dectst_self_test` `0xe984` |
| `ESC [ 5 n` | DSR brief | reply `ESC [ 0 n` (ok) or `ESC [ 3 n` (malfunction); does **not** clear error flags. **v1.8 omits zero parameters, so "ok" goes out as `ESC [ n`** [V, §15.35] |
| **`ESC [ n`** (`1B 5B 6E`, **no parameter** — the HTML OCR of RM Table A-1 and ch. 5 gives the bytes `027 091 110`) | DSR extended | ok: `ESC [ 0 n ESC [ ? 21 n` on the first request since power-on, `ESC [ 0 n ESC [ ? 20 n` afterwards (`?21n` = "first since power-up", `?20n` = "not first"). Malfunction: `ESC [ 3 n ESC [ ? Pn ; … n`. **Clears error flags.** (Brief DSR is `ESC [ 5 n`.) **v1.8 never sends `?21n`:** it is `ESC [ n ESC [ ? 20 n` every time (`dsr_reply` `0xdff2`, §15.35) [V] |
| DSR error codes | | **22** communication failure, **23** input buffer overflow, **24** last NVR operation failed, **25** phonemic-transcription error, **26** error in private (DCS) sequence, **27** last DECTST failed |
| `ESC c` | RIS | hard reset to power-up state **without touching line speeds/formats**; pending text flushed; user dictionary deleted; phone on-hook; host speech forced on; log/terminal flags re-read from NVR (Tables 5-1/5-2 below); **also updates the "last NVR operation" status flag reported by DSR (24)**. DEC recommends sending `DT_PHONE:PH_HANGUP` first (a status read during RIS's implicit hang-up may report off-hook). ⇒ ROM `dispatch_esc_command` calls `settings_reset(0,0)` [V] |
| `ESC [ ! p` | DECSTR | soft reset: like RIS but **pending text kept, user dictionary kept**, local-terminal convenience features untouched, phone hung up, host speech forced on. Same "send PH_HANGUP first" advice |
| `ESC [ Pn ; Pm ! r` | DECNVR | **Pn 0 = restore** all feature settings from NVR — may change line speeds/formats (communication with the host/terminal can be lost), does **not** delete the user dictionary and does **not** hang up the phone; **Pn 1 = store** current settings — DECtalk stops processing host-line commands until stored. **Pm 0 = user memory** (read/write; used at power-up), **Pm 1 = factory memory** (read-only, always holds the factory defaults; used at power-up if memory 0 cannot be used; diagnostics may use it to force factory settings). Success/failure of the last NVR operation is remembered and readable via DSR (24) ⇒ `nvram_save_settings` `0xf79c` |
| `ESC SP F` / `ESC SP G` | S7C1T / S8C1T | 7-bit vs 8-bit C1 in **transmitted** sequences (`0x822e8`) |
| `ESC SP 6` / `ESC SP 7` | DECTC1 / DECAC1 | truncate vs accept the high bit on received C1 (`0x822e6`). **v1.8 strips bit 7 of every received byte under DECTC1, and DECTC1 is on at power-up** (factory record word 9), so 8-bit text needs DECAC1 first [V, §15.35] |
| `ESC ( B`, `ESC ( <` (and `) * +`) | designate G0–G3 | `B` = ASCII, `<` = DEC supplemental. Power-up: ASCII in G0/G1, DEC multinational in G2/G3 |
| `SO`/`SI`, `ESC n`, `ESC o`, `ESC ~ } \|`, `SS2`/`SS3` | shifts | LS1/LS0, LS2, LS3, LS1R/LS2R/LS3R, single shifts ⇒ `dispatch_esc_command`, `handle_single_shift` [V] |

**Firmware-revision probe** (RM ch. 5, "Determining firmware revision level") — a ready-made
emulator test for the DSR error-flag word: (1) send extended DSR `ESC [ n` to clear errors;
(2) send the phoneme `[+]` (`ESC P 0;0 z + ESC \`); (3) send extended DSR again. **v1.8 replies
`ESC [ 3 n ESC [ ? 25 n` (error 25); v2.0 reports no error.**

**Table 5-1 — Restoring DECtalk operating features** (recovered from the PDF scan, RM PDF p. 65 = printed
p. 53; the HTML OCR of these two tables was noise). *"PUP" = power-up.*

| Feature | Factory default | Restored from NVR by |
|---|---|---|
| Local line speed | 9600 baud | PUP, DECNVR |
| Local line format | no parity | PUP, DECNVR |
| Host line speed | 1200 baud | PUP, DECNVR |
| Host line format | no parity | PUP, DECNVR |
| Host C1 transmit mode | 7 bit (S7C1T) | PUP, DECNVR |
| Host C1 receive mode | 7 bit (DECTC1) | PUP, DECNVR |
| Local log flags (`DT_LOG`) | **0** | PUP, DECNVR, **RIS** |
| Local terminal flags (`DT_TERMINAL`) | **6** (= `TERM_SPEAK 2` + `TERM_EDITED 4`) | PUP, DECNVR, **RIS** |

**Table 5-2 — DECtalk actions performed at resets:**

| Action | Performed by |
|---|---|
| Implied `DT_SPEAK` (speech enabled) | PUP, RIS, DECSTR |
| Hang up telephone | PUP, RIS, DECSTR |
| **Delete user dictionary** | **PUP, RIS** (not DECSTR, not DECNVR) |
| **Flush all pending text** | **PUP, RIS** (not DECSTR) |
| Turn host speech on | RIS, DECSTR |

So the line speeds/formats and C1 modes come back **only** from power-up or DECNVR (RIS/DECSTR leave the
lines alone), and the default `DT_TERMINAL` = 6 means a fresh unit speaks and line-edits local typing but does
not forward it to the host. The real implementation is `settings_reset` `0xf3be` — its mode arguments
should map onto these columns (RIS = `(0,0)`, DTMF factory reset = `(2,1)`; §13.10).

DECtalk recognizes only SS2, SS3, DCS, CSI, ST among C1 codes (others ignored; the ROM
additionally consumes OSC/PM/APC strings up to ST/BEL and logs "OSC/PM/APC ignored").
Accented supplemental letters are spoken without the accent; other supplemental symbols
are ignored. **DEC Special Graphics (line drawing) is dropped** [V].

### 5.6 Control characters
| Code | Meaning in DECtalk |
|---|---|
| NUL `00`, DEL `7F` | always deleted (not logged) |
| BS `08` | overstrike handling — a word containing BS is reduced by hierarchy *letters/digits > punctuation > underline* (`a BS _`→a, `ab BS BS de`→de …) |
| HT `09`, LF `0A`, FF `0C`, CR `0D`, SP `20` | **all "same as a space"** = word terminator (RM Table 1-1; SP is the "normal word terminator"); ROM maps CR→LF (`host_task_main`) |
| **VT `0B` = CTRL-K** | **clause terminator / flush** ("Clause terminator" in RM Table 1-1); also exits phonemic text mode. Internally generated on the 5 s host timeout (§12) |
| **SUB `1A`** | RM Table 1-1: **on a communication error (parity/framing/overrun) DECtalk replaces the bad character with SUB, and SUB acts as a clause terminator.** ROM: `host_task_main` maps SUB→`stream_putc(0x0B)`. ⇒ check that `duart_isr` (`0x16fe`) substitutes `0x1A` for errored bytes and sets DSR error 22 (§13.4) |
| SO `0E` / SI `0F` | LS1 / LS0 locking shifts |
| XON `11` / XOFF `13` | flow control |
| CAN `15` (`dectlk.h`: `#define CAN 0x15`; = ^U), SUB `1A` (^Z) | abort an in-progress escape sequence (ROM: `read_host_char_collect_csi` aborts on both). Note DEC's "CAN" here is `0x15`, not the ASCII-standard `0x18` |
| ESC `1B` | introducer |
| STX/ETX (`02`/`03`) | **not** phonemic delimiters (use DT_PHOTEXT or `[ ]`) |
| Any other C0 | ignored |

---

## 6. SETUP mode (`main_task` `0x2700`, formerly `setup_mode_main_loop`; `tdparse` `0xfe16`) — Owner's Manual ch. 3

**Entering and leaving [V] (§15.37).** SETUP runs in the `main` task, on the local terminal only. A BREAK on the
local line, or the `SET INTERRUPT` character, ends the local session and enters SETUP. For the BREAK, the DUART gives
a NUL with its received-break status; a typed NUL is dropped. The prompt is `SETUP>`, spoken as "setup." with LOCAL
SPOKENSETUP on. `EXIT` leaves.

The words are lower-cased and matched against the command tree. The upper-case letters of a keyword in the ROM are
the abbreviation that must be typed (`SAve`, `SHow`, `LOCal`, `SPEEd`), so `lo` matches neither `LOG` nor `LOCal`. A
line is run after RETURN. The line editor takes DEL/BS, ^U and ^R.

| Command | Effect (ROM) |
|---|---|
| `BREAK` / `LBREAK` | the host line's break for 230 ms / 3.5 s (host device op 3) |
| `EXIT` | leave SETUP; the new local session takes the mode LOCAL EDITED now gives |
| `HELP [words…]` | the help text of the entry the words lead to (71 texts, `print_help_text`); a bare HELP gives the summary |
| `ONLINE` | LOG = RAWHOST only; LOCAL HOST on; LOCAL SPEAK and EDITED off; host speak on |
| `OFFLINE` | LOG all off; LOCAL HOST off; LOCAL SPEAK and EDITED on; host speak on |
| `RECALL [USER\|FACTORY]` | `settings_reset(2, 0/1)`, as DECNVR; "Failed." on an NVR error |
| `SAVE` | `nvram_save_settings(0)`, the USER record; "Failed." on an NVR error |
| `SHOW …` / `SET …` | below |
| `TEST POWER\|HDATA\|HCONTROL\|LDATA\|SPEAK` | DECTST 1-5; "Failed." when the test fails |

`SET` and `SHOW` parameters. `SHOW <class>` lists all the class's flags; `SHOW LOCAL` and `SHOW HOST` give the line's
speed and format first.
- `LOG TEXT|PHONEMES|RAWHOST|INHOST|OUTHOST|ERROR|TRACE|DEBUG ON|OFF`: DT_LOG bits 0x01-0x80. `DEBUG` has no help text.
- `LOCAL SPEED s | FORMAT EVEN|ODD|NONE | HOST|SPEAK|EDITED|HARDCOPY|SPOKENSETUP ON|OFF`: the flags are DT_TERMINAL
  bits 0x01-0x10.
- `HOST SPEED s | FORMAT … | SPEAK ON|OFF | MODEM ON|OFF`:
  - SPEAK is `speak_enabled` (DT_SPEAK);
  - MODEM is bit 0 of `0x822d0`, sent to the host device as op 6 (modem control);
  - `SHOW HOST` does not list MODEM, which is in class 0.
- `MODE SQUARE|ASKY|MINUS ON|OFF`: DT_MODE bits 0x01-0x04.
- `INTERRUPT c | ^c | OFF`: the character that enters SETUP (`0x822e4`, NVRAM word 10). `SHOW INTERRUPT` prints
  "off", "^B" or the character.
- `HISTOGRAM lo hi` (hexadecimal) and `SHOW HISTOGRAM`: **undocumented, and not in the help**. They start the kernel's
  PC-sampling profiler over [lo, hi) and print its buckets (`profile_start`, `profile_report`).

Speeds (`parse_baud_rate`, exact text): **75/1200, 110, 150, 300, 600, 1200, 2400, 4800, 9600**, the DUART's codes 0,
1, 3, 4, 5, 6, 8, 9 and 11.

Factory settings: host 1200 and local 9600 baud, both with format none; LOCAL SPEAK and EDITED on; MODE SQUARE on;
interrupt character off.

Manual notes (OM Table 3-2, HTML):
- `LOG TRACE` "works like INHOST except that escape sequences are first converted into their symbolic
  meaning".
- `LOG OUTHOST` prints control characters as a mnemonic (`ESC`) or caret-letter (`^C`).
- `HOST SPEED` below 300 baud "will not supply text quickly enough for natural speech".
- If DECtalk is attached to a computer, the local speed should be at least the host speed.
- `LOCAL SPEAK` echoes typed characters to the speech pipeline.
- `RECALL FACTORY` uses the read-only factory memory, and `SAVE` overwrites USER memory ("use it carefully").

---

## 7. Text-to-speech pipeline: manual model ↔ ROM

Manual's nine modules (OM ch. 1) and where they live in the ROM:

| # | Module | ROM implementation | Status |
|---|---|---|---|
| 1 | Sentence parser (words, clause boundaries, phonemic text, `[:…]` commands) | clause scanner `clause_readin` `0x3182`, word flush `readin_flush` `0x3580`, `newclause` `0x3912`, `clause_putsym` `0x35f8`, `parse_bracket_command` `0x7d1e` | named; the whole text pipeline, from the text pipe to the klsyn messages, **in C word for word** (§15.24-15.30) |
| 2 | Word parser (compounds, spell-out) | `pronounce_word` `0x5a90`, `pronounce_word_or_abbrev` `0x7192`, `spell_char` `0x621a`, `emit_punctuation_symbol` `0x60aa` | **done, and in C word for word** (§15.26) |
| 3 | Number formatter | `token_dispatch` `0x4062` (formerly `numeric_token_dispatch`; every token passes through it) + family, `out`/`outn` (§7.3) | **done: patterns decoded, and in C word for word** (§15.27) |
| 4 | Dictionary manager | `lookup_word*`, `dict_hash_*`, `pronounce_dictionary_word` | **done: trie decoded, and in C word for word** (§15.25) |
| 5 | Letter-to-sound | `lts_rule_engine` `0x6a64` (+`lts_env_match`, `lts_rule_apply`, `lts_alloc`, `lts_unlink`) | **done, and in C word for word** (§15.10, §15.24) |
| 6 | Phrase-structure / intonation | not located | **next** (`dapi/src/PH/ph_inton.c`, `ph_claus.c`, `ph_main.c`) |
| 7 | Phoneme-to-voice (durations, allophones, F0) | `klclause` `0x7a04` → **`phalloph` `0x843e`**, **`phtiming` `0x8ebe`** (durations + hat-pattern F0 commands), frame loop `phclause_draw_frames` `0xa782`: **`phsettar` `0xaa08`**, **`phdraw` `0xa804`**, **`pht0draw` `0xc722`** (all [V], §15.17-15.20) | **done, and in C word for word** from `phalloph` on (§15.18-15.20) |
| 8 | Synth command output | `dsp_post_frame` `0x7b56` → `dsp_queue` → `dsp_command_queue_isr` `0x12258` (`dsp_send_speech_frame` `0x122a4` for `0x6000`) | **done**: word map §16.9, builder §15.17 |
| 9 | Digital signal processor | TMS32010, `dsp/dsp_v1.8.lst` | **disassembled** (§16) |

### 7.1 Word-processing order (Owner's Manual App. A "Word spellout strategies") — matches `pronounce_word`
1. Break input at whitespace/control chars. 2. If not already phonemic, expand numbers.
3. Numeric abbreviations (Table A-1; cannot be overridden by the user dictionary).
4. Special-case `Dr.`/`St.` (below). 5. **Dictionary**: user dictionary first, then built-in;
lookup strips prefixes/suffixes (`pre-`, `-ed`, `-ing`…) ⇒ `lookup_word_with_suffix_stripping`.
6. Not found ⇒ strip leading `" ' ( {` and trailing `. , ; : ! ? ) } "`; square brackets removed only
if MODE SQUARE is off; a leading `)` with MODE SQUARE off ⇒ spelled out. 7. If punctuation was
removed, retry as abbreviation (`foo.` → `foo`). 8. Acronyms `A.P.O.` (upper-case letters
separated by `.`) → "aye pea oh"; `a.p.o.` is spelled with "period"s. 9. Lower-case and retry;
hyphen/`'` handling; require **both a vowel and a consonant** (`y` counts as both) and no embedded
punctuation, else spell out (`sys$system`). 10. Pronounceable ⇒ per-compound-part dictionary,
else **letter-to-sound rules**. 11. Trailing `" ' ) ] }` give one brief silence; `. , ; : ! ?`
end the clause; `Hello...` ≡ `Hello.`. Spelled-out words include their punctuation (a trailing
`.`/`,` is treated as punctuation, not spelled). Upper/lower case is irrelevant to speech;
multinational letters lose their accents.

**ROM check (§15.26) [V].**
- The order holds.
- A `)` word is spelled with SQUARE off only when the whole-word lookup fails first (`)read` is still the verb form).
- Acronyms need a period after each capital (`U.S.A.`, `A.P.O`); `NASA` is lower-cased and goes to the rules.
- One clause mark per word: a second one is spelled ("Why?!" → "why exclamation point").
- **A `?` after a wh- word is spoken as a period** (question words keep statement intonation).
- "Losing accents" is v1.8 stripping bit 7: `é` becomes `i`, not `e`.

**`Dr.` / `St.`** [M+V]: default **"drive"/"street"**; if the *next word on the same line is
capitalized* → **"doctor"/"saint"** (`Dr. Zhivago Dr.` → doctor zhivago drive). ROM:
`pronounce_word_or_abbrev` defers a standalone `st.`/`dr.` one word, tests the next word's first
two char classes (upper, lower); capitalized ⇒ re-feeds the literal `"Dr."/"St."` through the
pipeline (built-in dictionary maps them to Doctor/Saint — Table A-2), else the literal words
`drive`/`street`. This **resolves** the old "not confirmed" open item (§12).

**Alternate pronunciations**: with MODE SQUARE on, a leading `)` before a word selects its
second pronunciation from Table B-1 (App. B). Also `)` is the "verb phrase introducer".

### 7.2 Dictionaries
- **Built-in**: a plain trie in ROM `0x20000-0x3fe81`, 6,508 words (`lookup_word` `0x68fc`; layout and decoder in
  §15.25). It includes the abbreviations of Table A-2, the alternate pronunciations of Table B-1 (keys `)word`) and
  the names of the characters spelled out (keys space + character). **Cannot be edited.**
- **User dictionary** (`dict_hash_*`, 32-bucket hash at `0x820a0`), loaded only via `DT_DICT`
  (host on-line). Searched **before** the built-in one; searched in **entry order**;
  **upper-case letters in a name match only upper-case text, lower-case letters match either**;
  a name ending in `.` requires the period in the text and that period does not end the
  clause; empty substitution deletes the entry; substitution is phonemic text (commands like
  `:ra 120` allowed); max 256 chars; status via R2=50. Deleted by RIS / DECTST POWER.
- Sample entries (`xtras/guide/dictionary.user`, tab-separated `word<TAB>phonemes`):
  `barometric baeraxm'ehtrixk`, `downtown d'awn*t\`awn`, `MBTA 'ehm*b\`iy*t\`iy*'ey`,
  `A.M. \`ey*'ehm`, `Ipswich 'ihpswihch`, `yesterday's y'ehstixdxeyz` … (backtick = secondary
  stress, `*` = morpheme boundary, `'` = primary stress).

### 7.3 Numbers (Owner's Manual App. A) — matches the ROM number engine
Six classes: **cardinal** (`123`, `123,456`, `12.345`, `01234`, `+1.2E-4`, `12%`), **ordinal**
(`1st 23rd`; wrong suffix like `2th` is *not* recognized), **fractions** (`1/2 2/3 44/100%
2/3rds`; numerator 1-99, denominator 1-100), **money** (`$` prefix or preceding word; `$12,345`,
`$12.34`, `$1.23 million` → "one point two three million dollars"), **dates** (Digital
format `23-Sep-1983`, `23-Sep`, `23-Sep-83`; *not* `Sep. 23, 1983`), **time** (VMS
`11:04:03.01`; `12:00` → "twelve, oh oh" — *no* "noon", no AM/PM logic).

Rules: leading/trailing punctuation stripped first (`(123)` → 123). Commas must group by
three. Exponent < 100. `+`/`-` before a cardinal always say "plus"/"minus" (MODE MINUS
only governs ambiguous hyphens like `10-15`). First digit `0` ⇒ digit string (zip-code style).
> 999,999,999 ⇒ digit groups with pauses (`12345678901` → "123, 456, 78901"; commas
control pauses) ⇒ `speak_digits_individually`. Four digits without comma: `5000` → "five
thousand", `1984` → "nineteen eighty-four" (year style) ⇒ mode 7 of `cardinal_number_to_words`.
Quantity words after money (must *immediately* follow the money word; verified against the PDF page):
**thousand million billion trillion quadrillion quintillion sextillion septillion octillion nonillion
decillion undecillion duodecillion tredecillion quatturodecillion [sic — the manual's spelling; the ROM
string is the correct `quattuordecillion`] quindecillion sexdecillion septdecillion octodecillion
novemdecillion vigintillion zillion**. Known ambiguity examples: `(617) 493-8255` → "six hundred seventeen,
four nine three dash eight two five five"; ISO dates `83.09.20` and part numbers are *not*
recognized.

ROM mapping [V] (all in C, word for word, §15.27): **`token_dispatch`** `0x4062` (formerly `numeric_token_dispatch`;
the scanner hands it every token) classifies with `classify_number` `0x40e0` against the pattern tables
**`0x12e56`** (normal) and **`0x12f26`** (after a `$` or while money is pending), **decoded** in §15.27; then
`number_to_words` `0x4bd0` (modes 2/3 dollars and cents, 4/6 money, 7 year, 9 cardinal, 10/11 fraction denominator
"fourth"/"fourths", 12 ordinal), `cardinal_number_to_words` `0x50a2` (scale words `0x12fc2`, strings from `0x133cf`:
`zillion` (idx 0) `thousand million …`), `speak_digit_group` `0x50e8` (ones `0x1301e`, ordinals `0x1306e`, tens
`0x130a2`), `speak_digits_individually` `0x4ee4` (more than nine digits), `flush_currency_suffix` `0x4ace`,
`speak_number_sign` `0x4b98`, units `speak_unit_abbrev` `0x56c2` (table `0x130c2`). Month table `0x12f8e`.
Punctuation-word table `0x12f76` (6 entries for chars `0x2A-0x2F`): unreachable (§15.27). The time strings
`0x1392e/30/32` are **","**, not "colon": `12:00` = "twelve, oh oh," [V: emulator] (§12.46).

**ROM number vocabulary, dumped as NUL-separated strings from `0x13346-0x139f0`** [V] (in this
order): `minus dot over`; month pairs `janJanuary febFebruary … decDecember` (3-letter match key
immediately followed by the full name); scale words `zillion thousand million billion trillion
quadrillion quintillion sextillion septillion octillion nonillion decillion undecillion duodecillion
tredecillion quattuordecillion quindecillion sexdecillion septdecillion octodecillion novemdecillion
vigintillion`; `zero`…`nineteen`; ordinals `zeroth first … eleventh twelvth` (**sic — the ROM
string is spelled "twelvth"; how it is spoken depends on the phoneme conversion, so do not "fix"
the string blindly**); tens/ordinal stems `twen
thir for fif six seven eigh nine`; then the unit table as `abbrev, singular, plural[, plural-abbrev,
plural, plural]` groups: `m meter meters`, `cm`, `mm`, `km`, `in inch inches` + `ins`, `ft foot feet`,
`yd yard yards` + `yds`, `mi mile miles`, `ha hectare hectares`, **`sq.in`/`sq.ft`/`sq.yd`
(`square inch/foot/yard`, `square inches/feet/yards`) — present in the ROM but absent from the
manual's Table A-1**, `ml`, `l`, `tsp tea-spoon(s)`, `tbsp table-spoon(s)`, `qt`, `gm mg g kg`, `lb lbs
pound(s)`, `oz ozs ounce(s)`, `nsec nano-second(s)`, `usec micro-second(s)`, `msec milli-second(s)`,
`sec secs`, `min minute(s)`, `hr hour(s)`, `deg degree(s)`, `kt kts knot(s)` (hyphenated compound
pieces are spoken as two words), then the tail strings `one`, `0`, `0`, `times ten to the`, `minus`
(scientific-notation exponent, matching `12.34E56` in the manual's examples). **`dot`** (and `times plus comma minus`,
the `0x12f76` words) is never spoken in v1.8; **`over`** is spoken by the fraction code at `0x139cc` ("1/1" = "one over
one"), not by the `0x13350` copy (§15.27). Lineage: this is the same job as MITalk's FORMAT numeral logic (§14.2).

**Table A-1 — numeric abbreviations** (recognized after a cardinal, need trailing `.`, upper/lower
case both ok, singular/plural generated): length `cm. mm. km. m. in. ins.(inches) ft. yd.
yds. mi.`; area `ha.`; volume `l. ml. qt. tsp. tbsp.`; mass `g. gm. mg. kg. oz. ozs. lb. lbs.`;
time `hr. min. sec. secs. msec. usec. nsec.`; misc `deg. kt. kts.`. (ROM strings such as
`centimeter`/`centimeters` at `0x135e3` are this table.)

**Table A-2 — abbreviations in the built-in dictionary** (complete, transcribed from the PDF scan, OM
PDF file-order pp. 94-95; upper-case letters in an entry match only upper-case text; a trailing `.` must be
present in the input and then does not end the clause). Entry → spoken word:
`all-in-1`/`all-in-one` → "All in one" · `Apr.` April · `Assoc.` Associates · `Aug.` August · `Av.`/`Ave.` Avenue ·
`Blvd.` Boulevard · `ca.` approximately · `CH.`/`Ch.` Chapter · `cm.`/`cms.` centimeters · `Co.` Company ·
`cont.` continued · `cu.` cubic · `dec` "DEC (as in Digital)" · `Dec.` December · `deg.` degrees · `Dept.`
Department · `doz.` dozen · **`Dr.` "Doctor or Drive"\*** · `e.g.` "E G" (not "for example") · `esp.` especially ·
`est.` established · `etc.` et cetera · `ext.` extension · `Feb.` February · `fig.` figure · `fn.` footnote ·
`Fr.` Father · `Fri.` Friday · `ft.` feet (not foot) · `Ft.` Fort (not Foot) · `Gen.` General · `gm.` grams ·
`Gov.` Governor · `hrs.` hours · `i.e.` "I E" (not "that is") · `Inc.` Incorporated · `Jan.` January · `Jr.` Junior ·
`Jul.` July · `Jun.` June · `kg.`/`kgs.` kilograms · `km.` kilometers · `lb.`/`lbs.` pounds · `Ltd.` Limited ·
`Mar.` March · `mg.`/`mgs.` milligrams · `misc.` miscellaneous · `ml.` milliliters · `Mon.` Monday · `mr.` mister ·
`mrs.` missus · `ms.` miz · `msec.`/`msecs.` milliseconds · `mss.` manuscripts · `Mt.` Mount · `Nov.` November ·
`nt.wt.` net weight · `Oct.` October · `op.cit.` "op cit" · `oz.`/`ozs.` ounces · `p.p.d.`/`ppd.` post paid ·
`pat.pend.` patent pending · `Pl.` Place · `pp.` pages · `Pres.` President · `Rd.` Road · **`recd.`** received
(*not* `rec'd`) · `Rep.` Representative · `Rev.` Reverend · `rsts` "ris-tis" · `rsts/e` "ris-tis-ee" · `Sat.` Saturday ·
`Sen.` Senator · `Sep.`/`Sept.` September · `sq.` square · `Sr.` Senior · **`St.` "Saint or Street"\*** ·
`Sun.` Sunday · `Thu.`/`Thurs.` Thursday · `Tue.`/`Tues.` Tuesday · `Univ.` University · `Vol.` Volume ·
`vs.` versus · `Wed.` Wednesday · `yds.` yards. (\* = the `Dr.`/`St.` rule of §7.1: only same-line, capitalized next
word ⇒ Doctor/Saint.) Some entries are also in Table A-1: if the abbreviation is recognized during number
processing (it follows a cardinal), the Table A-1 English form is spoken; otherwise the built-in dictionary
form is used, and a user-dictionary entry overrides the built-in one. The number-context unit strings at
`0x135e3+` (§7.3) are Table A-1. **Every entry of Table A-2 is a key of the built-in dictionary** (decoded, §15.25),
with the capitals as printed.

**Table B-1 — words with an alternate ("verb") pronunciation** (default is the noun form;
`)word` with MODE SQUARE on gives the listed one; useful to validate a decoded dictionary trie).
**Verified against the PDF scan** (OM PDF file-order pp. 99-100); the table also has a *Morphology* column:
every entry is "Noun/verb" (second form = verb) **except** `a` Det/letter, `august` Noun/Adj, `lead`
Noun,verb1/verb2, `read` Verb1/verb2, `tear` Noun1/noun2,verb, and `0` Zero/oh.
a `'ey`; abstract `aebstr'aekt`; advocate `'aedvaxkeyt`; affix `axf'ihks`; august `aog'ahst`;
compress `kaxmpr'ehs`; conduct `kaxnd'ahkt`; conflict `kaxnfl'ihkt`; console `kaxns'owl`;
construct `kaxnstr'ahkt`; contract `kaxntr'aekt`; convert `kaxnv'rrt`; convict `kaxnv'ihkt`;
coordinate `kow'aordeneyt`; decrease `diykr'iys`; deliberate `daxl'ihbrreyt`; digest
`dayjh'ehst`; duplicate `d'uwplixkeyt`; elaborate `axl'aebrreyt`; estimate `'ehstixmeyt`;
excuse `ixksk'yuz`; export `ehksp'owrt`; extract `ehkstr'aekt`; implant `ixmpl'aent`; import
`ixmp'owrt`; incense `ixns'ehns`; incline `ixnkl'ayn`; increase `ihnkr'iys`; insert `ixns'rrt`;
insult `ixns'ahlt`; interchange `ixntrrch'eynjh`; intimate `'ihntaxmeyt`; lead `l'ehd`; lives
`l'ihvz`; misuse `mixsy'uwz`; moderate `m'aadaxreyt`; object `axbjh'ehkt`; overrun
`owvrrr'ahn`; permit `prrm'iht`; pervert `prrv'rrt`; predicate `pr'ehdixkeyt`; proceeds
`praxs'iydz`; produce `praxd'uws`; project `praxjh'ehkt`; read `r'ehd`; recall `rixk'aol`;
record `rixk'owrd`; recount `riyk'awnt`; refill `riyf'ihl`; refund `riyf'ahnd`; refuse
`rixf'yuz`; reject `rixjh'ehkt`; relapse `rixl'aeps`; rerun `riy*r'ahn` (note the `*` morpheme boundary); retake `riyt'eyk`;
rewrite `riyr'ayt`; segment `sehgm'ehnt`; separate `s'ehpaxreyt`; subject `saxbjh'ehkt`;
subordinate `saxb'owrdeneyt`; syndicate `s'ihndixkeyt`; tear `t'eyr`; torment `towrm'ehnt`;
transform `traensf'owrm`; transplant `traenspl'aent`; transport `traens*p'owrt` (also has a `*`);
transpose `traensp'owz`; upset `axps'eht`; use `y'uws`; 0 `'ow` (zero/oh). The list above now matches the
printed table character-for-character (two earlier transcription errors — `rerun`, `transport` — fixed).
**ROM check (§15.25):** 67 entries equal the ROM's `)word` key exactly. The ROM's `)use` is `` y`uwz `` (secondary
stress, `z`), not `y'uws`. `a` and `0` have no `)` key (handled outside the dictionary [I]). The ROM also has
`)present` = `priyz'ehnt`, which is not in our transcription.

---

## 8. Voices, bracket commands and the phonemic alphabet

### 8.1 `[:…]` voice commands (`parse_bracket_command` `0x7d1e`, table `0x15a34`)
Enabled by MODE SQUARE. Syntax: `[:cmd params … :cmd …]`; every command starts with `:`; separate
from text by a word boundary; several commands/phonemes may share one bracket; **all
parameters of one `:dv` must precede the next command**; conflicting commands ⇒ last wins;
values are clamped to min/max; invalid commands ignored; a command persists until overridden;
all `:dv` changes are lost at power-down. A voice change needs a brief silence (an implicit
comma).

| Command | Range / meaning |
|---|---|
| `:ra n` | speaking rate **120–350 wpm** (default ≈180) |
| `:cp n` / `:pp n` | *added* comma / period pause **0–9000 ms** (defaults 160 ms / 640 ms) |
| `:np :nb :nh :nf :nk :nr :nu :nv` | voices: Perfect Paul, Beautiful Betty, Huge Harry, Frail Frank, Kit the Kid, Rough Rita, Uppity Ursula, Variable Val (user, defaults to Paul until `[save]`); `:nw` Whispery Wendy is 2.0-only |
| `:dv p v …` | design voice; params below |

ROM layout of the table (**decoded**, 31 records × 14 bytes, NULL-terminated): record =
`{name_ptr:4, kind:1 (+1 pad byte, always 0), min:2, max:2, desc_ptr:4}`. Index N (0-27) maps to
`(short*)0x81c44 + N`. `kind & 7` selects the unit string in the 4-entry pointer table at
**`0x15a24`** = `"" , "%", "Hz", "dB"`; `kind & 8` is tested by `print_voice_param_table`
(see §12 for what it does). Records 28-30 are the argument-less commands `list`, `listall`, `save`.

| # | id | meaning | ROM min–max | unit |
|---|---|---|---|---|
| 0 | `sex` | speaker sex (`0`/`f` female, `1`/`m` male; also bare `[m]`/`[f]`) | 0–1 | – |
| 1 | `sm` | smoothness | 0–100 | % |
| 2 | `as` | assertiveness | 0–100 | % |
| 3 | `ap` | average pitch | 30–300 | Hz |
| 4 | `pr` | pitch range | 0–250 | % |
| 5 | `br` | breathiness | 0–70 | dB |
| 6 | `ri` | richness | 0–100 | % |
| 7 | `nf` | samples in open period | 0–100 | – |
| 8 | `la` | laryngealization | 0–100 | % |
| 9 | `hs` | head size | 40–200 | % |
| 10-13 | `f4 b4 f5 b5` | cascade formants 4/5 freq & bandwidth | 2000–4650, 100–2048, 2500–4950, 100–2048 | Hz |
| 14-15 | `p4 p5` | parallel formants 4/5 freq | 2500–4950 | Hz |
| 16-19 | `gf gh gv gn` | gain of frication / aspiration / voicing / nasal | 0–80 | dB |
| 20-24 | `g1…g5` | gain of resonators 1-5 (raise/lower to cure "squawks" after head-size/sex changes) | 0–80 | dB |
| 25 | `ft` | F0-dependent spectral tilt | 0–100 | % |
| 26-27 | `bf ef` | beginning / end pitch baseline fall | 50–200 | Hz |

Notes: the manual's Table 5-3 lists *different* ranges (ap 50–300+, br 0–60, hs 75–150, nf 0–60,
sm 0–24 dB, f4/f5 interdependent) and an extra `fo` "forte voice" — that describes a later
firmware; the manual's own `[list]` example and the ROM agree (ranges above).
**Changing `sex`** (only when the value actually changes) adjusts **head size and the F4/F5
cascade formants** [V, from the decompile + index→address arithmetic: block word N lives at
`0x81c44 + 2N`, so `0x81c56` = #9 `hs`, `0x81c58` = #10 `f4`, `0x81c5c` = #12 `f5`]:
`hs += short[0x15bf6 + 2·newsex]` (**−18** for female, **+18** for male) and
`f4`, `f5` `*= Q12(short[0x15bfa + 2·newsex])` (**0x137E ≈ 1.218** for female, **0x0D22 ≈ 0.821**
for male), skipping a formant whose value is the sentinel `0x9C4` (2500). Four more shorts follow
(`0x15bfe…`: `0x0BEA 0x0ABE 0x0D16 0x0BEA` = 3050, 2750, 3350, 3050: **`f2max_by_sex` / `f3max_by_sex`**, the
per-sex F2/F3 ceilings `phdraw` clamps to, lowered when F4 is low; §15.17). The unit strings sit right after: `""`, `"%"`, `"Hz"`, `"dB"` at `0x15c06-0x15c0e`,
then `"sex"` at `0x15c0f`. (The old notes and the `parse_bracket_command` plate comment said "pitch and pitch-range" — that was wrong.)
`[list]` prints to the local console, never to the host (console queue `0x80328`). The manual says it is honored
only from the local terminal in off-line mode, but in v1.8 `[:dv list]` in *host* text also prints the table on the
console (§15.22). `[save]` copies the
28 values to the Val slot; `[:nv]` recalls it.

**Voice table `0x16146`** = 9 big-endian pointers, in this order: `0x15fbe` (`:np` Paul), `0x1602e`
(`:nb` Betty), `0x15ff6` (`:nh` Harry), `0x16066` (`:nf` Frank), `0x16066` (**`:nd` Dennis** — a
second pointer to Frank's record; the ROM's `:`-command name table has `:nd` but no `:nw`), `0x1609e`
(`:nk` Kit), `0x160d6` (`:nu` Ursula), `0x1610e` (`:nr` Rita), `0x81c7c` (`:nv` Val, RAM). Each ROM
record is **28 big-endian shorts = 56 bytes** (stride `0x38`), in exactly the block order of the table
above, so it is presumably block-copied into `0x81c44…` by `load_voice_definition` (`0x8150`) when a
voice is selected [I — the copy itself is not re-checked; the layout is].

**ROM-verified voice records** [V — dumped with Python from `merges/dectalk_v1.8_full.bin`, 2026-09]:

| id | Paul | Harry | Betty | Frank | Kit | Ursula | Rita |
|---|---|---|---|---|---|---|---|
| sex | 1 | 1 | 0 | 1 | 0 | 0 | 0 |
| sm | 34 | 34 | 44 | 36 | 44 | 64 | 34 |
| as | 100 | 100 | 50 | 50 | 40 | 100 | 50 |
| ap | 120 | 78 | 222 | 153 | 306 | 264 | 106 |
| pr | 100 | 50 | 160 | 90 | 180 | 135 | 80 |
| br | 0 | 0 | 56 | 50 | 40 | 0 | 49 |
| ri | 20 | 14 | 100 | 20 | 100 | 0 | 100 |
| nf | 0 | 10 | 0 | 0 | 0 | 10 | 0 |
| la | 0 | 0 | 0 | 100 | 0 | 100 | 6 |
| hs | 100 | 120 | 100 | 90 | 85 | 95 | 95 |
| f4 | 3300 | 3300 | 4000 | 3300 | 3600 | 4200 | 4000 |
| b4 | 160 | 200 | 250 | 200 | 400 | 100 | 250 |
| f5 | 3900 | 3850 | 2500 | 3900 | 2500 | 2500 | 2500 |
| b5 | 130 | 240 | 2048 | 240 | 2048 | 2048 | 2048 |
| p4 | 3300 | 3200 | 4100 | 3500 | 4300 | 4100 | 4100 |
| p5 | 4050 | 4000 | 2500 | 4050 | 2500 | 2500 | 2500 |
| gf | 73 | 71 | 66 | 70 | 66 | 66 | 66 |
| gh | 70 | 70 | 66 | 70 | 66 | 70 | 70 |
| gv | 71 | 71 | 71 | 72 | 71 | 72 | 71 |
| gn | 69 | 69 | 71 | 69 | 74 | 72 | 75 |
| g1 | 71 | 71 | 70 | 68 | 70 | 71 | 69 |
| g2 | 61 | 61 | 67 | 62 | 68 | 62 | 65 |
| g3 | 50 | 52 | 46 | 50 | 40 | 49 | 48 |
| g4 | 59 | 59 | 49 | 60 | 48 | 52 | 47 |
| g5 | 72 | 71 | 72 | 72 | 70 | 70 | 67 |
| ft | 35 | 20 | 50 | 50 | 50 | 50 | 0 |
| bf | 115 | 107 | 107 | 107 | 107 | 50 | 107 |
| ef | 100 | 107 | 107 | 107 | 107 | 107 | 107 |

**Paul matches the Owner's Manual `[list]` example exactly** (sm 34, as 100, ap 120, pr 100, br 0, ri
20, nf 0, la 0, hs 100, f4 3300, b4 160, f5 3900, b5 130, p4 3300, p5 4050, gf 73, gh 70, gv 71, gn 69,
g1 71, g2 61, g3 50, g4 59, g5 72, ft 35, bf 115, ef 100). The HTML OCR of the manual gets every one of
these right; the old DjVu OCR (since deleted) had g3/g4/ft/bf/p5 wrong (60/69/38/118/4000) — a concrete
example of why OCR numbers need ROM checks. So the block layout, the value order and the printed ranges of `[list]`
(ap 30–300, hs 40–200, br 0–70 …) are all **confirmed for v1.8**. Sexes match the manual (Paul/Harry/
Frank male, Betty/Ursula/Rita female; Kit is "either" — the ROM stores 0). Val (`0x81c7c`) starts as a
copy of Paul.

`[list]` vs `[listall]` — **resolved (2026-09-26, §15.22, run in the emulator):** v1.8's `[:dv list]` prints 15
rows and hides `nf p4 p5 gf gh gv gn ft` (`kind & 8`); `[:dv listall]` prints all 28. The manual's `[list]` example
with 28 rows is `listall` output (or another firmware).

Manual voice characteristics: whisper example `[:np :dv br 60 gv 0 g5 80 sm 0 ri 0]`; monotone
`pr 0`; robot voice etc. `:n_` accepts `p b h f k r u w v` in the manual's Table 5-2 (`w` = Wendy, 2.0);
the ROM's token-name table also lists `:nd` (Dennis).

### 8.2 Phonemic text (`[ ]` with MODE SQUARE, or `DT_PHOTEXT`)
- 2-character symbols (default) — vowels: `aa ae ah ao aw ax ay eh ey ih ix iy ow oy uh uw yu`,
  syllabic `rr el em en`; consonants: `b ch d dh f g hx jh k l m n nx p r s sh t th v w y z zh`;
  `_` = silence. (17 vowels + 24 consonants.) Case-insensitive. `MODE ASKY` switches to a 1-char
  alphabet (Tables 4-1/4-2 of the OM; the two systems cannot be mixed).
- Stress/syntax: `'` primary, `` ` `` secondary, `"` emphatic; `-` syllable boundary, `*` morpheme
  boundary, `#` compound-noun boundary, space word boundary, `)` verb-phrase start (and
  alternate-pronunciation prefix), `,` `.` `?` `!` clause ends (force output).
- Duration/pitch: `phoneme<dur_ms,pitch_hz>` (either may be empty/0 = default; pitch is the
  *target reached at the end* of the phoneme). Pitch values **1-37** are musical notes
  (C2…C5, equal tempered: `f = 64 · 2^((n-1)/12)` Hz, 64.0…512.0 — **confirmed against OM Table 4-4**, e.g.
  1=C2 64.0, 13=C3 128.0, 25=C4 256.0, 37=C5 512.0) and add vibrato ("singing"). Timing: with a tone
  number DECtalk reaches the pitch within ~100 ms of the phoneme's start and adds vibrato; with an explicit
  Hz value it reaches the pitch at the very end of the phoneme with no vibrato. Table 4-4 also has a bar chart
  of bass/baritone/tenor/alto/soprano ranges against the tone numbers (not transcribed; OM PDF file-order
  p. 65). Stops `p t k b d g` cannot be sustained; all other phonemes can (e.g.
  `[_<,120>ah<10000,120>]` holds "ah" for 10 s at 120 Hz).
- **`[+]` (silent phoneme) is new in firmware 2.0**: on v1.8 it raises DSR error **25** — a handy
  emulator test for the phoneme-error flag (and the manual's official firmware-revision probe, §5.5).
- **Stress goes *before* the vowel** in DECtalk (`d'aag`), unlike printed dictionaries which put
  the mark after the vowel nucleus (OM App. C).

### 8.3 ROM phoneme codes and the name table at `0x19704` [V]

The internal phoneme code is a small integer, and the ROM has a NUL-separated string table that gives
the printable name of every code (used for `LOG PHONEME` output and the `Illegal phoneme "%s"` error
whose text follows it at `0x19846`). It starts at `0x19704`; **index = code**:

| code | name | code | name | code | name | code | name |
|---|---|---|---|---|---|---|---|
| 0 | `_` (silence) | 14 | `uw` | 28 | `hx` | 42 | `z` |
| 1 | `iy` | 15 | `rr` | 29 | `rx` | 43 | `sh` |
| 2 | `ih` | 16 | `yu` | 30 | `lx` | 44 | `zh` |
| 3 | `ey` | 17 | `ax` | 31 | `m` | 45 | `p` |
| 4 | `eh` | 18 | `ix` | 32 | `n` | 46 | `b` |
| 5 | `ae` | 19 | `ir` | 33 | `nx` | 47 | `t` |
| 6 | `aa` | 20 | `er` | 34 | `el` | 48 | `d` |
| 7 | `ay` | 21 | `ar` | 35 | **`em`** | 49 | `k` |
| 8 | `aw` | 22 | `or` | 36 | `en` | 50 | `g` |
| 9 | `ah` | 23 | `ur` | 37 | `f` | 51 | `dx` |
| 10 | `ao` | 24 | `w` | 38 | `v` | 52 | `tx` |
| 11 | `ow` | 25 | `y` | 39 | `th` | 53 | `q` (glottal stop) |
| 12 | `oy` | 26 | `r` | 40 | `dh` | 54 | `ch` |
| 13 | `uh` | 27 | `l` | 41 | `s` | 55 | `jh` |

So there are **56 phoneme codes (0-55)**; `dapi/src/INCLUDE/l_us_ph.h` uses the *same* numbering
except that dapi's code 35 is `DZ` (dentalized d) where the v1.8 ROM has `em` (syllabic m), and dapi
adds code 56 `DF`. **Codes are therefore interchangeable with dapi for 0-34 and 36-55.** `ir er ar or ur`
are the r-colored vowels (by analogy with MITalk's IXR/EXR/AXR/OXR/UXR — "beer/bear/far/for/poor"
style; not checked in the ROM); `rx`, `lx` and `tx` are allophone codes that plain text never names
(exact contexts not verified); `q` is the glottal stop (dapi comment).

The same string table continues with the **control/token names** (their codes follow 55 in table
order — the numbering is *not* dapi's `100+` scheme): `#` `'` `` ` `` `"` `-` `*` (space) `(` `)` `[` `,`
`!` `?` `.` `A` `E` `I` `O` `U` `H` `X` `C` `J` `Q` `@` `~~` `::` `:++` `:--` (space) (space) `:==`
`:$$` `0 1 2 3 4 5 6 7 8 9` `:##` `:vo` `:ra` `:in` `:re` (space)`:` `:pp` `:cp` `:np` `:nb` `:nh` `:nf`
`:nd` `:nk` `:nu` `:nr` `:nv` `:se` `:dr` `:db` — i.e. the `:`-prefixed entries are the internal token
codes for the voice/rate commands (`:ra` rate, `:pp`/`:cp` pause, `:n?` voices incl. Dennis,
`:dr`/`:db` = probably `:dv` internals, `:se` = `sex`). **Resolved (§15.10):** the single letters
`A E I O U H X C J Q` (codes **70-79**, `0x46-0x4F`) are the **letter-to-sound grapheme codes** for the ten letters
that have no default phoneme; the other 16 letters enter the LTS list directly *as their phoneme code*
(`b`→`b`, `s`→`s`, `y`→`y` … via the ASCII table `0x13b48`). Inside the LTS rules, `:--` (84) is the **suffix
boundary** (Hunnicutt's `+`), `:++` (83) the **prefix boundary** (Hunnicutt's `=`), space (62) the word
boundary, and `?` (68) a temporary palatalization marker; `@`/`~~` share the boundary class (§15.10). A 56-byte
array of small per-phoneme feature values (`00/02/03/05/07/0f`, ending at `0x19703`) sits immediately in front
of the name table [I].

**The ROM's ASKY input table [V] (§15.29).** `lookup_phoneme` reads `asky_codes` `0x191e4` (a code per character
0x20-0x7e). It agrees with the table below except for `jh`, which is **`J`** in the ROM (as in dapi); `j` has no
code. The ROM also has `Y` yu, `Z` zh, `&` dx, `Q` tx, `q` q, `_` silence, the marks (`# ' \` " - * ( ) [ , ! ? .`,
space) and the voices `P B H F K V` (`:np :nb :nh :nf :nk :nv`).

**1-character (`MODE ASKY`) alphabet** — **verified against the PDF scan** of OM Tables 4-1, 4-2 and C-1
(the HTML OCR had several glyphs wrong: `0`↔`o`, `A` for `^`, `X` for `x`, `V` for `v`) and cross-checked with
`dapi/src/INCLUDE/usa_phon.tab` (`usa_ascky`, revised 1997-98; where the two differ prefer the manual for v1.8):

| 2-char | 1-char | 2-char | 1-char | 2-char | 1-char | 2-char | 1-char |
|---|---|---|---|---|---|---|---|
| `aa` | `a` | `ay` | `A` | `ix` | `\|` | `p` | `p` |
| `ae` | `@` | `b` | `b` | `iy` | `i` | `r` | `r` |
| `ah` | `^` | `ch` | `C` | `jh` | `j` (dapi: `J`) | `rr` | `R` |
| `ao` | `c` | `d` | `d` | `k` | `k` | `s` | `s` |
| `aw` | `W` | `dh` | `D` | `l` | `l` | `sh` | `S` |
| `ax` | `x` | `eh` | `E` | `el` | `L` | `t` | `t` |
| `em` | `M` | `en` | `N` | `m`,`n` | `m`,`n` | `th` | `T` |
| `ey` | `e` | `f`,`g` | `f`,`g` | `nx` | `G` | `uh`,`uw` | `U`,`u` |
| `ih` | `I` | `hx` | `h` | `ow`,`oy` | `o`,`O` | `v`,`w`,`y`,`z` | `v`,`w`,`y`,`z` |
| `yu` | `Y` | `zh` | `Z` | `_` | `_` | | |

(Table 4-1 prints `ao` = `c`; the manual's own Table C-1 prints `o` for `ao` — a manual inconsistency, since `ow`
is `o`; `c` is right and matches dapi. `v` is lower-case `v`, `ix` is the vertical bar `|`, `ax` is lower-case `x`.
Table 4-1 examples: `aa` bob, `ae` bat, `ah` but, `ao` bought, `eh` bet, `ey` bait, `ih` bit, `iy` beet,
`ow` boat, `uh` book, `uw` boot, `ax` about, `ix` Dennis, `ay` bite, `oy` boy, `aw` bout, `yu` beauty, `rr` bird,
`em` ransom, `el` bottle, `en` Martin.) The ASKY table itself is **not** present as a contiguous string in the ROM
(searched for `iIeE@aAW`), so the ROM probably encodes it in code — look for the `MODE_ASKY` (bit 1
of `0x822ce`) test in the phonemic-text parser.

---

## 9. Data-table and string catalog (ROM addresses)

| Address | What | Tag |
|---|---|---|
| `0x12bac` | boot task table | V |
| `0x12d11` | **`readin_class`**: `clause_readin`'s class of each character, index -1 (the end of the text) to 0x7f: bits 0-1 the overstrike rank, bits 2-3 the action (4 control, 8 store, 0xc spoken as "control X"), 0x10 `, ! ? .`, 0x20 line end (LF FF CR), 0x40 end of text (NUL, VT, DEL, -1), 0x80 blank; a byte 0x80-0xff would read the strings after it (§15.30) | V |
| `0x12d96`, `0x12d9d`, `0x12da5` | `"escape"`, `"control"` (said for control characters) and `log_debug`'s `"long word at clause readin: \"%s\"\n"` (§15.30) | V |
| `0x12e56`, `0x12f26` | **`pat_number`/`pat_money`**: `{pattern, class}` pairs for `classify_number`; the patterns (`0x13162-0x13334`) are a small regular-expression language, all 34 decoded in §15.27 | V |
| `0x12f76` | 6 punctuation-word pointers (`* + , - . /`); unreachable in v1.8 (§15.27) | V |
| `0x12f8e` | month-name table (3-letter match + full name) | V |
| `0x12fc2` | scale-name pointer table (idx0 `zillion`, `thousand`, `million`, …) strings from `0x133cf` | V |
| `0x1301e` / `0x1306e` / `0x130a2` | number words: ones/teens, ordinals, tens | V |
| `0x13346-0x139e3` | number vocabulary strings (`minus`, `dollars and`, `no cents`, `percent`, `point`, `hundred`, …); units strings ~`0x135e3` | V |
| `0x13a9c` | **`char_class`**, 128-entry char-class flags: `0x01` opening / `0x02` closing punctuation, `0x04` vowel letter, `0x08` small, `0x10` capital, `0x20` clause mark (`! , . : ; ?`), `0x40` digit, `0x80` consonant letter (`y` is neither) (§15.26) | V |
| `0x13b24-0x13b34`, `0x13b58` | suffix-allomorph phoneme strings; `"Illegal suffix"` | V |
| `0x13b48 + ascii` | ASCII → LTS code, 128 bytes (ends at `0x13bc8`): 16 letters → their phoneme code, `A E I O U H X C J Q` → 70-79, space → 62, ``' " ` ( ) * [ !`` → their own token codes, `]` → 66 (the `,` code), `~` → 81, DEL → 0, digits and other punctuation `0xFF` (§15.10) | V |
| `0x13bc8` | sentinel node for the LTS phoneme list | V |
| `0x13bd2` / `0x13bda` | `Dr.`/`St.` alternate / default (`drive`,`street`) pointer tables; strings `0x13be6-0x13bf4`; `"st.\0dr.\0"` at `0x13bfb` | V |
| `0x13c04` | LTS **feature-class masks**: 31 longwords, index = class number used in rule tests (vowel `0x1c`, consonant `0x1d`, boundary sets `0x12`/`0x13`, … — table in §15.10) | V |
| `0x13c80` | LTS **per-code feature longwords**, index = code (phonemes 0-55, tokens, letters 70-79, boundaries 83/84); `0x13d78` = entry 62 (space) | V |
| `0x13e10-0x156e1` | **LTS rule records**, fully decoded (§15.10; Hunnicutt-style rules, §14.9) | V |
| `0x156e2-0x159cd` | LTS delta table: **374** big-endian shorts; `delta[i]` = byte length of record `i-1`; cumulative sum seeded at −1; the last entry (`0x000a`) is the length of rule 372 | V |
| `0x159ce` | `0x000018d2` = total byte length of the rule stream (`0x13e10 + 0x18d2 = 0x156e2`) — the former "6 unexplained bytes" | V |
| `0x159d2` | LTS rule count = **373** (`0x0175`) | V |
| `0x15a24` | unit strings `"" % Hz dB` (4 pointers) | V |
| `0x15a34` | bracket-command table (31 × 14 B) | V |
| `0x15bf6`, `0x15bfa` | per-sex adjust shorts: head-size delta {−18,+18}; F4/F5 Q12 scale {0x137E, 0x0D22} (see §8.1); `0x15bfe-0x15c05` `f2max_by_sex` {3050, 2750}, `f3max_by_sex` {3350, 3050} (female, male; §15.17); units strings `0x15c06-0x15c0e` | V |
| `0x15c0f-0x15ecc` | bracket-command names + descriptions; `"%-3s %4d %-2s (%4d .. %4d) %s\n"` at `0x15ecf` | V |
| `0x16146` | voice pointer table (9: p b h f **d(=f)** k u r v); 56-byte records `0x15fbe…0x1610e` (§8.1) | V |
| `0x15eee` | `bndval[9]`: `phalloph` boundary value of symbols 61-69 (`* space ( ) [ , ! ? .`: 0x40 0x80 0x100 0x100 0x180 0x180 0x200 0x380 0x200; §15.20) | V |
| `0x161fe` | `featb[]` (phoneme feature bits, duration engine) | V |
| `0x162d2` | `inhdr[]` — **56 big-endian shorts**, inherent duration per phoneme code **in 6.4 ms frames** (vowel values × 6.4 ≈ MITalk Table 9-1 INHDUR; §9.1) | V |
| `0x16342` | `mindur[]` — 56 big-endian shorts, minimum duration per phoneme code, 6.4 ms frames | V |
| `0x1616a` | `sung_vibrato_steps`: 4 shorts `-6 0 6 0`, added to F0 when singing, stepped every 6 frames (`pht0draw`) | V |
| `0x16172` | `notetab`: 37 shorts, sung notes C2-C5 in **tenths of Hz** (640 … 5120, semitone steps); = dapi `notetab` | V |
| `0x169c4` | `f0segtars`: 56 shorts, segmental F0 perturbation per phoneme code; **identical to dapi `p_us_rom.c` `f0segtars` for codes 0-55** (read by `pht0draw`) | V |
| `0x16aa4` / `0x16b14` | `begtyp` / `endtyp`: 56 shorts each, transition-type class per phoneme code; `begtyp` identical to dapi; `endtyp` identical except code 8 `aw` (ROM 5, dapi 3) (read by `phsettar`) | V |
| `0x196cc…0x19703` | 56 per-phoneme small feature bytes (values 0/2/3/5/7/0xf) | I |
| `0x191e4`, `0x19684` | **`asky_codes`** (DT_MODE ASKY: the symbol code of each character 0x20-0x7e, shorts, `0xff` = none; read at `+2c`) and **`ph_letter_class`** (a byte per character 0x20-0x7e: bits 0-1 = 2 stands alone, 1 must take a second letter, 3 may; bit 2 = can follow a class-1 letter, bit 3 = a class-3 one) (§15.29) | V |
| `0x191f2`, `0x19207`, `0x1984b` | `"bad <dur,f0> format"`, `">;]\n"` (where `parse_phoneme` resumes after it), `"Illegal phoneme \"%s\""` | V |
| `0x192e2` | **`sym_table`**: per symbol code 0-119, 8 bytes `{1-character name, 0, name pointer (into `0x19704`), code \| flags}`; flag `0x4000` = the symbol takes a value (`:vo :ra :in :re : :pp :cp :se :dr :db`); `sym_max` `0x196a2` = 120. Read by `phoneme_name` (§15.28) | V |
| `0x1920c-0x19221` | `phoneme_name`'s formats: `"%u"`, `" "`, `">"`, `"{?%d.?}"`, `"%s"`, `"<??>"` | V |
| `0x12dc8-0x12e46` | `clause_putsym`/`newclause`/`log_clause` strings: `"spdef overflow (%d entries)"`, `"BUG: phone 0x%x has value, no pvalue[]"`, `"bug: newclause message size %d > max %d"`, `"newclause"`, `":dv"`, `"%s"` | V |
| `0x19704` | **phoneme/token name table**: NUL-separated strings, index = code (§8.3); `Illegal phoneme "%s"` at `0x19846` | V |
| `0x1819e`, `0x1821e` | OSC-terminator lookup (index `0xA0-0xFE`); DEC-supplemental remap for charset `<` | V |
| `0x18656-0x18823` | `DT_*` command strings (`tone dial %s`, `pulse dial %s`, `answer in %d ring`, `hangup`, `phone on hook`, …) | V |
| `0x1899a` | DTMF digit map `"D1234567890*#ABC"` | V |
| `0x19862-0x1987e` | powers of ten for `uint_to_decimal_string` | V |
| `0x1b850+` | HELP text (SET MODE …, SHOW MODE …) | V |
| `0x2646-0x268e` | crt0 init/copy tables (`0x2680` table: entry `0x2628` unidentified) | V |

---

### 9.1 Duration arrays vs MITalk Table 9-1 [V]

`inhdr[]`/`mindur[]` (index = §8.3 phoneme code, unit = one **6.4 ms** parameter frame). Multiplying by
6.4 reproduces MITalk's (Klatt's) inherent durations for the vowels to within a few ms — which
confirms both the unit and that DECtalk inherited the MITalk duration table (rules 1-10 in
`phtiming`/`p_us_tim.c` are Klatt's too, §14.3):

| ph | ROM min/inh (frames) | ms @6.4 | MITalk min/inh (ms) | | ph | ROM min/inh | ms @6.4 | MITalk |
|---|---|---|---|---|---|---|---|---|
| iy | 10 / 25 | 64 / 160 | 55 / 155 | | ay | 20 / 39 | 128 / 250 | 150 / 250 |
| ih | 10 / 24 | 64 / 154 | 40 / 135 | | aw | 20 / 41 | 128 / 262 | 100 / 260 |
| ey | 14 / 30 | 90 / 192 | 100 / 190 | | oy | 20 / 44 | 128 / 282 | 150 / 280 |
| eh | 10 / 24 | 64 / 154 | 70 / 150 | | ow | 16 / 35 | 102 / 224 | 80 / 220 |
| ae | 8 / 36 | 51 / 230 | 80 / 230 | | uw | 8 / 33 | 51 / 211 | 70 / 210 |
| aa | 14 / 38 | 90 / 243 | 100 / 240 | | ao | 20 / 38 | 128 / 243 | 100 / 240 |
| ah | 10 / 22 | 64 / 141 | 60 / 140 | | uh | 8 / 25 | 51 / 160 | 60 / 160 |
| er | 22 / 42 | 141 / 269 | 130 / 270 (EXR) | | ir | 22 / 36 | 141 / 230 | 100 / 230 (IXR) |
| dx | 3 / 3 | 19 / 19 | 20 / 20 | | m | 10 / 11 | 64 / 70 | 60 / 70 |

Other values: silence `_` 31/48 frames (198/307 ms); **consonant inherent durations are re-tuned**
(e.g. `w` 5/6 = 38 ms inherent vs MITalk's 80 ms; `s` 8/20 = 128 ms vs 105; `ch` 20/25 vs 50/70), and the
`mindur` values differ from MITalk's for most vowels — DECtalk tuned them after MITalk. Use the
ROM arrays, not the book, for anything numeric; use the book to understand *why* (§14.3).

## 10. Named-function inventory (Ghidra, 305 of 362)

Boot/RTOS: `reset_entry 0x1f6`, `scheduler_context_switch 0x112a`, `trap1_dispatch_via_a1 0x1106`,
`task_create 0x116e`, `task_create_impl 0x117c`, `spawn_system_tasks 0x310a`, `boot_init 0x1024`, `main_task
0x2700`, the vector-table entries (§15.15) and the whole system-call API with its `_impl` handlers (§15.16: `sem_*`,
`task_*`, `event_*`, `tick_hook_*`, `clock_tick`, `dev_getc`/`dev_putc`/`dev_control`, `mutex_*`, `malloc`/`free`,
`knode_*`, `catch_*`, `task_signal`, `wait_on`/`wake_one`, `duart_init`, `phone_init`, `chardev_create`,
`mbox_*`, `msg_create`, `dsp_queue_init`).

Console/streams: `kprintf 0xd248`, `vformat_string 0xd260`, `console_print_decimal 0xd334`,
`console_print_string 0xd38c`, `console_putchar_caret 0xd3e4`, `stream_putc 0xd99e`,
`uint_to_decimal_string 0x1167c`, and the C library (§15.16): `_doprnt 0x12556`, `fprintf 0x11c88`, `printf 0x11cf4`,
`sprintf 0x11d16`, `strcat`/`strcmp`/`strcpy`/`strlen` `0x11d62-0x11e24`, `tolower`/`toupper`/`islower`/`isupper`,
`_flsbuf 0x11efe`, `fflush_if_pending 0x11ec4`, `fdopen 0x12b5a`, `fclose 0x12b3c`, `pipe_open 0x11fdc`, `pipe_close
0x1204c`, `profile_start`/`profile_probe`/`profile_report` `0x11aaa-0x11b9e`.

SETUP: `main_task 0x2700` (the "main" task; formerly `setup_mode_main_loop`), `read_line_with_prompt 0x2a8c`, `tokenize_words 0xfd8c`,
`strcpy 0x11e00`, `tdparse 0xfe16`, `showbit 0x107ee`, `parse_boolean 0x10744`,
`parse_baud_rate 0x10774`, `parse_parity 0x107be`, `parse_onoff_or_ctrlchar 0x10714`,
`print_channel_speed 0xfe84`, `print_channel_format 0xfee6`, `print_help_text 0xff48`, `setup_puts 0x300a`,
`keyword_match 0x10844`, `parse_hex 0x1088c` (the SETUP functions are C now, §15.37),
`nvram_save_settings 0xf79c`, `settings_reset 0xf3be` (formerly `spdef_settings_io`), `nvram_load_settings 0xf69a`,
`nvram_checksum 0xf7ca` (§15.33), `line_configure 0xf5c6`.

Host link: `host_task_main 0xd59e`, `read_host_char_collect_csi 0x11290`, `read_host_byte 0xefc8`,
`dispatch_esc_command 0xdad8`, `handle_single_shift 0xd83e`, `consume_control_string 0xd9f4`,
`output_graphic_char 0xd94c`, `is_graphic_char 0xd906`, `send_escape_response 0xef5c`,
`send_control_sequence 0x114e0`, `host_line_putc 0xef90` (formerly `echo_digit_to_host`; the host-line byte sink),
`log_trace 0xd21e`, `log_error 0xd1ca` (formerly `log_control_error`), `log_debug 0xd1f4` (renamed 2026-09-23 from
`log_recognized_escape` / `log_charset_shift`; §12.2), `dcs_command_dispatch 0xe152`,
`csi_command_dispatch 0xddd8`, `dsr_reply 0xdff2`, `parse_dict_entry_command 0xe8ec`, `dt_stop 0xfa52`,
`print_rom_date 0xee74`. **All of these are C now (§15.35).**

Phone: `phtask_main 0xf128` (formerly `phtask_main_loop`), `dtmf_diagnostic_menu 0x116e2`, `dectst_self_test 0xe984`,
`send_dcs_reply 0xef00` (formerly `send_dsp_event`: the `DCS 0;R2;R3 z ST` reply builder, §15.23). **The phone task
is C now (§15.36).**
Phone device (§15.34): `phone_tlc_isr 0x214e`, `phone_ring_poll 0x21ca`, `phone_start_answer 0x2084` (formerly
`phone_line_status_from_dsp`), `phone_set_idle 0x1ff4` and `phone_go_offhook 0x2034` (formerly
`phone_line_state_enter2/3`), `phone_keypad_enable 0x2106`, `phone_keypad_disable 0x2118`, `phone_hook_release 0x212a`,
`phone_hook_seize 0x213c`; device helpers `dev_rx_post 0xcf6`, `dev_rx_timeout_hook 0xdbc`. `duart_isr 0x16fe`, `duart_rx_char 0x1870` (formerly `duart_tx_drain_queue`),
`duart_tx_char 0x17f8` (§15.23),
`dsp_command_queue_isr 0x12258`, `dsp_send_speech_frame 0x122a4` (24-word DSP frame, §16).
DT_PHONE and tones (§15.32): `dt_phone_command 0xe5f6` (the DT_PHONE sub-commands), `phone_dial 0xeb54` (tone and
pulse dialing), `dcs_text_putc 0xeecc` (the DCS text buffer), `dsp_tone_timeout_hook 0x124a2`, `dsp_tone_done_hook
0x124fe`, `selftest_tlc_isr 0x7f8` (the power-up DTMF loopback test's interrupt handler).

Tasks/sync: `klsyn_task_main 0x3c20`, `dttask_main 0xf946`, `host_timeout_task_main 0xf070`,
`stop_task_main 0xfa7a` (it calls `task_suspend` on itself; formerly `wait_for_stop_signal`),
`emit_sync_marker 0x3d56` (DT_SYNC: blocks in `sem_wait(&sync_sem)`), `index_mark_reached 0xfaaa` (formerly
`FUN_0000faaa`; §15.23).

Clause/pronunciation: `newclause 0x3912`, `clause_putsym 0x35f8` (§15.28, with the DT_LOG phoneme log `log_clause
0x3a3e`, `phoneme_name 0x10d9c`, `phoneme_name_values 0x10f98` (klsyn's) and `frames_to_ms 0xd50c`, all newly named),
`spell_char 0x621a`,
`out 0x556e`, `outn 0x5614`, `pronounce_word 0x5a90`, `pronounce_word_or_abbrev 0x7192`,
`str_match_fold 0x4e80`, `emit_punctuation_symbol 0x60aa`, `pronounce_dictionary_word 0x62d8`,
`lookup_word 0x68fc`, `lookup_word_with_suffix_stripping 0x65c8`, `stem_has_vowel 0x65ee`,
`dict_hash_insert_or_delete 0xfc36`, `dict_hash_lookup 0xfb56`, `dict_hash_clear_all 0xfb10`,
`lts_rule_engine 0x6a64`, `lts_env_match 0x6c0c`, `lts_rule_apply 0x6ad4`, `lts_alloc 0x7108`, `lts_unlink 0x7150`
(§15.24), `clause_readin 0x3182`, `readin_flush 0x3580` and `dttask_getc 0xf9ea` (formerly `FUN_00003182`,
`FUN_00003580`, `FUN_0000f9ea`; §15.30), `parse_phonemic_text 0x3dcc` (formerly `FUN_00003dcc`) with `parse_phoneme 0x10a9e` and `lookup_phoneme
0x1100e` (both newly named, §15.29), `panic 0x1d618` (a jump to itself; §15.25),
`check_wh_word 0x5bd0` (formerly `FUN_00005bd0`), `spell_chars 0x6198` (formerly `FUN_00006198`; §15.26).

Tokens and numbers (§15.27): `token_dispatch 0x4062` (formerly `numeric_token_dispatch`), `split_token 0x4114`,
`flush_lead 0x4a40`, `flush_trail 0x4a6c`, `defer_money 0x4a98`, `is_scale_word 0x40aa`, `speak_unit_abbrev 0x56c2`,
`classify_number 0x40e0`, `pattern_find 0x7310`, `pattern_match 0x7352` (all newly named), `number_to_words 0x4bd0`,
`cardinal_number_to_words 0x50a2`, `speak_digit_group 0x50e8`, `speak_digits_individually 0x4ee4`,
`flush_currency_suffix 0x4ace`, `speak_number_sign 0x4b98`, `out 0x556e`, `outn 0x5614` (Ghidra's `FUN_00005554`,
`caseD_6 0x56e0` and `FUN_00005a84` are fragments of their tails, not functions), `long_divide 0x1d6ae`, `long_modulo 0x1d784`, `long_multiply 0x1d73a`,
`frac_mul_q14 0x1d80a`, `frac_mul_q12 0x1d862`.

klsyn/duration: `parse_phoneme_param_stream 0x7788`, `load_voice_definition 0x8150`, `ms_to_frames 0xd54c`,
`skip_blanks 0x7c10`, `skip_blanks_eq 0x7c3c`, `read_dv_value 0x7c7e`, `strchr 0x11d90` (§15.22),
(§15.21; the klsyn side from `parse_phoneme_param_stream` down is C, word for word),
`parse_bracket_command 0x7d1e`, `print_voice_param_table 0x8072`, `save_voice_params 0x81a8`,
`phtiming 0x8ebe` (= `phtiming()` of `dapi/src/PH/p_us_tim.c`), `prdurs_stub 0xa77a` (no-op debug
call marking rule boundaries), `kl3_push_event 0xa6e2` (= dapi `make_f0_command`, F0 event list, not a DSP
writer; §12.33), **`phsettar 0xaa08`** (= `phsettar()` of
`dapi/src/PH/ph_setar.c`; structure [V], §15.17), **`pht0draw 0xc722`**
(= `pht0draw()` of `dapi/src/PH/ph_drwt01.c`, per-frame F0; [V], §15.14).

Frame builder (§15.17): `speech_init 0x30c8`, `klclause 0x7a04` (= dapi `phclause`), `phalloph 0x843e` ([V], §15.20),
`phclause_draw_frames 0xa782`, `phdraw 0xa804`, `setloc 0xc420`, `diph_time_scale 0xc570`, `setspdef 0x81e6`,
`set_formant_limits 0x7d42`, `dsp_post_frame 0x7b56` (= dapi `send_pars`), `dsp_link_init 0x7aec`,
`spc_reset_wait 0x122fa`, `muldiv_globals 0x1d81c`.

Still-unnamed but important: `FUN_00007c7e` (numeric arg parse + clamp), `FUN_00007108/00007150` (LTS list node alloc/free), `FUN_0001d832`
(16×16 multiply used by the divide helpers).

---

## 11. dapi cross-reference map (later x86 code shares names/algorithms)

The `dapi/` folder was removed on 2026-09-27; this map stays as the record of which dapi file each part matches.

| Need | dapi file(s) |
|---|---|
| duration rules | `src/PH/p_us_tim.c`, `ph_timng.c`, `ph_time1.c` |
| intonation / F0 | `src/PH/ph_inton.c`, `ph_inton1.c`, `Ph_inton2.c`, `p_us_st1.c` |
| clause / phrase structure | `src/PH/ph_claus.c`, `ph_main.c` |
| allophones → parameters | `src/PH/ph_aloph.c`, `ph_draw.c`, `ph_drwt0.c`, `p_us_rom.c`, `p_us_vdf.c` (voice definitions) |
| LTS rules & suffixes | `src/LTS/l_us_ru1.c`, `ls_rule.c`, `ls_rule1.c`, `ls_rule2.c`, `l_us_suf.c`, `ls_suff.c`, `l_us_con.c`, `ls_dict.c`, `ls_main.c` |
| text/command parsing | `src/CMD/cm_text.c`, `cm_pars.c`, `cm_cmd.c`, `cm_phon.c`, `cm_us_co.h`, `cm_defs.h`, `INCLUDE/cmd.h` |
| control codes, DCS/R2 codes | `src/INCLUDE/esc.h`, `ansi_ch.h` |
| DSP/SPC interface | `src/INCLUDE/spc.h`, `src/KERNEL/spc_driv.c`, `hardw.h` |
| phoneme tables | `src/INCLUDE/phonlist.h`, `l_us_ph.h`, `ph_data.h` |
| RTOS ideas | `src/KERNEL/*` (different OS, but queue/sema/pipe concepts) |

Caveat: dapi's LTS control flow differs from the ROM's (ROM tries **all 373 rules, in order,
against every position**; dapi picks one rule per cursor position) even though the rule record
format matches.

---

## 12. Corrections — what earlier notes (ex-FINDINGS/BRIEF) and older plate comments got wrong

1. **`0x0B` is CTRL-K / VT = "clause flush"**, not a vague "attention marker". (`dectlk.h`
   `VT`, BASIC sample `VT$ = chr$(ascii('K')-64)` "DECtalk flush char", manual: CTRL-K
   terminates a clause and leaves phonemic mode.) Consequences:
   - `host_task_main` mapping host `SUB` (0x1A) → `stream_putc(0x0B)` = flush the clause.
   - `host_timeout_task_main` = the manual's **5-second speech timeout** ("if nothing is sent
     within 5 seconds and text is buffered, speak it as though a comma had been sent"): it
     counts idle periods (reset by `read_host_byte`), and after 5 emits VT. The manual notes
     firmware 2.0 fixed *logging* of this internally generated CTRL-K.
   - `0x1A` written by `emit_sync_marker` is the pipeline's **sync marker** (DT_SYNC).
2. **`DAT_000822ca` is the `DT_LOG` flag word** with `dectlk.h` bit values. So:
   `log_error` = **LOG_ERROR (0x20)** logger; `log_recognized_escape` = **LOG_TRACE
   (0x40)** logger; `log_charset_shift` = the **`0x80` "debug (reserved for Digital)"** logger —
   it is *not* charset-specific (also prints `"Host timeout"`, `"dtsync: %s"`, …).
   **Applied in Ghidra 2026-09-23:** `log_recognized_escape`→`log_trace`,
   `log_charset_shift`→`log_debug`, `DAT_000822ca`→`g_log_flags`, `DAT_000822ce`→`g_mode_flags`.
   Also `read_host_byte`'s flag `0x04`/`0x08` echoes are LOG_RAWHOST / LOG_INHOST.
3. **`print_voice_param_table` / `[list]` vs `[listall]`** (was open item): the 32-bit read at
   record+4 is `{kind, 0, min}`, so the tested bit `0x8000000` is **`kind & 8`**. `list`
   (`param_1=0`) skips rows with that bit (nf, ft, p4, p5, gf, gh, gv, gn — 8 rows);
   `listall` (`param_1=1`) prints all 28. Units index = `kind & 7`. Confirmed in the emulator (§15.22): the
   manual's 28-row `[list]` example is `listall` output. Record layout corrected to
   `{name:4, kind:1, pad:1, min:2, max:2, desc:4}`.
4. **`Dr.`/`St.`** open item is **resolved** by manual + ROM (§7.1). Default drive/street;
   capitalized next word ⇒ doctor/saint.
5. **Time format strings** (`0x1392e/30/32`) — the old notes called one of them "colon"; the manual
   says `12:00` → "twelve, oh oh". **Resolved (§12.46):** all three are `","`.
6. **`0x8011e`** is the host-line queue *read* by `read_host_byte`; the old "TX path only"
   description of `duart_isr` for that queue should be re-checked (it services both directions
   of the DUART).
7. **`event_wait(100,0,0)`** (formerly `queue_send`) in `host_timeout_task_main` is a ≈1-second wait if 5 periods = 5 s
   (manual) — confirm the tick unit rather than assuming.
8. **Voice ranges:** the ROM table matches the manual's `[list]` output, not its Table 5-3
   (later firmware) — don't "fix" the ROM decode to match Table 5-3.
9. `DT_PHONE_HOME` is only the ROM's internal name; the documented mnemonic is `DT_PHONE`
   (`P2_PHONE`, P2=60).
10. **`[:dv sex …]` side effects** (the old notes and the plate comment of `parse_bracket_command`
    say "rescales average pitch and pitch-range"): wrong. Block word `0x81c56` is **`hs`** (#9),
    `0x81c58`/`0x81c5c` are **`f4`/`f5`** (#10/#12). A sex change adds ∓18 to head size and
    multiplies F4/F5 by ≈1.218 (→female) / ≈0.821 (→male). The `"fm"` bytes at `0x15bf4` are
    just the two chars used by the `m`/`f` parse; the "unexplained blob" open item is these
    tables (four trailing shorts still unidentified). Update the plate comment when Ghidra is back.

**Second documentation pass (HTML OCR + MITalk book) — items 11-20** (third pass, items 21-27, follows):

11. **The earlier DjVu OCR (`docs/old/`, deleted 2026-09-24) had wrong digits and lost most tables; the
    HTML versions superseded it.**
    Proven on Paul's `[list]` example: the HTML numbers match the ROM record exactly, the DjVu ones
    were wrong for g3/g4/ft/bf/p5. When the two OCRs disagree, check the ROM (Python, §0.8) or
    the PNG in `docs/*_files/` before believing either.
12. **Extended DSR request is `ESC [ n`** (no parameter, bytes `1B 5B 6E`); the earlier "likely the
    `?`-private form" guess in §5.5 was wrong.
13. **`DT_MASK` (P2 83) is documented for firmware 1.8/2.0** with a full bit map (§5.2); the earlier
    note "not in dectlk.h — maybe not implemented" is withdrawn (it is in the RM's DECTLK.H listing;
    the ROM implementation is still to be located).
14. **Control characters (RM Table 1-1) recovered**: HT/LF/FF/CR/SP = space, VT = clause terminator,
    SUB = substitute-for-bad-character *and* clause terminator (§5.6). Previously CR/FF were only
    inferred.
15. **`LOG_DEBUG 0x80` is a documented flag** ("reserved for DECtalk internal use"); §12.2 stands.
16. **Voice pointer table order** is `p b h f d k u r v` (9 entries): index 4 is the **Dennis** (`:nd`)
    slot, pointing at Frank's record; `:nw` (Wendy) does not exist in v1.8.
17. **`sq.in/sq.ft/sq.yd`** are in the ROM's unit table although the manual's Table A-1 omits them;
    the ROM string `twelvth` is spelled that way (§7.3).
18. **Phoneme code order is now known** (56 codes, `em` = 35 — §8.3) and **`inhdr[]`/`mindur[]` are 56
    big-endian shorts in 6.4 ms frames** (§9.1). The old notes only said "phone→feature/duration
    lookup tables" without entry width or unit; §9.1 and §15.8 are authoritative.
20. **Everything in the old `FINDINGS.md` now lives in §15** with its wrong statements corrected in place
    (the `[corrected]` tags); `BRIEF.md` was a shortened copy and was dropped.

**Third pass — checking OCR-damaged pages against the PDF scans — items 21-27:**

21. **RM Tables 5-1/5-2 recovered** (§5.5). Corrections to earlier statements: the user dictionary is deleted by
    **PUP and RIS only — DECSTR keeps it** (the earlier "DECSTR keeps it?" hedge is resolved); pending text is
    flushed by PUP and RIS only; DECSTR *does* hang up the phone and force host speech on; default
    `DT_TERMINAL` is **6**, default `DT_LOG` is 0; the log/terminal flags are restored by RIS too, the line
    speeds/formats/C1 modes only by PUP/DECNVR. DECNVR restore does not delete the dictionary or hang up the phone.
22. **OM Table B-1**: `rerun` is `riy*r'ahn` and `transport` is `traens*p'owrt` (earlier transcriptions had lost
    the `*`); the table's *Morphology* column was never captured (§7.3).
23. **OM Table A-2** transcribed in full with expansions; `recd.` (not `rec'd`); extra entries `cms. kgs. mgs. msecs.
    mss. ml. sq. Ltd.`. The quantity-word list spells `quatturodecillion` (manual typo; ROM: `quattuordecillion`).
24. **1-char alphabet**: `ao` = `c` (Table 4-1; Table C-1 misprints `o`), `v` = lower-case `v`, `ax` = `x`, `ix` = `|`
    — all confirmed on the scans (§8.3).
25. **MITalk Table 10-1** levels were wrongly summarized earlier (demonstrative pronoun is level 6 with verb, noun/
    adjective/adverb/contraction is 7, then 8-14); duration **rules 7 and 10 percentages** recovered (§14.3).
26. **Hook-flash `^` (RM dial text) confirmed on the scan** (RM printed p. 49 = PDF p. 61): tone dialing accepts
    `0123456789*#ABCD!^`, pulse dialing `0123456789!^`; `!` = 1 s delay, `^` = 250 ms flash. The HTML OCR's `*`
    is an OCR error (§5.3). Figure 4-1 (PDF p. 62) gives wire examples: answer after 1 ring `ESC P0;60;10;1z ESC\`
    → `ESC P0;70;0z ESC\`; caller answers → `70;1z`; 20 s keypad timeout `60;30;20z`; timeout reply `70;2z`;
    host hang-up `60;11z` → `70;0z`.
27. **The PDFs are scans of images only**; `docs/*.pdf` are the ground truth for any table the HTML OCR mangles
    (workflow and extractor in Appendix A).
28. **LTS payload (§15.10, fourth pass with `docs/hunnicutt_lts.pdf`):** the earlier notes said "rule 0's leading
    `0x42` is not plain ASCII" and "bytes ≥ `0x80` are a repeat/optional-count escape". Corrected: payload bytes
    < `0x80` are **LTS codes** (phoneme codes, token codes, letter codes 70-79; `0x42` = code 66 = `,`); a byte
    ≥ `0x80` is signed `n = byte + 0x1C`: **n < 10** opens a group of n `(class, polarity)` feature tests (optionally
    preceded by a literal code byte | `0x80`), **n ≥ 10** is a **repeat prefix** allowing up to n − 10 repetitions of
    the next item (`f8` = up to 10 = Hunnicutt's `C0`). Also, `right_env_len` **includes the matched items**, and
    the replacement is a count byte followed by items in the same encoding. The "6 unexplained bytes" are the last
    delta entry and the 32-bit rule-stream length (§9).
29. **The token names `A E I O U H X C J Q` are LTS letter codes**, not stress or hat-pattern markers (§8.3); the
    ROM's LTS is a compact descendant of **Hunnicutt 1976** (§14.9), not of dapi's `l_us_ru1.c` rule layout.
30. **`0x13d78` is not a separate constant**: it is entry 62 (space) of the per-code feature table `0x13c80`.
31. **The ROM's intonation is Klattalk's hat pattern (dapi `ph_inton1.c` / `ph_drwt01.c`), not MITalk's
    O'Shaughnessy algorithm.** §14.4 had presented the O'Shaughnessy constants as "the checklist for the ROM
    intonation code"; they are background only (§14.10). The per-frame F0 routine is `pht0draw` `0xc722` (§15.14).
32. **Old `lts_rule_engine` / `lts_env_match` plate comments** (per-letter jump table, "4 parallel byte tables")
    were rewritten in Ghidra on 2026-09-23 to match §15.10. The `parse_bracket_command` plate comment is fixed
    (it now describes head size and F4/F5). The pending renames are applied.
33. **`kl3_push_event` is not the DSP path.** Older notes (§3, §7 table, §13.9, §15.8 and the `phtiming` plate comment)
    said it schedules DSP events. It is dapi's `make_f0_command`: it fills `0x817b8[]`/`0x81754[]` for
    `pht0draw`, and all its callers are in the tail of `phtiming` (the hat-pattern pass). DSP frames go through
    `dsp_command_queue_isr` / `dsp_send_speech_frame` (§16.7). The old `phtiming` plate also gave `inhdr`/`mindur`
    in ms (they are 6.4 ms frames); fixed.
58. **The DSP listing and §16 (2026-09-27, §16.10).** The C translation found three errors:
    - §16.6 and the listing's comment at `0x5E8` swapped the cascade's pointers: the states are read downwards from
      `0x4A` and the coefficients from `0x6E`.
    - The listing still called the `0x6000` frame the "speech frame" at `0x30D`/`0x316`; it is the speaker frame
      (§16.9).
    - §16.2 and the listing listed `RESET_COEFFS` and five reset constants as open; their roles, and the five words
      nothing reads, are in §16.10.

    `dsp/tmsdis.py` is fixed and the listing regenerated.
57. **The host-timeout task (2026-09-27, §15.38).** Its old plate called the CTRL-K an "attention/interrupt marker"
    (it is the clause flush, §12 and AGENTS §4). It also read `0x80160` as an "inhibit flag while speaking"; that is
    the host device's XOFF-sent flag. And it used the old name `queue_send` for `event_wait`. The plate is rewritten.
56. **SETUP (2026-09-27, §15.37).**
    - The lower-case `setup>` (§6, §15.3) is the spoken prompt "setup.", not a second prompt.
    - §15.3's "`tdparse`'s opcode range `0xc00-0x1200` is host-serial-port config" was wrong. Those kinds are SET flag
      (`0x0c`), SET SPEED and FORMAT (`0x0d`, `0x0e`, the only line settings), SHOW/SET HISTOGRAM (`0x0f`, `0x10`) and
      SHOW/SET INTERRUPT (`0x11`, `0x12`).
    - `main_task` does not exit when `tdparse` returns 0. EXIT ends SETUP and a new local session starts; the task
      never returns.
    - NVRAM word 10 `0x822e4` ("not traced") is the SET INTERRUPT character, and word 12 `0x822d0` is the SET HOST
      MODEM flag. Their C names are now `setup_interrupt_char` and `host_modem`.
    - §6 did not list LOG DEBUG, HOST MODEM or HISTOGRAM, nor give the abbreviation rule. ONLINE and OFFLINE also
      change DT_LOG.
    - Named: `setup_puts`, `keyword_match`, `parse_hex`.
55. **`host_phone` did not test the answer (2026-09-27, §15.36).** §15.35 said the corpus entry exercises answering,
    the keypad and the timeout. It did not:
    - a `hostfeed.h` bug made the ring start at the beginning of the `\W` meant to precede it;
    - the reset that DT_PHONE 10 posts then cancelled the answer.
    The dialing and hang-up parts did run. The entry and the tool are fixed. The old `phtask_main` and
    `dtmf_diagnostic_menu` plates used old names (`echo_digit_to_host`, `spdef_settings_io`, `queue_receive`); they are
    rewritten.
54. **Host-side names and facts (2026-09-27, §15.35).**
    - Renamed: `spdef_settings_io` → **`settings_reset`** (its modes are RIS, DECSTR, DECNVR restore and power-up;
      nothing to do with speaker definitions); `echo_digit_to_host` → **`host_line_putc`** (it sends any byte);
      `log_control_error` → **`log_error`**.
    - Named: `FUN_0000dff2` → `dsr_reply`, `FUN_0000ee74` → `print_rom_date`, `FUN_0000fa52` → `dt_stop`,
      `FUN_0000f5c6` → `line_configure`.
    - §5.2 expected DT_MASK in v1.8; it is not there.
    - §5.5's DSR replies are what the manual prints; v1.8 omits zero parameters (`ESC [ n`) and never sends `?21n`.
53. **The phone device (2026-09-27, §15.34).**
    - **`phone_line_status_from_dsp` `0x2084` has nothing to do with the DSP.** It is phone device op 9, which starts
      counting rings before answering (now `phone_start_answer`).
    - **`phone_line_state_enter2/3` are ops 7 and 8:** on hook and idle (`phone_set_idle`), and off hook
      (`phone_go_offhook`).
    - **No `g_phone_queue` or `list_unlink_node`.** The events travel through the device's input with `dev_rx_post`.
    - **The old `phtask_main_loop` plate was wrong in two places.** The 2 s `event_wait(200)` after `0x84-0x86` is a
      plain pause, not "reset the DSP side". Its "digit_mode_flag" is the off-hook flag `0x822d6`.
    - **`phtask_main_loop` is renamed `phtask_main`**, like the other task entries.
52. **"NVR fault. Using factory settings." at power-up is an emulator artifact (2026-09-27, §15.33).** The emulator's
    default NVRAM image is v2.0's settings record (version 5). v1.8 accepts only version 4, so it reports an NVR
    fault on every emulated boot and loads the same values from its factory record. A real v1.8 unit with a sound
    NVRAM does not say it. **Fixed the same day:** a v1.8 ROM now gets its own image, and the references were
    regenerated.
51. **`FUN_0000eecc` is not the DT_PHONE handler (2026-09-27, §15.32).** It only appends a byte to the DCS text
    buffer `0x81f66` (now `dcs_text_putc`). The DT_PHONE sub-commands are `dt_phone_command` `0xe5f6`, called by
    `dcs_command_dispatch`, and the dialing is `phone_dial` `0xeb54`. §13.2, §15.4 and §16.8 named the wrong function.
    Also: the emulator logs had no tone frames because it **skips the power-up self-test** (IP4 held low, the
    "skip self-test" jumper), not because the boot plays no tones.
50. **The typed tables (2026-09-27, §15.31).**
    - **Reads outside a table.** The single-block layout of `ph_rom.c` and `tx_rom*.c` hid three ROM reads outside
      a table. Splitting the blocks and running the corpus under AddressSanitizer found them:
      - `phsettar` reads `divtab[-1]`;
      - `lts_rule_apply` reads one byte past the last rule;
      - the old `tx_rom_sym.c` block ended at `0x19860`, one byte short of the NUL of `"Illegal phoneme \"%s\"\n"`,
        so that message ran on into the next array, and its output matched only by luck.

      The first two are what the ROM does and are now in the tables; the third was a bug in the C.
    - **The symbol list ends at record 119** (a NULL name). Record 120, the one `sym_max` (120) still lets
      `phoneme_name` read, is not a record at all: it is `sym_max` and `ph_letter_class` read as one.
49. **The clause scanner and the console printer (2026-09-27, §15.30).**
    - `kprintf` (`vformat_string`) is the ROM's own printf and knows only `%d` `%D` `%c` `%s`. The formats with `%x`
      or `%o` ("BUG: phone 0x%x has value, no pvalue[]", "no spell for '%c' %d. 0x%x", "Illegal character %o in
      out()") print the letter itself and take no argument: `0xx`, `o`. The C harnesses had stood in for `kprintf`
      with the C library's `vsnprintf`; the C now has the ROM's printer (`src/kernel/console.c`).
    - The `dttask_main` plate comment called the scanner `FUN_00003182` and its getc `FUN_0000f9ea`; they are
      `clause_readin` and `dttask_getc`, and the comment is rewritten.
    - §13 item 5 planned the scanner after `dapi/src/CMD/cm_text.c` with a "12-word limit". The ROM's scanner counts
      no words: a clause ends at `, ! ? .`, a line end after one of them, the end of the text, or when the clause
      buffer fills (174/199 words, §15.28).
    - `test_frames --task` compared only the `[:dv]` part of klsyn's console text, so the DT_LOG 0x80 voice table
      that `klclause` prints was never checked. `console.tsv` now records whether klsyn wrote each byte; the entry
      `readin_ctl` checks the table (1,375 bytes).
48. **Phonemic text (2026-09-27, §15.29).**
    - The 1-character alphabet (§8.3, from the manual) gives `j` for `jh`. The ROM's input table has `J`, and `j`
      is not a phoneme; ASKY also needs DT_MODE's SQUARE bit, or `[` does not start phonemic text.
    - The old `parse_phonemic_text` plate comment said "Not in C yet"; it is now C, and the comment is rewritten.
47. **The clause buffer (2026-09-27, §15.28).**
    - The old `clause_putsym` plate comment read its two kprintf messages as checks on bad input and its codes as
      "boundary markers". The first message is the `[:…]` text overflowing a clause, the second a symbol with values
      but no values. The function is the clause buffer: it merges boundary marks, ends the clause on `,` `!` `?` `.`
      or when full, and stores symbols with values. Rewritten in Ghidra.
    - `newclause` does not post "console_queue-style"; it posts to `klsyn_mbox` `0x807a2` and takes the next message
      from `klsyn_free_pool` `0x80790`. With DT_LOG `0x02` it first writes the clause to the console (`log_clause`).
    - `emit_punctuation_symbol` calls `newclause` after `.` `!` `?`, but `clause_putsym` has already ended the clause
      there, so every sentence end also posts a one-word clause `[0x41]` (§15.28).
46. **Numbers (2026-09-27, §15.27).**
    - `numeric_token_dispatch` is not called "whenever a buffered token looks numeric" (§15.12): the token flush sends
      it **every** token of ordinary text, and a non-number falls through to `out()`. Renamed `token_dispatch`.
    - The pattern tables `0x12e56`/`0x12f26` are decoded (§7.3 and §9 said "undecoded"); `FUN_000040e0` is
      `classify_number`, over the regular-expression matcher `pattern_match` `0x7352`.
    - The time separators are `","`, not "colon" (§12.5, §15.12): `12:00` = "twelve, oh oh,".
    - "dot" and the other `0x12f76` words are never spoken, and `outn()` is unreachable: the validation of class 13
      admits no character that would use them. "over" is the fraction code's ("one over one").
    - A lone `0` (and `00`) is "oh", not "zero" (MITalk's rule, §14.2, does not hold); after a point 0 is "oh" too;
      "zero" is said for zeros only after the point ("12.00") and in more-than-nine-digit strings.
    - `cardinal_number_to_words` falls back to `speak_digits_individually` only for **more than nine digits**;
      comma-grouped numbers up to nine digits get scale words (§15.12 said "or already comma-grouped input").
      `number_to_words` has no "digit-string" mode: 10/11 are fraction denominators (singular/plural), 12 ordinals.
    - The stage-2 C (`tx_dict.c`) turned `lookup_word`'s address 0 (a one-character word like `$` whose first
      character has no dictionary words) into a pointer and crashed; the ROM treats it as "not found" after counting
      a hit. Fixed; the dictionary tests missed it because no corpus word was a lone such character.
45. **Word details (2026-09-26, §15.26).**
    - `0x81d76` is not an "abbreviation count" (old `emit_punctuation_symbol` plate, §15.11). It is `wh_question`: a `?`
      after a clause that starts with a wh- word is sent as a period.
    - Glued text such as "St.Louis" does not match `str_match_fold`, so it does not "skip the deferral" (§15.11). Only a
      closing punctuation mark after the abbreviation makes it speak "drive"/"street" at once ("Dr.," "St.)").
    - The old `pronounce_word` plate spoke of "ordinal/roman-numeral-looking all-caps runs". The capitals path is for
      acronyms with a period after each letter. The "forced allophone" for "a" is `spell_char('a')`, and only before a
      clause mark ("A,").
    - "Multinational letters lose their accents" (manual) is bit 7 stripped: `é` becomes `i` [V: emulator].
    - The §4 row "`0x81d74` in rule-derived word flag" was stale (it is `prev_word_stressed`, item 44); it is removed.
44. **Dictionary details (2026-09-26, §15.25).**
    - `0x81d74` is not "the clause has a word". `pronounce_word` subtracts 1 from it at every word, a primary stress
      sets it to 1 and `newclause` clears it, so during a word it is ≥ 0 when the previous word was stressed. It is now
      `prev_word_stressed`. A dictionary `)` (verb marker) is spoken only then, and never on an -ing or -ed form.
    - The built-in dictionary is a **plain trie** (49 roots, 19,703 nodes, none shared), not a DAWG, and its format is
      not dapi's (§7.2, §15.9 said "trie/DAWG … matching dapi's compiled dic format").
    - `FUN_00003dcc` is not a "word-path/homograph handler" (§7): it is `parse_phonemic_text`, which speaks phonemic text
      (`[ ]` text and user-dictionary substitutions).
    - `FUN_0001d618` is `panic`, a `JMP` to itself.
43. **LTS details (2026-09-26, §15.24).**
    - A repeat item in a rule pattern is matched **greedily, without backtracking**. §15.10 and the old
      `lts_env_match` plate said "with backtracking". When a repeated item stops matching, it is skipped and the next
      pattern item is tried on the same list node.
    - The ASCII map's `0xFF` ("no code") entries are **not** rejected. The test compares the sign-extended byte with
      `0xff`, so such a character would become code −1. None reaches LTS, since punctuation and digits split words
      first.
    - Words reach `lts_rule_engine` lower-cased, apostrophes kept, and at most 79 characters long.
42. **The emulator's text feed is the local terminal (2026-09-26, §15.23).** `dtc01_feed_text` drives DUART channel B,
    which the v1.8 ROM uses for the **local terminal** (console device `0x80328`); the host line is channel A (host
    device `0x8011e`). So every corpus text before §15.23 was typed on the local terminal. That is why escape sequences
    were spoken (§5.1: the local terminal does not parse them) and why XOFF never came (only the host device sends it).
    The emulator now has a host-line feed, `dtc01_feed_host`. Other old names and texts that were wrong:
    - `duart_tx_drain_queue` `0x1870` is the **receive** handler: renamed `duart_rx_char`, and `0x17f8` is `duart_tx_char`;
    - `send_dsp_event` `0xef00` has nothing to do with the DSP: it builds the `DCS 0;R2;R3 z ST` replies (`send_dcs_reply`);
    - `emit_sync_marker` **waits** on `sync_sem`; it does not "wake" anything;
    - the `klsyn_task_main` plate called `sem_signal` "a DSP-transfer primitive" and `stop_pending` "a busy/backlog flag";
    - §15.22 said `[:dv list]` prints on the console "even from host text". That was tested on the local terminal.
      Re-tested on the host line, it holds: 952 bytes on the console, nothing to the host.
41. **`[:dv]` details (2026-09-26, §15.22).** §8.1 and the manual imply that `[list]` shows every parameter and only
    works from the local terminal. In v1.8, `list` hides 8 parameters (`listall` shows them), and `[:dv list]` in host
    text prints on the console. The old `parse_bracket_command` plate said an unknown name goes to "error recovery
    FUN_7d42/FUN_81e6": those are `set_formant_limits`/`setspdef`, which every `[:dv]` runs at its end. Also, `=`
    only works after a blank (`ap =9`).
40. **`parse_phoneme_param_stream` plate (2026-09-26, §15.21).** It said the normal words "accumulate into the
    per-phoneme duration table". In fact it compacts the item into `phonemes[]`. Only a symbol word with bits `0x3000`
    carries a duration (and F0), and those go to slot index + 5. The `0x4000` flag is in `symbol_table` records;
    `DAT_82296` is `sprate`, and case `0x65` is `:ra`. The plate has been rewritten.
39. **`phalloph` has no `phsort` step (2026-09-26, §15.20).** The old plate comment and §15.17 called `0x843e`
    "dapi phsort + phalloph". The ROM's routine only rewrites the stream (phones, stress and boundary symbols) into
    allophones and structure bits. Buffer `0x81d78`, called `g_klsyn_scratch` in the `klsyn_task_main` plate, is the
    clause's `phonemes[]` when `phalloph` runs. User durations/F0 for input phone n sit at `[n + 5]`, not `[n]`.
38. **Harry's speaker word 8 is 0, not 40 (2026-09-26, §15.17).** §16.9 said Harry's `la` word was 40 although
    his `la` is 0. That misread one column: word 8 (`la`) is 0, and the 40 is word 15, nf × 4 with Harry's nf = 10.
    `setspdef` sends `cur_voice.la` unchanged, and all seven voices match.
37. **DSP frame roles were swapped (2026-09-26, §16.9).** The 24-word `0x6000` frame is the per-voice speaker
    definition (f4/b4/f5/b5 scaled by head size, p4/p5, g1-g5, gf/gn/gv/gh, ri, nf, br, la, pr, ap), sent on voice
    changes. The 19-word `0x4000` frame is the 6.4 ms speech frame (T0 F1 F2 F3 FNZ B1 B2 B3 AV AH A2-A6 AB TLT).
    §16.4 called the 24-word frame "the speech frame", and §12.34 said the 19-word frame was pitch-rate data. Both
    were wrong, and the emulator log settles it.
36. **RTOS and library names (2026-09-26, §15.16).** Several early names were guesses that the full kernel read-through
    contradicts. The functions were renamed and the old names kept in their plate comments:

    | Old name | New name | Why |
    |---|---|---|
    | `notify_sync_point` `0x88c` | `sem_wait` | decrements and blocks, so `emit_sync_marker` (DT_SYNC) **waits** for `klsyn` to `sem_signal` |
    | `wait_for_stop_signal` `0x8d2` | `task_suspend` | the stop task suspends itself; `0xfa52` wakes it with `task_resume` (`0x8fe`), very likely on DT_STOP [I] |
    | `queue_send` `0x986` | `event_wait` | timed/evented wait, not a queue send |
    | `queue_receive` `0xc00` | `dev_getc` | blocking byte read from a device ring |
    | `console_queue_putc` `0xc70` | `dev_putc` | any device, not only the console |
    | `critical_section_enter_exit` `0xe04` | `dev_control` | device op dispatcher; the "enter/exit" pairs are lock/unlock of the output side |
    | `list_unlink_node` `0xade` | `tick_hook_remove_impl` | removes a periodic tick hook; the phone status routine re-arms its debounce hook, it does not post to a queue |
    | `str_copy`, `fprintf_stream`, `flush_stream_buffer`, `maybe_flush_stream` | `strcpy`, `fprintf`, `_flsbuf`, `fflush_if_pending` | standard stdio roles |

35. **`setup_mode_main_loop` is the `main` task (2026-09-26).** Its Ghidra entry was `0x26c8`, on zero padding
    (`ori.b #0,d0`), so nothing could call it. The real entry `0x2700` is stored only in the task descriptor
    `0x268e` `{0x800, 0, 0x2700, "main"}`, which `boot_init` `0x1024` passes to `task_create_impl` through the
    startup table `0x265e`. Renamed **`main_task`**. The claim that `host_task_main` calls it was wrong. For the
    speech/host split (§13.15) it belongs to the host-terminal side.
34. **DSP tables and the 19-word frame (2026-09-23, with `docs/klatt1980.pdf`).** §16 first called `0x1C7-0x2A6` a
    bandwidth/decay table and `0x014-0x089` undecoded; the first is parwav's `B0[224]` and most of the second is the
    nasal anti-resonator tables (§16.2). §16.4 said speech-frame words 14/15 form the pitch period; they set the
    open phase `nopen`, and the pitch period `T0` is word 1 of the 19-word frame. §16.5's "per-voice constants"
    guess for the 19-word frame is withdrawn. `BW_TABLE` in `dsp/tmsdis.py` is now `B0_TABLE`.
19. **Hook-flash char stays `^`** (§5.3): the HTML RM OCR shows `*`; that is an OCR error confirmed by
    `dtlib.rno` (`'_^'`).

---

## 13. Prioritized next steps (manual-driven checklist)

1. **`dcs_command_dispatch`**: confirm the P2 switch constants (§5.2), name each handler
   (`dt_photext`, `dt_stop`, `dt_sync`, `dt_speak`, `dt_index*`, `dt_dict`, `dt_phone`, `dt_mode`,
   `dt_log`, `dt_terminal`, `dt_mask` — the manual says P2 83 exists in 1.8, §5.2), find where the
   **R2/R3 replies** are built (via `send_control_sequence`), and how index-reply/query interact with
   speech progress. Also find the CR-after-reply logic that DT_MASK enables. **Partly done (§15.23):** the reply
   builder is `send_dcs_reply` `0xef00`; DT_INDEX/_REPLY/_QUERY, DT_SYNC and DT_STOP (P2 20/21/22/11/10) are
   confirmed in the emulator on the host line, and the index reply is sent when the marked phone is reached.
2. **DT_PHONE**: the handler is `dt_phone_command` `0xe5f6`, not `FUN_0000eecc` (§12.51). **Partly done (§15.32):**
   the sub-commands, the dialer `phone_dial` `0xeb54` (2 s pre-dial wait, `!`, `^`, pulse timing, the tone tables) and
   the DSP tone path are read. **The phone task's side is read too (§15.34):** the device ops, the TLC interrupt, ring
   counting, the task's states and replies, and the stand-alone phone mode at power-up. **The emulator models the
   line** (rings, caller keys, hook state; §15.34), and a whole call runs as the manual says. Still open: where R3 = 3
   (dial text too long) is sent (`dcs_text_putc` only drops the excess), and the C of the host side. **Both done
   (§15.35):** `dcs_command_dispatch` sends R3 = 3 when the text fills its 256-byte buffer, and the host task is C,
   `dt_phone_command` and `phone_dial` included. **The phone task is C too (§15.36)**, `dtmf_diagnostic_menu`
   included, checked end to end on the whole corpus (`test_host --phone`).
3. **`csi_command_dispatch`**: DA (`ESC [ ? 19 c`), DSR brief/extended, DECTST 1-5, DECSTR,
   DECNVR; find the **DSR error-flag word** (test `0x81f12`: bit ↔ error 22-27, extended DSR
   clears it, first-since-power-on `?21n` vs `?20n`).
4. ~~**XON/XOFF thresholds** and host queue capacity (≈250 ms @ 9600 baud); find who sends
   XOFF/XON (duart ISR or queue watermark code).~~ Done (§5.1, §17.14): a 304-byte ring, XOFF above 64 waiting
   (`duart_rx_char` `0x192c`), XON below 16 after a read (`0x1996`).
5. ~~**Clause scanner** `FUN_00003182` / `FUN_00003580` / `FUN_00003dcc`: decode the char-class table `0x12d11`.~~
   **Done (2026-09-27, §15.30):** `clause_readin`, `readin_flush` and `dttask_getc` are named, the class table is
   decoded, and the scanner and `dttask_main` are C, checked end to end. There is no word limit (§12.49).
6. ~~**Number-pattern tables** `0x12e56` / `0x12f26`: decode against §7.3 classes and Table A-1;
   verify "12:00" handling and the year-style mode.~~ **Done (2026-09-27, §15.27):** 34 patterns decoded, "12:00" =
   "twelve, oh oh,", years and all classes in C, word for word.
7. ~~**Built-in dictionary trie**: decode, validate with Table B-1 and A-2.~~ **Done (2026-09-26, §15.25):** 6,508
   words decoded; all of Table A-2 is there, and Table B-1 matches except `)use` (and `)present`, `a`, `0`).
8. **Phrase-structure / intonation / allophone→parameter** code in `0x8000-0xb000` (next to
   `phtiming`): `dapi/src/PH/ph_inton.c`, `ph_claus.c`, `ph_aloph.c`, `ph_draw.c`; MITalk Ch. 8-11 explain the
   algorithms and give constants to grep for (§14.3-14.5).
9. **DSP frame word meanings** — **done 2026-09-26 from the emulator log (§16.9)**, and the 68000 code that builds
   each frame is traced (`phsettar` → `phdraw`/`pht0draw` → `dsp_post_frame` → `dsp_queue`, §15.17). Old text (§16.4-16.5, §16.8): the layout is known from the DSP disassembly; map each word
   to a Klatt parameter from the 68000 side (who fills the queue items `dsp_send_speech_frame` sends) against
   `klsyn/parwav.c` `gethost()` and dapi `spc.h`.
10. ~~**NVRAM layout** (X2212 `0x94000`): user vs factory memory (DECNVR Pm 0/1, SETUP SAVE/RECALL);
    what the `settings_reset` modes mean.~~ **Done:** the record layout, version and checksum (§15.33); the modes of
    `settings_reset` (formerly `spdef_settings_io`) are 0 RIS, 1 DECSTR, 2 DECNVR restore (the DTMF menu's factory
    reset is `(2,1)`), 3 power-up (§15.35).
11. Loose ends carried over (the LTS payload encoding and the 6-byte gap are **solved**, §15.10): the exact meaning
    of the dynamic LTS feature bits (classes `0x14`/`0x15` ≈ primary/secondary stress, `0x18` ≈ inflectional
    suffix, `0x19` ≈ stress-affecting suffix, `0x01`/`0x02` — all **[I]**, confirm in `lts_rule_apply` `0x6ad4`
    and by running made-up words that can't be in the dictionary through the emulator with `LOG PHONEME`, then
    comparing with the decoder's listing); whether the `0xFF` "invalid" test at `0x6be8`
    (`cmpi.w #$ff` after `ext.w`, so it compares against −1 and never fires) lets digits reach the rules;
    ~~the four unidentified shorts at `0x15bfe`~~ (= `f2max_by_sex`/`f3max_by_sex`, §15.17); A4=`0x10000` origin
    for the `host` task; `0x2680` init-table entry `0x2628`; what signals `wait_for_stop_signal`
    (very likely DT_STOP).
12. DSP ROM — reopened and disassembled (§16); its own open items are listed in §16.8.

13. **Second-pass leads** (from the HTML manuals + MITalk): (a) find the readers of the phoneme name
    table `0x19704` (xref `0x19704`, `0x19846`) — that is the phonemic-text parser and the `LOG PHONEME`
    printer (the printer is found: `log_clause`/`phoneme_name`, which read it through `sym_table` `0x192e2`, §15.28); identify the unexplained token names `@ ~~ ::: :== :$$ :## :vo :in :re :se :dr :db` (`A E I O U H X C J Q`,
    `:++`, `:--` are solved, §8.3/§15.10); (b) ~~locate the `MODE_ASKY` (bit 1 of `0x822ce`) test to find the 1-char alphabet~~ (done: `phoneme_name` prints
    `sym_table`'s 1-character names, §15.28, and `lookup_phoneme` reads them through `asky_codes`, §15.29);
    (c) ~~xref the ROM number strings `dot`/`over` (§7.3)~~ (done: "dot" is unreachable, §15.27); (d) read `print_voice_param_table` `0x8072`
    to settle `list` vs `listall` (**done**, §15.22); (e) verify `duart_isr` substitutes `0x1A` for errored bytes (§5.6);
    (f) the per-frame F0 routine is found (`pht0draw` `0xc722`, §15.14). Still to find: the hat-pattern command
    generator, dapi `ph_inton1.c`, which fills `0x817b8[]`/`0x81754[]`. Find it through the writers of those arrays;
    the O'Shaughnessy constants of §14.4 do **not** apply (§14.10).
14. **Phonetic component** — **frame path done 2026-09-26 (§15.17)**: `phsettar`, `phdraw`, `pht0draw`, `setspdef`,
    `dsp_post_frame` and the target/locus/diphthong/amplitude tables are identified. `phalloph` is done (§15.20). The
    `DAT_82294` gate is resolved: `draw_mode_82294` is `.data` `'s'` with no writer, so the frame loop always runs;
    `phmode_8229c` is likewise a constant 0. Remaining: `0x81bf6` (written by `phsettar`, read by `pht0draw`), the
    table *contents* (row/column meaning of `maleloc`/`maldip`), and whether `#`/`-` really act as primary stress in
    text input (§15.20).

15. **Compilable-C target and the speech / host-terminal split** (user, 2026-09-25). **Started 2026-09-26:**
    steps 1-4 (§15.15-15.17); step 5, the frame path in C (§15.18), step 6, `phtiming` in C (§15.19), and step 7,
    `phalloph` in C (§15.20), and step 8, `klclause` and the klsyn work item (§15.21), reproduce the ROM word for
    word. The whole klsyn side of speech is now C, from the work item the text pipeline sends to the words posted
    to the DSP. Step 9, `parse_bracket_command` (`[:dv …]`, §15.22), and step 10, the klsyn task loop with DT_SYNC,
    DT_STOP and index markers (§15.23), are done too. **Step 11, the text pipeline, is done** (§15.24-15.30). The LTS
    engine (§15.24), the dictionaries (§15.25), the word layer (`pronounce_word`, spelling, punctuation, §15.26) and
    the token layer with the number engine and `out`/`outn` (§15.27) and the clause buffer (`clause_putsym`,
    `newclause` and the DT_LOG phoneme log, §15.28) and phonemic text (`parse_phonemic_text`, §15.29) are in C, word
    for word, and so are the clause scanner and `dttask` (§15.30): the whole text pipeline is C, checked end to end
    from every character read from the text pipe to every message posted to klsyn. Speech now runs in C from the
    text pipe to the DSP words. **The host side has started (2026-09-27, §15.35):** the `host` task (escape parser,
    ESC/CSI/DCS commands, DT_PHONE and the dialer, DECTST, the settings and the NVRAM record) is C, checked end to end
    on the whole corpus, and so are the `phone` task with its spoken DTMF menu (§15.36), the `main` task (the local
    terminal and SETUP, §15.37) and the `host timeout` and `stop` tasks (§15.38). **All the tasks are C now; so is
    the speech side's kernel (§17.9), which runs `dttask` and `klsyn` from power-up; the host side's is not.** **The DSP program is C too (2026-09-27, §16.10):** `dsp_synth.c` gives every sample the ROM's DSP
    gives, on the whole corpus and the self-test's tones.

    **The speech synth becomes a library (user, 2026-09-27).** It is to be compiled as a DLL (Windows) or a shared
    object (Linux), and the host terminal emulator becomes a program that uses it.
    - The exports should follow dapi's: `dapi/src/dectalk.def` (`TextToSpeechStartup`, `…Speak`, `…Sync`, `…Reset`,
      `…Pause`/`…Resume`, the user-dictionary calls, …) and `dapi/src/API/TTSAPI.H`.
    - **The API is drafted first, before the library is built.**
    - It should have an **index callback**. dapi's `TextToSpeechStartup` takes a `DtCallbackRoutine`, and index marks
      arrive through it as `TTS_MSG_INDEX_MARK`.
    - The pieces that map onto it today:
      - `ph_index_reply_hook` (the `:re` reply, the only call from speech back into the host side, §15.23);
      - `last_index` (DT_INDEX_QUERY);
      - `emit_sync_marker` (DT_SYNC → `TextToSpeechSync`);
      - the stop task with `stop_pending` (DT_STOP → `TextToSpeechReset`, §15.38);
      - the text pipe (`TextToSpeechSpeak`);
      - `dict_hash_insert_or_delete` / `dict_hash_clear_all` (DT_DICT, RIS → the user-dictionary calls).
    - Index marks need no host side: `[:in n]` and `[:re n]` in the text work on their own (§15.35).
    - **The draft is in §17** (`src/api/ttsapi.h`, `dectalk.def`: dapi's names, `DECtalk.dll` /
      `libtts_us.so`). The user answered its questions on 2026-09-27 (§17.6): a thread inside, dapi's three outputs
      (device, file, memory), 10 kHz only, dapi's window-message startup on Windows, and an event queue as its Linux
      counterpart (§17.7).
    - **dapi's SAY and speak samples** (`samples/`) are the project's speaking programs on Windows and Linux, next
      to the host terminal emulator (plan §17.8). **Done (2026-09-27, §17.12):** SAY as portable C, speak as one core
      with a Win32 and a GTK 3 front end, and one CMake build (`CMakeLists.txt` at the top of the project) for the library and the
      programs.

    The goal
    is C source that compiles, split into two separable parts once decompilation is complete:
    - **Speech** (standalone if possible): `dttask` text pipeline (clause scanner, numbers, dictionary, LTS),
      phonetic component (`phtiming`, `phsettar`, `pht0draw`, …), `klsyn` front end, the DSP frame output and the DSP
      program (or a C model of it). **The in-text command system stays with speech** (user, 2026-09-25):
      `parse_bracket_command` `0x7d1e` and every `[:…]` command (voice selection `[:np]` etc., voice design
      `[:dv …]`, rate and the rest, §8.1), and phonemic text / phoneme streams including singing (`notetab`, §15.14).
    - **Host terminal** (host-side simulator; everything that is not speech): the `host` task (escape parser, DCS/CSI, SETUP, logging, XON/XOFF), the `host timeout`/`stop`
      tasks, and the `phone` task (DTMF decode, tone/pulse dialing, ring/hook). It stays a program of its own next to
      the synth. A possible later use is internet/VoIP connectivity through its phone/modem side.

    **Host-terminal I/O (user decision, 2026-09-27).**
    - **Serial: no custom driver.** Each serial line (host and local terminal) goes through one "line" interface
      that only moves bytes, with interchangeable backends:
      - a Win32 COM port: a real USB-serial adapter, or a com0com virtual pair (an existing signed driver) when
        another Windows program has to see a COM port;
      - a TCP socket (terminal programs, scripts, other machines; later the internet/VoIP route);
      - stdin/stdout (tests and scripts).

      The protocol (escape parser, XON/XOFF) stays in the host code, above the line.
    - **Phone: a simulated line.** Outgoing tones are what the ROM sends: the DSP tone commands (§15.32) are
      played through the same audio output as speech, so the power-up self-test tones and DT_PHONE dialing come out
      as on the hardware, with the order and timing decided by the ROM's code. The DSP C model renders them; until it
      exists, a small two-sine generator reading the same commands stands in. Incoming events (ring, off hook / on
      hook, the caller's DTMF keys, hang-up) are injected where the 68000 reads them from the TLC, from the host
      terminal (keys, commands or TCP messages). The self-test's loopback (§15.32) is served by the same
      simulated DTMF receiver. A VoIP backend (SIP, RFC 2833/4733 DTMF events) can come later behind the same
      interface.
    - **No NVRAM file (user decision, 2026-09-27).** The X2212 stays in memory, starting from the built-in default
      image (the v1.8 factory record, §15.33) at every power-up. SETUP `SAVE` and DECNVR store change it only for the
      run (a state snapshot keeps it).
      - In v1.8 the record holds only the host and terminal settings (words 0-12).
      - It never holds voices: `[:dv]` changes such as a customized Val are lost at power-up on a real unit too.

    The coupling points to define as an explicit interface: the host → `dttask` text queue, DT_STOP/DT_SYNC/index
    replies back to the host, the phone task's use of the DSP handshake (tone mode, §16.3), and the shared settings
    (NVRAM, voice parameters). Agreed first steps when the user starts: bulk-export Ghidra's decompilation of all 291
    functions with a report on the problem functions; fix the shared types (TCB, queue items, voice record, DSP frames,
    syscall convention); identify the C runtime helpers; use the emulator's DSP input-FIFO log as the equivalence
    test (item 9, §16.8 item 4); port dapi code where the ROM matches it.

    **Requirement: build without the ROMs** (user, 2026-09-27). The finished project must compile and run with no ROM
    image present, so every piece of data the firmware uses has to be extracted from the ROMs into the source tree.
    - **Done so far.** The C already builds without a ROM. The data it reads is in generated C files kept in the tree:
      - `ph_rom.c` (`gen_ph_rom.py`);
      - `tx_rom.c`, `tx_rom_lts.c`, `tx_rom_dict.c` (`gen_tx_rom.py`);
      - a few strings written inline, such as the `<ESC>` names in `console.c`.

      **All the speech tables are named, typed tables** (2026-09-27, §15.31); no speech code reads by ROM address
      any more.

      Only the generator scripts read `merges/dectalk_v1.8_full.bin`. Treat their output as source: keep it in the
      tree, and never make the build regenerate it.
    - **Still to extract.**
      - Everything the rest of the firmware reads (the host side's tables are typed C now: the host, phone and main
        tasks, §15.35-15.37): the kernel and boot (`.data` image `0x1d408`, the task table `0x12bac`),
        NVRAM defaults, and the voice records that are not yet in `ph_rom.c`.
      - The DSP ROM: its tables (`amptable`, parwav's `B0[224]` at DSP `0x1C7`, §16) and a C model of its program,
        so the synth needs no TMS32010 image.
    - **Testing still needs the ROMs.** The emulator, the captures and `decomp/reference/` are how the C is checked,
      and they need the ROM images. That is verification only; the compiled program must not depend on them.
    - **Form of the data (user, 2026-09-27): named, typed C tables.** Once a table is fully understood, turn it from
      a verbatim byte block read by ROM address into a named C table with a real type: arrays of shorts, structs for
      records, strings, enums for codes. The C then uses it by name.
      - **Until then.** A table that is not yet understood stays in the byte blocks. (None is left in speech.)
      - **Keep the results identical.** After each conversion every `check_frames.py` mode must still pass.
      - **Out-of-bounds reads.** Where the ROM reads outside a table, the typed table must keep that behavior or the
        C must handle it, with a comment. Examples: the LTS code reads a table at index -1; the dictionary trie links
        by relative offsets; `readin_class` would be read past its end by bytes ≥ 0x80 (§15.30).
      - **Tooling.** The extraction may stay a script (`gen_*.py` writing the typed tables), but its output is source
        and the build must not need the ROM.

**Dynamic verification with the emulator** (see §2 API): feed the vectors in App. C and read
`dtc01_read_host_tx`. Good first checks: DA reply bytes; DSR before/after `[+]` (the manual's own
firmware probe, §5.5); `DT_INDEX_QUERY` timing after `DT_SYNC`; 5-second timeout flush; a `DT_MASK`
round-trip (§5.2); number/abbreviation expectations in App. C. **Escape sequences: resolved (§15.23).**
`dtc01_feed_text` drives channel B, the **local terminal**, where escapes are spoken, not parsed. Use
**`dtc01_feed_host`** (channel A, the host line; `spclog`/`phcapture -H`, `native/hostfeed.h`) and read the replies
with `dtc01_read_host_line_tx`. The host feed honours the ROM's XOFF. **Caveat 2 (2026-09-26, §15.20):** the
local-terminal feed pushes text as fast as the DUART takes it, and the console line never sends XOFF, so a long text
spoken slowly (about 110+ characters at `[:ra 120]`) loses its end there [I: probably the ROM's input buffer
overflowing]. Keep local-terminal entries short, or use the host line. Both feeds deliver bytes far faster than any
real baud rate, which matters for timing races such as text sent right after DT_STOP (§15.23). **Caveat 3
(2026-09-26, §15.25):** the host line loses its end too, for about 300 characters or more: the whole text is in the
ROM's input buffer before its XOFF arrives. Split long host-line entries with `\w`. (Check that the last word shows up
in the capture's `dict.tsv`/`lts.tsv`.)

---

## 14. MITalk lineage (`docs/mitalk.html`) — the algorithms DECtalk was built from

*From Text to Speech: The MITalk System* (Allen, Hunnicutt, Klatt; Cambridge UP, 1987) documents the MIT
system whose text analysis, rules and Klatt synthesizer became DECtalk. It is **algorithmic background,
not ROM documentation**: MITalk was a research system (~12,000-morph lexicon, ATN parser, software
synthesizer); DECtalk simplified it and re-tuned it. Use it to understand *why* a ROM routine/table exists
and to sanity-check decoded constants; do not expect its numbers to appear verbatim in the ROM (searched:
the vowel formant targets of MITalk Table 11-1 — e.g. iy F1/F2/F3 = 310/2020/2960 — are **not** stored as
plain 16-bit values in the ROM).

Book structure (part I analysis / part II synthesis): 2 Text preprocessing (FORMAT) · 3 Morphological
analysis (DECOMP, morph lexicon + FSM) · 4 Phrase-level parser (ATN; noun/verb groups; parts of speech,
Appendix A) · 5 Morphophonemics & stress adjustment · 6 Letter-to-sound & lexical stress (SOUND1) · 7
Survey · 8 Phonological component (PHONO1/2: stress rules, segmental rules, pauses) · 9 Prosodic component
(PROSOD: durations) · 10 F0 generator (O'Shaughnessy) · 11 Phonetic component (PHONET: synthesis-by-rule,
App. C targets) · 12 Klatt formant synthesizer · 13 Intelligibility tests · 14 Implementation. Appendices:
A part-of-speech processor · B Klatt symbols (phone names) · C context-dependent PHONET rules and target
tables (C-1 … C-7) · D-G test material. *(Tables in the HTML are partly OCR-garbled — B-1, 10-1, 11-x; text
and Tables 2-1, 9-1, 12-1 are good.)*

### 14.1 Stage map: MITalk → DECtalk manual modules (§7) → ROM/dapi
| MITalk stage | DECtalk module | ROM / dapi |
|---|---|---|
| FORMAT (2.4): paragraph/sentence/abbreviation/numeral/hyphen handling | sentence parser, word parser, number formatter | clause scanner + `pronounce_word`, `token_dispatch` family (§15.27); `dapi/src/CMD/cm_text.c` |
| DECOMP + morph lexicon (3) | dictionary manager | `lookup_word*`, built-in trie. The ROM appears to do *whole-word + suffix-strip* lookup (`lookup_word_with_suffix_stripping`) rather than MITalk's morph FSM **[I — trie not decoded]** |
| PARSER (4): phrase groups, part of speech | phrase structure | dapi `PPSTART/VPSTART/RELSTART` codes; the `)` verb-phrase marker; function-word tables (not located) |
| SOUND1 (5-6): affix stripping → consonant rules → vowel/affix rules → stress rules | letter-to-sound | `lts_rule_engine` `0x6a64` — **all four stages are rules in the one 373-rule table** (§15.10, Hunnicutt 1976 §14.9); dapi `l_us_ru1.c`, `l_us_suf.c` are later |
| PHONO1/2 (8): stress, segmental phonology, pauses | phoneme-to-voice | dapi `ph_aloph.c`, `ph_main.c`; ROM `0x8000-0xb000` |
| PROSOD (9): durations | phoneme-to-voice | `phtiming` `0x8ebe` = Klatt rules 1-11 (§14.3) |
| F0 generator (10) | phoneme-to-voice (intonation) | **DECtalk uses Klattalk's hat pattern instead** (§14.10): per-frame `pht0draw` `0xc722` found; the command generator (dapi `ph_inton1.c`) not located yet |
| PHONET (11, App. C): parameter targets + transitions | phoneme-to-voice / synth commands | dapi `ph_setar.c`/`ph_draw.c`; ROM `phsettar` `0xaa08` [I], `kl3_push_event` `0xa6e2` |
| KLSYN (12): 39-parameter cascade/parallel synthesizer | DSP | TMS32010 program `dsp/dsp_v1.8.lst` (§16) + `klsyn_task_main` `0x3c20`, `klsyn/parwav.c` |

### 14.2 FORMAT rules (§2.4) that the ROM number/word code still follows
- Paragraph start (leading whitespace + capital) ⇒ a period (⇒ pause); an extra period is inserted after
  sentences longer than **five words** ("pause for breath") — compare DECtalk's "speak when the buffer
  nears ~12 words" rule (§5.1).
- A period ends a sentence only if not an abbreviation period, *or* the abbreviation is at end of line
  and followed by whitespace + capital (DECtalk: "checks the characters after the period"; `Dr.`/`St.`, §7.1).
- Table 2-1 (MITalk's abbreviation list): `Ms→miz Mr→mister Mrs→mizzes Dr→doctor Num→number Jan…Dec→month
  names etc→etcetera Jr→junior Prof→professor` — ancestor of the manual's Table A-2 / the ROM dictionary.
- All-caps or letter+digit words are *spelled* (letter-by-letter, "USA", "MIT"); embedded apostrophes stay in
  the word; a trailing `'` after `s` too; other `'` are quotes. Hyphen between words ⇒ two words; hyphen at
  end of line ⇒ join with next line; runs of dashes ⇒ one dash ⇒ pause. `%` → "per cent", `&` → "and".
- **Numerals**: comma-grouped triads (`hundred`/tens/teens rules), `.` between digit strings ⇒ **"point"** and the
  fraction is read digit by digit with **0 = "oh"** (a lone `0` is "zero": `71.50` → "seventy one point five
  oh"); `$` ⇒ "dollars"/"and"/"cents" with two-digit cents; **years**: 4 digits starting with 1 ⇒ "nineteen
  oh six", "eighteen hundred"; other digit strings > 3 digits without commas, and strings < 4 digits starting
  with 0, are read digit by digit. DECtalk generalizes this (the manual: `5000` = "five thousand", `1984`
  = "nineteen eighty-four", `01234` = digits, > 9 digits = groups). The ROM number vocabulary (§7.3)
  has both `zero` and two bare `0` strings near its tail — **[I]** probably the zero-vs-"oh" distinction;
  confirm in the decompile.

### 14.3 Durations (Ch. 9) — `phtiming`
`DUR = ((INHDUR − MINDUR) · PRCNT / 100) + MINDUR`, PRCNT starts at 100 and each rule does
`PRCNT = PRCNT · PRCNT1 / 100`; result rounded up to a multiple of 5 ms in MITalk (DECtalk works in
6.4 ms frames, §9.1). Rules (percent values in brackets): (1) pause insertion — 200 ms before
sentence-internal main clauses and at syntactic commas; (2) clause-final lengthening [140] of the last
vowel/syllabic and following consonants; (3) non-phrase-final shortening [60] (phrase-final postvocalic
liquid/nasal [140]); (4) non-word-final shortening [85]; (5) polysyllabic shortening [80]; (6)
non-initial-consonant shortening [85]; (7) unstressed shortening (MINDUR halved for unstressed segments; then
unstressed **and 2-stressed** segments shortened by PRCNT1 = **50** for a word-medial syllabic, **70** for other
syllabics, **10** for a prevocalic liquid/glide, **70** for all others — table recovered from the PDF scan,
MITalk PDF file-order p. 110 = printed p. 97); (8) emphasized vowel
lengthened [140]; (9) postvocalic context of vowels [open word-final
120, before voiced fricative 160, voiced plosive 120, nasal 85, voiceless plosive 70, else 100; if the
vowel is non-phrase-final use `70 + 0.3·PRCNT1`]; (10) shortening in clusters (PDF p. 111): **vowel followed by a
vowel 120, vowel preceded by a vowel 70, consonant surrounded by consonants 50, consonant preceded by a
consonant 70, consonant followed by a consonant 70** (not across phrase boundaries; word boundaries ignored);
(11) +25 ms for a
stressed vowel/sonorant after a voiceless plosive. **Speaking rate** `SPRATE` 60-300 wpm, default **180**
(DECtalk `:ra` = 120-350, default 180); at rates < 150 wpm a short pause follows each content word;
most rate change is done by pause durations. Cross-check ROM `phtiming` against `dapi/src/PH/p_us_tim.c`
(already an exact match) and the book when naming constants (200 ms pause, 140/60/85/80 %).

### 14.4 F0 (Ch. 10) — O'Shaughnessy algorithm (MITalk background; the ROM uses Klattalk's hat pattern instead, §14.10)
High-level system: **Tune A** (declaratives: linear fall, sharp fall on last content word), **Tune B**
(yes/no questions: rise then flat then sharp terminal rise), **Tune C** (wh-questions — a third tune added in
MITalk's adjustments to the algorithm: high peak on the question word, steeper fall, higher last accent). Content words get a rise-fall on the stressed syllable;
function words / unstressed syllables a **5 Hz** excursion. Word accent numbers run from **0** (one-syllable
articles) to **11+n** (sentential adverb with n syllables); parts of speech are ranked in nine distinguished
levels — **Table 10-1 (recovered from the PDF scan, MITalk file-order p. 114 = printed p. 101):** **0** article ·
**1** conjunction, relative pronoun · **2** preposition, auxiliary verb, (unstressable modal, vocative) · **3**
personal pronoun · **6** verb, demonstrative pronoun · **7** noun, adjective, adverb, contraction · **8**
(reflexive pronoun) · **9** stressable modal · **10** quantifier · **11** interrogative adjectives · **12** (negative
element) · **14** (sentential adverb) (parenthesized classes are supplied by the F0 program, not the lexicon).
Level 6 (verb) and above make a peak ("content" words); demonstrative pronouns, contractions, modals,
quantifiers and interrogative adjectives are labelled "function" words by PHONO1 but elevated to content
importance here. The F0 program emits **two target values per phonetic segment** (onset and mid-value),
interpolated every **5 ms**, and PHONET outputs **20 variable parameters per 5 ms** (Table 11-3), versus
DECtalk's 6.4 ms frames (§14.5).
Low-level constants: first (highest) peak ≤ **~190 Hz**, longer sentences start higher; lower declination line
**110 Hz** (Tune A) / **125 Hz** (Tune B); basic rise **40 %** of the distance from that line to the peak, basic
fall **20 %**; within-phrase rises/falls further reduced **30 %**; two adjacent accents ⇒ rises −40 %; accents
separated by 2/3/4 unaccented syllables ⇒ rises +15/+20/+30 %; peak after 2/3 unaccented ⇒ −15/−25 %,
before 2/3 ⇒ +10/+15 %; an isolated word after ≥ 3 unaccented syllables rises to `peak − 95 Hz`; final F0 in a
statement reaches **75 Hz** (lowered a further 10 Hz in MITalk's adjustments), yes/no final rise ends 20 % above any earlier
peak; continuation rises **16 Hz** (before non-terminal punctuation/conjunction) and **8 Hz** (last word of a
non-final phrase after > 5 words); +20 % rise on an initial unvoiced consonant goes to the vowel's left edge;
a 5 Hz local perturbation on unstressed flat/falling syllables; a dip at glottalization. In DECtalk terms
**[I]**: the `ap`/`pr`/`as`/`bf`/`ef` voice parameters presumably scale this contour (`bf`/`ef` = beginning/end
baseline fall — compare the 110/125 Hz floors; Paul's `bf 115 ef 100`).

### 14.5 Klatt synthesizer (Ch. 12) vs the DTC01 voice parameters
Table 12-1 lists **39 control parameters** (22 varied per frame): AV, AF, AH, AVS (amplitudes, 0-80 dB), F0
(0-500 Hz), F1-F3 (variable), F4 (typ 3300), FNZ, AN, A1-A6, AB, B1-B3 (variable); constants: cascade/parallel
switch SW, glottal resonator FGP/BGP, glottal zero FGZ/BGZ, **B4 typ 250, F5 typ 3850, B5 typ 200**, F6 4900/B6 1000,
FNP/BNP/BNZ, BGS, **SR (sampling rate 5000-20000, typ 10000)**, **NWS (samples per chunk, typ 50)**, GO (overall gain,
typ 48), NFC (number of cascaded formants 4-6, typ 5). DTC01 correspondence: sample rate **10 kHz** ✓; frame =
64 samples = 6.4 ms (NWS 50 = 5 ms in MITalk); the per-voice constants are the same family — Paul's `f4 3300`
equals MITalk's typical F4 and Harry's `f5 3850` equals the typical F5 (Paul 3900); `b4/b5` per voice replace
MITalk's fixed 250/200. Manual Table 5-3 describes `nf` as "samples in glottal pulse open phase" and `sm` as
"high-frequency attenuation" — Klatt-synthesizer glottal-source controls. Use this to interpret the
DSP frames (§16.4-16.5): the 24-word speech frame carries 22 parameters (6 frequency-type, 10 dB amplitudes,
6 raw) and the 19-word frame the per-voice constants [I].

### 14.6 Phone names (App. B, "Klatt symbols") ↔ DECtalk 2-char codes
MITalk vowels `AA AE AH AO AW AX AXR AY EH ER EXR EY IH IX IXR IY OW OXR OY UH UW UXR YU`; sonorants `EL HH HX LL
LX RR RX WW WH YY`; nasals `EM EN MM NN NG`; fricatives `DH FF SS SH TH VV ZZ ZH`; plosives `BB DD DX GG GP KK
KP PP TT TQ`; affricates `CH JJ`; pseudo-vowel `AXP` (plosive release). **Careful**: MITalk `ER` = the vowel in
"bird" = DECtalk **`rr`**; MITalk `RR` (consonant r) = DECtalk **`r`**; `LL`→`l`, `WW`→`w`, `YY`→`y`, `NG`→`nx`,
`IXR/EXR/AXR/OXR/UXR`→ `ir/er/ar/or/ur` (matches the ROM duration values in §9.1), `JJ`→`jh`, `DX`→`dx`, `TQ`→`tx` **[I]**.
Syntax/stress symbols (App. B-2): `'` primary, `"`(or 2) secondary, `-` syllable boundary, `*` morpheme boundary,
`C:`/`F:` begin content/function word, `.` `?` end of declarative/question, `)` phrase/clause boundaries — the
same vocabulary that survives in DECtalk's phonemic text (§8.2).

### 14.7 Letter-to-sound (Ch. 6) — what it says about the ROM's rule payload
- Three stages: **(1) affix detection** — suffixes right-to-left, longest match first; each affix carries (a) its
  possible parts of speech, (b) the parts of speech of a suffix that may precede it, (c) word-final-only or not,
  (d) whether it turns preceding `y`→`i` / drops a preceding `e`; prefixes left-to-right, longest first; an affix
  is never removed if nothing would remain (`finishing` → `finish+ing`, not `fin+ish+ing`); **(2) consonant
  rules** left-to-right over the root; **(3) vowels, digraphs and affixes** over the whole string.
- A rule has a **left and a right context**, each of which can be **letters or already-converted segments**
  (four context types) — this is exactly the ROM's `{match_len, right_env_len, left_env_len}` record whose
  payload mixes grapheme codes and phoneme codes/feature masks (§9: `0x13c04` masks, `0x13c80` feature records).
- **Ordering is the priority**: rules are ordered longest-string-first, then specific-context-first (`cch` before
  `cc`/`ch` before `c`/`h`; prefixes `com`/`con` before `co`; suffix `…s` before `s`). This is why the ROM can
  simply try all 373 rules *in order* and take the first that matches (§7) — **rule position in the ROM table
  = priority**, so never reorder or dedupe when decoding. Consonants are converted before vowels so the
  converted consonant segments serve as context for vowel rules (`a` before `r`+consonant = `aa` unless
  preceded by segment `ww`).
- Then a separate ordered **stress-rule** pass (Halle–Keyser, reduced to 3 levels: primary, non-primary,
  none; input is fully phonemic) and later `ah rr → er`, reduced-vowel and `ion → iy ah nn` style rules. In the
  ROM these are **ordinary rules at the end of the same table** (rules 336-372, §15.10), not a separate program.
  The ROM's suffix-allomorph strings (`0x13b24-0x13b34`) are the tails that the *dictionary* path
  (`pronounce_dictionary_word`, §15.10) appends; the LTS rules strip and pronounce their own suffixes (rules
  2-35, 92-205). `Illegal suffix` (`0x13b58`) presumably belongs to the dictionary path too **[I]**.
- The detailed ancestor of this chapter — the full 557-rule listing and the stress rules in linguistic notation —
  is Hunnicutt's 1976 paper, §14.9.

### 14.8 Things the book does *not* give you
Nothing about the TMS32010, the 68000 memory map, the RTOS, the host escape protocol or SETUP — those come
from the RM/OM, `native/`, and the decompile. The DECtalk-specific voices (Paul, Betty, …), the `[:dv]`
parameter set, the 1-char alphabet and the DEC abbreviation/dictionary lists are DECtalk additions.

### 14.9 Hunnicutt 1976 (`docs/hunnicutt_lts.pdf`) — the rule set the ROM's LTS descends from
Sharon Hunnicutt, *Phonological Rules for a Text-to-Speech System*, American Journal of Computational
Linguistics, microfiche 57 (1976); 82 pages (pp. 1-72 paper and appendix, then an AJCL author index). The PDF has an
OCR text layer (`pdftotext -layout` works) but the rule listing is multi-column typewriter text that OCR scrambles;
**read the rule pages (PDF pp. 64-72) as images** (Ghostscript, App. A). Unlike the MITalk book (§14.7), this paper
**prints the complete rule set**, so it is the best reference for decoding and naming LTS rules.

**Design (pp. 9-14):** three stages. Stage 1 finds suffixes right-to-left, longest first, with a
parts-of-speech compatibility test between adjacent suffixes, then prefixes left-to-right. An affix is never removed
if the remainder would lose its last consonant or vowel. Stage 2 converts consonants left-to-right over the root only.
Stage 3 converts prefixes, vowels/digraphs and suffixes over the whole word. Then come the stress rules. Rules are
ordered longest string first, then specific context before general. The last rule for a letter has no context
(the default).
Worked examples: `denominations`, `ptolemaic`, `table`, `caribou`, `scenario`, `subversion`, `science`.
Double letters (and `sc`) are kept as **double phonemes** (`KK`, `SS`, `^J^J`) so the vowel and stress rules
can see "orthographically double", then reduced to one later. The ROM does the same (`s H → sh sh`,
`p H → f f`; rules 365-366 reduce `l l`, `r r`).

**Stress rules (pp. 28-50, Figs. 4-5):** a modified Halle ordered set with 3 levels (1 = primary, 2 = secondary, 0).
- The **cyclic** rules run first on the root, then on root + one more suffix per cycle: Main Stress Rule,
  Stressed Syllable Rule, Alternating Stress Rule.
- The **non-cyclic** rules follow: Destressing, Compound Stress, Strong First Syllable, Cursory, Vowel Reduction.
- Main Stress Rule, algorithmic form:
  1. If the final syllable is the only one, or is long vowel + ≥ 1 consonant, stress it.
  2. Otherwise, if there are two syllables, or the penult ends in > 1 consonant or is long vowel + consonant, stress the penult.
  3. Otherwise, stress the antepenult.
- Suffixes that force stress onto a syllable:
  - final, retained: `EE EER ESCE ESQUE ETTE OON SELF`;
  - final, later reduced by the Compound Stress Rule: `FUL HOOD IFY IZE OID SHIP`;
  - penult: `ISM ARY ORY ERY ATORY ITION IFIC IC`.
- Stress-neutral suffixes skip their cycle: `ABLE ABLY AGE DOM ED EN ER ESQUE ES EST ETH FUL HOOD IBLE IBLY ILE ING ISH
  ISM IZE LESS LET LY MENT NESS OR RY SELF SHIP SOME TY URE` (this list is from the OCR text layer, pp. 36-37, not
  checked against the images).
- This matches the ROM's end-of-table stress rules (336-361) and their "special suffix" feature bit (§15.10) **[I]**.

**Evaluation (pp. 51-58):**
- Tested on 2,375 5-letter words: acceptable pronunciations = 2,135 preferred + 228 alternate + 12 verbal-pronunciation-of-noun/verb.
- The tests led to eight nested rule sets of **557 / 531 / 453 / 413 / 359 / 308 / 286 / 277** rules.
- Accuracy on 6-letter words for the 557-rule set: 73 % (Heritage), 69 % (Brown), 65 % (Stedman's). The 277-rule set scored 66 / 62 / 43 %.
- MIT's running system used the **413-rule** set.
- The ROM's 373 rules are **not** one of these sets. They are a DEC re-encoding using feature classes, with the suffix, prefix and stress stages all folded into one table.

**Appendix (pp. 61-72), the rule listing** (the numbers are the listing's own):
- Format: `[n] S1 > S2 <ctx> LEFT ← RIGHT`. A leading integer n means the rule is dropped from every set smaller than
  the n-th largest (unnumbered lines are in all eight sets).
- `<ctx>` is one of four markers giving the context types: `]` letter/letter, `[` phoneme left + letter right,
  `(` phoneme/phoneme, and one more that the scan does not make legible **[I]** (the page-62 legend is only partly
  readable).
- `←` marks where S1 sits. `$X` is a variable; in a left context it is written `X$`. `#` is a word boundary, `+` a
  suffix boundary, `=` a prefix boundary.
- **Letter variables** (p. 65, legible on the scan):

  | Variable | Members |
  |---|---|
  | `$W` | vowels incl. `+A…+Y`, `LE#`, `RE#` |
  | `$F` | front vowels `I E Y` (+`+I +E +Y`) |
  | `$^F` | `A O U` |
  | `$L` | consonant letters and `+C…+'S` |
  | `$X` | the palatalizing suffixes `+IENT +IOUS +ION +IAL +IAN +IENCE +EON IA#` |
  | `$^A` | voiceless `P T K F S C SH CH H` |
  | `$^I` | `+ED +ING +ER +EST +ES E# E+` |

  `$^L $^M $^N $^P $^O` are the special-stress suffix lists (`+IC +ITY +IFY …`, `+IVE +AL +OUS +AGE +LY`,
  `+IBLE … +ATIVE`, `+ATORY +ITION … +UOUS`, `+SIS +TY`).
- **Phoneme variables** (p. 65):

  | Variable | Members |
  |---|---|
  | `$C` | consonant phonemes |
  | `$G` | `P B F M` |
  | `$J` | voiceless |
  | `$K` | voiced |
  | `$^H` | `K G` |
  | `$M` | `D T` |
  | `$N` | sibilants |
  | `$Q` | vowels, plain or with a suffix boundary |
  | `$Z` | `# = B P M F V K G H T D N` |
  | `$I` | palato-alveolars |
  | `$A` | `^D ^Z V` |

  **These are the ancestors of the ROM's feature classes** (§15.10: the vowel and consonant classes and the boundary
  sets).
- Sections, in order: `%QCON` (`CHEM PSYCH TECH ARCH`), triple consonants (`CCH CHR CHL CHN NCH NGU NQU SCH THM TCH`),
  double consonants, single consonants, `%PREFIX PHO`, `/SUFFIX PHO`, digraph rules A-/E-/I-/O-/U-, double vowels,
  vowel rules, then the `%PREFIX` and `%SUFFIX` recognition lists.
- In the `%SUFFIX` list each suffix carries a type letter and an octal code.
  - Type letters: `F` regular; `E` a short vocalic suffix recognized only before inflectional/consonantal
    suffixes; `P` a non-vocalic suffix before which `y`→`i`.
  - Octal code: last digit = parts of speech that may *precede* it (1 adjectival, 2 verbal, 4 nominal, summed); the
    middle digit = its own parts of speech; a leading `1` = word-final only. Example: `F 166-ES`.
- **Phoneme table (p. 64):** a typewriter code with `↑` (printed `^`) and `"` marking the second and third
  vowel of a letter:

  | Code | IPA | Code | IPA | Code | IPA |
  |---|---|---|---|---|---|
  | `E` | i | `↑I` | ɪ | `A` | eɪ |
  | `↑E` | ɛ | `"A` | æ | `↑A` | a |
  | `↑O` | ɔ | `O` | o | `↑U` | ʊ |
  | `U` | u | `↑Y` | ʌ | `YR` | ɝ |
  | `I` | aɪ | `"O` | ɔɪ | `"U` | aʊ |
  | `"I` | ɨ | `Y` | ə | `YL` / `YR` | syllabic l / r |
  | `↑T` | θ | `↑D` | ð | `↑S` | ʃ |
  | `↑Z` | ʒ | `↑C` | tʃ | `↑J` | dʒ |
  | `↑G` | ŋ | `↑W` | ʍ | `J` | y |

**What carried over into the v1.8 ROM (compare the decoded listing, §15.10):**
- Same stage order. The ROM rules run: suffix stripping (0-35) → consonants (36-91) → suffix pronunciation
  (92-205) → prefixes (206-231) → digraphs (232-262) → single vowels (263-335) → stress, reduction and cleanup
  (336-372).
- Same double-phoneme trick.
- Many rules are recognizably the same:
  - `CHR → k r`, `SCH → s k`, `TCH → ch`, `DG → jh`, `PH → f`, `GH → ∅`, `NK → nx k`, `QU → k w` (`QU → k` before `E#`),
  - `WR → r` word-initially,
  - `C → k` before a non-front vowel or consonant, else `s`,
  - `S → z` between a vowel and a voiced segment,
  - `TI`/`SI`/`CI` + vowel palatalization (the ROM inserts a `?` marker, rules 61 and 109, where Hunnicutt uses `$X`).
- The prefix set is Hunnicutt's `%PREFIX` minus the rare ones (`ANTI CIRCUM HYPER MAL MICRO …`) plus
  `OVER` and `POST`.
- Differences worth knowing: `NCH → n sh` in the ROM (Hunnicutt `N^C`), and `WH → w` word-initially (Hunnicutt `HW`).
  The ROM also has no morph lexicon: the DECtalk dictionary plays that role (§7.2).

### 14.10 Klatt 1980 and Klattalk 1982 (`docs/Klatt_1980_CascadeParallelFormantSynthesizer/`, `docs/Klatt_1982_KlattalkTTS/`)
These two folders are **third-party study notes**, not the papers: `abstract.md` (verbatim abstract plus an
interpretation), `notes.md` (summary, equations, parameter tables), `claims.yaml`, `citations.md`,
`description.md`. They come from someone else's research collection (they mention a "Qlatt" project and
`[[…]]` links to papers that are not here). Treat numbers in them as secondary and check them against
`klsyn/parwav.c/.h` (the priority source for the synthesizer) or the ROM before relying on them.

**Klatt 1980, "Software for a Cascade/Parallel Formant Synthesizer"** (JASA 67, 971-995). This is the paper behind
`klsyn/`. The notes cover:
- the 39-parameter set, with defaults and ranges; it is the same list as MITalk Table 12-1 (§14.5);
- the resonator equations: `C = −exp(−2πBW·T)`, `B = 2exp(−πBW·T)·cos(2πF·T)`, `A = 1 − B − C`, and an
  anti-resonator with `A' = 1/A`, `B' = −B/A`, `C' = −C/A`;
- the source models: impulse → RGP/RGZ, a quasi-sinusoidal voicing path through RGS, and noise made by summing
  16 random numbers, 50 % amplitude-modulated when voiced;
- the AV/AVS update rule: they change only at a glottal pulse, while AF/AH are interpolated, and a jump of more
  than 50 dB in AF adds the `PLSTEP` burst step;
- cascade/parallel routing with alternating signs;
- the `NDBSCA` dB offsets (A1 −58 … AB −84, AV −72, AH −102, AF −72, AVS −44) and the `NDBCOR`
  formant-proximity boost table (+10 dB at 50 Hz … +1 dB at 500 Hz);
- Table II vowel targets and Table III consonant targets (the author's male voice);
- the fixed values `F0 < 40 Hz → 40`, `NFC = 4` for female voices, `NWS = 50` (5 ms).

**Klatt 1982, "The Klattalk Text-to-Speech Conversion System"** (ICASSP 1982). This is the direct ancestor of
DECtalk, since Klatt licensed Klattalk to DEC:
- ~500 LTS rules "from Hunnicutt [1980]" (Hunnicutt's QPSR review of the 1976 rules, §14.9);
- a ~1500-word exceptions dictionary, with affix stripping (`-ing`, plurals, past) to widen it;
- abbreviations are spoken as a word if they contain a vowel and spelled otherwise;
- a simple clause/phrase finder that uses commas, clause-introducing words and common verbs from the dictionary;
- a binary STRESS feature. Consonants before a stressed vowel become stressed if they are in the same morpheme and
  form a legal onset cluster;
- segmental rules for boundary cues;
- a **"hat-pattern" F0** (after Maeda 1974) in four steps:
  1. a declining baseline;
  2. a rise to a plateau on the first stressed syllable of a syntactic unit and a fall on its last;
  3. local stress bumps;
  4. **segmental perturbations** (vowel height, consonant voicing);
- Klatt's percentage duration rules with a minimum-duration floor (§14.3);
- a phonetic component that sets per-phone targets, then smoothing, voicing-onset delay for voiceless stops, and
  bursts;
- about 20 synthesizer parameters, updated per pitch period;
- the example phonemic output `DHAX 'OWLD M'AEN / S'AET IHN AX R'AAKRR.`, which uses DECtalk's own 2-char
  spelling and `'` stress.

**The papers themselves (`docs/klatt1980.pdf`, `docs/klatt1982.pdf`, added 2026-09-23).** Both have a usable text
layer (`pdftotext -layout`), but the two-column FORTRAN listing comes out interleaved, so read the listing pages as
images (Ghostscript, `-r200`; Appendix B starts on PDF p. 21 and runs to the end, p. 25; `PARCOE` is on p. 23). What the papers add to the notes:
- **Klatt 1980 Appendix B** has the full listings `HANDSY.FOR`, `PARCOE.FOR`, `COEWAV.FOR`, `SETABC.FOR`, `GETAMP.FOR`
  (D. H. Klatt, 8/1/78). Confirmed from the scan: `NDBSCA = −58 −65 −73 −78 −79 −80 −58 −84 −72 −102 −72 −44` and
  `NDBCOR = 10 … 1` (the notes are right).
- **Notes correction:** the plosive burst step fires when AF rises by **49 dB or more** (`IF (NNAF-NAFLAS.LT.49) GO TO
  151`, checked on the scan), not "more than 50 dB". `PLSTEP = GETAMP(G0 + NDBSCA(11) + 44)`.
- Details the notes omit: impulse amplitude scaled by F0 (`IMPULS = IMPULS·F0`); `NPULSN = SR/F0`; RGP bandwidth
  widened with F0 (`BGP·100/F0`, "more sinusoidal at high F0"); RGP gain fixed at `AGP = .007`; quasi-sinusoidal
  amplitude `10·GETAMP(AVS)`; parallel-branch level corrections from F1/F2 (`A2COR = (F1/500)²/(F2/1500)`,
  `A3COR = (F1/500)²·(F2/1500)²`) plus the `NDBCOR` proximity boosts; cascade formants run in descending order
  "to minimize transients"; output ×170, and the step decays by ×.995 per sample.
- `SETABC` carries the comments **"replace by R = EXPTAB(FB)"** and **"replace by B = COSTAB(F)"** for speed. The DSP
  does this for the cosine (`COS_TABLE`, §16.2).
- `GETAMP` converts dB with two small tables (`STABLE`, powers of 2 per 6 dB; `DTABLE`, 1.8 … 0.555 within 6 dB). The
  DSP uses parwav's later `amptable[88]` instead, and parwav's natural glottal source (`B0`) instead of the 1980
  impulse → RGP/RGZ source. **The DSP program therefore follows the 1982-88 `parwav.c` generation, not the 1980
  FORTRAN** (§16.6). The 1980 `A2COR`/`A3COR` corrections are not in `parwav.c` and have not been seen in the DSP.
- **Klatt 1982 (ICASSP, pp. 1589-1592)** matches the notes. Points worth keeping: the phonetic component produces "a
  set of 20 synthesizer control parameters every pitch period", and the synthesizer module converts them "into gains
  and difference equation constants to control a special-purpose synthesizer chip", a "somewhat simplified" version
  of the 1980 synthesizer. That matches the split found in the DSP: the 68000 sends dB and Hz values plus a pitch
  period (§16.5), and the TMS32010 computes gains and coefficients. The stress rules are "expressed in the same
  formalism as the letter-to-phoneme rules", as in the ROM (rules 336-361, §15.10). Klattalk ran on a PDP-11/60
  plus Lincoln Labs' LDSP; the paper says a microcomputer version was coming through a DEC licence. Reference [3] is
  Hunnicutt 1980, "Grapheme-to-Phoneme Rules: A Review", STL-QPSR 2-3/1980; the hat pattern cites Maeda 1974 (MIT
  QPR 114).

**How this ties to the v1.8 ROM (checked 2026-09-23):**
- **F0 is Klattalk's hat pattern, not MITalk's O'Shaughnessy algorithm.** The ROM's per-frame F0 routine
  `pht0draw` `0xc722` is dapi `ph_drwt01.c` `pht0draw()` [V].
  - It adds step 4 from `f0segtars` `0x169c4`, byte-identical to dapi (§15.14).
  - dapi's `ph_inton1.c` (hat rise/fall, fixes by "DK" 1985) is the counterpart of the not-yet-located ROM routine
    that writes the F0 commands `pht0draw` consumes (`0x817b8[]`/`0x81754[]`).
  - The voice parameters `bf`/`ef` (baseline fall, §8.1) are step 1.
  - Treat §14.4's O'Shaughnessy constants as background only.
- **Allophones:** Klattalk's segmental rules (glottal-stop insertion, postvocalic /r/ and /l/, glottalized final /t/)
  explain the ROM codes that plain text never names: `q` (53), `rx` (29), `lx` (30), `tx` (52) (§8.3) [I, via dapi].
- **Dictionary plus affix stripping** = `lookup_word_with_suffix_stripping` (§15.9). The ROM LTS has 373 rules, not
  ~500, because DEC folded the rules into feature-class form and added stress rules (§14.9, §15.10).
- **Synthesizer constants live in the DSP, not the 68000.** Klatt's 88-entry dB→linear `amptable` (`klsyn/parwav.h`
  `amptable[88]`: 13 zeros, then 6, 7, 8 … 32767) sits **verbatim in the TMS32010 ROM** at word addresses
  `0x8a-0xe1` (`merges/dsp_v1.8_A.bin`, big-endian words; found by a raw-byte search first; the DSP is now
  disassembled, §16).
  - The 68000 ROM has neither `amptable` nor the `NDBSCA`/`NDBCOR` tables (searched as bytes and as shorts).
  - It also does not have Klatt's Table II/III formant values as plain shorts (e.g. `310 2020 2960`,
    `310 1060 1380`, FNP/FNZ `270 450`).
  - So the 68000 sends **dB amplitudes and Hz values** in its DSP frames, and the DSP does the `DBtoLIN` and
    resonator-coefficient math. This was **confirmed by the DSP disassembly** (§16.4).
- **Target tables** for synthesis-by-rule (dapi `maltar`, `femtar`, `maldip`, `malamp`) are *not* byte-identical in
  the ROM. `phsettar` `0xaa08` is where to find them (§13).

---

## 15. Decompilation evidence trail (folded in from the former `FINDINGS.md`)

Per-function evidence and behavior writeups from the Ghidra sessions, grouped by subsystem. Names and
addresses are in §10 (the inventory); host-facing behavior is in §5-§8. Statements that the old
`FINDINGS.md`/`BRIEF.md` got wrong have been **corrected in place here and tagged `[corrected]`**; §12 keeps
the list of what changed. Ghidra plate comments carry the same evidence per function. (~120 of 291
functions named.)

### 15.1 Sources cross-referenced; ROM assembly
- **`native/`** — from-scratch emulator (Musashi 68000 + TMS32010 + SCN2681 DUART); ground truth for the
  memory map. **`dapi/`** — full C source of a much later (Windows-era) x86 DECtalk; shares algorithms,
  constants and often literal variable/function names with this firmware. **`klsyn/parwav.c` + `parwav.h`** —
  Klatt's own 1982-88 reference formant synthesizer; documents the per-sample resonator math that runs on the
  TMS32010 (not reachable from this ROM), and its parameter/table layout (`spdef_name[]`, `par_name[]`)
  confirmed the 28-field voice-definition struct.
- The 16 raw chip dumps (`E1…E8`, `E15…E22`) byte-interleave in pairs (`En` = high byte, `E(n+14)` = low)
  into eight 32 KB banks. Bank 0 (chips `E8`/`E22`) was independently reconstructed by brute-forcing the
  pairing against a 68000 reset-vector plausibility check, and matched `merges/dectalk_v1.8_full.bin` byte
  for byte — so that merged file is the complete, correct 256 KB image loaded into Ghidra.

### 15.2 RTOS evidence
- **`task_create`** `0x116e` — thin `TRAP #1` thunk (`LEA` args into A0, handler into A1, `TRAP #1`).
  **`trap1_dispatch_via_a1`** `0x1106` — generic trap entry that calls through whatever A1 points to (reused
  by other traps). **`task_create_impl`** `0x117c` — allocates a TCB+stack block, fills priority/entry/name,
  links into the global task list (head/tail `0x80000`/`0x80008`), fabricates an initial stack frame ending in
  return address `0x11fc` (the "entry function returned" handler), stores the fabricated SP at TCB+`0xb4`.
- **`scheduler_context_switch`** `0x112a` — the context-switch ISR: compares current task (`0x80004`) to the
  ready-queue head (`0x80000`); if different, `MOVEM.L`-saves `D2-D7/A2-A6/USP` on the supervisor stack, then
  restores the same set from the incoming TCB at **TCB+`0x9C…0xCC`** and `RTE`s. So A4 etc. are genuine
  per-task state, not runtime-patched or global.
- `setup_mode_main_loop` is **not** a boot-table task, but it *is* a task: it is **`main_task`**, the first task,
  created by `boot_init` (§12.35) **[corrected 2026-09-26]**. The old line said `host_task_main` calls it; no
  static call to `0x2700` exists in any encoding.
- **`host_timeout_task_main`** `0xf070` — the lowest-priority task (−100). Sleeps in `queue_send(100,0,0)`
  periods (the RTOS timed-wait idiom; the tick unit is unconfirmed) and counts idle periods since the last
  byte arrived (`read_host_byte` resets the shared counter `DAT_00081d72`); an inhibit flag (`DAT_00080160`,
  probably "speaking") suppresses the count. After **5** periods with no host activity it logs `"Host timeout"`
  and writes control byte **`0x0B` = CTRL-K, the manual's clause-flush character** into the current output
  stream — the same `0x0B` that `host_task_main`'s SUB (`0x1A`) handling emits. **[corrected]** The old text
  called `0x0B` an "attention/interrupt marker"; this task is the manual's **5-second speech timeout**
  ("speak buffered text as if a comma had been sent").
- **`stop_task_main`** `0xfa7a` — does nothing but block in `wait_for_stop_signal` (a `TRAP #1` thunk using the
  same A1 convention as `task_create`) until a stop request arrives — almost certainly `DT_STOP`; the
  signaling path was not traced. On waking it calls **`emit_sync_marker`** `0x3d56` tagged `"h_stop"` and loops.
- **`emit_sync_marker`** is a small shared utility: it inserts the sync marker `0x1A` into the output stream,
  wakes waiters via **`notify_sync_point`** `0x88c` (another `TRAP #1` thunk) and logs `"dtsync: %s"` with a
  caller-supplied tag. It is the primitive behind `DT_SYNC` and is reused by `dcs_command_dispatch`,
  `dectst_self_test`, `phtask_main`, `main_task` (on exit from SETUP) and `stop_task_main`.
  **`maybe_flush_stream`** `0x11ec4` flushes only once the output buffer passes a threshold; used by
  `host_timeout_task_main` and `emit_sync_marker` after inserting a single control byte.
- Boot (`reset_entry` → crt0) and console/`kprintf` chain: see §3.1 and §3.3 (unchanged).

### 15.3 SETUP mode evidence
`main_task` `0x2700` (formerly `setup_mode_main_loop`; its Ghidra entry sat on the zero padding at `0x26c8` until
2026-09-26. Earlier, Ghidra auto-analysis had truncated the function 5 bytes short; needed
`clear_flow_and_repair` — re-apply name/comment if it resets). It checks the DUART jumper bit; if set it
prints the boot banner (`"DECtalk version %s is running."`, `"one point eight"`, NVR-fault warning). Loop:
`read_line_with_prompt` `0x2a8c` (prompt `SETUP>`, spoken "setup.") → `str_copy` `0x11e00` (plain strcpy) →
`tokenize_words` `0xfd8c` → `tdparse` `0xfe16` (self-identifies `"Bug: tdparse"`); SETUP ends when `tdparse`
returns 0 (EXIT), and a new local session starts. The command tree, the flags and the help are decoded, and the
task is C (§15.37; the old "opcode range 0xc00-0x1200 is host-serial-port config" was wrong, §12.56).
`nvram_save_settings` `0xf79c` packs current settings into a **64-word record** written to the `LED_NVRAM`
region `0x94000` (matches `native/dtc01.c`); the record, and why the emulator's boot says "NVR fault": §15.33.

### 15.4 DTMF / telephone evidence
`phtask_main` `0xf128` (formerly `phtask_main_loop`; self-identifies `"Bug: phtask c=%d."`) reads codes with
`dev_getc` `0xc00` from `phone_dev` `0x80552`: `0x81` ring, `0x82` **off hook / answered**, `0x84-0x86` reset,
hang-up and ringing stopped (the full account is §15.34). On answer it
runs `dtmf_diagnostic_menu` `0x116e2` or drops into raw digit-forwarding, translating DTMF codes 0-15 through
`"D1234567890*#ABC"` (`0x1899a`) and relaying via `host_line_putc` `0xef90`; inactivity timeout →
`"dtphon (timeout)"` + `send_dcs_reply` `0xef00` (the phone status reply, §15.23). `dtmf_diagnostic_menu` is a *complete* configuration/test
menu spoken and controlled entirely over the phone line (the DTMF equivalent of SETUP): welcome banner,
`*` = factory reset (`settings_reset(2,1)`), `#` = numbered self-test menu with pass-count entry, driving
`dectst_self_test` `0xe984` (also the source of the power-on speech banner "Hello. This is DECtalk…").
The **host-triggered** dial/answer/hangup implementation is `dt_phone_command` `0xe5f6` (formerly given as
`FUN_0000eecc`, §12.51), reached from `dcs_command_dispatch` (ROM name `DT_PHONE_HOME` = the manual's `DT_PHONE`,
P2 60); read in §15.32.

### 15.5 Serial / interrupt evidence
`duart_isr` `0x16fe` (IRQ 6) → `duart_isr_service` `0x1710` reads the DUART ISR (`0x9800B`): bit 1 (RxRDYA) →
`duart_rx_char` `0x1870` on the host device `0x8011e`, bit 5 (RxRDYB) → `duart_rx_char` on the console device `0x80328`
(label `g_console_queue`), bits 0/4 (TxRDY) → `duart_tx_char` `0x17f8`, bit 3 → the counter tick (`clock_tick`).
**[corrected twice]** Older notes called `0x1870` "TX drain"; it is the receive side: receive errors become `0x1A`
(the SUB substitution of §5.6, [V]), the console handles XON/XOFF from the terminal, and the host device sends XOFF
when more than 64 bytes wait (§15.23). Phone-line status is
**not** on the DUART: it comes through the telephone interface's registers `0x9c004`/`0x9c006` (SPC/TLC region) and
their level-4 interrupt (§15.34; the old account here, with `phone_line_status_from_dsp` posting via
`list_unlink_node`, was wrong, §12.53). `dsp_command_queue_isr` `0x12258` (IRQ 5 path) drains
a command queue (head `0x82256`) into SPC registers `0x9c000/0x9c002` — the transport for `klsyn`'s
voice/parameter data.

### 15.6 Host link escape-sequence parser (`host_task_main`)
A genuine VT100/ANSI-compatible parser on the host RS-232 link; `dcs_command_dispatch`/`csi_command_dispatch`
implement DECtalk's private `DT_*` commands on top of it.
- **`read_host_char_collect_csi`** `0x11290` — the core "next logical character" primitive shared by
  `host_task_main`, `handle_single_shift` and `consume_control_string`. It collects CSI/DCS parameters (up to
  16 numeric params, a leading private marker `< = > ?`, up to 2 intermediates `0x20-0x2f`) into the shared
  parser-state struct at `0x81f2c` and returns the lead-in code once a final byte (`0x40-0x7e`) arrives. The
  same struct is read back by `dispatch_esc_command` (`0x81f2e/0x81f52/0x81f53`) and by
  **`send_control_sequence`** `0x114e0` when building replies (field layout in the plate comment).
- **`read_host_byte`** `0xefc8` — raw byte source: pulls from the host queue, resets the idle timer, cancels a
  pending DTMF dial-tone/critical-section state on any non-XON traffic (real host activity interrupts a dial).
- **`dispatch_esc_command`** `0xdad8` — 7-bit `ESC <final>`/`ESC <intermediate> <final>`: G0-G3 designation
  (`ESC ( ) * +`), S7C1T/S8C1T, DECTC1/DECAC1, LS2/LS3/LS1R/LS2R/LS3R, and RIS (`ESC c` → `settings_reset(0,0)`,
  the same entry the DTMF menu's factory reset uses).
- **`handle_single_shift`** `0xd83e` (SS2/SS3 for C1 `0x8e/0x8f`); **`consume_control_string`** `0xd9f4` reads and
  discards OSC/PM/APC up to ST or BEL (DECtalk's own commands ride on DCS).
- **`output_graphic_char`** `0xd94c` / **`is_graphic_char`** `0xd906` — final emission through the active
  charset: ASCII passes, DEC Supplemental is remapped via the table at `0x1821e`, and **DEC Special Graphics
  (line drawing) is silently dropped**.
- **`stream_putc`** `0xd99e` / **`flush_stream_buffer`** `0x11efe` — unformatted putc/flush counterparts of
  `fprintf_stream` `0x11c88` on the current output stream (`0x81f1e`). **`send_escape_response`** `0xef5c` sends
  a reply via `send_control_sequence` using `host_line_putc` as a generic byte sink.
- **Loggers** `0xd21e` / `0xd1ca` / `0xd1f4`: three independently flag-gated (`DAT_000822ca` bits `0x40` /
  `0x20` / `0x80`) wrappers around `vformat_string`. **[corrected]** `DAT_000822ca` is the manual's **`DT_LOG`**
  word, so these are the **LOG_TRACE**, **LOG_ERROR** and **debug (`0x80`)** loggers (pending renames
  `log_trace`, `log_error`, `log_debug`) — not "recognized escape / malformed sequence / charset shift".
  `log_charset_shift` also prints `"Host timeout"` and `"dtsync: %s"`, which no charset-specific function would.
- **`uint_to_decimal_string`** `0x1167c` — itoa via subtraction against a power-of-ten table
  (`0x19862-0x1987e`), used by `send_control_sequence` for outgoing Ps parameters.
- **`0x9a`** (old VT100 DECID, before `CSI c`) is handled exactly like a primary DA request: `host_task_main`
  replies with the same `"OLDID"`-format string via `send_escape_response`.

### 15.7 `klsyn` front end, voice table, `parse_bracket_command`
The 68000 side does **not** run `parwav.c`'s resonator math (that is on the DSP); it validates/dispatches
parameter and voice-select records first. `klsyn_task_main` `0x3c20` consumes work items from `0x807a2` (the
queue `dttask_main`/`newclause` also post to). `parse_phoneme_param_stream` `0x7788` decodes phoneme/control
records — `'d'` select voice, `'e'` rate scalar, `'i'/'j'` duration scaling; source of `"Illegal voice %d\n"` and
`"Bad pseudo-phoneme %d\n"`. `load_voice_definition` `0x8150` copies 28 words from one of 9 voice slots (table
`0x16146`) into the active speaker block `0x81c44`. Voice records and values: §8.1 (labels are verbatim ROM
strings at `0x15c13-0x15e99`); only 8 distinct ROM records exist (Frank/Dennis share `0x16066`); no Wendy.
`parse_bracket_command` `0x7d1e` is a *second*, separate command interpreter for in-text `[:…]` commands
(distinct from `tdparse`, which only runs in SETUP). Its table (`0x15a34`, 31 × 14-byte records, decoded) names
the 28 speaker-block fields index-for-index — record layout and the parameter table are in §8.1
(record = `{name_ptr:4, kind:1, pad:1, min:2, max:2, desc_ptr:4}`; the old text said `kind:2`).
- **`list`/`listall`** (records 28/29) → **`print_voice_param_table`** `0x8072`, format
  `"%-3s %4d %-2s (%4d .. %4d) %s\n"`: prints mnemonic, value, units, range, description to the console. The
  per-record bit tested is `kind & 8` (§12.3): `list` shows 15 rows, `listall` 28 (confirmed, §15.22).
- **`save`** (30) → **`save_voice_params`** `0x81a8` copies the 28 active values into the RAM "Val" slot `0x81c7c`.
- **`sex` side effect [corrected]:** when the value actually changes, `parse_bracket_command` adds ∓18 to **head
  size** and multiplies **F4/F5** by ≈1.218 / ≈0.821 (`frac_mul_q12` against the tables at `0x15bf6`/`0x15bfa`).
  It does **not** rescale average pitch or pitch range (an earlier misreading of the block index). The `"fm"`
  bytes at `0x15bf4` are just the `m`/`f` parse characters (§8.1).

### 15.8 Duration engine evidence
(`phtiming` is rebuilt in C and verified word for word: §15.19.)
**`phtiming`** `0x8ebe` *is* `phtiming()` of `dapi/src/PH/p_us_tim.c` (Klatt's duration rules) — an exact match
down to variable names (`phocur`/`feacur`/`struccur`/`durinh`/`durmin`/`deldur`/`prcnt`) and surviving diagnostic
strings. Each numbered "Rule N" in the dapi source corresponds to a call to **`prdurs_stub`** `0xa77a` — a literal
no-op here (the debug printer `prdurs()` is `EABDEBUG`-only), but the call sites mark rule boundaries.
Arrays (dapi names): `allophons[]` `0x80f82` and `allofeats[]` `0x812a2` (input phone sequence + feature bits);
`featb[]` `0x161fe`, `inhdr[]` `0x162d2`, `mindur[]` `0x16342` (phone→feature/duration tables — **56 big-endian
shorts, 6.4 ms frames**, §9.1); `allodurs[]` `0x81112` — the **output**, also read by `parse_phoneme_param_stream`.
Fixed-point helpers: `frac_mul_q14` `0x1d80a`, `frac_mul_q12` `0x1d862`, `long_multiply` `0x1d73a` (full 32-bit
signed multiply, matching the header's MS-to-frames overflow note). **`kl3_push_event`** `0xa6e2` is dapi's `make_f0_command`
(self-identified by `"kl3_c: totev > MAXEV, %d"`); it builds the F0 command list for `pht0draw`, not DSP events
[corrected 2026-09-23, §12.33]. Its callers sit in the tail of `phtiming` (`0xa00c-0xa544`), which is therefore
also the hat-pattern intonation pass.

### 15.9 Dictionary / word lookup evidence
Found via the `DT_DICT` string chain. The lookup machinery is called directly from the host command dispatcher
and (via `lookup_word_with_suffix_stripping`) from the clause pipeline.
- **`dcs_command_dispatch`** `0xe152` (reached from `host_task_main`'s C1 handling) — every command
  self-identifies via a bug string: `DT_PHOTEXT`, `DT_STOP`, `DT_SYNC`, `DT_SPEAK`, `DT_INDEX_TEXT/REPLY/QUERY`,
  `DT_DICT` (→ `parse_dict_entry_command`), `DT_PHONE_HOME` (`"tone dial %s"`, `"pulse dial %s"`,
  `"answer in %d ring"`, `"hangup"`, `"phone on hook"`), `DT_MODE`, `DT_LOG`, `DT_TERMINAL`. `csi_command_dispatch`
  `0xddd8` is the CSI sibling (DA, DSR, DECSTR, DECNVR, DECTST). `parse_dict_entry_command` `0xe8ec` parses
  `"name definition"` and calls `dict_hash_insert_or_delete`.
- **User dictionary:** `dict_hash_insert_or_delete` `0xfc36`, `dict_hash_lookup` `0xfb56`, `dict_hash_clear_all`
  `0xfb10` — a 32-bucket hash (`0x820a0`, **case-folded additive character-sum hash**), singly-linked nodes
  `{length byte, word, NUL, pronunciation}`; an empty pronunciation deletes; cleared on factory reset (from
  `settings_reset`).
- **`lookup_word`** `0x68fc` — master lookup: user dictionary first, then a **trie** (the built-in
  dictionary): per-first-character tables of `{sibling char-list, child-offset-list, pronunciation-data-base}`
  [corrected: a plain trie, not a DAWG, and not dapi's format; decoded in §15.25].
- **`lookup_word_with_suffix_stripping`** `0x65c8` (cf. `dapi/src/LTS/l_us_suf.c`, `ls_suff.c`): tries the whole
  word, then on a miss strips `-ed/-ing/-s/-es/-ly` (with `y→i` and doubled-final-consonant undo for
  "carried"/"hopped"), guarded by **`stem_has_vowel`** `0x65ee`, and retries on the stem.

### 15.10 Letter-to-sound (LTS) rule engine and rule-table byte layout
- **`pronounce_word`** `0x5a90` — top level, called per word-token from `pronounce_word_or_abbrev`. Tokenizes on
  punctuation/hyphens/quotes, then per piece tries **(1) `pronounce_dictionary_word`, (2) `lts_rule_engine`,
  (3) `spell_char` per character**. Hyphenated compounds go piece by piece, with symbol `0x38` between the pieces
  [corrected: the "a" special case is `spell_char('a')` before a clause mark; full description in §15.26].
- **`pronounce_dictionary_word`** `0x62d8` — wraps `lookup_word_with_suffix_stripping`; on a hit it emits the
  phoneme string via `clause_putsym`, appending a suffix-specific allomorph tail (voicing-dependent `/s/`-`/z/`-
  `/ihz/`, `/t/`-`/d/`-`/ihd/`, cf. `l_us_suf.c`). **Returns 0 on a miss; it does not itself fall through to the
  rule engine** — that happens in `pronounce_word`.
- **`lts_rule_engine`** `0x6a64` — the algorithmic fallback: builds a linked list of letter nodes and applies the
  compiled rule table until the word is fully converted. The rule record matches the 4-field entry documented at
  the top of `dapi/src/LTS/ls_rule.c` (grapheme / replacement / left-env / right-env), **but the ROM's control
  flow differs from dapi's `ls_rule_lts()`/`ls_rule_rule_match()`**: the ROM tries **every rule, in order,
  against every position of the word's letter list**, rather than picking one rule per cursor position.
  **`lts_env_match`** `0x6c0c` tests a left or right context (direction selectable) including character-class
  tests via the grapheme-feature tables `0x13c04`/`0x13c80` (≈ `ls_rule_env_match()`). **`lts_rule_apply`**
  `0x6ad4` consumes the matched letters and splices in the replacement phonemes in place (one compact list holds
  both letters and phonemes, unlike dapi's separate `GRAPH[]` and `PHONE` list).

**Rule-table byte layout — fully decoded [V]** (fourth pass, 2026-09-23: `lts_env_match` / `lts_rule_apply`
disassembled with capstone while Ghidra was down, then every record parsed with no leftover bytes; the
rules read as a Hunnicutt-1976-style rule set, §14.9):
1. `0x156e2` is **not** a per-letter jump table. It is a flat sequential table: `lts_rule_engine` tries rule 0, 1,
   2 … up to `_DAT_000159d2 − 1`, each at every position of the list (loop at `0x6c5c-0x6d90`, rule index in
   `0x80f60`).
2. The rule stream's true origin is **`0x13e10`**, not the auto-labeled `0x13e11` (Ghidra labeled `DAT_00013e11…14`
   one byte late; the engine's running offset is seeded at −1 and the code reads the header at `0x13e10+off+1`).
3. **Delta table** `0x156e2`: 374 big-endian shorts. `delta[i]` = byte length of record `i-1`, and a cumulative
   sum seeded at −1 gives each record's offset. The last short (`0x000a`) is the length of rule 372. It is followed
   by the longword `0x000018d2` = the total stream length (`0x13e10 + 0x18d2 = 0x156e2`). Those are the former
   "6 unexplained bytes".
4. **Rule count** at `0x159d2` = `0x0175` = **373**. `0x159d4` starts unrelated `"Illegal …"` strings.
5. **Rule record:**
   ```
   +0  uint8 match_len   items consumed and replaced (0 = pure insertion)
   +1  uint8 right_len   number of items in the RIGHT pattern, which INCLUDES the match_len matched items
   +2  uint8 left_len    number of items in the LEFT pattern (matched backwards, nearest item first)
   +3  right pattern items, then left pattern items, then
       uint8 rep_count   followed by rep_count replacement items
   ```
   **Item encoding** (same in patterns and replacements):
   - `b < 0x80` → one literal **LTS code** (same numbering as the name table `0x19704`: phonemes 0-55,
     tokens, letters `A E I O U H X C J Q` = 70-79, `:++` 83, `:--` 84).
   - `b ≥ 0x80` → `n = (int8)b + 0x1C`:
     - **n < 10**: a *test group*. An optional literal byte `c | 0x80` (code c must match) is followed by n pairs
       `(class, polarity)`. A pair is true when `(feat(node) & mask[class]) != 0` equals `polarity != 0`.
       `mask[]` = `0x13c04` (31 longwords); `feat(node)` is the node's longword, initialized from `0x13c80[code]`.
       `e5` = 1 pair, `e6` = 2, `e7` = 3.
     - **n ≥ 10**: a **repeat prefix**: the next item may occur up to n − 10 times (`ef` = optional, `f8` = up to 10,
       Hunnicutt's `C0`). Matched greedily, **without** backtracking (§12.43).
   - In a **replacement**, a literal writes that code with its default features. A test group sets or clears the
     listed feature bits instead: on its own it modifies the matched node in place; with a literal it creates a
     new node. Examples: `{=:-- !MB,f18}` inserts a suffix boundary with bit 0x18 set, and `{f14,!f15}` marks a
     vowel.
6. **Letter input:** `lts_rule_engine` maps each ASCII char through `0x13b48` (§9). Sixteen letters become their
   phoneme code at once (`b d f g k l m n p r s t v w y z`). The other ten become the letter codes 70-79. The word is
   wrapped in space (62) nodes (the word boundary `#`). After the rules run, the list is emitted with
   `clause_putsym` (`0x6d94-0x6e08`). The leading space is skipped, `*` (61) becomes a space, and **an item equal to the
   previous one is not emitted again**. That is how the doubled phonemes (`sh sh`, `ch ch`, `f f`, …) collapse to one.

**Feature classes** (`0x13c04`; members computed from `0x13c80`; names are ours):

| Class | Members | Hunnicutt |
|---|---|---|
| `0x1c` | all vowels + syllabics `el em en` + letters `A E I O U` | vowel `V` |
| `0x1d` | all consonant phonemes + letters `H X C J Q` | consonant `C` / `$L` |
| `0x11` | every phoneme and letter (not boundaries) | "any segment" |
| `0x12` | silence, `" *`, space, punctuation, `:--`, `:==`, `@`, `~~`, `:::` | word/suffix boundary `$B` (`#`,`+`) |
| `0x13` | same but `:++` instead of `:--` | word/prefix boundary `$D` (`#`,`=`) |
| `0x1b` / `0x1a` | `:++ :--` / plus `@ ~~` | any morph boundary |
| `0x04` | front `iy ih ey ix ir y` + letters `E I` | `$F` (front) |
| `0x0c` | voiced (vowels, sonorants, `v dh z zh b d g jh`, vowel letters) | `$K` |
| `0x0f` / `0x17` | fricatives+affricates / sibilants `s z sh zh ch jh` | `$N`-like |
| `0x16` | liquids `r l rx lx el` | `$H` |
| `0x0b` | nasals `m n nx em en` | |
| `0x03` | long (tense) vowels `iy ey ay aw ow oy uw rr yu ir er or ur` | `[+long]` |
| `0x10` | diphthongs `ay aw oy` | |
| `0x05 0x06 0x07 0x08 0x09 0x0a 0x0d 0x0e 0x1e` | vowel-height/backness and consonant place/manner sets (see the dump) | |
| `0x14 0x15 0x18 0x19` | **no static members**: set by rules | stress 1, stress 2, inflectional-suffix mark, special-stress-suffix mark **[I]** |

**Rule sections** (rule numbers; `:--` = `+`, `:++` = `=`):

| Rules | Stage | Examples (ROM notation → Hunnicutt) |
|---|---|---|
| 0-35 | suffix stripping (stage 1): `-ed -ing -ingly -ly -ally -ily -ies -es -hood -ness -less -ment -ful -ship -er -est 's s'` and `e`-restoration/deletion before `+` | 3: `I` + `ED#` → `y +`; 8: `E` → ∅ / `ng _ +ing` |
| 36-91 | consonants (stage 2) | 36 `C H r → k r` (CHR), 38 `s C H → s k`, 39 `t C H → ch ch`, 40 `s H → sh sh`, 41/42 `t H → dh` before `E#`, else `th`, 44 `d g → jh jh`, 48 `n k → nx k`, 55 `p H → f f`, 56-58 `Q U → k` / `k w`, 59 `w H → w` (initial), 60 `w r → r r`, 63-65/86 `C → k` / `s`, 71 `H → hx` before a vowel, 73-75 syllabic `l`, 76 initial `p` silent before a consonant (`psych`), 82-84 `s → z` |
| 92-205 | suffix pronunciation (stage 3) | 95 `+IFY`, 106 `+ABILITY`, 108 `+ITY`, 136 `+ATE → ey t`, 146/147 `+ED → d` / `t`, 173 `+IFICATION`, 177 `+ION → iy ax n` |
| 206-231 | prefixes: `be bi com con co de dis em en extra ex im inter in mis non per pre pro re trans under un over post` (`→ … :++`) | Hunnicutt's `%PREFIX PHO` minus rare ones, plus `over`, `post` |
| 232-262 | vowel digraphs `AI AU AY AW EAR EA EI EU EW EY IA IE OA OE OI OY OU OW UE UI EE OO AL` | "DIGRAPH RULES" |
| 263-335 | single vowels `A I Y O U E`, each ending in its context-free default (`A → ae`, `I → ih`, `Y → ih`, `O → aa`, `U → ah`, `E → eh`); silent final `e` | "VOWEL RULES" |
| 336-361 | stress placement (sets bits `0x14`/`0x15`) and vowel reduction (360/361: unstressed short vowel → `ix` front / `ax` other) | Main/Stressed-Syllable/Alternating stress, Vowel Reduction |
| 362-372 | cleanup: insert `'` before a primary-stressed vowel; stressed `ax → ah`, `ix → ih`; `l l → l`, `r r → r`; `ix r / ax r / ah r → rr`; unstressed `ax l / ix l → el`; `r hx → r` | double-phoneme reduction, `ah rr → er` |

**Decoder** (regenerates the full 373-rule listing in a minute; run it rather than trusting memory):
```python
import struct
b = open('C:/Users/abart/Desktop/DTC-01/merges/dectalk_v1.8_full.bin', 'rb').read()
names, p = [], 0x19704
for i in range(100):
    e = b.index(b'\0', p); names.append(b[p:e].decode('latin1') or '?'); p = e + 1
names[0x3e] = '#'
def item(r, i):
    x = r[i]
    if x < 0x80: return names[x], i + 1
    n = x - 256 + 0x1c
    if n >= 10: return '*%d' % (n - 10), i + 1           # repeat prefix, applies to the next item
    i += 1; lit = ''
    if r[i] >= 0x80: lit = '=' + names[r[i] - 0x80] + ' '; i += 1
    prs = []
    for _ in range(n): prs.append(('' if r[i+1] else '!') + 'c%02x' % r[i]); i += 2
    return '{' + lit + ','.join(prs) + '}', i
def items(r, i, cnt):
    out = []
    while len([t for t in out if not t.startswith('*')]) < cnt:
        t, i = item(r, i); out.append(t)
    return out, i
n = struct.unpack_from('>H', b, 0x159d2)[0]; off = -1; st = []
for k in range(n): off += struct.unpack_from('>h', b, 0x156e2 + 2*k)[0]; st.append(0x13e11 + off)
st.append(0x156e2)
for k in range(n):
    r = b[st[k]:st[k+1]]; m, rl, ll = r[0], r[1], r[2]
    right, j = items(r, 3, rl); left, j = items(r, j, ll); rep, _ = items(r, j + 1, r[j])
    print(k, ' '.join(right[:m]) or '(ins)', '>', ' '.join(rep) or '(del)',
          '/', ' '.join(reversed(left)), '_', ' '.join(right[m:]))
```
(`cNN` = feature class NN from the table above; a leading `!` = must NOT have it.)

### 15.11 Vocabulary output: `out`/`outn`, `pronounce_word_or_abbrev`
A unifying architectural finding: the number, date and duration-allomorph code reuse the *same*
dictionary+LTS pipeline as ordinary text.
- **`out`** `0x556e` / **`outn`** `0x5614` are **word tokenizers, not phoneme emitters**: they buffer characters
  and, on each space (or end of string / explicit length), flush the word to **`pronounce_word_or_abbrev`**.
  That is why the ROM stores `"hundred"`, `"dollars"`, month names, `"half"/"halves"`, `"percent"` … as plain
  ASCII (spoken on the fly like typed text). Confirmed by their diagnostics `"Illegal character %d in out()"` and
  `"Long word in number output: %s"` (originally built for the number engine, later reused generally).
- **`pronounce_word_or_abbrev`** `0x7192` — per-word dispatcher: thin pass-through to `pronounce_word`, plus the
  **"St."/"Dr." rule**. A standalone `St.`/`Dr.` (matched case-insensitively by **`str_match_fold`** `0x4e80`) is
  deferred one word to inspect the next word's first letters; if that word is capitalized it re-feeds the literal
  `"Dr."`/`"St."` (the built-in dictionary maps these to Doctor/Saint), otherwise the default `"drive"`/`"street"`
  (ASCII strings at `0x13be6-0x13bf4`). A closing mark after it (`"Dr.,"`) skips the deferral and gives
  drive/street [corrected: glued-on text such as `"St.Louis"` does not match at all, §12.45]. **[corrected]** the old "plausible, not independently confirmed" is now **confirmed by the
  manual** (§7.1).
- **`emit_punctuation_symbol`** `0x60aa` maps a punctuation char to a `clause_putsym` pause/boundary code and calls
  `newclause()` on sentence-ending punctuation (`.`, `!`, `?`; a `?` is sent as `.` after a clause that starts with a
  wh- word [corrected: the flag is `wh_question`, not an abbreviation count, §15.26]); `) ] }` soften to a comma pause. Used by `out`/`outn` and by `pronounce_word`'s
  hyphen/glued-remainder loops.

### 15.12 Number-to-words engine
**[corrected 2026-09-27: superseded by §15.27, which rebuilds all of this in C; see §12.46.]** Every token goes to
`token_dispatch`; the number words are then spoken through `out()` and `pronounce_word` like any other word.
Numbers are recognized at the clause-tokenizer level as a token class distinct from words — they never enter
`pronounce_word`/dictionary/LTS [corrected: their words do, through `out()`, §15.11]. Found via the `dollars/dollar/thousand/million/hundred/point/percent/no cents`
cluster at `0x13346-0x139e3` plus the scale-name table (`zillion … vigintillion`, §7.3) at `0x133cf`.
- **`numeric_token_dispatch`** `0x4062` (now `token_dispatch`) — top-level classifier, called from the token-flush routine
  `FUN_00003580` (unnamed) whenever a buffered token looks numeric [corrected: for every token, §12.46]. Handles `$` currency, ordinal suffixes,
  fractions `N/D`, scientific notation (`NeN` → "times ten to the [minus] N power"), dates (`Mon-DD[-YY]`, month
  table `0x12f8e`, day as ordinal, year as 2-digit cardinal), times (`HH:MM[:SS]`), else a bare cardinal.
  **[corrected]** Time fields are separated by `","`, not the word "colon" (§12.46).
- **`number_to_words`** `0x4bd0` — core formatter, mode-selected by the 2nd argument: dollars-and-cents ("forty
  two dollars and six cents"/"no cents"), decimal point ("three point one four", fraction digits read
  individually), digit-string/ordinal/fraction-denominator modes (down to `speak_digit_group`), and a trailing
  `%` → "percent" in every mode.
- **`cardinal_number_to_words`** `0x50a2` — groups a digit string into 3-digit chunks and speaks each with its
  scale name (`zillion` at index 0 is the base/overflow placeholder); leading/all-zero runs go digit by digit;
  falls back to `speak_digits_individually` for > 9-digit or already comma-grouped input [corrected: only for more
  than nine digits, §12.46].
- **`speak_digit_group`** `0x50e8` — 1-3 digit group (ones/teens/"-ty" tens/hundred) with mode-selected word
  forms: cardinal, ordinal (table `0x1306e`), fraction denominators ("half(ves)", "third(s)", "quarter(s)").
- **`speak_digits_individually`** `0x4ee4` — digit-by-digit reader for > 9 significant digits or comma-grouped
  input: paced groups of 3 with pauses, or singly at commas. **`flush_currency_suffix`** `0x4ace` emits a pending
  "dollar(s)" plus any deferred sign/number. **`speak_number_sign`** `0x4b98` emits "minus" for a negative sign.
- **`long_divide`** `0x1d6ae` / **`long_modulo`** `0x1d784` — signed 32/32-bit division/modulus (68000 `DIVU/DIVS`
  take only a 16-bit divisor), used for scale-group indices and stripping 3-digit groups; same wide-math family as
  `long_multiply` `0x1d73a`.
- No `dapi` source matches `cardinal_number_to_words`'s grouping (dapi's number normalization is host-side
  text preprocessing not shipped here); the identification rests on the ROM strings and control flow. MITalk's
  FORMAT numeral rules (§14.2) are the conceptual ancestor.

### 15.13 Old open-items list — where each item went
DSP ROM (disassembled 2026-09-23, §16) · `DT_PHONE_HOME` handler `FUN_0000eecc` (§13.2) · small table `0x2680-0x26a0`
(pointers `0x2700`/`0x2628`; partially explored, `0x2628` never identified — §13.11) · `host` task A4 = `0x10000` origin (§13.11) · clause tokenizer `FUN_00003182/3580/3dcc` (§13.5) ·
`Dr.`/`St.` confirmation (**resolved**, §7.1) · LTS payload encoding and 6-byte gap (§13.11) · `list`/`listall` bit
and the `"fm"` blob (**partly resolved**: `kind & 8`, and the blob is the per-sex tables, §8.1/§12.3/§12.10; the
`list` question resolved in §15.22) · what signals `wait_for_stop_signal` (§13.11).

---

### 15.14 Phonetic component: `pht0draw`, `phsettar` and their tables (2026-09-23, Ghidra)
Found by searching the ROM for dapi `p_us_rom.c` arrays (script: parse each `short name[] = {…}` and search the
first six values as big-endian shorts), then following the cross-references:
- **`f0segtars` `0x169c4`**: 56 values identical to dapi for codes 0-55 (dapi's 57th entry, for `DF`, is absent).
  The only reader is `FUN_0000c722`.
- **`begtyp` `0x16aa4`**: 56 values, identical. **`endtyp` `0x16b14`**: identical except code 8 `aw`, which is 5 in
  the ROM and 3 in dapi. The only reader of both is `FUN_0000aa08`.
- `featb` `0x161fe` (known): the first 28 values match dapi's `featb`. `malamp` (6-value hit at `0x166dc`),
  `maltar`, `femtar`, `maldip`, `femdip`, `inhdr` and `mindur` do *not* match dapi byte for byte; `inhdr`/`mindur`
  differ because the ROM has its own values (§9.1).
- **`pht0draw` `0xc722`** (renamed) = dapi `ph_drwt01.c` `pht0draw()`. The decompile shows:
  - the sung-note path: an F0 command ≥ 2000 is offset by 2000; notes 1-37 index `notetab` `0x16172`, and a
    note > 37 gives `"Sung note %d > %d"` and `"If intended to be an F0 target, …"`;
  - the F0-target path: 50-511 → `value·10`, i.e. tenths of Hz, with `"F0 > %d"` above that;
  - a 4-step vibrato `0x1616a` (`-6 0 6 0`, advanced every 6 frames);
  - impulse (odd) vs step (even) F0 commands from `0x817b8[]` with durations in `0x81754[]`, filtered by a Q14
    two-pole smoother (`0x600`, `0x3a00`, `0x3000`);
  - the segmental term `f0segtars[phoneme]`, quartered when the segment is unstressed (`0x812a2[] & 6` = 0),
    plus a voicing-onset term (4 or 11 frames) chosen via `featb` bits;
  - the result is scaled around **1200 (120.0 Hz)** by `frac_mul_q12` with voice parameters (`0x81c32`, `0x81c1e`)
    and clamped to **500-5121 (50-512 Hz)**;
  - it also sets a spectral-tilt value `0x822c6` from F0 (capped at 28, +18 for one `featb` class);
  - output goes to `0x81bd8`, then `FUN_0001d81c(400, 1000, F0)` → `0x822a6`.
- **`phsettar` `0xaa08`** (renamed) = dapi `ph_setar.c` `phsettar()`. The structure is confirmed and the tables are
  found (2026-09-26, **§15.17**).

### 15.15 Bulk C export and kernel gap fill (2026-09-26) — step 1 of the C plan (§13.15)
**Ghidra coverage.** The first bulk export found code Ghidra had never turned into functions, all in the
hand-written kernel area below `0x2680`. The C area `0x2700-0x12b3c` and the runtime library had no gaps. Fixed in
Ghidra and saved:
- **71 functions added** (291 → **362**). Most are **system-call stubs** `lea handler,A1; trap #n; rts` (38 stubs,
  34 handlers). The handler follows the stub and was only reachable through the `lea` operand. The rest are the
  self-test failure path (`0x71e`/`0x724`/`0x73e`, LED error codes, then `jmp 0x1cc`) and the ISR state machines at
  `0x19f6-0x1f40`, whose continuations are stored as pointers (e.g. `move.l #0x1cb4,0x80542`).
- **Data found inside the code area:** `dev_op_offsets` `0xe40` (count 9, then word offsets from `0xe40` to the
  handlers `0xebc 0xec4 0xecc 0xed4 0xe54 0xf44 0xf44 0xef6 0xf26`; dispatched by `0xe12` through `jmp 0xfaa`);
  `tbl_019d6` (7 words 6, 14, 32, 80, 110, 254, 376; read at `0x1602`/`0x1664`; meaning open); the string pool
  `0x2628` `"main"`, `0x262d` `"null"`; and the jump table `0x2636`.
- **Boot structures:** `boot_copy_table` `0x2646` copies **ROM `0x1d408-0x1d617` → RAM `0x82286`** (the initialized
  `.data` section, 0x210 bytes, labelled `data_init_image`). `boot_init_calls` `0x265e` holds 16-byte records
  `{argB, argA, func, result_ptr}`: `task_create_impl(0x268e, 0)` (the `main` task), `0x15d0()`, `0x22ba()`.
  **`boot_init`** `0x1024` (named): clears RAM, sets up the heap header `0x82496`, copies `.data`, builds the idle
  TCB at `0x80018` (name `"null"`, return `halt_loop`), runs the startup calls and enters the scheduler.
- **Vector table [V]:**

  | Vector | Handler |
  |---|---|
  | 2-11 (bus/address error, …) | `0x30`, which holds `STOP #$2700` |
  | 28 = level-4 autovector | **`tlc_isr`** `0x10d8`, the telephone interface (`native/dtc01.c` `IRQ_TLC`); calls `*0x8060a` |
  | 29 = level 5 | **`spc_isr`** `0x1fdc`, the DSP semaphore; calls `dsp_command_queue_isr(*0x82268)` |
  | 30 = level 6 | `duart_isr` `0x16fe` → **`duart_isr_service`** `0x1710`: RxRDY/TxRDY for both ports, counter tick → `0xb72`, input-port change → LED byte `0x94001` |
  | 32 = TRAP #0 | **`trap0_entry`** `0x10f8` |
  | 33 = TRAP #1 | `trap1_dispatch_via_a1` `0x1106` |
  | 34 = TRAP #2 | **`trap2_entry`** `0x1116` |
  | 46 = TRAP #14 | **`trap14_restart`** `0x10ea` (warm restart; the wrapper is `system_restart` `0x10e8`) |
  | 47 = TRAP #15 | **`trap15_entry`** `0x972` (yield into `scheduler_context_switch`) |

  Each TRAP entry masks interrupts, calls `(A1)` (the handler the user stub loaded) and leaves through
  `0x1126`/`scheduler_context_switch`. TRAP #0 passes no arguments, #1 passes `A0`, #2 passes `D0-D1`.
- One bogus function start of mine (`0xf58`, mid-instruction) was removed and `0xe86` folded back into `0xe54`. A
  check that every kernel function start follows a terminator, data or a known entry found no other problems.

**Export.** A Ghidra script decompiles every function into one file each, plus `functions.tsv` (metrics),
`calls.tsv`, `datarefs.tsv` and `symbols.tsv`, and groups the output into draft files `speech.c`, `host.c`,
`shared.c`, `kernel_asm.c`, `runtime-lib.c`, `boot_isr.c` and `unreached.c` (about 16,500 lines; kept in the session
scratchpad, regenerable). **All 362 functions decompile.** Functions were classified by reachability from the task
entry points. Each side's traversal stops at the other side's task roots. Direct calls plus function addresses
and pointer tables found through data references give 312 extra edges.

| Class | Funcs | Named | Bytes | Clean¹ | Notes |
|---|---|---|---|---|---|
| speech (`klsyn`, `dttask`) | 70 | 33 | 41,290 | 61 | 8 recovered switches; `phsettar` has a register leak |
| host (`main`/SETUP, `host`, `host timeout`, `stop`, `phone`) | 58 | 37 | 17,770 | 56 | `tdparse`, `showbit` leaks |
| shared | 36 | 18 | 6,388 | 36 | printf/stream library, logging, `load_voice_definition`/`save_voice_params`, `newclause`, `send_dcs_reply` (formerly `send_dsp_event`), escape-response senders: the interface to define (§13.15) |
| kernel / hand-written asm (`< 0x2680`) | 176 | 29 | 9,274 | 113 | 38 syscall stubs + 34 handlers; register calling convention |
| runtime library (`0x1d618-0x1da2f`) | 13 | 5 | 1,048 | 13 | long mul/div/mod, Q12/Q14 multiplies |
| boot / ISR only | 2 | 2 | 540 | 2 | |
| unreached | 7 | 2 | 452 | 5 | `caseD_6` `0x56e0` and `caseD_0` `0x109da` are switch cases split off from their parents (merge); `0x11b30`, `0x1204c` have no reference; `0x124a2`, `0x124fe`, `0x12b3c` are loaded as immediates (callbacks) |

¹ No register leaks (`in_`/`unaff_` registers), no unrecovered jump table, no bad data. The only unrecovered jump
table is the kernel's `dev_op_offsets` dispatch. The compiled C area is clean apart from four functions; the
remaining work there is **types** (`undefined*` in about 22 functions) and **names** (about 127 `FUN_`s).

**Next steps for the C plan** (in order): shared types (TCB, queue item, stream `FILE`-like struct at `0x81f1e`,
voice record, DSP frames) and prototypes for the 38 syscall stubs; write the kernel as a small C/asm RTOS shim
rather than decompiling its hand-written asm; add the DSP input-FIFO log to the emulator as the equivalence
test (§16.8 item 4); then work subsystem by subsystem.

### 15.16 RTOS API, C library and shared types (2026-09-26) — step 2 of the C plan (§13.15)
All names below are applied in Ghidra, with plate comments and C prototypes on the user stubs. Types live in the
Ghidra category **`/DTC01`**.

**System calls.** Stub = C-callable wrapper, `_impl` = supervisor handler. "Retries" means the stub loops while
the handler returns −1, which is how blocking works: the handler parks the task and returns −1, and when the task
is woken its result is written into `tcb_t.frame_d0`.

| Area | Calls (stub address) |
|---|---|
| Semaphores `ksem_t {waiters, count}` | `sem_wait` `0x88c`, `sem_signal` `0x8b0` |
| Tasks `tcb_t` | `task_create` `0x116e`, `task_delete` `0x1202` (also where a returning task entry lands, `0x11fc`), `task_suspend` `0x8d2`, `task_resume` `0x8fe`, `task_set_priority` `0x930`, `wait_on` `0x1298`, `wake_one` `0x12ba`, `set_ipl` `0x966` (TRAP #15) |
| Events and time | `event_wait(timeout, list, mask)` `0x986` (sleep = `event_wait(n,0,0)`), `event_register` `0x9b4` (asynchronous), `event_signal(list, value)` `0xaf8`, `event_node_free` `0xa22`, `event_cancel_mine` `0xa46`, `set_ticks` `0xa7e`, `tick_hook_add`/`tick_hook_remove` `0xab0`/`0xad0`; **`clock_tick`** `0xb72` (tick count `0x8010e`, hooks `0x80118`, timers `0x80114`, 25-tick round robin) |
| Signals | `catch_push(mask)` `0x12e2` (setjmp-like), `catch_pop` `0x131c`, `task_signal(task, sig)` `0x1358`, `raise_signal` `0x1346` |
| Mutexes `kmutex_t {next_held, owner, waiters, count}` (recursive) | `mutex_lock` `0x2538` (retries), `mutex_claim` `0x258a`, `mutex_unlock` `0x25c4`, `mutex_release` `0x25e0` |
| Devices `chardev_t` | `dev_getc` `0xc00`, `dev_putc` `0xc70` (both retry), `dev_control(dev, op<<16 \| side, …)` `0xe04` (generic ops 1-4 = lock/unlock/claim/release of `rx_lock`/`tx_lock` via `dev_op_offsets` `0xe40`; device ops through `dev->ops`: `duart_dev_ops` `0x19d6`, `phone_dev_ops` `0x2632`), `chardev_create(ring_size)` `0x2490`, `duart_init` `0x15c6`, `phone_init` `0x231c` |
| Heap | `malloc` `0x1438` (first fit, list `0x80010`, size at ptr−4), `free` `0x14d6` (coalescing), `heap_free_total` `0x1494`, `heap_largest_free` `0x14b2`, `knode_alloc`/`knode_free` `0x1534`/`0x157e` (32-byte nodes, pool `0x80014`) |
| Mailboxes (handlers in C, `0x12088-0x12178`) | `mbox_init(mb, notify)` `0x876`, `mbox_put(mb, msg)` `0x860`, `mbox_get(mb)` `0x84c` (retries), `msg_create(nwords)` `0x838`; `mbox_init_pool(mb, n, nwords)` `0x121c0` |

**Structures** (`/DTC01`):
- **`tcb_t`** (0xf6 bytes, then the task stack):
  - `+0 next`, `+4 on_list`, `+8 priority` (higher first; idle `0x8000`), `+0xa suspend`;
  - `+0xc cpu_ticks`, `+0x10 name`, `+0x14 all_next`, `+0x18 catch_list`, `+0x1c event_nodes`, `+0x20 mutexes_held`;
  - `+0x9c` saved `D2-D7`, `+0xb4` USP, `+0xb8` saved `A2-A6`;
  - `+0xcc`–`+0xe1` the trap frame: `D0 D1 A0 A1 SR PC`, where `D0` carries the wake-up result and `PC` holds the entry.
- **`ring_t`** `{size, count, put, get, bytes[]}`.
- **`chardev_t`**: a 0x40-byte header `{rx_notify, tx_notify, ops, rx_ring, rx_waiters, rx_lock, rx_aux, tx_ring,
  tx_waiters, tx_lock}`. Two devices extend it:
  - `duart_dev_t` (`hw_base` `+0x46`) for `host_dev` `0x8011e` and `console_dev` `0x80328`;
  - `phone_dev_t` (`debounce` tick hook `+0x54`, `line_state` `+0x68`, ring `+0x70`) for `phone_dev` `0x80552`.
- **`mbox_t`** `{head, tail, notify, waiters, count}` and **`msg_t`** `{next, nwords, …, home mailbox +8, payload
  +0x10}`. A consumer returns a message with `mbox_put(msg->home, msg)`.
- **`dsp_msg_t`**: a `msg_t` whose `w[24]` payload starts with the §16 frame header; `tone_arg` at `+0xe`.
- **`stream_t`** `{cnt, ptr, base, flags, dev}`, flags `1` read, `2` write, `0x100` LF→CRLF. **`_iob[20]`** at
  `0x82306` in `.data` (stdin, `stdout_` `0x8231a`, `stderr_` `0x8232e` on the console). `cur_stream` `0x81f1e`.
- **`voice_t`** (28 shorts, §8.1 order): `cur_voice` `0x81c44`, `val_voice` `0x81c7c`, the seven ROM voices and
  `voice_table` `0x16146`. **`dv_param_t`**: `dv_param_table` `0x15a34`.
- **`_ctype`** `0x1d354`: 1 upper, 2 lower, 4 digit, 8 space, 0x10 punct, 0x20 control, 0x40 hex, 0x80 blank.

**Kernel globals:** `ready_list` `0x80000`, `current_task` `0x80004`, `all_tasks` `0x80008`, `heap_free_list`
`0x80010`, `knode_pool` `0x80014`, `idle_tcb` `0x80018`, `tick_count` `0x8010e`, `timeslice_left` `0x80112`,
`timer_list` `0x80114`, `tick_hooks` `0x80118`, `duart_isr_hook` `0x80532`, `tlc_isr_hook` `0x8060a`, `dsp_queue`
`0x82256` (the DSP command mailbox; its notify `spc_irq_enable` writes `0x9c000 = 0x40`), `sync_sem` `0x81f26`.

**Speech-side protocol visible now.** `klsyn_task_main` owns `klsyn_mbox` `0x807a2`, with a free pool
`klsyn_free_pool` `0x80790` of 3 × 400-word messages. Messages are:
- `nwords == 1 && w0 == 0x68`: sync, so it calls `sem_signal(sync_sem)`;
- `w0 == 0`: `[:…]` text, one char per word, passed to `parse_bracket_command`;
- anything else: a phoneme/parameter stream for `parse_phoneme_param_stream`.

`pipe_open` `0x11fdc` builds reader/writer streams over a fresh `chardev_t` and is used by the speech pipeline.

**Library:** `_doprnt` `0x12556`, `fprintf`, `printf` (to `stdout_`), `sprintf`, `strcat`, `strcmp`, `strcpy`,
`strlen`, `tolower`, `toupper`, `islower`, `isupper`, `_flsbuf`, `fflush_if_pending`, `fdopen`, `fclose`, `pipe_open`,
`pipe_close`, plus a PC-sampling profiler (`profile_start`/`profile_probe`/`profile_report`).

**Effect on the export:** named functions 126 → **239** of 362, untyped references 786 → **599**. The C area still
has only the four known register-leak functions. The kernel stubs now show `return in_D0`, because Ghidra can't see
that TRAP returns the result in D0. That is cosmetic, since the C build replaces the kernel. One prototype mistake
was caught by the metrics: `dev_control` is called with two or three arguments, so it is variadic.

### 15.17 Frame builder: from `phsettar` to the DSP FIFO (2026-09-26) — step 4 of the C plan (§13.15) [V]
(Now rebuilt in C and verified word for word: §15.18.)
Traced backwards from the FIFO writer PC in the emulator log (`0x1245a`, inside `dsp_command_queue_isr`). Each
routine matches a dapi 1984-86 routine (Dennis Klatt's phonetic component), so the dapi names are used.

**Call chain** (speech side; all 68000):

```
parse_phoneme_param_stream 0x7788  (klsyn task, one work item; :vo/:ra end a clause early, §15.21)
 └ klclause 0x7a04                  = dapi phclause (ph_claus.c); ROM string "Missing CLSTART in klclause"
    ├ dsp_post_frame(spdef_packet 0x81c10, 23)    if spdef_dirty 0x8229e (set by setspdef)
    ├ phalloph 0x843e                phonemes 0x81d78 -> allophons/allofeats/allodurs, nallotot 0x81752 (§15.20)
    ├ phtiming 0x8ebe                durations -> allodurs 0x81112; tail = hat-pattern F0 commands (§15.8)
    └ phclause_draw_frames 0xa782    only when draw_mode_82294 is 's' or 'a' (always: .data 's', no writer); per 6.4 ms frame:
         if (++tcum >= durfon) { tcum -= durfon; if (++nphone > nallotot) return; phsettar(); }
         phdraw(); pht0draw();
         dsp_post_frame(parstochip 0x822a4, 18)
            └ mbox_put(dsp_link_queue) ─> spc_isr ─> dsp_command_queue_isr 0x12258 ─> 19 words to 0x9C002
```

dapi calls `pht0draw` before `phdraw`; the ROM calls it after. The two write different words, so the order only
matters if one reads the other's output (they share `phcur`, set by `phsettar`).

**`parstochip` `0x822a4`** (the 19-word frame buffer, 18 words sent plus the trailer):
- The header `0x4000` is never written by code; it is the `.data` initial value (ROM `0x1d426`).
- `pht0draw` writes T0 (`+2`) and TLT (`+0x22`).
- `phdraw` writes F1…AB (`+4`…`+0x20`).
- `dsp_post_frame` `0x7b56` (dapi `send_pars`) copies the words into a message from `dsp_frame_pool` `0x81cb6`
  (48 messages of 0x1a words, i.e. about 300 ms of frames; `dsp_link_init` `0x7aec`). It appends the trailer
  `0x43D4`, or, for a `0x6000` header, the checksum −sum, then posts the message to `dsp_link_queue`
  `0x81cc8` (= `dsp_queue`).

**Tracks: `ph_params` `0x8197e`**, `ph_param_t[15]` (new Ghidra type). Each track is dapi's `PARAMETER` without the
`outp` pointer, 0x20 bytes: `tarcur durlin deldip dipcum ftran dftran btran dbtran tbacktr tspesh pspesh tarnex
tarlas tarend *ndip`. The order is F1 F2 F3 FNZ B1 B2 B3 AV AH A2 A3 A4 A5 A6 AB, i.e. the frame order. TILT is not a
track: `partyp` has 0 there, and `pht0draw` makes TLT.

**`phdraw` `0xa804`** = dapi `ph_draw.c` `phdraw()` (DK 1984) without the later additions (F2 vowel-vowel
coarticulation, breathy B1 widening, trills, glottal stop):
- **Formants and bandwidths (F1 … B3):**
  - A diphthong line steps when `tcum > durlin`: `durlin`/`deldip` come from `ndip`, and `tarcur += dipcum/8`.
  - The value is `tarcur + dipcum/8 + ftran/8`, with `ftran -= dftran`.
  - From `tbacktr` on, `btran/8` is added and then `btran += dbtran`.
  - While `tcum < tspesh`, `pspesh` replaces the value.
  - Afterwards **F2 ≤ `f2max`** and **F3 ≤ `f3max`**.
- **Amplitudes (AV … AB):** the same forward/backward smoothing without diphthong lines, plus two rules:
  - one frame after `tspesh`, A2…AB drop 10 dB when ≥ 10 (dapi's "double burst");
  - AV > 40 starts 8 dB low at `tspesh` and rises 1 dB per frame for 8 frames (onset ramp).

**`phsettar` `0xaa08`** = dapi `ph_setar.c` (structure [V]; values differ from dapi). It runs at each phone
boundary:
- **Phone context.** It sets `durfon = allodurs[nphone]`, `phcur`/`pholas`/`phonex`/`phonex2`, their `featb` words
  (`feacur` …), and `begtyp`/`endtyp`.
- **Duration ratio.** `dur_ratio_q14` `0x81c00` = 16384·durfon/inhdr. `diph_time_scale` `0xc570` uses it to rescale
  diphthong time points.
- **Target per track**, chosen by `partyp` `0x16d8c` (= dapi `ph_romi.c` except TILT):
  - **Type 3 (F1-F3) and type 4 (B1-B3):** from `p_tar`, 7 rows × 56 phones (F1 F2 F3 B1 B2 B3 AV). A value < −1 is
    an index into `p_diph`: a list of `{value, time}` pairs ending in −1, expanded into `dipspec_buf` `0x81b80`,
    with slopes from `divtab` `0x16d9c` (= dapi, byte for byte).
  - **FNZ (type 1):** `fnz_default` `0x16d74` = **300**, or **527** (`0x20f`) next to a nasal. This matches the FNZ
    values in the log.
  - **AV (type 0):** its `p_tar` row.
  - **AH:** 0, or 58 for code `0x1c`.
  - **A2-AB (type 2):** from `p_amp`, 6 per row, via `burst_amp_index` `0x16b84`.
- **Coarticulation.** F1-F3 targets get 15 % coarticulation, `0.85·target + 0.15·neighbour` (Q14 `0x3667`/`0x999`,
  dapi `N85PRCNT`/`N15PRCNT`).
- **Transitions.**
  - `setloc` `0xc420` gives `bouval` `0x81bfa` = locus + percent·(target − locus) and `durtran` `0x81bfc`, from
    `plocu` `0x16bf4` and `p_locus`.
  - The forward transition is `ftran`/`dftran`; the backward one is `btran`/`dbtran`/`tbacktr`.
  - `durtran` is capped at 20 frames and at `durfon`.
- **Plosives and aspiration.**
  - Burst length comes from `stop_burst_dur` `0x16d50` (codes ≥ 45), and VOT from `vot_frames` `0x16d66` (6, 4, 2,
    and +3).
  - During aspiration AH is held at **58** or **55** dB. That is exactly the AH seen in the log for /h/ and aspirated
    /p t/.
  - B1/B2 are widened (+250/+80) during it.
- **Reset values.** F1-F3 `tarend` = 600/1600/2600 (`0x16d6e`).
- **Sex-selected tables.** The pointers `p_locus` `p_diph` `p_tar` `p_amp` (`0x81bda`-`0x81be6`) are assigned in the
  same order as in dapi `setspdef`:

| | male | female |
|---|---|---|
| `p_locus` | `maleloc` `0x16e00` | `femloc` `0x17b1c` |
| `p_diph` | `maldip` `0x17186` | `femdip` `0x17ea2` |
| `p_tar` | `maltar` `0x163b2` | `femtar` `0x1750a` |
| `p_amp` | `malamp` `0x166c2` | `femamp` `0x1781a` |

**`pht0draw` `0xc722`** — the end of the routine (§15.14 has the rest):
- **F0.** `f0_tenths` `0x81bd8` = `((f0 − 1200)·f0scalefac >> 12) + f0minimum`, clamped to 500-5121.
- **T0.** `T0 = muldiv_globals(400, 1000, f0_tenths)` = 40000/F0. `muldiv_globals` `0x1d81c` takes its arguments in
  the globals `0x81f14/16/18`, and `frac_mul_q14` does the same.
- **TLT** = `max(0, ((1200 − f0)>>3)·f0_dep_tilt >> 12) + spdeftltoff`, plus 18 when `featb[phcur] & 0x20`, capped
  at 28. With Paul's sm = 34, `spdeftltoff` = 34·36/100 = **12**, and 12 + 18 is capped to **28**. These are the two
  TLT values seen in the log.

**Speaker packet: `setspdef` `0x81e6`** = dapi `ph_vset.c` `setspdef()`. It runs from `load_voice_definition` and
`parse_bracket_command` after `set_formant_limits` `0x7d42`. It writes the speaker state `0x81c06`-`0x81c42`:

| Address | Name | Value |
|---|---|---|
| `0x81c06` | `malfem` | sex |
| `0x81c08` | `spdeftltoff` | sm·36/100 |
| `0x81c0a` | `assertiveness` | as·41 |
| `0x81c0c` | `f2max` | limit (see below) |
| `0x81c0e` | `f3max` | limit (see below) |
| `0x81c10`… | `spdef_packet` | the 23 packet words (0x6000 header + 22 parameters) |
| `0x81c3e` | `f0basefall` | bf·10 |
| `0x81c40` | `ef_x10` | ef·10 |
| `0x81c42` | `f0_dep_tilt` | ft·41 |

The packet is dapi's `SP_CHIP` with **two extra words**: word 7 = `f0minimum` (ap·10) and word 17 = `f0scalefac`
(pr·41). They sit inside the packet because the 1984 layout kept them there. dapi later moved both out ("not sent to
chip, just used by higher level routines").
- **68000 side:** `pht0draw` reads both from the packet (`0xd124`, `0xd146`).
- **DSP side:** the DSP stores them in RAM `0x8B` (`LDPK 1`, `IN 0Bh`) and `0x1A`, and adds them to the checksum.
  No other instruction reads either address directly, so the DSP ignores them ([I]: only direct addressing was
  checked). This closes the open item "speaker words 7 and 17" (§16.9). It also makes the `RESET_COEFFS` note
  "`0x1A` = 31 = FNZ index offset" doubtful, since every speaker frame overwrites `0x1A`.

Other differences from dapi:
- `nopen1 = 4000 + 160·ri`; dapi later uses 160·(100 − ri).
- F4/F5 above 4950 Hz are zapped to 2500/2048 (dapi `ZAPF`/`ZAPB` under `MSDOS`).
- `set_formant_limits` `0x7d42` loads `f2max`/`f3max` from `f2max_by_sex` `0x15bfe` and `f3max_by_sex` `0x15c02`:
  female 3050/3350, male 2750/3050. These are the "four unidentified shorts" of §8.1; dapi uses the constants
  `F2max 2500` and `F3max 3500` "to keep SPC from overloading". If F4 is set and `f4 < f3max + 300`, the limits
  become `f3max = f4 − 300` and `f2max = f4 − 600`. The routine also forces `f5 ≥ f4 + 250`.

**Also named:**
- `speech_init` `0x30c8` (called from `main_task`): `settings_reset(3,0)`, `dsp_link_init`,
  `spawn_system_tasks`, `load_voice_definition`, `save_voice_params`, `newclause(0)`;
- `spc_reset_wait` `0x122fa`: SPC control 1 then 0, then wait for status bit `0x80`.

**Labels (Ghidra):**
- **Frame loop:** `tcum` `0x81bce`, `durfon` `0x81bd0`, `nphone` `0x81bd2`, `nallotot` `0x81752`, `nphonemes`
  `0x81f08`.
- **Allophone arrays:** `allophons`, `allodurs`, `allofeats`.
- **Tables:** `partyp`, `divtab`, `plocu`, `maltar` / `malamp` / `maldip` / `maleloc` and the female set.

**What this gives the C rebuild.**
- The whole path from allophones to FIFO words is now a named, typed set of routines that correspond to dapi code:
  `phsettar`, `phdraw`, `pht0draw`, `setspdef` and `dsp_post_frame`.
- The remaining unknowns are *values*, not structure. These are the ROM's own target, locus, diphthong and amplitude
  tables, which a C port copies verbatim, plus `phalloph`.
- The corpus in `decomp/reference/` checks every word.

### 15.18 The frame path in C (2026-09-26) — step 5 of the C plan (§13.15) [V: word for word]
**Result.** `src/speech/` holds compilable C for the whole frame path of §15.17. It reproduces every frame
the ROM sends for the reference corpus, word for word: 11 sentences, 19 clauses, 4,762 speech frames and every
speaker packet. It passes both when each clause starts from the ROM's state and in `--chain` mode, where the C
state carries across clauses by itself. The routines are `phclause_draw_frames`, `phsettar`, `setloc`,
`diph_time_scale`, `phdraw`, `pht0draw`, `dsp_post_frame`, `set_formant_limits` and `setspdef`. They build without
warnings at MSVC `/W4`.

**Files.**

| Path | What |
|---|---|
| `src/speech/ph_frame.h`, `ph_frame.c` | the routines and their state. Every global is a ROM RAM variable (address in the header), with dapi names |
| `src/speech/ph_math.h` | the ROM's fixed-point helpers with exact 68000 results |
| `src/speech/ph_rom.c/.h` | generated by `decomp/scripts/gen_ph_rom.py`: the ROM block `0x1616a-0x18226` copied verbatim, with each table (`maltar` … `femdip`, `featb`, `divtab`, …) as a pointer into it, plus `partyp` and the F2/F3 limits |
| `native/phcapture.c`, `phcap_hook.h`, `build_phcapture.bat` | capture build of the emulator. Musashi's instruction hook is switched on through a force-included header, so `dtc01.c` is unchanged. At each `phclause_draw_frames` entry it dumps RAM `0x80000-0x93FFF`; at each `dsp_post_frame` entry it logs the words |
| `decomp/test/test_frames.c`, `decomp/build_ph_test.bat` | the harness: imports a clause's RAM into the C globals, runs the C frame loop, compares with the ROM's posted frames and with `decomp/reference/*.frames.tsv`, and checks `setspdef` against the imported speaker state |
| `decomp/scripts/check_frames.py` | runs the capture and the harness (fresh and `--chain`) over `make_reference.CORPUS` |

The captures (2 MB) and binaries go to `decomp/build/` and are regenerated by the scripts. A deliberate one-token
change (the second-burst drop set to 9 dB instead of 10) fails at the exact frame and word (`stops` frame 159, A3),
so the check is sensitive.

**68000 details the C has to reproduce** (each one is written down in `ph_math.h` / `ph_frame.c`):
- `frac_mul_q14` shifts the `muls` product with `lsr.l #14` (logical). Only the low word is used, so this equals an
  arithmetic shift. `frac_mul_q12` is `(a·b) >> 12` via `lsl #4` / `swap`.
- `muldiv_globals` uses `divs.w`. On overflow D0 is left unchanged, so the result is the low word of the product.
- **Ghidra's decompile of `diph_time_scale` `0xc570` is wrong in one branch.** The listing computes
  `b = 0xC000 − 2·max(ratio, 0x2001)`, stored as a word; Ghidra shows `−2a − 0x4000`. The plate comment now says so.
- The ROM computes `pht0draw`'s tilt before the glottal-stop dip and the F0 clamp; T0 is `divs(400·1000, f0_tenths)`.
- `pht0draw` has a dead block at `0xcc28`: `clr.w` sets Z, so the following `beq` is always taken. It is left out.
- `f0tim[]` holds 50 entries, so `f0tim[50]` reads `f0tar[0]` (kept). `dipspec_buf` has 39 words in the ROM
  (C: 64).
- In the AV backward transition, `durtran = 0` happens even when the last test of the condition fails. The C keeps
  that side effect.

**Coverage.**
- **Exercised by the corpus:**
  - rule F0 (`f0mode` 0) with steps and impulses;
  - sung notes and F0 targets (`f0mode` 1, vibrato);
  - all seven voices and `[:dv sex f]`, so both the male and female tables;
  - stops and affricates (bursts, VOT, aspiration), diphthongs, nasals;
  - the glottal stop `q` (`featb` bit `0x2000`): the new `glottal` corpus entry.
- **Not exercised yet:**
  - DT_STOP truncation (`stop_pending`) and index markers (`index_marks[]`): **done in §15.23**, on the host line;
  - `phmode_8229c` ≠ 0 (dead in v1.8, §15.20);
  - the F0 error paths (note > 37, F0 ≥ 512);
  - diphthong segments of 50 frames or more (`long_divide` branch);
  - `f0mode` other than 0/1 (where the ROM reads an uninitialised local).

**Finding on the way (host side, not investigated):** text fed through `dtc01_feed_text` does not have its escape
sequences parsed. `ESC P 0;20;5 z` was spoken as "escape P zero semicolon …". Either the emulator feeds the local
terminal line, where §5.1 says escapes are ignored, or a mode is needed first. So index markers could not be tested;
see §13. **[resolved, §15.23]** It was the local terminal: the emulator fed DUART channel B. `dtc01_feed_host` now
feeds the host line, and index markers and DT_STOP are tested (the harness replays DT_STOP in every mode).

**Not in C yet** (at the time; `phtiming` and `phalloph` followed in §15.19-15.20): everything upstream of the frame
loop and the index/stop hooks' host side. The harness imports their results from the capture.

### 15.19 `phtiming` in C: durations and F0 commands (2026-09-26) — step 6 of the C plan (§13.15) [V: word for word]
**Result.** `src/speech/ph_timing.c` holds `phtiming` (0x8ebe) and `make_f0_command` (0xa6e2, the ROM's
`kl3_push_event`). They reproduce everything the ROM's `phtiming` writes, for every clause of the corpus:
- durations, `/t d/` allophones and the final-stop release;
- the F0 command list, the hat state and the rate factors.

The C frame path then turns those results into frames identical to `decomp/reference`. With `--timing --chain`,
all durations and F0 commands of every clause come from the C code alone, and the frames still match. The corpus is
now 12 sentences, 22 clauses and 5,588 frames. The new `prosody` entry covers `[:cp]`/`[:pp]` and emphatic stress.

**Test.** `phcapture` also saves RAM at `phtiming` entry (`<name>.t<N>.ram`). `test_frames --timing` starts each
clause there, runs the C `phtiming`, and compares all of these with the ROM's RAM at the frame loop:
- the allophone, feature and duration arrays and `nallotot`;
- `f0tim`/`f0tar` and `nf0tot`;
- `cumdur`, the hat state (`hatstate`/`hatsize`/`hatfall`), `emphasissw`;
- `sprate`/`sprat1`/`sprat2` and the working globals.

It then runs the C frame loop. `check_frames.py` runs four modes per sentence. A factor changed from `0x2ccd` to
`0x2800` fails immediately (`stops`: a duration 8 vs 9). A change to `0x2cce` does not: the smaller value rounds to
the same result.

**What the ROM's `phtiming` is** (it is older than dapi's `p_us_tim.c`, so the C follows the 68000 code):
- **Rate.** `init_timing` runs only when `sprate` changes. It clamps `sprate` to 120-350 and compresses rates above
  250. It sets `sprat1` (pause scale) and `sprat2` (segment scale), in Q14, through `muldiv`.
- **Allophones of /t d/** before the rules (only when `phmode_8229c` is 0 or 2):
  - before `yu`/`y` they become `ch`/`jh`;
  - in some contexts /t/ becomes `tx`;
  - unstressed after a vowel they become the flap `dx`.
- **Durations.** Rules 1-11 (the ROM's `prdurs` checkpoints) scale `prcnt` (128 = 100 %) and add `deldur`. The
  result is `durxx = q14((prcnt·(durinh − durmin)) >> 7 + durmin, sprat2) + q14(deldur, sprat2)`, capped at
  2·durinh − 1.
  - The ROM's own values differ from dapi's: rule 9's voiceless-consonant term is `deldur −= deldur/4` (dapi /2), and
    rule 10 has an /s th/+/sh/ special case of 2 frames.
  - Pauses are `q14(10 | compause+16 | perpause+75, sprat1)`, at least 10 frames.
  - User durations (`allodurs[n] ≠ 0`) skip the rules.
- **F0 commands** (the hat pattern, only for `f0mode` 0), per stressed syllable:
  - the step is `f0_stress_steps[count] + f0_stress_steps[5 + stress]` (ROM `0x15f00`: `0 210 70 20 0` by stressed
    syllables so far; `0 0 1 31 51 71 261` by stress level; odd values are impulses);
  - hat rise and fall commands scaled by `assertiveness`, plus clause-final fall and continuation commands;
  - `f0mode` 1 turns the per-phone notes in `f0tar[]` into commands in place.
- **Clause end.** A clause-final stop gets a 4-frame schwa release (`ax`/`ix`, feature `0x1000`).

**68000 details kept in the C:**
- `f0tim[50]` and `f0tar[200]` are one area, `f0cmd_area` (`0x81754-0x81947`). `f0tar` holds 200 entries, because it
  also carries the per-phone notes. `f0tim[50]` is `f0tar[0]`.
- The next-phone variable keeps its old value on the last phone, as the ROM's stack local does.
- In the two-to-one `featb` locals, the decompile shows `local_a` for what is really the previous phone's features
  (Ghidra merged two stack slots). The plate comment now says so.

**Coverage.**
- **Now exercised:** rule F0, sung/target F0, user durations, rate 250, comma/period pause settings, emphasis,
  hat rise and fall, clause and sentence ends.
- **Not exercised:** `phmode_8229c` ≠ 0, `f0_halfsteps` = 1, more than 50 F0 commands, too many sung notes, and the
  `durxx ≤ 0` debug print.

### 15.20 `phalloph` in C: phonemes to allophones (2026-09-26) — step 7 of the C plan (§13.15) [V: word for word]
**Result.** `src/speech/ph_alloph.c` holds `phalloph` (0x843e). It reproduces everything the ROM's
`phalloph` writes, for every clause of the corpus:
- `allophons`/`allofeats`/`allodurs` and `nallotot`;
- `f0tar` (the moved user F0 targets), `index_marks` and `f0_halfsteps`.

With `--alloph --chain` each clause goes from its phoneme stream to DSP frames through C code only (`phalloph` →
`phtiming` → the frame loop), and all frames match `decomp/reference`. The corpus is now 15 sentences, 28 clauses
and 7,593 frames. The three new entries are `rcolor` (every r-coloured merge, `*`, a user duration and F0 on a
merged r), `slow` (`)` rewritten at rates 120 and 130, a `?` end) and `exclaim` (`!`, s-cluster stress, "the" before
a vowel).

**Test.** `phcapture` also saves RAM at `phalloph` entry (`<name>.a<N>.ram`). `test_frames --alloph` runs the C
`phalloph` from there and compares everything it writes with the ROM's RAM at `phtiming` entry. It then goes on as
`--timing`. `check_frames.py` now runs six modes per sentence. Eight planted bugs were each caught at the exact
array slot: syllable positions, one r-merge, the `!` loop, the rate rewrite, s-cluster stress, the merged-r
duration, "the", and the boundary spread.

**What the ROM's `phalloph` is.** It is older than dapi's `ph_aloph.c`, which takes stress and boundaries from a
separate `sentstruc[]`. There is no `phsort` step (correction 39).
- **Input** `phonemes` `0x81d78[nphonemes]` (klclause guarantees `[` first and `,` last). The codes come from the
  name table `0x19704`:
  - 0-55 phonemes;
  - 56-60 stress symbols `# ' \` " -`;
  - 61-69 boundaries `* space ( ) [ , ! ? .`;
  - `0x66`/`0x67` plus one word: an index marker, stored as `code << 16 | word` in `index_marks[nallo]`.

  `parse_phoneme_param_stream` puts input phone n's user duration and F0 target in `allodurs[n + 5]` /
  `f0tar[n + 5]`. `phalloph` moves them to the output slot; the 5-slot lead keeps the output from overtaking input
  it has not read yet.
- **Phones.**
  - `dh ax` becomes `dh iy` before a vowel.
  - After a vowel, unless a symbol with `featb` `0x1000` (`# ' \``) follows, `l` becomes `lx` and `r` becomes `rx`.
  - `rx` then merges with the vowel: `ax`→`rr`, `iy ih`→`ir`, `ey eh ae`→`er`, `aa ah`→`ar`, `ow ao`→`or`,
    `uw uh`→`ur`. The r's user duration is added to the vowel, and its F0 target moves on to the next phone.
  - Every output phone gets `stress + 0x20` until the word's first vowel; the vowel itself gets `stress`.
- **Stress symbols.**
  - `'` is 5 (primary), `\`` is 2 (secondary), `"` is 6 (emphatic, and sets `f0_halfsteps`).
  - **`#` and `-` also give 5.** `#` appears in "good-bye" and "forty-five" and makes the preceding `d`/`t` a
    stressed onset [V: capture]. `-` is untested.
  - The stress is copied back over the onset: one consonant, two if the first is an obstruent, three for s +
    obstruent + consonant.
  - Each symbol also picks the syllable for the next hat rise: priority 5 for `' # - "`, 2 for `\``, 1 for the first
    syllabic.
- **Boundaries.**
  - `(` and `)` depend on the speaking rate: at `sprate` ≤ 120 they become `,`; at ≤ 140, `(` becomes `)`.
  - A word end (every boundary but `*`) marks syllable positions in words of 2+ syllables: 8 first, `0x10` middle,
    `0x18` last.
  - The boundary value is `bndval` `0x15eee`: `* 0x40`, space `0x80`, `( ) 0x100`, `[ , 0x180`, `! . 0x200`,
    `? 0x380`. It is added (bits `0x3c0`) to the phones back to the last syllabic. Above `0x100` a silence phone
    carrying it is inserted.
  - The hat rise `0x400` goes on the chosen syllable at `)` after 2+ stresses, at `(` after 4+, and at every
    clause-level boundary. It raises that syllable to at least 5. `! ? .` add the fall `0x800`, and `!` turns primary
    stresses back from there into emphatic ones.

**Found on the way.**
- `phmode_8229c` (`.data` 0) and `draw_mode_82294` (`.data` `'s'`) have no writer anywhere in the ROM: no absolute
  store, and no pointer to them in ROM data. The `phmode` branches in `phalloph`, `phtiming` and `phsettar` are dead
  in v1.8, and `klclause` always runs the frame loop. The C keeps both tests (§13.14).
- `(` is dropped before `phalloph` when typed in phonemic input, and the text parser did not emit it in any test. So
  `(`'s own path (priority 4+ hat, `(`→`)` at rate 121-140) is not exercised.
- The emulator feeds host text as fast as the DUART takes it and ignores XOFF. A long text spoken slowly (about 110+
  characters at rate 120) loses its end, so corpus entries are kept short (§13, caveat).

**Faithfulness and safety.** The ROM has no bounds checks, and for clauses of more than about 195 symbols its
indexes run past 199 into the following variables. `ram16()` in `ph_alloph.c` reproduces that address mapping for
the word variables (`allophons` → `allodurs` → `allofeats` → … → `f0tar` → `nf0tot`, `f0_halfsteps`, `perpause`,
`compause`, `tm_dpause`). `phonemes[200]` is `nphonemes`. Writes into `index_marks` halves or unknown RAM go to a
dummy word, so the C never writes outside its arrays.

**Coverage.** Exercised: all r-merges, `l`→`lx`, "the", `* space ) [ , ! ? .`, `' \` " #`, the rate rewrite,
1-3 consonant stress spread, syllable positions, user durations and F0 on a merged r. Not exercised: index markers
(escapes do not reach the host parser, §13), `(`, `-`, symbols above 69, `phmode_8229c` ≠ 0 (dead), and clauses of
195+ symbols.

### 15.21 `klclause` and the klsyn work item in C (2026-09-26) — step 8 of the C plan (§13.15) [V: word for word]
**Result.** `src/speech/ph_clause.c` holds five routines:
- `parse_phoneme_param_stream` (0x7788);
- `klclause` (0x7a04, dapi `phclause`);
- `load_voice_definition` (0x8150);
- `save_voice_params` (0x81a8);
- `ms_to_frames` (0xd54c, newly named).

With them, **the whole klsyn side of speech is C**: from the work item the text pipeline hands the klsyn task, to
every word posted to the DSP link. Speaker packets and their checksums are included.

**Test.** `phcapture` also saves RAM at `parse_phoneme_param_stream` entry (`<name>.p<S>.ram`). It writes each item's
word count to `<name>.streams.tsv` and tags every post in `posted.tsv` with its item. `test_frames --stream` runs
the C on each item and compares every posted word, in order, with the ROM's posts for that item. With `--chain`
only the first item's state is imported.

The corpus is now 17 sentences, 66 work items, 35 clauses and 8,572 frames. All pass in eight modes: fresh and chained
× {frame loop, `--timing`, `--alloph`, `--stream`}. The two new entries are:
- `voices2`: `:nd`, `:nv`, and a voice change inside one item;
- `voices3`: Val saved from a changed Harry with `[:dv ap 150 save]`, `:pp 60`, and `:ra` inside an item.

Eight planted bugs were all caught:
- the user-duration slot;
- the `ms_to_frames` rounding;
- the CLEND append;
- rate change before/after the clause;
- the restart position after `:vo`;
- `f0mode`;
- clearing `spdef_dirty`;
- the Val record.

**The work item.** The klsyn task copies each queue message's words to `phonemes` `0x81d78`. Its messages are 400
bytes, so an item always fits the 200-word buffer. `parse_phoneme_param_stream` then:
- clears `allodurs`/`f0tar[0..199]` and `f0mode`, then compacts the item in place;
- copies a word without bits `0x3000` (a symbol);
- otherwise takes `k = (w & 0x3000) >> 12` extra words, and the low byte is a symbol code. The flag word of symbol
  *c* is at `0x192e8 + 8c`, the last word of the 8-byte record `{ASCII char, name pointer, code | flags}` in
  `symbol_table` `0x192e2`. That table holds the 1-character alphabet and the names of all 256 codes.
  - **No `0x4000`:** a symbol with a user duration in frames and, for k ≥ 2, an F0 target (which sets `f0mode` = 1).
    Both go to `allodurs`/`f0tar[index + 5]`, capped at 199; this is the `+5` lead that `phalloph` reads (§15.20).
  - **`0x4000`:** a pseudo-phoneme with one argument:

    | Code | Action |
    |---|---|
    | `0x64` `:vo` | ends the clause so far (`klclause`) and restarts at `phonemes + 1`, keeping the `[`. Then `voice_code` `0x82298` = arg + `0x6b` and `load_voice_definition`. Outside `0x6b-0x73` it logs "Illegal voice %d" instead. |
    | `0x65` `:ra` | ends the clause so far, then `sprate` = arg. The clause before it keeps the old rate. |
    | `0x66`/`0x67` | index marker, kept in the stream for `phalloph` |
    | `0x69` `:pp`, `0x6a` `:cp` | `perpause`/`compause` = `ms_to_frames(arg)` = `(ms + 4)·10/64` |
    | anything else, including `0x68` | "Bad pseudo-phoneme %d", `dt_error_flags` \|= 8 |
- ends with `klclause(phonemes, out)`.

Every sentence also produces a one-word item `[` (a clause start with nothing after it), which `klclause` ignores
(`nphonemes` ≤ 1). Sung items arrive without a clause end, so `klclause` appends the `,`.

**Voices.** `voice_table` `0x16146` holds nine pointers:
- `:np :nb :nh :nf`;
- `:nd`, which points to Frank's record (§8.1);
- `:nk :nu :nr`;
- `:nv`, which points to **RAM `0x81c7c` (`val_voice`)**.

Only `save_voice_params` writes `val_voice`. It runs in `speech_init` (so Val starts as Paul) and for `save` inside
`[:dv …]`. `[save]` without the colon is read as phonemic text, and `[:save]` does nothing.

`spdef_dirty` `0x8229e` is a **byte**: `setspdef` does `move.b #1` and `klclause` does `tst.b`/`clr.b`. The harness
now imports it as one.

**Not exercised:**
- index markers (escapes, §13): **done in §15.23**;
- the error paths (bad pseudo-phoneme, illegal voice; `[:nx]` is dropped before klsyn), a missing CLSTART;
- `DT_LOG` `0x80` (`print_voice_param_table` is only a hook in the C);
- k = 3 items.

The `voices3` recording reports 10,215 output-FIFO underruns; every other entry has 0. The count is deterministic and
the frame log is byte-identical on rerun. It is the emulator's audio FIFO, which the frame comparison does not use [I].

### 15.22 `parse_bracket_command` (`[:dv …]`) in C (2026-09-26) — step 9 of the C plan (§13.15) [V: word for word, console text byte for byte]
**Result.** `src/speech/ph_command.c` holds:
- `parse_bracket_command` (0x7d1e);
- its helpers `skip_blanks` (0x7c10), `skip_blanks_eq` (0x7c3c) and `read_dv_value` (0x7c7e), all newly named;
- `print_voice_param_table` (0x8072).

`strchr` (0x11d90) is also named now. `klclause` now calls the real `print_voice_param_table` for `DT_LOG` `0x80`.
The ROM tables come from `gen_ph_rom.py`: `dv_param_table`, `dv_units`, `sex_hs_delta`/`sex_f45_q12`.

**Test.**
- `phcapture` also saves RAM at `parse_bracket_command` entry (`<name>.b<B>.ram`) and exit (`<name>.x<B>.ram`). Not
  `B`: Windows file names ignore case, and a first version's exit dump silently overwrote the entry dump. It logs the
  argument and the returned pointer to `brackets.tsv`.
- It also logs every byte `dev_putc` (0xc70) sends to the console device `0x80328` to `console.tsv`, tagged with the
  command while it runs.
- `test_frames --bracket` runs each command's text through the C and compares:
  - `cur_voice`, `val_voice`, `dv_value`, the speaker packet and its state words, `spdef_dirty` and `dt_error_flags`;
  - where parsing stopped;
  - the console text (the C's `\n` counted as `\r\n`).
- `--stream --chain` now runs these commands in C between work items. From the second item on it imports only the item
  itself and the host-set flags (`stop_pending`, `DT_LOG`, error flags), so every voice and synth value is the C's own.

The corpus is now 18 sentences with 6 `[:dv]` commands, 76 work items, 38 clauses and 9,015 frames. All pass in nine
modes. The new entry is `dv_cmds`: clamping (`pr 999`), ` =40`, `list`, `listall`, `SEX M`, `ap=9`, and a 12-letter
name. Eight planted bugs were all caught:
- the 8-letter name limit;
- the clamp;
- the sex F4/F5 scale;
- the `list` filter;
- the `=` skip;
- case folding;
- the units;
- the error flag.

**What the ROM does.**
- **Delivery.** The text pipeline sends each `[:dv …]` as its own klsyn work item (first word 0), and the klsyn task
  copies one text byte per word to `0x81d78`. The text arrives as `" ap 150 save;"`: the pipeline **cuts it at the
  first `;`**, so `[:dv sex f; hs=90 list]` runs only `sex f`.
- **Names.** A name is up to 8 characters, case-folded. It ends at `; ] space tab newline`, but **not at `=`**:
  - `ap=9` is looked up as the name "ap=9", fails, sets `dt_error_flags` bit 8 and stops the command;
  - `ap =9` and `ap 9` work;
  - a longer name is cut after 8 characters and fails the same way.
- **Values.** They are unsigned decimal numbers with no minus sign, clamped to the table's range. A byte `0xb0-0xb9`
  also counts as a digit, because the ctype test uses `c & 0x7f`.
- **`sex`.** It takes `m`/`f` or 0/1. Only a real change adjusts `hs` (∓18) and `f4`/`f5` (Q12 × 4990/4096 for female,
  × 3362/4096 for male; 2500 is left alone).
- **`list` / `listall` — resolved [V: emulator].**
  - `listall` prints all 28 rows. `list` prints 15: it hides `nf p4 p5 gf gh gv gn ft`, which have `kind` bit 8.
  - The manual's `[list]` example with 28 rows is therefore `listall` output, or another firmware.
  - Both print to the **console (local terminal)** even when the command came from host text. The host never sees the
    table. (The corpus text here came in on the local terminal, §12.42; re-tested on the host line in §15.23, same result.)
  - The line format is `"%-3s %4d %-2s (%4d .. %4d) %s\n"` after one empty line.
- **The end.** Every command ends with `set_formant_limits` + `setspdef`, so the speaker packet is re-sent before the
  next clause, even after an unknown name.

**Not exercised:** a missing value (`[:dv ap]` gives the minimum), a digit byte with bit 7 set, NUL inside the text,
and `DT_LOG` `0x80` (now real C, not tested).

### 15.23 The klsyn task loop in C, and the host line in the emulator (2026-09-26) — step 10 of the C plan (§13.15) [V: word for word]
**Result.** `src/speech/ph_task.c` holds:
- `klsyn_task_main` (0x3c20) and its loop body, `klsyn_dispatch`;
- `index_mark_reached` (0xfaaa, newly named). `phsettar` now calls it directly (the old `ph_index_hook` is gone), and
  the host-side index reply goes through `ph_index_reply_hook`. (2026-09-27: its action is `index_mark_spoken`, and
  the library's `ph_mark_hook` can defer it to audio time, §16.13.)

`src/kernel/rtos.h` declares the RTOS calls the C uses (`msg_t`, `mbox_t`, `ksem_t`, `mbox_*`, `sem_*`), laid out
as in the ROM; the test harness stubs them. With this step the whole klsyn task is C, from its mailbox to the DSP posts.

**The loop** (priority 50). It creates `klsyn_free_pool` `0x80790` (3 messages of 400 words, taken by `newclause`) and
`klsyn_mbox` `0x807a2`, then takes one message at a time and gives it back to its pool before working on it:
- **one word `0x68`**: a sync point; it calls `sem_signal(&sync_sem)`;
- **first word 0**: `[:dv …]` text, one character per word from word 1; it goes as a C string to `0x81d78` and to
  `parse_bracket_command`. This path is **not** gated by DT_STOP;
- **anything else** (items start with `0x41`): a phoneme/parameter stream, copied to `phonemes` and handed to
  `parse_phoneme_param_stream(nwords)`, **unless `stop_pending` > 0**, in which case it is dropped.

**The emulator's two serial lines [V].** `duart_isr_service` `0x1710` sends RxRDYA to the host device `0x8011e` and
RxRDYB to the console device `0x80328`, and each device's `+0x46` points at its channel (`0x98000` A, `0x98010` B).
The emulator comes from a design where channel B is the host link, so `dtc01_feed_text` has always typed on the
**local terminal** (§12.42). Added to `native/`:
- `dtc01_feed_host`, which feeds channel A one byte at a time and pauses while the ROM has sent XOFF, and
  `dtc01_read_host_line_tx`;
- `native/hostfeed.h`, which gives `spclog` and `phcapture` a `-H` flag. The text may then hold `\e`, `\r`, `\n`, `\\`
  and `\w` (wait 0.5 s of emulated time before the rest).

Channel B is unchanged. The existing references re-record bit-identical, cycle stamps included.

**Capture.** `phcapture` also writes:
- `msgs.tsv`: every message klsyn takes (PC 0x3cca), with `stop_pending` and the DSP posts so far;
- `m1.ram`: RAM at the first message;
- `stops.tsv`: each `stop_pending` change in `stop_task_main` (0xfa92/0xfaa6), with the messages and posts so far and
  whether klsyn was waiting in `mbox_get`;
- `marks.tsv`: each `index_mark_reached` call with its post count;
- `hostline.tsv`: the bytes the ROM sent on the host line.

**Test.** `test_frames --task` imports RAM once, at the first message, and feeds every captured message to the C
`klsyn_dispatch`. It compares:
- every DSP post, in order;
- the post count and `stop_pending` at each message;
- the sync signals (after how many posts);
- `last_index` at every post against the ROM's marks;
- the index replies (value and post);
- that every message is handed back to its pool once;
- the `[:dv]` console text.

**DT_STOP is a host-side input.** The harness replays it in **every** mode, right after the post during which the ROM
changed it, or before the message klsyn was waiting for. This is exact because the stop task (priority 0) can only run
while klsyn (50) is blocked, either in `mbox_get` or in a DSP post on a full queue.

**Corpus and results.** There are five new host-line entries:
- `index`: DT_INDEX, DT_INDEX_REPLY and DT_INDEX_QUERY;
- `sync`: DT_SYNC, with a `[:dv]` after it;
- `stop`: DT_STOP mid-clause, then text 0.5 s later;
- `stop_early`: DT_STOP before the item reaches klsyn;
- `stop_drop`: DT_STOP, then `[:dv ap 140]` and text at once.

The corpus is now 23 sentences: 120 klsyn messages, 95 work items, 8 `[:dv]` commands, 48 clauses, 10,897 frames and
10,933 DSP posts. Everything passes in all **ten** modes. Seven of eight planted bugs in `ph_task.c` are caught:
- the drop test;
- the `[:dv]` start word;
- the stream copy length;
- `last_index`;
- replying to `0x66` marks;
- the missing `mbox_put`;
- `[:dv]` gated by DT_STOP.

The eighth, dropping the `nwords == 1` test on sync messages, cannot be observed in v1.8: `newclause` starts every
item with `0x41` (or 0 for `[:dv]`), so no longer item begins with `0x68`.

**What the ROM does [V: emulator, host line].**
- **DT_SYNC** (P2 11). `emit_sync_marker` writes `0x1A` into the text pipe and waits on `sync_sem`. `dttask_main`'s
  clause scanner returns at that byte and posts the one-word `0x68` message. klsyn signals when it takes that message,
  that is, after every earlier item has been posted to the DSP queue (`sync`: after post 175 of 418).
- **Index markers.**
  - DT_INDEX (P2 20) and DT_INDEX_REPLY (P2 21) put the pseudo-phonemes `0x66` and `0x67` into the item (`1066 0005`:
    the `0x1000` flag means one extra word).
  - `phalloph` stores `code << 16 | value` on the phone. When `phsettar` starts that phone, `index_mark_reached` sets
    `last_index`, and for `0x67` it sends `ESC P ;31;7 z ESC \` at that frame.
  - DT_INDEX_QUERY (P2 22) is answered at once by the host side: `ESC P ;32; z ESC \` before anything was spoken (R3 = 0
    is omitted, its `;` is kept), or `;32;5` after a DT_SYNC.
  - The `0x68` branch of `index_mark_reached` (signal `sync_sem`) is dead, since `phalloph` stores only `0x66`/`0x67`.
- **DT_STOP** (P2 10).
  - `stop_task_main` raises `stop_pending`, sends a sync ("h_stop") and waits for it, then lowers `stop_pending`.
  - While `stop_pending` is set, `phsettar` ends the clause after the phone it is starting (`stop`: cut at post 102, so
    146 frames instead of the whole clause). klsyn drops every phoneme stream but still runs `[:dv]` and the sync.
  - The stop task runs again only when klsyn and dttask (10) are idle. Text the host sends right after DT_STOP is
    therefore **dropped too** if it reaches klsyn first (`stop_drop`: "Then more." dropped, yet `[:dv ap 140]` applied
    and heard in the next sentence). Sent 0.5 s later, it is spoken (`stop`).
  - If the stop comes before the item reaches klsyn, the whole item is dropped (`stop_early`: the text pipeline takes
    over 0.5 s for "Stop this sentence now.").
  - Real serial timing is slower than the emulator's feed, so how often a real host loses text this way is [I].
- **Replies.** `send_dcs_reply(r2, r3)` `0xef00` (formerly `send_dsp_event`) builds every `DCS 0;R2;R3 z ST` reply:
  index 31, query 32, phone status.
- **`[:dv list]` from the host line** prints its table on the local terminal and sends nothing to the host.

**Not exercised:**
- DT_STOP while klsyn is blocked in console output (a `[:dv list]`); the replay rule would apply it one step early;
- the host side itself: `dcs_command_dispatch`, DT_STOP resuming the stop task, and the query reply. It is host-terminal
  code and not in C.

### 15.24 The text pipeline in C, stage 1: letter-to-sound rules (2026-09-26) — step 11 of the C plan (§13.15) [V: word for word]
**Plan.** The text pipeline (`dttask`) is about 50 functions and 20 KB of 68000 code. It is rebuilt bottom-up and
tested per call against the ROM, as the klsyn side was:
1. LTS (this section);
2. the dictionary (the user hash table, the built-in trie, suffix stripping);
3. `pronounce_word` and `spell_char`;
4. numbers (with `out`/`outn`, §15.27);
5. `clause_putsym` and `newclause` (§15.28), then `parse_phonemic_text` (§15.29);
6. the clause scanner and `dttask` (§15.30).

The last stage is compared end to end: every message `dttask` posts to klsyn and every console byte (§15.30).

**Result.** `src/speech/tx_lts.c` holds:
- `lts_rule_engine` (0x6a64), `lts_env_match` (0x6c0c) and `lts_rule_apply` (0x6ad4);
- the node allocator `lts_alloc` (0x7108) and `lts_unlink` (0x7150), both newly named.

`tx_text.h` declares the text-pipeline state. `scripts/gen_tx_rom.py` writes `tx_rom.c/.h`: ROM `0x12d00-0x15a34` as one
verbatim byte block, read by ROM address [superseded: named, typed tables since §15.31]. The LTS code reads a table at index −1 in one place, and its rules are byte
streams, so address reads keep it exact. The text pipeline has its own harness, `test/test_text.c` (built by
`build_tx_test.bat`), until `clause_putsym` is in C; it stands in for it and logs the calls.

**How the engine works [V].**
- **The list.** It is a doubly linked list of 14-byte nodes `{code, features, next, prev}`. The nodes come from a pool
  of 100 at `0x809e2`, bump-allocated, plus a LIFO free list. The head hangs off a pseudo node `lts_hdr` `0x809d4`,
  whose `next` field is the head pointer. The end marker is a **ROM address**, `lts_nil` `0x13bc8`, whose bytes read
  as code `0x00ff`, features 0 and links 0.
- **The rule loop.** For each rule it tries `nodes − match_len + 1` positions from the head. The quick test compares
  a literal first item; then comes the right context (forwards, including the matched items), then the left context
  (backwards from the previous node), then the replacement.
- **Counters during a rule.** Insertions go in before the matched node and raise the node, position and limit
  counters. Deletions lower the node and limit counters each time, but the position counter only once.
- **Output.** The final list goes to `clause_putsym(code, 0)`: the leading boundary is skipped, repeats are dropped,
  and a final boundary is added.
- **Pattern matching.** A repeat item is matched greedily, without backtracking (§12.43).
- **Two ROM checks that can never fire:** the "no code" test on the ASCII map (sign extension), and the unchecked
  allocation of the trailing boundary. Words are cut at 79 characters, so that allocation cannot fail.
- **Pool exhaustion.** It can still come from rule insertions: the 79-letter `exex…e` returns 0, and the word is then
  spelled (§15.10).

**Test.**
- `phcapture` now also saves RAM at each `lts_rule_engine` entry (`l<N>.ram`) and exit (`y<N>.ram`). It logs the
  arguments and the result to `lts.tsv`, and every `clause_putsym` call, tagged with the LTS call it came from, to
  `putsym.tsv`.
- `test_text --lts` runs each captured word through the C and compares:
  - the result and the `clause_putsym` symbols;
  - the ROM's state at the exit: the list (code, features, and **which pool slot** each node is in), the free list,
    the pool count, the four counters and `clause_has_word`.
- `check_frames.py` runs it as an eleventh mode. The captures now allow 90 s of speech (`-s 90`; runs still end at
  idle).

**Corpus.** There are four new host-line entries:
- `lts_words` and `lts_words2`: nonsense and rare words with suffixes, prefixes, digraphs and silent letters;
- `lts_caps`: capitals, `Mac`, apostrophes, hyphens;
- `lts_pool`: the pool-exhaustion case.

The corpus is now 27 sentences: 111 LTS words (825 symbols), 97 clauses, 16,698 frames and 175 klsyn messages.
Everything passes in all eleven modes.

**Planted bugs.** Six of ten are caught:
- the repeat count;
- the group skip length;
- the repeat dropping in the output;
- the position limit;
- the free list;
- the left context start.

The other four:
- **Unreachable.** A result code `*`: no rule writes 61, and a `*` never reaches LTS.
- **Dead in v1.8.** An insertion that copies the node: no rule uses a group without a literal in an insertion.
- **No effect found.** Decrementing the position on every deletion, and resetting the features when a literal group
  keeps the code. Neither changes the symbols or the final state for any of 1,170 generated words (a C-only
  search).

**Observed [V: emulator].**
- Words arrive lower-cased, apostrophes kept (`o'reilly's`). Punctuation splits words before LTS.
- Many common words are **not** in the built-in dictionary and go through the rules: "sum", "forty", "five", "dollars",
  "much", "too", "cents" (the `numbers` sentence).

**Not exercised:** rules whose shapes the corpus doesn't reach. Per-rule coverage isn't measured; the C-only driver
(scratch) could list it.

### 15.25 The text pipeline in C, stage 2: the dictionaries (2026-09-26) — step 11 of the C plan (§13.15) [V: word for word]
**Result.** `src/speech/tx_dict.c` holds:
- the user dictionary: `dict_hash_clear_all` (0xfb10), `dict_hash_lookup` (0xfb56), `dict_hash_insert_or_delete`
  (0xfc36);
- the lookups: `stem_has_vowel` (0x65ee), `lookup_word` (0x68fc), `lookup_word_with_suffix_stripping` (0x65c8);
- `pronounce_dictionary_word` (0x62d8), which speaks a hit.

`scripts/gen_tx_rom.py` now also writes `tx_rom_dict.c`: ROM `0x20000-0x3fe82` (130,690 bytes) as a second verbatim
block; `rom_at()` in `tx_rom.h` picks the block by address [superseded: typed tables, §15.31]. Three routines are not in C yet; the harness stands in for
them:
- `parse_phonemic_text` `0x3dcc` (newly named), which speaks a user entry's substitution;
- `clause_putsym`;
- `panic` `0x1d618` (newly named; a `JMP` to itself).

**The built-in dictionary [V].** `lookup_word`'s second half; decoded completely with a script (scratch).
- **Index.** Shorts `0x20000`/`0x20002` hold the first and last first character (`0x20`, `0x7a`). `0x20004` holds one
  12-byte record per first character: `{char lists, offset lists, pronunciations}`, three ROM pointers. 49 of the
  91 characters have words.
- **Nodes.** A node is a sorted list of characters, the last with bit 7 set, and a parallel list of 16-bit words at
  the same index. A word without bit 15 is the distance to the child node, in entries (both lists advance by it). A
  word with bit 15 is the offset of a pronunciation from the record's base, taken only if the word ends there. The
  character `0x7f` stands for the end of the word. Characters are compared as signed bytes.
- **Size.** 6,508 words and 19,703 nodes, none shared: a plain trie, not a DAWG (§12.44). Character lists
  `0x20448-0x26a79`, offset words `0x26a7e-0x336e1`, pronunciations `0x336e2-0x3fe81`. The last ROM bytes hold the
  build date, "11Oct83".
- **Pronunciations.** A pronunciation is a string of symbol codes (§8.3), with bit 7 set on its last byte: phonemes,
  `'` 57, `` ` `` 58, `*` 61, a space 62 (compound words), `(` 63 and `)` 64. `(` precedes the 38 prepositions (`about
  above across … with`). `)` marks verbs and some function words (831 pronunciations).
- **Keys.**
  - 6,313 lower-case words;
  - 52 capitalized ones, the Table A-2 abbreviations (`Dr.`, `Jan.`, `COD`, `I`, …);
  - 69 `)word` alternates (Table B-1);
  - 71 space-plus-character keys for spelling: `" !"` exclamation point, `" a"` … `" z"`, digits, punctuation;
  - `-`, `--`, `.`, `...`, `....`.

  Table A-2 is there in full. Table B-1 matches except `)use` (see the ROM check under Table B-1, §7).
- **Hits.** Every built-in hit adds 1 to `dict_hits` (`0x81f0e`); nothing reads it.

**The user dictionary [V].**
- **Hash.** An entry goes into bucket (sum of the name's characters, `A-Z` counted as `a-z`) & 31 of `dict_user_table`.
- **Insert and delete.** `dict_hash_insert_or_delete` first deletes an entry whose name is **byte for byte** equal.
  Unless the substitution is empty, it then appends the new entry at the **end** of the bucket, so a replaced entry
  moves to the end.
- **Lookup.** `dict_hash_lookup` compares lengths first. A name character in `a-z` matches either case, any other only
  itself: capitals in a name match only capitals, as the manual says.
- **Order.** `lookup_word` asks the user dictionary first, and only while `dict_user_count > 0`. It then stores the
  result in `dict_user_text` on every lookup, 0 on a miss. A user hit returns the address of `dict_user_text` as a
  marker.

**Suffix stripping.** `lookup_word_with_suffix_stripping` works on the word's last letter L.
- **The order of tries.** It tries the whole word, then:
  - -ed: stem+e, stem, y for i, undoubled;
  - -ing: i→e, stem, undoubled;
  - -s (not -ss);
  - -es;
  - -ies → y;
  - -ly.
- **Guards and patches.** Each stem must pass `stem_has_vowel` (`char_class` bit `0x04`). The y and e are patched into
  the caller's buffer and put back afterwards.
- **Suffix codes.** Its `*suffix` codes are 0 none, 1 -ly, 2 -ing, 3 -s, 5 -es and 6 -ed (4, 7 and 8 never occur). A
  code is left set by the last suffix tried, even on a miss.
- **Speaking a hit.** `pronounce_dictionary_word` retries a miss without a final `'s`, as a possessive. After the
  pronunciation it adds the suffix's tail (`dict_suffix_tails` `0x13b34`):
  - -ly: `l iy`;
  - -ing: `ih nx`;
  - -s: `ix z` after a sibilant (feature `0x400000`), `s` after a voiceless sound (no `0x800`), else `z`;
  - -es: the same, except that the default is `ix z`, so **"echoes" = `eh k ow ix z`** (ROM quirk);
  - -ed: `ix d` after `t`/`d`, `t` after voiceless, else `d`.

  Then comes the possessive's `ix z`/`s`/`z`, and a word boundary.
- **Symbol rules.** In a built-in pronunciation, a primary stress sets `prev_word_stressed = 1`. A `)` becomes a plain
  boundary unless the previous word was stressed and the suffix is not -ing/-ed (§12.44). So "the dog admits"
  keeps it, but "Admits," at a clause start and "admitting" do not.
- **ROM strings.** The `Dr.`/`St.` special case passes ROM strings (`0x13be6`, `0x13bea`). None of the ROM's strings
  can trigger a patch.

**Test.**
- `phcapture` now also saves RAM at each `pronounce_dictionary_word` entry and exit (`d<N>`/`e<N>.ram`) and each
  DT_DICT insert (`u<N>`/`v<N>.ram`). It logs them to `dict.tsv` (with the `parse_phonemic_text` call: text, suffix,
  result, `prev_word_stressed` after it) and `udict.tsv`.
- It also logs every `newclause` call to `newclause.tsv`: `clause_putsym` calls `newclause` when a clause is full,
  which clears `prev_word_stressed`.
- `putsym.tsv` gained the dictionary call and a "from `parse_phonemic_text`" flag.
- `test_text --dict` runs each word through the C, with the user dictionary rebuilt from the entry RAM, entry for
  entry. It compares:
  - the result and the symbols;
  - the word's bytes (the patches must be undone);
  - `dict_user_text` (mapped back to ROM addresses), `dict_hits` and `prev_word_stressed`.
- The stand-in for `parse_phonemic_text` checks its arguments and returns what the ROM's call did. The stand-in for
  `clause_putsym` replays a `newclause` where the ROM had one.
- `test_text --udict` compares every bucket after each DT_DICT.
- `check_frames.py` runs both as modes 12 and 13, and recaptures when `dict.tsv` is missing.

**Corpus.** Four new host-line entries:
- `dict_suffix`: every suffix path, the verb marker, possessives, the tails;
- `dict_abbrev`: Table A-2, `Dr.`/`St.`, `)` alternates, spelled characters;
- `dict_user`: capitals, a name ending in `.`, two names in one bucket, replacing an entry that is not the last,
  deleting, deleting a missing name, suffixes and a possessive on user words;
- `dict_user1`: exactly one entry, in bucket 19.

Totals: 31 sentences, 752 dictionary calls (242 hits, 13 of them user entries; 1,163 symbols), 11 DT_DICT entries,
125 LTS words, 144 clauses, 21,695 frames and 238 klsyn messages. Everything passes in all 13 modes.

**Planted bugs.** All 16 are caught:
- the case rule;
- the bucket mask;
- appending at the head;
- not re-adding after a delete;
- `>=` in the sibling scan;
- a leaf taken before the word ends;
- no undoubling;
- the -es code;
- the -ing verb rule;
- the possessive default;
- the sibilant tail;
- `stem_has_vowel`'s last letter;
- `dict_hits`;
- the i→e patch not undone;
- the user-dictionary test;
- the stress flag.

Five were missed at first, and the corpus got words for them: "skied", "the dog adapting", "echo's", `ab` before
`ba`, and `vex` alone.

**Found on the way.**
- A host-line entry of about 300 characters loses its end (§13, caveat 3); `dict_suffix` is split with `\w`.
- The session's Bash tool turns `\\` into `\`, even inside quoted heredocs. A corpus text passed through it had its
  `ESC \` become `ESC t`, and the next letter was lost. Pass such texts from Python files only.

### 15.26 The text pipeline in C, stage 3: words (2026-09-26) — step 11 of the C plan (§13.15) [V: word for word]
**Result.** `src/speech/tx_word.c` holds the word layer between `out`/`outn` and the dictionaries:
- `pronounce_word_or_abbrev` (0x7192) and `pronounce_word` (0x5a90);
- `check_wh_word` (0x5bd0) and `spell_chars` (0x6198), both newly named; `check_wh_word`'s code lies inside
  `pronounce_word`'s address range (`0x5bd0-0x5c06` and `0x609e-0x60a8`);
- `spell_char` (0x621a), `emit_punctuation_symbol` (0x60aa) and `str_match_fold` (0x4e80).

Its globals are:
- `word_count` `0x81f0a` (never read);
- `wh_question` `0x81d76`;
- `abbrev_pending` `0x80f66`;
- `spell_buf` `0x8228a`;
- `dt_mode`, the DT_MODE word `0x822ce`, which the host side sets.

`gen_tx_rom.py` now names `PUNCT_SYMS` `0x13a58`, `DASH_SYMS` `0x13a78` and `NO_SPELL_FMT` `0x13a7f`. The ROM strings
"Dr.", "St.", "drive" and "street" are writable arrays in the C; the ROM never writes them. `newclause` and `kprintf`
are not in C yet; the harness stands in for them.

**`char_class` `0x13a9c` [V]:**

| Bit | Meaning | Characters |
|---|---|---|
| `0x01` | opening punctuation | `"`, `'`, `(`, `[`, `{` and the backquote |
| `0x02` | closing punctuation | `" ' ) ] } ! , . : ; ?` |
| `0x04` | vowel letter | `a e i o u` in both cases |
| `0x08` | small letter | `a`-`z` |
| `0x10` | capital | `A`-`Z` |
| `0x20` | clause mark | `! , . : ; ?` |
| `0x40` | digit | `0`-`9` |
| `0x80` | consonant letter | not `y`/`Y`, which is only a letter |

The ROM reads the table with a sign-extended character, so bytes `0x80-0xff` would read the strings before it. None
arrives, because 8-bit text loses bit 7 first (below).

**`pronounce_word` [V].** It adds 1 to `word_count` and subtracts 1 from `prev_word_stressed`, then:
1. It tries the dictionaries on the whole word as typed. That is how `Dr.`, `)record` and a user entry `A.M.` hit.
2. It strips opening punctuation from the front (each `(`, `[` or `{` sends symbol `0x40`) and closing punctuation from
   the end. If nothing is left, or the word starts with `)` while DT_MODE SQUARE is off, it spells the whole word.
3. If something came off, it tries the word with its final period, then the bare word. A lone `a`/`A` before a clause
   mark is `spell_char('a')`, so "A," is the letter.
4. **Acronyms:** capitals each followed by a period (`U.S.A.`, `A.P.O`) are spelled letter by letter. A final period
   with more after it (`U.S.A.,`) is used up.
5. It lower-cases the capitals **in place** and tries the dictionaries again, with and without a final period.
6. **Pronounceable?** The word must be letters in parts split by `'` and `-`. Each part needs a vowel and a consonant
   (`y` is both); before a `'` or `-` one vowel alone will do (`e-mail`), two will not (`oa-la`). The last part needs
   both only when there was no `'`. Otherwise the word is spelled: `sys$system`, `bcd`, `hmm`, `a.p.o.` (with
   "period"s), `x-ray`.
7. It speaks the word part by part, a part being letters and apostrophes: the dictionary, else a one-letter part is
   spelled, else `lts_rule_engine`. A `-` sends symbol `0x38`. When the rules give up (the node pool runs out), the rest
   of the word is spelled from that part on.
8. **Punctuation after the word** goes to `emit_punctuation_symbol` one character at a time. It allows one clause mark;
   `...` at the end counts as one period. A second clause mark spells the rest: "Why?!" ends in "exclamation point",
   and "Stop.," ends in "comma".

**Spelling.** `spell_char` looks up the key `" c"` in the built-in dictionary (capitals folded) and adds a word
boundary. `spell_chars` spells a range. There, a `-` is "dash" (`DASH_SYMS`: `d 'ae sh` and a boundary), or with
DT_MODE MINUS on it is `spell_char('-')`, "minus". After a spelled word, a single clause mark stays punctuation;
anything else is spelled too, and its last character is also sent as punctuation.

**Punctuation symbols.** `emit_punctuation_symbol` maps `)`, `]` and `}` to `,`. It sends
`PUNCT_SYMS[c - 0x21]` for `0x21-0x3f`: `!` `0x43`, `,` `:` `;` `0x42`, `.` `0x45`, `?` `0x44`, every other character
0. After `.`, `!` and `?` it calls `newclause(0)`.

**Wh- questions [V].** `newclause` and `emit_punctuation_symbol` set `wh_question` to −1. The first word of the clause
spoken after that sets it to 1 if the word starts with "wh", else 0 (`check_wh_word`; an acronym always gives 0). A
`?` then counts as a period when `wh_question` is 1. So "Where is it?" ends like a statement (symbol `0x45`), while
"Is it?" ends with `0x44`. After "What, no?" the comma has started a new clause, so the `?` stays a question.

**`Dr.`/`St.` [V].** `str_match_fold` compares case-insensitively. It allows one closing punctuation mark after the
abbreviation, or a period and one. So:
- "Dr.," and "St.)" give "drive"/"street" at once and send the rest as punctuation;
- a lone "Dr."/"St." waits for the next word. A capital followed by a small letter gives the dictionary's `Dr.`/`St.`
  (doctor, saint), anything else "drive"/"street";
- at the end of the text (`pronounce_word_or_abbrev(NULL)` from `out`/`outn`) it gives the default;
- "St.Louis" does not match at all (§12.45).

"drive" and "street" are not in the built-in dictionary: the rules speak them, straight from the ROM strings.

**Test.**
- **Capture.** `phcapture` saves RAM at each `pronounce_word_or_abbrev` entry and exit (`w<N>`/`q<N>.ram`) and logs:
  - `words.tsv`: the word's address, and the dictionary, LTS and `clause_putsym` calls before and after it;
  - `nospell.tsv`: `spell_char`'s message.

  `newclause.tsv` gained the return address and the word call, and `dict.tsv` gained `wh_question` after
  `parse_phonemic_text`.
- **`test_text --word`.** It runs each call through the C, with everything below it in C: dictionaries, LTS and
  spelling. Only `parse_phonemic_text`, `clause_putsym`, `newclause` and `kprintf` are stand-ins; the `newclause` that a
  full clause triggers inside `clause_putsym` is replayed by its return address. It compares:
  - the symbols;
  - the clause ends (the symbol count before each);
  - the "no spell" messages;
  - the word's bytes (lower-cased in place);
  - `prev_word_stressed`, `wh_question`, `word_count`, `abbrev_pending`, `dict_hits`, `dict_user_text` and
    `spell_buf`.
- **`check_frames.py`** runs it as a 14th mode and recaptures when `words.tsv` is missing.
- **Fix.** `--lts` crashed on words at ROM addresses ("drive", "street"); it now reads them from `tx_rom`.

**Corpus.** Five new host-line entries:
- `word_punct`: wh- questions, stacked marks, `...`, brackets, quotes, acronyms, a lone `a`, capitals;
- `word_spell`: hyphens, apostrophes, unpronounceable words, every `Dr.`/`St.` case;
- `word_modes`: DT_MODE MINUS and SQUARE off, with `[ ] { }` as punctuation;
- `word_8bit`: 8-bit characters;
- `word_edge`: the cases the planted bugs asked for, among them a 77-letter word after `(` on which the rules give up.

Totals: 36 sentences, 442 word calls (3,721 symbols, 68 clause ends), 1,099 dictionary calls (459 hits, 13 of them
user entries; 2,177 symbols), 11 DT_DICT entries, 147 LTS words (1,015 symbols), 177 clauses, 28,184 frames and 291
klsyn messages. Everything passes in all 14 modes.

**Planted bugs.** 26 of 27 are caught. Six were missed at first; `word_edge` now catches them:
- the clause-mark test for a lone "a";
- the acronym's final period;
- the two-vowel part;
- the one-letter part;
- the rule-failure restart;
- the single mark after a spelled word.

The one left, `check_wh_word(p)` in place of `check_wh_word("")` for an acronym, changes nothing: an acronym holds
only capitals and periods, so it can never start with "wh".

**Observed [V: emulator].**
- **8-bit text loses bit 7** before `pronounce_word`: `é` (0xE9) arrives as `i`, `ï` as `o` and `§` as `'`. This is what
  the manual calls "multinational letters lose their accents" (§12.45). The text goes through the Windows ANSI code
  page (cp1252) on its way to the capture tools.
- **Control characters.** DEL is dropped. FS (0x1c) arrived as the words "x", "control" and "y". The clause scanner
  (stage 6, §15.30) does this.
- **`out`/`outn` end each text with `pronounce_word_or_abbrev(NULL)`.** "Hello world." makes four calls: NULL,
  `Hello`, `world.`, NULL.

**Not exercised.**
- The "no spell" message: every key `" !"` to `" ~"` exists, and control characters are turned into words first.
- `emit_punctuation_symbol`'s branch for a closing mark outside `0x21-0x3f`: only `]` and `}` would reach it, and they
  are mapped to `,` first.

### 15.27 The text pipeline in C, stage 4: tokens and numbers (2026-09-27) — step 11 of the C plan (§13.15) [V: word for word]
**Result.** `src/speech/tx_num.c` holds everything between the clause scanner and the word layer:
- `token_dispatch` (0x4062, formerly `numeric_token_dispatch`) and its helpers `split_token` (0x4114), `flush_lead`
  (0x4a40), `flush_trail` (0x4a6c), `defer_money` (0x4a98), `is_scale_word` (0x40aa) and `speak_unit_abbrev` (0x56c2);
- the number patterns: `classify_number` (0x40e0), `pattern_find` (0x7310) and `pattern_match` (0x7352);
- the speakers `flush_currency_suffix`, `speak_number_sign`, `number_to_words`, `cardinal_number_to_words`,
  `speak_digit_group` and `speak_digits_individually`;
- `out` (0x556e) and `outn` (0x5614).

The nine helpers are newly named (~272 of 362). The globals are listed in §4. The three token buffers are one array in
the C, because the ROM reads the byte before `tok_body` (below). `gen_tx_rom.py` names the tables `PAT_NUMBER`,
`PAT_MONEY`, `NUM_PUNCT_WORDS`, `NUM_MONTHS`, `NUM_SCALES`, `NUM_ONES`, `NUM_ORDINALS`, `NUM_TENS`, `NUM_UNITS` and
`out`'s and `outn`'s four message formats.

**Every token goes through it [V].** The token flush `FUN_00003580` passes every token of ordinary text to
`token_dispatch`; the scanner also calls it with ROM words and with NULL at the end of the text. A token that is not a
number goes to `out(tok_body, 1)` between its punctuation.

**`out` [V].** It builds words in `out_buf` and hands each finished one to `pronounce_word_or_abbrev`; a space ends a
word. Its flag says how the text joins the words around it:

| Flag | Meaning | Used for |
|---|---|---|
| 1 | a word of its own | most words |
| 0 | starts a word that the next call continues | "twen" + "ty-" + "one" = "twenty-one"; opening punctuation |
| −1 | continues the last word | closing punctuation, the "," pauses ("one," "twelve,") |

A word is handed over only when the next word starts. So a token's last word is spoken during the next token, and
`out(NULL)` at the end of the text flushes it. At 79 characters `out` hands the word over, prints
`Long word in number output "…"` and drops that character; `"5" + 78 ")"` reaches it. `out` has no callers outside
this file. `outn` is the same for n characters, but only a flag ≥ 1 ends an open word.

**The patterns [V].** `classify_number` tries the `{pattern, class}` pairs of `PAT_NUMBER` (0x12e56) in order, or
`PAT_MONEY` (0x12f26) after a `$` or while money is held back. It returns the class of the first pattern that
`pattern_find` finds. The patterns are a small regular-expression language (`pattern_match`):

| Opcode | Meaning |
|---|---|
| 1 c | the character c (capitals of the text folded) |
| 2, 3 | at the start (`pat_start`), at the end |
| 4 | any character |
| 5 n …, 6 n … | in / not in a set: n = item bytes + 1; an item is a character, or 0x0f lo hi for a range |
| 7 … 0x10, 8 … 0x10, 9 … 0x10 | zero or more, one or more, optional. 7 and 8 are greedy, then back off one character at a time, retrying the rest of the pattern |
| 10, 11, 12, 13 | a letter, a digit, a letter or digit, a control character or space |
| 14 (`N` below) | a digit, or a comma that groups three digits |
| 0x10 | the end |

`N` keeps its comma state within one call, but every repetition of `(N)` is a call of its own, so the state never
carries over. The effect:
- a comma fails after four digits;
- it needs three digits after it;
- a fourth digit after it fails unless it is `0` or `9`.

So "1,2349" is a cardinal ("twelve thousand three hundred forty-nine") and "1,2345" is a word.

| Class | `PAT_NUMBER` | Spoken |
|---|---|---|
| 7 | `^[1-9]DDD$` | year |
| 8 | `^(0)+$` | "oh" |
| 9 | `^(N)+(%)?$`, `^(N)+.(D)*(%)?$`, `^(N)*.(D)+(%)?$` | cardinal |
| 10 | `^1/D(D)?(%)?$`, `^1/100(%)?$` | "one" + a denominator: "one half", "one fourth" |
| 11 | `^D(D)?/D(D)?(%)?$`, `^D(D)?/100(%)?$` | numerator + plural denominator: "three fourths" |
| 12 | `^(N)*[04-9]th$`, `^(N)*1st$`, `^(N)*2nd$`, `^(N)*3rd$`, `^(N)*1Dth$` | ordinal |
| 13 | `^([.0-9])+e[+-]D(D)?$`, `^([.0-9])+eD(D)?$`, `^([.0-9])+$` | number with exponent |
| 14 | `^D(D)?-LLL-DD$`, `^D(D)?-LLL$` | date (a two-digit year gets "19") |
| 15 | `^D(D)?-LLL-DDDD$` | date |
| 16 | `^DD:DD:DD.D(D)+$`, `^D:DD:DD.D(D)+$`, `^D(D)?:DD:DD$`, `^D(D)?:DD$` | time |

`D` = digit, `L` = letter; `^(N)+(%)?$` appears twice.

| Class | `PAT_MONEY` | Spoken |
|---|---|---|
| 2 | `^1.DD$`, `^.01$` | "one dollar and … cents", "one cent" |
| 3 | `^(N)*.DD$` | "… dollars and … cents" |
| 4 | `^1$` | "one dollar" |
| 5 | `^(0)+$` | "oh dollars" |
| 6 | `^(N)+$`, `^(N)*.(D)+$`, `^(N)+.(D)*$` | "… dollars" |

**What the ROM says [V: emulator].**
- **Cardinals.** Groups of three with scale words, skipping a group of 000 with its word ("1000000" = "one million").
  At most nine digits get scale words, so above "million" the scale words are only matched as tokens after money
  ("$5 billion").
  - More than nine digits are read one by one, in threes with a "," pause: "12345678901" = "one two three, four five
    six, seven eight nine zero one". With commas after the first digits, the pause comes at each comma instead.
  - A leading zero reads every digit, 0 = "oh": "007", and a lone "0" is "oh".
  - After a point every digit is read, 0 = "oh" ("6.02" = "six point oh two"); only zeros after it = "point zero".
  - `%` = "percent"; `+`/`-` = "plus"/"minus".
- **Years.**
  - "5000" = "five thousand", "1900" = "nineteen hundred";
  - "1906" = "nineteen oh six", "2001" = "twenty oh one";
  - "1066" = "ten sixty-six".
- **Ordinals.**
  - "12th" is the ROM's "twelvth"; "20th" = "twentieth"; "100th" = "one hundredth".
  - **"1,000th" = "one thousand"**: the last group, 000, is skipped with its suffix.
  - "2th" is a word.
- **Fractions.**
  - The numerator is 1-99, else the token is a word ("123/4").
  - "3/2" = "three halves", "44/100%" = "forty-four one hundredths percent".
  - A denominator 1 or 0 gives "over": "1/1" = "one over one", "1/0" = "one oh".
- **Money** is held back one token, so that a scale word can follow: "$1.23 million," = "one point two three million
  dollars,".
  - "$1.00" = "one dollar and no cents"; "$.05" = "five cents"; **"$0.50" = "oh dollars and fifty cents"**;
    "$1.5" = "one point five dollars".
  - "$ 5" = "five dollars", but "$ abc" speaks the `$` as a word.
  - **"$5 6" = "six dollars"**: a number after held-back money replaces it ("$.01 5" too).
  - "$-5" = "minus five dollars"; "-$5" is a word.
- **Dates.**
  - "23-Sep-83" = "September twenty-third, nineteen eighty-three,".
  - A two-digit year always gets "19": "1-jan-00" = "January first, nineteen hundred,". A leading 0 of the day is
    skipped.
  - **The day is read from the unsplit token**, so "(4-Jul-76)" = "(July, nineteen seventy-six)".
  - A sign, day "00" or an unknown month make the token a word.
- **Times.** "12:00" = "twelve, oh oh,"; "11:04:03.01" = "eleven, oh four, oh three point oh one,". After a date or
  a time the "," is glued to the last word, unless the token ends in punctuation.
- **Exponents.** "1.2E-4" = "one point two times ten to the minus fourth power"; "1E+05" ends "to the oh five
  power". Class 13 is checked again: one point per mantissa with a digit on both sides, else the token is a word
  ("1.2.3", "3.e5", ".5e5"). For ".5e5" the check reads the byte before `tok_body`, the last byte of `tok_lead`.
- **Units** (Table A-1 plus `sq.in` `sq.ft` `sq.yd` `tsps` `tbsps`). A unit needs a period and must come right after
  a number: "5 cm." = "five centimeters", "(5 m.)" = "(five meters)".
  - It is singular after "1", after "1.5" ("one point five kilometer") and after 1/n ("one half inch").
  - It is not spoken after an ordinal, after money, or with other punctuation ("5 cm,").

**Unreachable [I: code].**
- **Class 13's punctuation words.** The words for `* + , - . /` (`NUM_PUNCT_WORDS`: "times plus comma minus dot
  over") and its `outn` call are never used: after the second check only digits, points, `e` and the exponent's sign
  remain. So `outn` is dead code in v1.8, and "dot" is never spoken.
- **`out`'s "Illegal character".** Control characters become words in the scanner, and 8-bit text loses bit 7.
- **`speak_digit_group`'s panics.**

**A stage-2 bug.** `tx_dict.c` crashed on "$" (from "$ abc"). `$` is one of the 42 first characters (of 91) that
have no dictionary words, and all of them share a stub root: `{0x26a7a, 0x26a7c, base 0}`. Its one entry is the end of a word
with offset 0, so a one-character word reaches the hit code. `dict_hits` goes up and the address returned is 0 + 0:
not found. The C turned address 0 into a pointer; it now returns NULL (§12.46).

**Test.**
- **Capture.** `phcapture` saves RAM at `token_dispatch`'s entry and at its common exit 0x40a0 (`k<N>`/`j<N>.ram`).
  It logs to:
  - `tokens.tsv`: the token, and the word, dictionary, LTS and `clause_putsym` calls at the entry and exit;
  - `outmsg.tsv`: the kprintf calls of `out` and `outn` (0x58a4, 0x5968).
- **`test_text --token`.** It runs each call with everything below it in C and the same stand-ins as `--word`. It
  compares:
  - the symbols;
  - the clause ends;
  - the "no spell" and console messages;
  - the six token and number buffers, byte for byte;
  - the number scalars, `out_ptr`, `pat_start` and the word-level globals.
- **`check_frames.py`** runs it as the 15th mode and recaptures when `tokens.tsv` is missing.
- **Unbuffered output.** `test_text` writes stdout unbuffered, so a crash keeps its output.

**Corpus.** Nine new host-line entries:
- `num_card`: cardinals;
- `num_year`: years and ordinals;
- `num_frac`: fractions;
- `num_money`: money;
- `num_date`: dates and times;
- `num_sci`: exponents and look-alikes;
- `num_units`: units, punctuation and look-alikes;
- `num_long`: `out`'s long word;
- `num_edge`: one cent, and a unit with a comma.

Totals: 45 sentences, 663 tokens (168 with digits), 955 word calls (7,721 symbols, 81 clause ends, 1 console
message), 2,094 dictionary calls (792 hits, 13 of them user entries; 4,129 symbols), 11 DT_DICT entries, 429 LTS
words (2,725 symbols), 246 clauses, 47,139 frames and 453 klsyn messages. Everything passes in all 15 modes.

**Planted bugs.** All 54 are caught. Two were caught only after `num_edge` was added: "cent" for one cent, and the
period a unit needs.

### 15.28 The text pipeline in C, stage 5: the clause buffer (2026-09-27) — step 11 of the C plan (§13.15) [V: word for word]
**Result.** `src/speech/tx_clause.c` holds the end of the text pipeline:
- `clause_putsym` (0x35f8) and `newclause` (0x3912);
- `newclause`'s DT_LOG phoneme log: `log_clause` (0x3a3e), `phoneme_name` (0x10d9c) and `frames_to_ms` (0xd50c).

The last three are newly named, as is `phoneme_name_values` (0x10f98), which `phtiming` uses on the klsyn side and
which is not in C (~276 of 362). The globals are in §4. `dt_log` stays defined in `ph_clause.c`.

`gen_tx_rom.py` writes a new `tx_rom_sym.c` with two blocks, read by address like the others [superseded: since
§15.31 these are typed tables in `tx_rom.c`, and `tx_rom_sym.c` is gone]:
- ROM `0x1920c-0x1984b`: `sym_table` with the name strings, and `phoneme_name`'s formats;
- ROM `0x1d354-0x1d3d4`: the C library's `_ctype`.

It also names the six strings of `0x12dc8-0x12e46`. `clause_putsym` now takes `(sym, const int16_t *val)`, with
`CLAUSE_RAW` = 1. `parse_phonemic_text` is the one caller that is not in C yet.

**The clause [V].**
- **The buffer.** A clause is one `klsyn_free_pool` message (400 words), of which the ROM uses 199. The first word is
  `0x41` (a phoneme clause) or 0 (`newclause(1)`: the text of a `[:…]` command, one character per word).
- **Posting.** `newclause` sets `nwords` and posts the message to `klsyn_mbox`. It then takes the next message, sets
  `clause_run` 0, `prev_word_stressed` 0 and `wh_question` −1, and clears `clause_silence`.
- **What ends a clause:**
  - `,` `!` `?` `.` (0x42-0x45): stored (or merged, below), then `newclause(0)`;
  - a word boundary once 174 words are used, or any symbol once 199 are: `clause_putsym` first stores `0x42`, so the
    clause ends like a comma;
  - `emit_punctuation_symbol` calls `newclause(0)` again after `.` `!` `?`. The clause it posts is only `[0x41]`, one
    word, which klsyn receives as a phoneme stream;
  - the other callers: `parse_phonemic_text` around a `[:…]` command (`newclause(1)`, the text, `newclause(0)`), and
    the clause scanner at the end of the text (return 0x3308) and on DT_STOP (0x31bc).

**Symbols [V].**
- **Raw (`val` 1).** A character of `[:…]` text is stored as it is. At word 199 the ROM prints "spdef overflow (0
  entries)", stores `0x42` and continues the text in a new message: the first part ends in `0x42`, a `B` to
  `parse_bracket_command`. `clause_marks` has a 216-character `[:dv …]`.
- **With values (bits 12-13 = n).** The symbol and its n value words are stored, with the clause ended first if they
  would reach word 199. Examples: `:vo`/`:ra` from `[:np :ra 400]` (one value), a sung `aa<99,90>` (two). NULL values
  would print "BUG: phone 0x%x has value, no pvalue[]" and hang (`panic`); no caller passes them.
- **Silence (0).** A second silence in a row is dropped (`Say "hi'" now`).
- **The marks** rank `*` 0x3d < word boundary 0x3e < `(` 0x3f < `)` 0x40 < clause start 0x41 < `,` < `!` < `?` < `.`.
  When a mark follows a mark:
  - a weaker or equal one is dropped (for example the second of two word boundaries);
  - a `)` is dropped after a stored `(` or `)` (`clause_bound`): "One ((two";
  - a `(` (the dictionary's phrase mark on function words: "in", "of", "at", …) is dropped unless 26 or more symbols
    were stored since the last `)` `,` `!` `?` `.` or the clause start (`clause_run`); then it becomes `)`;
  - a stronger one replaces the old mark in place. Exceptions: nothing is stored when the old mark is the clause
    start, and when `clause_mark` is not the old mark (below).
- **The mark quirk [V].** `clause_mark` remembers the last `)`-level mark. When a function word's `(` later becomes
  `)` in the same clause, the ROM deletes that earlier mark and puts `)` at the end. The word boundary the earlier mark
  had replaced is lost: in "He said (quietly that the enormous elephants wandered … in the evening", "quietly" and
  "that" are sent with nothing between them. The C does the same.
- **`#`.** A `#` (0x38, between the parts of a hyphenated word) after a word boundary replaces it.

**The DT_LOG phoneme log [V].** With DT_LOG `0x02`, `newclause` writes each clause it posts to the console (`stdout_`,
under `dev_control`'s lock) before posting it:
- **Names.** `phoneme_name` looks each code up in `sym_table`; DT_MODE ASKY (`0x02`) gives the 1-character names.
- **Values.** They follow the name as `<v1,v2>`. A phoneme's first value is a duration in frames, printed in ms: the
  typed `d<100,102>` logs as `d<102,102>`, because 100 ms becomes 16 frames, which prints as 102 ms. A `0x4000`
  symbol prints as `name v ` (`:vo 0`, `:ra 200`).
- **`[:…]` text** prints as `:dv` and the characters.
- **Lines** break before column 79, or at a word boundary (in text, a space) past column 70. A newline follows the
  last symbol and each `.`; in text it follows `;`, and also a capital `E`, which has the code of `.`.
- **Examples from the corpus:** `:vo 0 dhax kw'ihk br'awn f'aaks …`, `:ra 200 'ow?`, `:dv ap 100;`, and with ASKY
  `h'A DEr,`.
- **Shared state.** `phoneme_name`'s value state (`0x82120-0x82126`) is shared with klsyn's `phtiming`
  (`phoneme_name_values`), so the two tasks' logs could disturb each other [I: not tested].

**Test.**
- **Capture.** `phcapture` writes `clause.tsv`, one row per `clause_putsym` and `newclause` call, written at its exit
  (0x3908, 0x3a34). Each row has:
  - the arguments and value words;
  - the clause state at the entry and the exit: the pointers as offsets, the variables, `prev_word_stressed`,
    `wh_question`, DT_LOG, DT_MODE, `phoneme_name`'s state, and the payload up to the furthest pointer;
  - for `newclause`, the message it posts (0x39a8);
  - the console bytes of the outermost call.

  A `newclause` inside `clause_putsym` gets its own row, just before the `clause_putsym` row. Console bytes are
  counted only in the calling task.
- **`test_clause`.** A second harness (`decomp/test/test_clause.c`, built by `build_tx_test.bat`) runs each outermost
  call in C from the imported entry state. It compares:
  - the messages posted (word count and words);
  - the state at the exit, including whether a new message was taken;
  - the console text.

  `test_text` keeps its stand-ins for the lower modes.
- **`check_frames.py`** runs it as the 16th mode and recaptures when `clause.tsv` is missing.
- **Branch coverage.** A scratch script classified every ROM call by the branch it took. The corpus reaches every
  branch of `clause_putsym`.

**Corpus.** Four new host-line entries:
- `clause_log`: the log, with wrapping (one line reaches column 79 exactly), long `[:dv]` text with an `E`, values,
  and silences on both sides of a `[:dv]` clause;
- `clause_asky`: the log with ASKY;
- `clause_full`: a clause full at word 199 inside a phoneme string; values that end exactly at the limit; a `(` after
  25, 26 and 27 symbols; `()` in phonemes; a value between two silences;
- `clause_marks`: the mark quirk, `((`, two closing quotes, and the `[:…]` overflow.

**A pitfall.** The host task acts on an escape sequence as soon as it arrives. The pipeline runs about as fast as
speech, so it can be many seconds behind. A DT_MODE or DT_LOG change therefore applies to text sent before it:
`[d<100,102>aa<250>]` was spelled because ASKY was already on. Change modes before the text, or in their own entry.

Totals: 49 sentences; 9,638 `clause_putsym` calls (109 with values, 405 raw); 161 `newclause` calls plus 345 inside
`clause_putsym`; 506 clauses posted; 618 console bytes. The other modes: 758 tokens (168 with digits); 1,050 word calls
(8,295 symbols, 93 clause ends); 2,253 dictionary calls (839 hits, 13 of them user entries; 4,377 symbols); 11
DT_DICT entries; 460 LTS words (3,019 symbols); 274 clauses; 54,974 frames; 510 klsyn messages. Everything passes in
all 16 modes.

**Planted bugs.** All 40 are caught. Three first tries could never differ, so they were replaced:
- `*clause_last <= 0x45` as `< 0x45`: a stored `,` `!` `?` `.` always ends the clause;
- `< 0x41`: at the clause start the mark is already there;
- the duration limit at code 69 as 68: a `.` never carries values.

Ten bugs were missed at first, and the corpus gained texts for them:
- the exact value limit;
- a `(` after exactly 26 symbols;
- `()` in phonemes;
- a silence carried across a `[:dv]` clause;
- a value between two silences;
- column 79;
- the `E` newline;
- the `:dv` column;
- the value formatting (ms, and `<` then `,`).

### 15.29 The text pipeline in C, stage 5b: phonemic text (2026-09-27) — step 11 of the C plan (§13.15) [V: word for word]
**Result.** `src/speech/tx_phon.c` holds `parse_phonemic_text` (0x3dcc) and the two functions under it, both
newly named (~278 of 362):
- `parse_phoneme` (0x10a9e): one symbol and its values;
- `lookup_phoneme` (0x1100e): a name to a code.

Its callers are the clause scanner (the inside of `[…]`, suffix 0) and `pronounce_dictionary_word` (a user-dictionary
substitution, with the word's suffix code). `ms_to_frames` and `dt_error_flags` come from the klsyn side (`ph_clause.c`,
`ph_frame.c`), `skip_blanks` (0x7c10) is a static copy, and `log_error` is `kprintf` under DT_LOG `0x20`.
`gen_tx_rom.py`'s symbol block now runs from `0x191f2` to `0x19860` and names the new tables and strings.

**`parse_phonemic_text` [V].** It keeps two flags that belong to the caller, so they carry over from one call to the
next: `in_comment` and `in_dv`. It walks the text:
- **Comments.** `/*` opens a comment, even inside `:dv` text. Inside one, the character after each `*` is always
  consumed, so `**/` does not close it ("[ah /* x **/ ah */ ah]" says "ah ah"). A comment can span two `[…]`:
  in "[dh'ih /* split] ignored [still */ s]" the word "ignored" is ordinary text and is spoken. A `*` as the last
  character makes the ROM read past the end of the string [V: code; not tested].
- **`:dv`** (any case) calls `newclause(1)`. The characters that follow go to `clause_putsym(c, 1)` up to `;`, which
  is sent too and followed by `newclause(0)`. With `flush`, the end of the text closes it the same way.
- **Symbols.** Everything else goes to `parse_phoneme`. Codes 0x46-0x63 (the LTS letter codes, `@ ~~`, `:++ :--`, the
  digits, `:##`) and illegal names are rejected: `dt_error_flags |= 8`, nothing sent. `'` sets `prev_word_stressed`
  to 1. `)` becomes a word boundary when `prev_word_stressed` < 0 or the suffix code is 2, 6, 7 or 8, the same rule
  as `pronounce_dictionary_word` (§15.25).
- **Result.** The symbol of the last step, else 0.

**`parse_phoneme` [V].**
- **Values.** A plain symbol may take `<dur>` (ms, stored as frames, code `| 0x1000`) or `<dur,f0>` (`| 0x2000`).
  Blanks are allowed inside; an empty number is 0; the digits accumulate in 32 bits and are clamped to 0x7fff.
- **A bad `<…>`.** When the `>` is missing it logs "bad <dur,f0> format" (DT_LOG error), sets the error bit and skips
  to `>`, `;`, `]` or a newline.
- **`0x4000` symbols** (`:vo :ra :in :re : :pp :cp :se :dr :db`) take a number without `<>`: code `& 0xff | 0x1000`.
  `:ra` is clamped to 120-350, and `:ra 0` is 180.
- **Voice names.** `:np` … `:nv` become `:vo` with the voice number 0-8.
- **Rejected codes.** A code above `:nv` (`:se :dr :db`) sets the error bit and sends nothing.

**`lookup_phoneme` [V].** It returns `sym_table`'s code and flags for the name, or −1:
- **`:xy`.** A colon and two more characters, lower-cased, form one name.
- **ASKY.** Each character is one code, from `asky_codes` (§8.3).
- **Letters.** Otherwise the letters are lower-cased and `ph_letter_class` decides how many belong to the name:
  - consonants that are complete alone (`b c d f g j k p q s t v z` are class 3, `l m n r w y` class 3 as well; the
    punctuation and digits class 2) take a second letter only if it has bit 3 (`h`, `x`) or form `yu`/`rr`;
  - vowels (`a e i o u`, class 1) need a second letter with bit 2 (a vowel, `l m n r w y`, `h`, `x`);
  - a second `h` followed by `x` is left for the next name (`ahx` is `a` + `hx`, so it fails; `dhx` is `d` + `hx`).
- **Illegal.** It sets the error bit and logs `Illegal phoneme "%s"`. In several fail paths (a short `:x`, ASKY, a
  class-0 first letter, a vowel without a partner) the ROM's name buffer is not fully set, and the message prints
  old stack bytes after the first character. The C prints only what was set; the corpus logs only complete names.

**Test.**
- **Capture.** `phcapture` writes `phon.tsv`, one row per call, written at the exit (0x4058). Each row has:
  - the arguments and the text;
  - the clause state (as in `clause.tsv`) and `dt_error_flags` at the entry and the exit;
  - the result, `*in_dv` and `*in_comment`;
  - the messages posted during the call;
  - the console bytes.
- **The error bits of other tasks.** The capture also records the error bits other tasks set while the call was
  switched out. A post wakes klsyn, whose `[:dv]` parser sets bit 8 for "ap=9" in the middle of a call.
- **`test_clause --phonemic`.** It runs each call in C from the entry state, with the clause buffer and
  `ms_to_frames` in C too. `test_clause` now links the klsyn side (`ph_*.c`).
- **`check_frames.py`** runs it as the 17th mode and recaptures when `phon.tsv` is missing.

**Corpus.** Four new host-line entries:
- `phon_misc`: comments, `:ra` clamps, voices, `:DV`, rejected codes, `<…>` edge cases;
- `phon_log`: the two messages with DT_LOG error;
- `phon_asky`: ASKY input;
- `phon_dict`: a user entry `)bl'ihks` with suffixes.

Totals: 53 sentences, 143 `parse_phonemic_text` calls (2,050 characters; 8 set the error bit), 27 clauses posted from
inside them, 296 console bytes. `--phonemic` passes on all 53. The whole corpus was recaptured; the four new entries
pass in all 17 modes, and the other entries were not rerun in the other 16 modes, since nothing below this stage
changed.

**Planted bugs.** A run of 47 was started and stopped unfinished (each rebuilt the whole harness; too slow), so this
stage has no planted-bug count.

### 15.30 The text pipeline in C, stage 6: the clause scanner and dttask (2026-09-27) — step 11 of the C plan (§13.15) [V: end to end]
**Result.** The text pipeline is complete in C. `src/speech/tx_scan.c` holds `dttask_main` (0xf946) and three
functions, newly named (~281 of 362):
- `clause_readin` (0x3182): the clause scanner;
- `readin_flush` (0x3580): hands a word on;
- `dttask_getc` (0xf9ea): the scanner's getc.

`src/kernel/console.c` holds the ROM's console printer: `kprintf`, `vformat_string`, `console_print_decimal`,
`console_print_string` and `console_putchar_caret`. The RTOS calls it needs (`stream_t`, `dev_getc`, `dev_putc`,
`dev_control`, `pipe_open`) are declared in `src/kernel/rtos.h`. `gen_tx_rom.py` names the class table and the three
strings. `spell_chars` is now exported from `tx_word.c`, `dt_mode` starts at its power-up value 1, and `spell_buf` at
its `.data` value `" ?"`.

**`dttask_main` [V].**
- It opens the text pipe (0x40 bytes; the host side writes through `cur_stream`) and a pool of 2 messages.
- It then loops: `clause_readin(dttask_getc, dttask_in)` speaks the text up to the next `0x1A`, and a one-word `0x68`
  message to klsyn marks the sync point (DT_SYNC, §15.23).
- **`dttask_getc`.** It returns -1 once the `0x1A` has been read (`dttask_eof`). Under DT_LOG `0x01` it echoes every
  character it reads with `console_putchar_caret`.

**`clause_readin` [V].** It starts with `newclause(0)` and resets its state; `readin_in_dv` is never reset. Each
character's class comes from `readin_class` (§9). The steps for each character, in order:
1. **After the last `]`.** A `, ! ? .` right after it goes straight to `emit_punctuation_symbol`, so `[ah].` ends the
   sentence.
2. **A mark, then a line end.** Outside `[ ]`, a `, ! ? .` followed by a line end or the end of the text (not a space)
   flushes the word and calls `token_dispatch(NULL)`.
3. **Ends of a word.** A blank outside `[ ]`, a line end or the end of the text flushes the word: `readin_flush` hands
   it to `token_dispatch`, or inside `[ ]` to `parse_phonemic_text` (flush = 1 at the end of the text).
   - The end of the text is NUL, VT (CTRL-K), DEL or the `0x1A` marker. It also closes open phonemic text with a
     word boundary, calls `token_dispatch(NULL)` and posts the clause (`newclause(0)`); at the marker the scanner
     returns.
4. **The action** (class bits 2-3):
   - **Stored:** printable characters, and inside `[ ]` blanks (a tab is stored as a space).
   - **Backspace.** It moves back one character. What is typed next overstrikes only when its rank (class bits 0-1:
     letters and digits 2, punctuation 1) is not lower, and the characters after it stay. So "abc", two backspaces,
     "xy" reads "axy", and "a.", a backspace, "!" keeps the "!".
   - **Brackets.** `[` and `]` open and close phonemic text only with DT_MODE SQUARE; `0x02` and `0x03` do so
     always. The power-up message uses them for that reason: dttask reads
     `\x02:np :ra 180\x03DECtalk version one point eight is running.`. Brackets nest, and each `]` sends a word
     boundary.
   - **Spoken control characters.** The other control characters (`0x01`, `0x04`-`0x07`, `0x0e`, `0x0f`,
     `0x10`-`0x1f`, and a backspace at the start of a word) are spoken as "control" and the letter (`c + 0x40`,
     through `spell_chars`), an ESC as "escape", each followed by a comma.
   - **What arrives.** The host line drops SO, SI and DEL and parses ESC, so from there "escape" is never heard;
     the local terminal passes ESC.
5. **A word that is too long.** At 79 characters the word is cut. Under DT_LOG `0x80` the ROM logs "long word at
   clause readin" with the first 79 characters, then speaks them as one word and goes on with the rest.

**Unreachable in practice.** Both lines strip bit 7 before the text pipe (with the default settings), so bytes
0x80-0xff never reach the scanner. If one did, the class table would be read past its end, into the strings that
follow it; for example 0x84 would be spoken as "control" [V: code; the emulator delivered 0x84 as 0x04].

**The console printer [V].** `vformat_string` knows `%d`/`%D` (signed decimal), `%c` and `%s`. Any other letter
after `%` is printed and takes no argument (§12.49), and a `%` at the very end takes the NUL and reads on.
`console_putchar_caret` shows:
- control characters as `^X` (except BS and LF);
- LF as CR LF;
- bit 7 as a `~` before the low 7 bits;
- ESC, DCS, CSI and ST as `<ESC>` `<DCS>` `<CSI>` `<ST>`.

`console_print_string` sign-extends each character. The formats with `%x`/`%o` in the text pipeline are not reached
by any corpus entry. The "no spell" message is not reachable from text: every printable character has a spelling,
and control characters never get into a word.

**Test.**
- **Capture.** `phcapture` writes `scan.tsv` from reset on, the boot included, one event per line in the order they
  happen:
  - G: each character `dttask_getc` reads (0xfa14);
  - P: each message dttask posts (`mbox_put` 0x860; the pool's own filling is skipped);
  - C: each console byte dttask writes;
  - M: DT_MODE and DT_LOG whenever dttask runs with new values;
  - U / X: each `dict_hash_insert_or_delete` and `dict_hash_clear_all` call (the host task).
- **`test_dttask`.** It runs the C `dttask_main` from its power-up state on the same characters. The other tasks'
  changes are applied where dttask waited when they happened: at the next `dev_getc`, or at the next `mbox_get`
  after as many posts. Every post is compared word for word, and every console byte, the DT_LOG echo and phoneme log
  included.
- **Other harness changes.**
  - `check_frames.py` runs `test_dttask` as the 18th mode and recaptures when `scan.tsv` is missing.
  - `console.tsv` now tells which bytes klsyn wrote, so `test_frames --task` checks klsyn's whole console text
    (§12.49).
  - `test_clause` now links the real `console.c`.
  - `make_reference.py` writes control characters escaped in `corpus.tsv`.

**Corpus.** Five new entries, 58 in all:
- `readin_edit`: backspace overstrike, nested brackets, a mark after `]`, `0x02`/`0x03`, tabs;
- `readin_ctl`: spoken control characters, the DT_LOG `0x01` echo and a cut 84-character word under DT_LOG `0x80`;
- `readin_mode`: DT_MODE 0 (`[ ]` as text, `0x02`/`0x03` still phonemic);
- `readin_open`: text that ends inside `[ ]` (CTRL-K);
- `readin_esc` (local terminal): "escape".

**Results.**
- **End to end.** `test_dttask` passes on all 58 entries: 12,000 characters read, 921 messages posted (15,318 words),
  1,108 console bytes, 183 DT_MODE/DT_LOG changes and 12 DT_DICT changes applied. (Since the NVRAM fix, §15.33, the
  boot text is shorter: 9,970 characters, 689 messages, 12,650 words, 181 mode changes.)
- **All modes.** The whole corpus was recaptured and passes in all 18 modes.
- **Planted bugs.** A small, quick run: 8 mutants in `tx_scan.c`, each relinking one object and running the 58
  captures. Two were missed at first:
  - one was equivalent (`,` already has the `0x10` bit, so the ROM's extra `c == ','` test changes nothing) and was
    replaced;
  - the other, the word boundary at the end of the text inside `[ ]`, led to `readin_open`.

  Final: 8 of 8 caught.

### 15.31 The speech tables as named, typed C tables (2026-09-27) — §13 item 15 [V: all modes pass]
**Result.**
- **No more ROM addresses in speech.** No speech code reads by ROM address any more; every table is a named C object
  with its own type and exact size.
- **Generated, then kept as source.** `gen_ph_rom.py` and `gen_tx_rom.py` still write the tables from the ROM, but
  their output is source: the build never needs the ROM.
- **Test-only address map.** `decomp/test/tx_rom_map.c/.h` holds the ROM address of each text table and the
  `ROM_…` address constants. The harnesses use it to turn a capture's ROM addresses into C pointers and back; the
  product code does not link it.

**The klsyn side (`ph_rom.c`).** Each table has its own array:
- **Per-phoneme tables:** `featb`, `inhdr`, `mindur`, `begtyp`, `endtyp` and the rest, `[NPH]` = 56 each.
- **Targets:** `maltar`/`femtar` are `[7][NPH]` (rows F1 F2 F3 B1 B2 B3 AV), and `phsettar`'s `row` counts rows.
- **Burst amplitudes:** `malamp`/`femamp` are `[16][4][6]`: set, transition type 1-4 (5 uses 3), A2-AB. The ROM's
  `burst_amp_index` holds the short offset 1 + 24 (set − 1); the table is now `burst_amp_set`, holding the set
  number (0 = none).
- **Locus tables:** `maleloc`/`femloc` are `[50][3]` of `ph_locus_t {locus, pct_q14, durtran}`, one per F1-F3. The
  ROM's `plocu` holds 1 + 9 (set − 1); `plocu[3][NPH]` now holds the set number.
- **Diphthong lines:** `maldip`/`femdip` stay flat, because the targets hold offsets into them.
- **Left out** (the generator asserts they are 0, or no code reads them):
  - the leading 0 word of the locus and amplitude tables;
  - `pht0draw`'s three messages, which are literals in `ph_frame.c`;
  - six unreferenced words at `0x16d44`.
- **`divtab` has the 20 ROM words before it** (`divtab_ext`). `phsettar` reads `divtab[-1]` when two rescaled
  diphthong time points step back a frame, and the ROM then gets the last two bytes of `partyp`.

**The text side** (`tx_rom.c`, `tx_rom_lts.c`, `tx_rom_dict.c`).
- **Strings:** 66 named `char` arrays: the formats, the scanner's words and the number engine's `s_…` words.
- **Class tables with explicit index ranges:**
  - `readin_class` covers −1 to 0xfe;
  - `char_class` covers −128 to 127 (the bytes before `0x13a9c` included; a signed char indexes it);
  - `ctype_tab`, `punct_syms`, `dash_syms`;
  - `lts_ascii_map`: its first 31 entries are the last suffix-tail pointers and "Illegal suffix" in the ROM, kept
    as they are.
- **Symbols:** `sym_table[121]` of `tx_sym_t {ascii, pad, name, code}`, plus `sym_max`, `asky_codes[95]` and
  `ph_letter_class[95]`.
  - Record 119 ends the list.
  - Record 120 is the ROM's overlap: `sym_max`, then `ph_letter_class` read as a name pointer `0x02020202`. On the
    24-bit bus that is ROM `0x020202`, which reads as "n^".
- **Suffix tails:** `tail_ly` … `tail_t` and `dict_tails[9]`, whose entries 5-8 overlap the ASCII map in the ROM.
- **Number engine:**
  - `pat_number`/`pat_money` are `num_pattern_t {pattern, cls}` lists over 34 pattern byte arrays;
  - `num_ones`, `num_tens`, `num_ordinals`, `num_scales`, `num_months` and `num_punct_words` are string lists;
  - `num_units` entries hold their three NUL-separated parts ("cm\0centimeter\0centimeters").
- **LTS:**
  - `lts_class_masks[31]` and `lts_code_features[100]` (`uint32_t`);
  - `lts_rules` (byte records, plus the one byte the ROM reads ahead after the last rule);
  - `lts_rule_deltas[374]` and `lts_rule_count`.

  The engine walks `lts_rules + off + 1`, where the ROM formed `LTS_RULES + off` with off = −1 for rule 0.
- **Dictionary:**
  - `dict_chars` (the character lists; the last two bytes are the empty node of letters without words);
  - `dict_links` (native shorts, parallel to the characters);
  - `dict_prons`;
  - `dict_root[91]` of `{chars, links, prons}` pointers.

  `lookup_word` walks pointers where it walked addresses. The generator checks that each root lands in the three
  arrays, and that the empty node can never yield a pronunciation.

**Verification.**
- **AddressSanitizer.** The four harnesses were built with `/fsanitize=address` and run in all modes over the whole
  corpus. With the tables split, a read outside any table stops the run. That found `divtab[-1]`, the LTS
  read-ahead and the cut string (§12.50).
- **Standard check.** After the fixes, the AddressSanitizer runs are clean and all 58 entries pass in all 18 modes of
  `check_frames.py` with the normal builds.

### 15.32 DTMF tones: the power-up self-test and DT_PHONE dialing (2026-09-27) — §13 items 2 and 15 [V]
**Why.** The unit plays DTMF tones at power-up, before the announcement (user, 2026-09-27), but no emulator log had
a tone frame. `spclog -b` (new option: log from reset on) confirmed the emulator sends none, and the ROM explains why.

**The DSP tone command** (`dsp_command_queue_isr` `0x12258`, §16.3, §16.7):
- a queue item whose header has bit 15 set goes out as two words, `w[0]` and `tone_arg` (`+0xe`);
- each word is **`0x8000 | Hz` or `0x9000 | Hz`**: bit 12 picks one of the DSP's two oscillators, the low 12 bits
  are the frequency in Hz (which DSP oscillator is which is not traced, [I]);
- the ISR then arms the tick hook `0x82272`: after **16 ticks** (160 ms, 10 ms ticks) `dsp_tone_timeout_hook`
  `0x124a2` sends `0x8000` `0x9000` (0 Hz, silence), and **6 ticks** (60 ms) later `dsp_tone_done_hook` `0x124fe`
  returns the message to its sender and writes `0x40` to the SPC control.

**DT_PHONE dialing.** `dcs_command_dispatch` → `dt_phone_command` `0xe5f6` (sub-commands 0/10/11/20/21/30/40/41, §5.3;
status reply `send_dcs_reply(70, off-hook)`) → `phone_dial(tone, text)` `0xeb54`. The dial text is collected by
`dcs_text_putc` `0xeecc` into `0x81f66`.
- If on hook: go off hook (`dev_control(phone, 0x80000)`), then wait 2 s and poll every 1 s until the line
  state `0x822d6` is 1.
- `!`: 1 s per `!`. `^`: hook flash of 250 ms per `^` (`0x40000`/`0x50000` = hook relay), then 250 ms.
- Pulse (`41`): per digit n (0 = 10), n breaks of 60 ms on / 40 ms off, then 800 ms. Other characters are skipped.
- Tone (`40`): one queue item per digit, `w[0] = dial_tone_high[d−1]` (`0x1829e`) and `tone_arg = dial_tone_low[d−1]`
  (`0x182be`); `phone_dial` waits for the item to come back, so each digit is 160 ms of tone and 60 ms of silence.

  | Digit order (index 1-16) | High word | Low word |
  |---|---|---|
  | `1 2 3` / `4 5 6` / `7 8 9` / `0 * #` | `0x84B9` 1209, `0x8538` 1336, `0x85C5` 1477 (`0` = 1336, `*` = 1209, `#` = 1477) | `0x92B9` 697 / `0x9302` 770 / `0x9354` 852 / `0x93AD` 941 |
  | `A B C D` | `0x8661` 1633 | 697 / 770 / 852 / 941 |

  These are the standard DTMF frequencies exactly.

**The power-up self-test** (`reset_entry` `0x1f6`). The reset code reads DUART input port bit 4 (IP4, `0x9801b`). If
it is low (the "skip self-test" jumper) it jumps straight to `boot_init` `0x1024`. Otherwise it tests, blinking an
error code on the LED (`0x1cc`) at the first failure:
1. the CPU registers (`0x224`) and the ROM checksums (`0x270`);
2. the RAM `0x80000-0x8c000` (`0x33e`; LED `FE`);
3. the DUART (`0x464`): the status registers after reset, both channels in local loopback (`0x660`), the
   counter/timer interrupt (LED `FD`);
4. the DSP (`0x526`): the SPC flags, then a `0x6000` frame with a bad checksum (`0x16c`, ending `0x20A0`; error bit 5
   expected) and a good one (`0x19c`, ending `0x2000`; no error);
5. **a DTMF loopback** (`0x580`). For each of the 16 pairs at `selftest_dtmf_pairs` `0x11a`:
   - send `0x9000` `0x8000` (tones off);
   - wait until the receiver drops its "tone present" bit (`0x9c005` bit 7);
   - write `0x40` to `0x9c004`, send the pair (`0x9xxx` high tone, `0x8xxx` low tone);
   - wait for the TLC interrupt (level 4, `selftest_tlc_isr` `0x7f8`). It accepts the flags word only with bit 8
     clear and bits 7 and 6 set, reads the receiver's 4-bit code from `0x9c007` and clears `0x9c004`;
   - compare the code with `selftest_dtmf_codes` `0x15a`.

   The digits are **`0 1 2 3 4 5 6 7 8 9 * # A B C D`** (codes `0A 01 … 09 0B 0C 0D 0E 0F 00`, the standard
   16-code receiver map of §5.3). The tone generator is the DSP and the decoder the telephone chip, so this tests
   both.
6. Then `reset` and `jmp boot_init`: the boot and its announcement follow.

So the tones heard at power-up are the self-test's 16 digits; each lasts until the receiver has decoded it (no fixed
duration in the code).

**The emulator: self-test mode (2026-09-27) [V].** By default `native/duart2681.c` holds IP4 low, as MAME's driver
does, so the emulator skips the self-test. Run with IP4 high and nothing else, it stopped at the DUART test (LED `FD`
blinking, no SPC write in 30 s). **`dtc01_set_selftest(m, 1)`** (call it after `dtc01_create`, before running; `spclog -t`)
now boots as a unit with the jumper open. It turns on:
- **IP4 high.**
- **The board's RESET line.** The 68000 `RESET` instruction resets the DUART and the SPC/TLC latches (Musashi's
  `M68K_EMULATE_RESET` is now on; the callback does nothing outside this mode). The input pins keep their levels.
- **The DUART's accurate mode** (`duart_set_accurate`):
  - the command register as the datasheet gives it (bits 6-4: 1 reset MR pointer, 2 reset receiver, 3 reset
    transmitter, 4 reset error status). The default mode keeps the old decoding, where 3 resets the MR pointer;
  - local loopback (MR2 bits 7-6 = 10) through a holding and shift register, each character taking its time at the
    CSR baud rate (both baud-rate sets, 5-8 bits, parity, stop bits) and arriving cut to its length;
  - TxRDY/TxEMT from that model, and interrupt updates after CR and THR writes.
- **The DTMF receiver on the TLC.** Goertzel filters on the eight DTMF frequencies listen to the DAC output, over
  256-sample blocks (25.6 ms):
  - a digit counts when the signal is above about 1% of full scale, the two tones carry at least half of it, each
    is 6 dB above its group's runner-up, and the twist is within 9 dB;
  - two blocks in a row raise StD (`0x9c004` bit 7) and latch the code (`0x9c007`); two blocks without drop StD;
  - level 4 is raised while StD is set and the tone interrupt is enabled (latch bit 6). Ring detect is not
    modeled.
- **Byte writes to the SPC/TLC registers** (`move.b` to `0x9c001`) write the whole word, the byte on both halves as
  the 68000 drives it. This one is not gated: only the self-test and its error loop do it.

**Result.** The whole self-test passes, and the boot continues as usual (`unmapped` 0):
- **0-3.0 s:** CPU, ROM, RAM, then the DUART loopback and counter tests.
- **3.0 s:** the DSP frames: the bad-checksum one first, then the good one.
- **3.0-4.6 s:** the 16 digits `0 1 2 … 9 * # A B C D`, in the ROM's order (codes 10, 1-9, 11-15, 0). Each sounds for
  about 50 ms (until the receiver has it), with about 50 ms of tones-off between digits.
- **4.66 s:** `boot_init` (LED `DA`), then the announcement from about 5.3 s.

In the audio each digit's two frequencies stand far above the other six. The DSP makes the high tone about 6 dB
stronger than the low one, and the peak level is about 74% of full scale. `spclog -t -b` logs:
- the two test frames (`0x6000` … `0x20A0`, bad checksum; `0x6000` … `0x2000`);
- 32 tone commands (`9000 8000` before each pair, then `9538 83AD` = `0`, and so on), from PC `0x626`.

**Default mode unchanged.** With the mode off, all 58 reference frame logs regenerate byte-identical, and a
recaptured corpus passes every `check_frames.py` mode.

The same receiver model serves the host-terminal phone design (§13 item 15) and the DSP C model's tone mode.

### 15.33 The NVRAM settings record and the "NVR fault" at power-up (2026-09-27) [V]
**Question (user).** The emulator's power-up announcement is "DECtalk version one point eight is running. NVR fault.
Using factory settings." Is the second part a leftover emulator limitation?

**Where it comes from.** `main_task` `0x2700` speaks the version line. It adds `"NVR fault."` and `"Using factory
settings."` when `dt_error_flags` (`0x81f12`) bit 2 (`0x0004`, the DSR "last NVR operation failed" flag) is set.
`settings_reset` `0xf3be` sets that bit at power-up when `nvram_load_settings(rec, 0)` `0xf69a` rejects the
user record; it then loads the factory record instead (`"Bug: no factory settings"` and a panic if that fails too).

**The record** (read by `nvram_load_settings`, written by `nvram_save_settings` `0xf79c`):
- The X2212 holds 256 nibbles, in the low 4 bits of the even bytes `0x94000-0x941fe`. They form **64 words**; word
  k's nibble j (least significant first) is at `0x94000 + 2·(4k + j)`. `nvram_save_settings` then writes 1 to
  `0x94200` (the store trigger).
- Words:
  - 0 = **record version, 4 in v1.8**;
  - 1 `DT_LOG`, 2 `DT_TERMINAL`, 3 `DT_MODE`;
  - 4-5 the two lines' speed codes (factory 6 and 11 = 1200 and 9600 baud, the DUART's CSR codes) and 6-7 their
    formats (both 2);
  - 8 S7C1T/S8C1T (`0x822e8`), 9 DECTC1 (`0x822e6`), 10 the SET INTERRUPT character (`0x822e4`, §15.37);
  - 11 host speak (`0x822d2`), 12 SET HOST MODEM (`0x822d0`);
  - 13-62 zero;
  - 63 the checksum.
- **Checksum** `nvram_checksum` `0xf7ca` over words 0-62: u = `0xFFFF`; per word u = (w ⊕ u)·2, plus 1 if bit 16
  is then set; the low 16 bits count.
- The user record is accepted only if the checksum matches **and** word 0 is 4.
- The **factory record** `factory_settings` `0x189da` is the same 64 words with version 4 and no checksum (it is
  used directly):
  `4, 0, 6, 1, 6, 11, 2, 2, 1, 1, 0, 1, 0, …`. With version 4 its checksum would be **`0x352D`**.

**The emulator's image.** `native/dtc01.c` `DEFAULT_NVRAM_*` was decoded from **v2.0**'s embedded copy (its own
comment says so, and that MAME's notes expect a different v1.8 image). Read as a v1.8 record, it is exactly the
factory record, except:
- word 0 is **5** (v2.0's record version);
- its checksum is `0xB52D`, which is valid for version 5.

So v1.8 rejects it on every emulated boot, sets the fault flag and loads the same values from ROM: the settings end
up identical, but the announcement gains the fault sentences (about 3.9 s of speech), and DSR reports error 24.

**Checked** with a scratch build whose image has word 0 = 4 and checksum `0x352D`: the nibbles at `0x00` and
`0xFF` change from 5/B to 4/3. At power-up:
- `dt_error_flags` is `0x0000` (it was `0x0004`);
- the boot speech ends at 3.00 s instead of 6.90 s: only "DECtalk version one point eight is running.";
- no unmapped access.

A real v1.8 unit whose NVRAM holds a record it saved itself (SETUP `SAVE`, DECNVR store) boots without the fault
sentences. The fault is an artifact of the emulator's v2.0-derived NVRAM, not of the ROM or the hardware.

**Fix (applied 2026-09-27).** `native/dtc01.c` `nvram_default_v18` builds v1.8's default image from the ROM itself:
- it recognizes v1.8 by `"one point eight"` at `0x12c66`;
- it takes the factory record `0x189da` (version 4) and adds its checksum (`0x352D`): exactly what `nvram_save_settings`
  writes after a factory reset.

Any other ROM keeps the old table (v2.0's image). The user chose to keep a built-in default rather than an NVRAM
file (§13 item 15).

**Results.**
- **Boot:** the emulator's v1.8 boot now says only "DECtalk version one point eight is running." (`dt_error_flags` 0,
  speech over at 3.00 s instead of 6.90 s). Self-test mode is unaffected (16 digits, then `boot_init`).
- **References:** the change moves every later cycle stamp, so `decomp/reference/*.frames.tsv` were regenerated
  (`make_reference.py`) and the corpus recaptured; the C passes all 18 `check_frames.py` modes on all 58
  entries.
- **What changed in the logs.** Besides the cycle stamps, 1,728 of the 65,725 frames changed words:
  - the first 20-50 frames of each log: the first utterance glides from where the boot speech ended, now "running."
    instead of "settings.";
  - in a few logs, F1 in the last frames of an utterance. These are probably values the ROM reads from what earlier
    speech left in its buffers [I].

  Both are the ROM's behaviour. The captures hold the RAM, so the C matches them.
- **`test_dttask` totals** (the boot text is shorter): 9,970 characters, 689 messages (12,650 words), 1,108 console
  bytes, 181 DT_MODE/DT_LOG and 12 DT_DICT changes.

### 15.34 The phone task's side of DT_PHONE (2026-09-27) — §13 item 2 [V: read from the ROM]
**The telephone interface (TLC)** sits at `0x9c004`/`0x9c006` and has its own interrupt, level 4.
- **Written bits of `0x9c004`:** 14 ring interrupt enable, 8 hook relay (1 = off hook), 6 DTMF-tone interrupt enable.
- **Read bits of `0x9c004`:** 15 ring signal, 7 tone present (StD).
- **`0x9c007`:** the DTMF receiver's 4-bit code (low byte of the word at `0x9c006`).

`phone_init_impl` `0x22ba` builds `phone_dev` `0x80552`: a character device whose input carries events and keys,
with ops table `phone_dev_ops` `0x2632` and tick hook `phone_ring_poll`. It installs `phone_tlc_isr` `0x214e` in
`0x8060a` and writes `0x4000`.

**Device ops.** `dev_control(dev, n << 16, …)` runs `dev->ops` entry n (`0xfaa`: count, then offsets from the
table); a negative n runs generic op −n (`0xe40`).

| Code | Handler | Effect |
|---|---|---|
| `0x20000` (op 2) | `phone_keypad_enable` `0x2106` | `0x9c004 \|= 0x40`: caller keys interrupt (DT_PHONE 20) |
| `0x30000` (op 3) | `phone_keypad_disable` `0x2118` | clears it (DT_PHONE 21) |
| `0x40000` (op 4) | `phone_hook_release` `0x212a` | clears bit 8: on hook; pulse-dial break |
| `0x50000` (op 5) | `phone_hook_seize` `0x213c` | sets bit 8: off hook; pulse-dial make |
| `0x70000` (op 7) | `phone_set_idle` `0x1ff4` | state 2 (on hook, idle), ring poll off, `0x9c004 = 0x4000` |
| `0x80000` (op 8) | `phone_go_offhook` `0x2034` | state 3, ring poll off, posts `0x82`, `0x9c004 = 0x100` |
| `0x90000, n` (op 9) | `phone_start_answer` `0x2084` | count n rings (below), `0x9c004 = 0` |
| `-0x60000, t` (generic 6) | `0xf44` | input timer: a waiting `dev_getc` returns `0x80000` after t ticks without input (`dev_rx_timeout_hook` `0xdbc`) |
| `-0x70000` (generic 7) | `0xf44` | input timer off |
| `-0x80000, v` (generic 8) | `0xef6` → `dev_rx_post` `0xcf6` | inject v into the device's input |

Pulse dialing (`phone_dial`) uses ops 4/5 for 60 ms break and 40 ms make, the standard 60/40 ratio. A hook flash
(`^`) is op 4 for 250 ms.

**The interrupt** `phone_tlc_isr`:
- idle (state 2) with the ring signal: posts `0x81`;
- off hook (state 3) with tone present: posts the receiver code (0-15);
- otherwise nothing.

**Answering: `phone_ring_poll`.** The poll runs every 4 ticks (40 ms). While counting, `line_state` holds the ring
signal level (0 or `0x8000`).
- A ring counts when the signal falls after at least two polls high.
- After the wanted number of rings: hook relay on (`0x9c004 = 0x100`, state 1). 2.5 s later: state 3, post `0x82`.
- If the signal does not change for 250 polls (10 s): back to idle, post `0x86`.

**The task `phtask_main`** (`0x822d6` = off hook, reported as R3):
- **Idle:** on hook, input timer off; wait for an event.
  - `0x81` ring: ignored unless `0x822da` (rings to answer, DT_PHONE 10) is set. Then `phone_start_answer(rings)`.
    On `0x82`, set off hook, then:
    - in stand-alone mode (`0x822d4`, below): run `dtmf_diagnostic_menu`, emit `"dtphon (fsm)"`, hang up, wait
      2 s;
    - otherwise reply **R3 = 1** and enter digit mode.
  - `0x82` without a ring (after `phone_go_offhook`, i.e. dialing): off hook, digit mode.
  - `0x84` (DT_PHONE answer, RIS / power-up reset via `settings_reset`, first host byte), `0x85` (DT_PHONE
    hangup), `0x86` (ringing stopped): hang up (op 7), off-hook flag 0, **wait 2 s**; after `0x85` reply
    **R3 = 0**. That is the manual's "reply delayed until on-hook".
- **Digit mode:** the 1 s input timer runs (op −6, 100 ticks).
  - Codes 0-15: reset the idle-second count `0x81d6e`. Unless `phone_dial` is dialing (`0x81d6c`, so the unit
    ignores its own tones), send `"D1234567890*#ABC"[code]` to the host as a plain character
    (`host_line_putc`).
  - `0x80000`: one more idle second. When the DT_PHONE 30 timeout `0x822d8` is reached, clear it, emit
    `"dtphon (timeout)"` and reply **R3 = 2**.
  - Any other event: timer off, keypad off (op 3), and the event is handled as in idle.

**Stand-alone phone mode at power-up.** `speech_init` calls `settings_reset(3, 0)`, which sets `0x822d4`, and
`main_task` then sets rings to answer = 1.
- Until the host sends its first byte other than XON, the unit answers the first ring and gives the caller the spoken
  DTMF diagnostic menu (`*` factory settings, `#` self-tests).
- That first byte (`read_host_byte` `0xefc8`) clears both and posts `0x84`, so from then on the host decides
  (DT_PHONE).

**The emulator's telephone line (2026-09-27) [V].** `native/dtc01.c` now models the line. With none of the calls
used, it stays quiet, and all 58 reference logs regenerate byte-identical.
- **API:**
  - `dtc01_phone_ring(m, n)`: n rings in the US cadence (2 s on, 4 s off) on TLC bit 15. Ringing stops when the unit
    goes off hook, as the exchange does.
  - `dtc01_phone_ringing(m)`: the ring signal now.
  - `dtc01_phone_offhook(m)`: the hook relay (latch bit 8).
  - `dtc01_phone_line_in(m, pcm, n)`: far-end audio, 10 kHz, a 3.2 s queue.
  - `dtc01_phone_keys(m, "12#", on_ms, off_ms)`: the caller's keys, synthesized as DTMF tones at −10 dBFS each into
    the far-end queue.
- **The DTMF receiver hears the line.** While off hook, that is the unit's own output plus the far-end audio, so it
  also decodes the unit's own dialing, which `phtask_main` ignores. In the self-test mode it hears the DAC; on hook
  it hears silence, and far-end audio waits.
- **The TLC interrupt is latched.** It is requested when "tone present and bit 6" or "ring and bit 14" becomes
  true, and cleared when the 68000 reads `0x9c004` (both ROM handlers read it first). `phone_tlc_isr` never clears the
  source, so a level-triggered line would take the interrupt again and again for as long as a ring or a key lasts.
  Read `0x9c004` has the ring signal in bit 15.
- **`hostfeed.h`** (spclog and phcapture `-H`) has `\W` (wait 5 s), `\g`/`\gN` (ring once or N times) and
  `\kKEYS;` (caller keys), so the existing tools can script a call. `\w` is unchanged. Until §15.36 a `\g` or `\k`
  right after a pause fired at the pause's start; it now fires at its end.

**Checked** (scratch harness, from the end of the boot):
- *Stand-alone mode* (no host byte yet):
  - the ring runs 0-2 s; the unit goes off hook as the ring ends and starts the spoken menu 2.5 s later;
  - the caller's `5` and `*` are decoded (codes 5 and 11), and the unit answers each with speech.
- *Host control:*

  | Host or line | Result |
  |---|---|
  | DT_PHONE `10;1` (answer on 1 ring) | `ESC P ; 70 ; z ESC \` (R3 = 0) at once |
  | 1 ring | off hook at the ring's end; R3 = 1 2.5 s later |
  | `20`, `30;3` (keypad on, 3 s timeout) | R3 = 1 for each |
  | caller `12#` | the host receives `1`, `2`, `#` as plain characters |
  | 3 s without a key | R3 = 2 |
  | `11` (hang up) | on hook at once; R3 = 0 2 s later |
  | `40` + `5551234` (tone dial) | off hook, 2 s, then 7 digits (decoded by the receiver but not sent to the host), then R3 = 1 |

- The self-test mode still passes (16 digits, boot at 4.66 s).
- A call scripted through `spclog -H` (answer on 1 ring, `\g`, keypad on, `\k12#;`, tone dial `123`) logs each dialed
  digit as a tone command (`84B9 92B9` = 1209 + 697 Hz for `1`), and the silence pair `8000 9000` 159 ms later. That
  confirms the 160 ms of tone and 60 ms of silence read from the code (§15.32).

This is what the manual describes for DT_PHONE (§5.3), including the delayed hang-up reply.

### 15.35 The host task in C (2026-09-27) — §13 item 15 [V: end to end]

**Scope.** The first part of the host terminal is C: the `host` task, i.e. `host_task_main` and everything it calls
that is not kernel or speech. It is checked end to end against the ROM, from every byte read on the host line to
everything the task writes and does.

| File | What |
|---|---|
| `src/host/host.h` | the parser state (`host_seq_t`, the 0x81f2c layout, also the reply layout), the settings and phone variables with their ROM addresses, device ops |
| `src/host/hs_parse.c` | `host_task_main`, `read_host_byte`, `read_host_char_collect_csi`, `handle_single_shift`, `consume_control_string`, `is_graphic_char`, `output_graphic_char`, `dispatch_esc_command`, the loggers `log_error`/`log_trace`/`log_debug` |
| `src/host/hs_command.c` | `csi_command_dispatch`, `dsr_reply`, `dcs_command_dispatch`, `dcs_text_putc`, `parse_dict_entry_command`, `dt_phone_command`, `phone_dial`, `dectst_self_test`, `print_rom_date`, `dt_stop`, `emit_sync_marker` |
| `src/host/hs_reply.c` | `send_control_sequence`, `send_escape_response`, `send_dcs_reply`, `host_line_putc`, `uint_to_decimal_string` |
| `src/host/hs_settings.c` | `settings_reset` (formerly `spdef_settings_io`), `line_configure`, `nvram_load_settings`, `nvram_save_settings`, `nvram_checksum` |
| `src/host/hs_rom.c` | the tables, typed: `dec_supplemental_ascii[96]` (0x1823e), `da_reply` (0x18304), `dial_tone_high/low[16]`, `factory_settings[64]`, `line_format_codes[3]`, the ROM dates; strings are inline |
| `src/kernel/stream.c/.h` | the C library's stdio as the host side uses it: `stream_putc` (putc on `cur_stream`), `stream_flsbuf` (`_flsbuf`), `stream_flush` (`fflush_if_pending`), `stream_printf` (`fprintf` with the ROM's `_doprnt`: flags `-` `0`, width, precision, `b d o u x` and the unsigned capitals, `c`, `s`; `q`/`r` not rebuilt) |
| `decomp/test/test_host.c`, `decomp/build_host_test.bat` | the harness; `check_frames.py` runs it as mode `host` |

**Boundary.** The C calls out for the kernel (`dev_getc`, `dev_putc`, `dev_control`, `event_wait`, `task_resume`,
`sem_wait`, `mbox_init`/`_put`/`_get`, `heap_free_total`, `system_restart`, `panic`; declared in `src/kernel/rtos.h`,
where `dev_control` is now variadic for the ops with a third argument) and for the user dictionary
(`dict_hash_insert_or_delete`, `dict_hash_clear_all`, speech: `tx_dict.c`). The shared variables DT_LOG, DT_MODE,
`last_index`, `dt_error_flags`, `cur_stream` and `sync_sem` stay defined with speech; the host side defines its own
(`dt_terminal`, `speak_enabled`, the character sets, the line settings, the phone state).

**The check.** `phcapture` writes `<prefix>.host.tsv` from reset on: every byte the host task reads (`R`), every
`dev_putc` with its device (host line, console, text pipe, phone), every `dev_control` and its result, every
`event_wait`, `task_resume`, `sem_wait`, mailbox call (the dialer's tone messages), dictionary call and
`heap_free_total` result, in order. It also tracks the 28 variables the host task shares with the other tasks and
the NVRAM:
- `W`/`V` are what other tasks changed while the host task was switched out;
- `Z`/`Y` are the host task's own writes, reported before its next action.

An interrupt taken while the host task runs counts as the host task; otherwise a write loop such as the NVRAM store
would be split. `test_host` runs the C `host_task_main` from the same start on the same bytes. It checks each action
and its arguments, and at every action it compares all 28 variables and the NVRAM with the ROM's. The other tasks'
writes are applied before the C's next action, where the ROM's task saw them. The dictionary calls are compared and
return the ROM's result.

**Result: all 65 corpus entries pass** (7,583 bytes read, 11,883 actions matched, 111 writes by other tasks applied;
67 entries and 7,595 bytes since §15.36). The 19 local-terminal entries only see the XON the task sends at
start. Seven new host-line entries (`host_esc`, `host_charset`, `host_bad`, `host_reset`, `host_phone`, `host_supp`,
`host_long`) exercise:
- DA, DECID, DSR, the error bits, DT_SPEAK, DT_TERMINAL and DT_MASK;
- the character sets, 8-bit C1 input and output, and OSC/PM/APC strings;
- malformed sequences;
- DT_DICT, DECNVR, DECTST 5, DECSTR and RIS;
- DT_PHONE on the emulator's line: answer, keypad, timeout, hang-up, tone and pulse dialing (the answer, the keys
  and the timeout only since §15.36, which fixed the entry's timing);
- every DEC supplemental character;
- texts that are too long.

They pass every speech mode too. `hostfeed.h` gained `\xHH` for bytes a command line cannot carry (8-bit C1 codes,
SO/SI). An AddressSanitizer build of `test_host` passes the ten host-heavy entries. While writing `hs_rom.c`, four
entries of the supplemental table were first typed wrong (0xc6, 0xd7, 0xe6, 0xf7). A check against the ROM bytes
caught them, and the table is now generated from those bytes, so it matches the ROM exactly.

**Findings [V]** (from the code, confirmed by the captures):
- **No DT_MASK in v1.8.** P2 83 is handled like any unknown P2: DSR error 26 and `DT_XXX: P2 = 83`. No CR is ever
  added after replies or keys (§5.2; probably a 2.0 feature).
- **R3 = 3 ("too long")** is sent by `dcs_command_dispatch` when DT_PHONE's text fills the 256-byte buffer, so at
  256 characters or more. DT_DICT uses the same test for its R3 = 2. §13 item 2's open question is answered.
- **Replies omit zero parameters** (`send_control_sequence`). So DSR "ok" is `ESC [ n`, not `ESC [ 0 n`, and an
  on-hook reply is `ESC P ; 70 ; z ESC \`.
- **Extended DSR never sends `?21n`** ("first report since power-up"). With no error it sends `?20n`. Error bits 0-5
  of `dt_error_flags` (0x81f12) give codes 22-27 in that order, and the report clears them. `dsr_reply` is 0xdff2.
- **DECTC1 is on at power-up.** Factory record word 9 is 1, so received bytes lose bit 7 (`é` arrives as `i`) until
  the host sends DECAC1 (`ESC SP 7`). The manual's Table 5-1 says the same ("7 bit (DECTC1)").
- **The DEC supplemental set** (0x1823e) is spoken as base letters: accented letters lose the accent, `¹²³` become the
  digits, `ª º` become `a o`, and NBSP becomes a space. `Ø`/`ø` become the digit **`0`**. `Æ æ`, 0xd7/0xf7 and the
  symbols are dropped. It serves both GR (0xa0-0xff) and a GL with `<` designated (0x20-0x7e).
- **Designating a set** (`ESC ( <` …) does not change GL or GR until SO, SI, LS2, LS3 or LS1R-LS3R. RIS restores
  G0/G1 = ASCII, G2/G3 = DEC supplemental, GL = ASCII, GR = supplemental.
- **DT_SPEAK:** DT_STOP and DT_SYNC turn speaking back on, and DT_SPEAK without P3 turns it off.
- **In-text commands.** Index marks go into the text pipe as `\n STX :in n ETX \n` (DT_INDEX) or `:re n`
  (DT_INDEX_REPLY), with P3 & 0x7fff. DT_PHOTEXT wraps its text in STX … ETX.
- **Aborting a sequence.** CAN is 0x15 (^U) as in `dectlk.h`. It and SUB abort a sequence. 0x18 does not: it goes to
  the pipe and the sequence goes on.
- **`settings_reset` modes** (§13 item 10): 0 = RIS, 1 = DECSTR, 2 = DECNVR restore, 3 = power-up.
  - It never flushes the text pipe, although the manual says RIS does.
  - RIS and DECSTR reset the phone task (event 0x84), which hangs up.
- **`phone_dial` runs in the host task.** Host input waits while a number is dialed.
- **DECTST.** 5 speaks the test message, including the ROM dates "05-Dec-1983" (code, 0x1fff0) and "11-Oct-1983"
  (dictionary, 0x3fff0), the free heap (17,486 bytes in the emulator) and the ROM's own typo "If you can here this".
  1 syncs and restarts. 2-4 run the device loopback ops and then set the line up again (`line_configure`).
- **The text pipe's stream is unbuffered** (`fdopen`: no buffer). Every character the host task writes is one
  `dev_putc` to the pipe.

**Still to do on the host side:** the other host-terminal tasks, `main_task` (SETUP), the host-timeout and stop
tasks, and the RTOS itself; then the line backends (§13 item 15). The phone task, SETUP and the two small tasks are
done (§15.36-15.38).

### 15.36 The phone task in C (2026-09-27) — §13 items 2 and 15 [V: end to end]

**Scope.** The `phone` task is C: `phtask_main` `0xf128` and `dtmf_diagnostic_menu` `0x116e2`, in
`src/host/hs_phone.c`. Everything they call was already C: `send_dcs_reply`, `host_line_putc`,
`emit_sync_marker`, `settings_reset` and `dectst_self_test` (§15.35), and the stream and console code. The phone
device stays with the kernel (its ops, `phone_tlc_isr`, `phone_ring_poll`; §15.34). The task only sees it through
`dev_getc` and `dev_control`, whose ops and events now have names in `host.h`:
- device ops: `DEV_RX_TIMER` (−6), `DEV_RX_TIMER_OFF` (−7), `PHONE_SET_IDLE` (7), `PHONE_START_ANSWER` (9);
- events: `PHONE_EV_RING` `0x81`, `PHONE_EV_OFFHOOK` `0x82`, `PHONE_EV_RING_STOPPED` `0x86`, and `DEV_TIMEOUT`
  `0x80000`.

The menu's tables are typed in `hs_phone.c`: the key characters `"D1234567890*#ABC"` (`0x1899a`), the spoken key names
(`0x1987e`: `dee one … zero star sharp aye be sea`) and the names of tests 2-5 (`0x198be`).

**The check.** `phcapture` now traces two tasks with the same code, one state for each.
- `<prefix>.host.tsv` is unchanged, byte for byte.
- `<prefix>.phone.tsv` is new. It has the same events as `host.tsv`, except that `R` is each value `dev_getc` returned
  from the phone device: an event `0x81`-`0x86`, a key code 0-15, or `0x80000` from the input timer.

`test_host --phone` runs the C `phtask_main` on those values. `check_frames.py` runs it as a 20th mode, `phone`.

The phone task found two gaps in the harness, and both are fixed:
- **Writes during a wait.** What other tasks wrote while `event_wait`, `sem_wait` or `mbox_get` waited is now applied
  when the wait returns. Before, it was applied only before the next action. `phone_dial` reads `phone_offhook` right
  after its `event_wait`, and once `host_phone` really answered, the C dialer saw the old value.
- **The end of the capture.** The capture may end while the task's last call (a wait) has not returned. The C stops
  there instead of reporting a mismatch.

**Result: all 67 entries pass all 20 modes** (1,340 checks).
- The `phone` mode read 106 events and matched 1,823 actions, with 179 writes by other tasks applied.
- The `host` mode now reads 7,595 bytes and matches 11,900 actions.
- Elsewhere the phone task sees little. In the 19 local-terminal entries it gets no event at all. In the other 44
  host-line entries it gets one reset, `0x84`, posted by the host's first byte. `host_reset` gives three: the first
  byte, RIS and DECSTR.
- An AddressSanitizer build of `test_host` passes both modes on the nine host- and phone-heavy entries.

**The corpus.**
- **`host_phone` did not do what §15.35 said.** It never answered a call and never passed a key to the host (§12.55).
  - **A bug in `hostfeed.h`.** A `\g` or `\k` placed right after a `\w`/`\W` has the same byte offset as the pause,
    so it fired at the start of the pause instead of the end. Each action now also records its segment.
  - **The ROM's behaviour.** The ring still came before the host task had taken DT_PHONE 10. The reset event `0x84`
    that DT_PHONE 10 posts then arrived while the answer was being counted, which cancels it.
  - **The fix.** A `\W` after DT_PHONE 10 in `host_phone`.
  - **What the entry does now:**
    - answer on 1 ring, R3 = 1;
    - DT_PHONE 20 and 30;3, then the caller's `12#`, which the host receives as `1`, `2`, `#`;
    - 3 s without a key: `dtphon (timeout)`, R3 = 2;
    - hang-up, with R3 = 0 two seconds later;
    - then the rest as before.
- **`phone_menu`** (new). A call before the host's first byte, i.e. the stand-alone mode:
  - the banner;
  - `5`: "You pressed five.";
  - `*`: factory settings;
  - `#9#`: "DECtalk has no test 9.";
  - `#1#`: "Power up reset test not allowed.";
  - `#*` and `#2*`: "Quit." at the test number;
  - then the host's first byte ends the menu.
- **`phone_tests`** (new). The self tests:
  - `#2#3#`: test 2 with 3 passes;
  - `#3#12#`: a pass count over ten;
  - `#A#`: test 13;
  - `#4#*`: quit at the pass count.
  It runs to the 90 s limit, so the menu's 30 s timeout is not reached. That exit takes the same branch as the host
  byte in `phone_menu`.

**Findings [V]** (from the code, confirmed by the captures):
- **The menu is spoken through the text pipe,** like host text. It starts with `[:np :ra 180]`. The caller may key while the unit speaks: the keys queue in the phone device,
  and the emulator's receiver decodes them over the unit's own speech.
- **Keyed numbers count every code as a digit.** `0` (code 10) adds nothing. `D` (code 0) also adds 0, but `A`-`C` add
  13-15: `#A#` asks for test 13.
- **`*` is `settings_reset(2, 1)`,** DECNVR's restore from the factory record. It sets DT_LOG, DT_TERMINAL, DT_MODE,
  DT_SPEAK and both lines (the `line_configure` device ops), but writes nothing to the NVRAM.
- **Passes stop at the first failure** ("Failed in pass %d."). In the emulator tests 2 and 3 fail on pass 1, because
  it has no loopback. Test 2 first sends bytes 0x00-0xFF on the host line. The ROM spells the out-of-range reply
  "Running ten passis."
- **The menu returns on any event that is not a key:** its 30 s input timer (op −6, 3000 ticks), or the host's first
  byte (`0x84`). The input timer is turned off only on the way out of the key loop; from inside a number entry it
  returns without that, and the task's idle loop turns it off anyway. The keypad interrupt is left on until the task
  hangs up (`PHONE_SET_IDLE` writes `0x9c004 = 0x4000`). The task then says `dtphon (fsm)` (a DT_SYNC), hangs up and
  waits 2 s. The `0x84` has been consumed, so it does not arrive again.
- **Stand-alone mode lasts only until the host's first byte.** That byte also clears rings to answer, which
  `main_task` sets to 1 at power-up. From then on a ring is ignored until DT_PHONE 10 sets the count; the answer then
  gives R3 = 1, and the keys go to the host.
- **A reset during the answer cancels it** (DT_PHONE 10, RIS, DECSTR or the first host byte). A host that sends
  DT_PHONE 10 must do so before the ring, as the manual's examples do.

### 15.37 SETUP and the local terminal in C (2026-09-27) — §13 item 15 [V: end to end]

**Scope.** The `main` task is C from the point where `speech_init` returns. All of it is in
`src/host/hs_setup.c`:
- `main_task`, the local-terminal session and the SETUP loop;
- the line editor `read_line_with_prompt` and `setup_puts`;
- `tokenize_words` and `tdparse`, with `keyword_match`, `parse_hex`, `parse_boolean`, `parse_baud_rate`,
  `parse_parity` and `parse_onoff_or_ctrlchar`;
- `showbit`, `print_channel_speed`, `print_channel_format` and `print_help_text`.

The tables are typed:
- the command tree: 15 nodes of `setup_entry_t` `{keyword, action, help}`;
- the flags: 18 `setup_flag_t` `{var, mask, name}`;
- the speed, line and format names;
- the 71 help texts, in `hs_help.c`. They were generated once from `0x19c04` and the offsets at `0x1d2c4`, and are
  kept as source.

Three things stay outside, declared in `host.h`:
- `speech_init`, the boot;
- `duart_input_port()`, the read of `0x9801b`, whose IP4 bit decides the banner;
- the kernel's profiler, `profile_start` and `profile_report`.

**Names.**
- Ghidra: `FUN_0000300a` → `setup_puts`, `FUN_00010844` → `keyword_match`, `FUN_0001088c` → `parse_hex`. 305 of 362
  functions are named.
- C variables: `host_line_op6` → `host_modem`, `nvr_word10` → `setup_interrupt_char`.

**The check.** `phcapture` traces the main task too, into `<prefix>.main.tsv`, from PC `0x2710`, where `speech_init`
has returned. The events are those of `host.tsv`, plus:
- `R`: each character `dev_getc` returns from the local terminal;
- `D`: the DUART input-port byte;
- `H lo hi` and `J`: the profiler calls, whose insides are not recorded.

`test_host --setup` runs the C `main_task` on it. `check_frames.py` runs it as mode `setup`, the 21st.

**Typing on the local terminal.** The emulator could not send a BREAK or type a long session.
- **A received break.** `duart2681.c` now keeps a status per received character. `dtc01_feed_break` queues a NUL with
  the received-break bit (SR bit 7), as the SCN2681 does. The ROM's `duart_rx_char` turns a break into 0 and drops a
  plain NUL, so only this enters SETUP.
- **A typed feed.** `phcapture -T` and `spclog -T` feed the local terminal with `hostfeed.h`'s escapes, with `\B` for a
  BREAK. Corpus entries marked `'term'` use it.
- **Pacing.** v1.8 has no flow control on the local line (the unit sends no XOFF there), and the console input ring
  overflows while SETUP prints: the help texts, or LBREAK's 3.5 s. The first try lost everything after about 580
  characters. So `-T` types like a person:
  - at most 32 bytes at a time;
  - only when the console device's input ring (count at `0x803b4`) and the DUART are empty;
  - each `\w`/`\W` counts from the end of the text before it.
- **A self-test boot.** `phcapture -t` boots in the self-test mode (IP4 high), as `spclog -t`. It was used once, to
  check the banner.

None of this changes the existing runs: the other entries still match their reference logs.

**Result: all 71 entries pass all 21 modes** (1,491 checks).
- The `setup` mode read 2,442 characters and matched 12,689 actions.
- The 19 local entries go through the line editor: every text they speak is typed at the `>` prompt.
- An AddressSanitizer build passes all three modes on twelve entries.
- A self-test boot (`phcapture -t`, not in the corpus) passes too: the input port reads `0xF5`, and the banner goes
  into the pipe.

Four new `'term'` entries run SETUP:
- **`setup_show`:** SHOW LOCAL/HOST/LOG/MODE/INTERRUPT, HELP, HELP SET LOG TEXT, SET INTERRUPT ^B, abbreviations, bad
  lines, and too many words.
- **`setup_edit`:** the line editor (DEL, BS, erasing a tab and a control character, ^U, ^R recall and redisplay, the
  132-character limit), HARDCOPY erasing, and single-character mode, entered with an interrupt character.
- **`setup_cmds`:** speeds, formats, MODEM, BREAK, LBREAK, SAVE, RECALL (user and factory), TEST HDATA and HCONTROL
  (which fail in the emulator), flags, HISTOGRAM, bad values, the HELP forms, and ONLINE/OFFLINE.
- **`setup_spoken`:** LOCAL SPOKENSETUP (the prompts, the output, help, ^U, "exit."), then TEST SPEAK.

Not in the corpus: TEST POWER (a restart) and TEST LDATA (a loopback of the line the test is typed on).

**Findings [V]:**
- **SETUP is on the local terminal only;** the host cannot enter it. A BREAK enters it, or the SET INTERRUPT
  character, which is off at the factory.
- **The session mode is chosen when a session starts,** so SET LOCAL EDITED takes effect at EXIT.
- **EDITED sessions read lines after a `>` prompt.** An empty line puts CTRL-K (a clause flush) into the pipe. With
  LOCAL HOST on, the line and a CR also go to the host line.
- **Without EDITED,** characters pass one by one, without echo.
- **^R before anything is typed recalls the previous line** (the buffer keeps it). Later on it redisplays the line.
- **The column counting is inconsistent:** typing counts a control character's `^X` as one column, but the recount
  for erasing a tab counts two.
- **A full line** (132 characters) takes no more characters, silently.
- **ONLINE sets DT_LOG to 4** (LOG RAWHOST: host input is shown on the terminal) and drops all other logging.
  OFFLINE clears DT_LOG. Both turn host speak on.
- **`SHOW HOST` omits MODEM.**
- **HISTOGRAM is a hidden debugging command**, for the kernel's PC-sampling profiler.
- **DEBUG** (DT_LOG `0x80`) can be set but has no help text.
- **The ROM's `setup>` is not a second prompt.** It was read that way earlier; it is the spoken prompt "setup.",
  used with SPOKENSETUP.
- **NVRAM words 8-12** (§15.33): 8 S7C1T/S8C1T (`0x822e8`), 9 DECTC1 (`0x822e6`), 10 the interrupt character
  (`0x822e4`), 11 host speak (`0x822d2`), 12 host modem (`0x822d0`).

**Still to do on the host side:** the host-timeout and stop tasks, then the RTOS and the line backends (§13 item 15).
Both tasks are done (§15.38).

### 15.38 The host-timeout and stop tasks in C (2026-09-27) — §13 item 15 [V: end to end]

**Scope.** The last two host-side tasks are C, in `src/host/hs_tasks.c`:
- **`host_timeout_task_main` `0xf070`** (priority −100, the lowest). It sets `host_idle` `0x81d72` to 5 and wakes
  once a second:
  - While `host_idle` is 5 it does nothing. `read_host_byte` sets it to 0 on every host byte.
  - While the unit holds the host off (XOFF sent: `host_dev + 0x42`, `0x80160`), it resets the count to 0, since the
    host is not idle then.
  - Otherwise it counts. At 5 seconds without host input it logs `Host timeout` (DT_LOG `0x80`) and puts a CTRL-K
    (clause flush) into the text pipe, so a clause the host left without its end gets spoken. Then it sleeps until
    the next host byte.
- **`stop_task_main` `0xfa7a`** (priority 0). It suspends itself until DT_STOP (`dt_stop`) resumes it, then:
  `stop_pending++`, `emit_sync_marker("h_stop")`, `stop_pending--`. What `stop_pending` does to klsyn is in §15.23.

Two kernel calls were added to `rtos.h`: `task_suspend` (with `current_task`) and `dev_rx_held`. The latter reads the
XOFF flag the ROM takes straight from the device record.

**The check.** `phcapture` writes `<prefix>.timeout.tsv` and `<prefix>.stop.tsv` from the tasks' entries, with the
events of `host.tsv` plus `N` for `task_suspend`. `test_host --timeout` and `--stop` run the C tasks on them, and
`check_frames.py` runs them as modes `timeout` and `stop`.

The shared variables now include `stop_pending` and the host device's XOFF flag. The DUART interrupt writes the flag,
and an interrupt taken while a traced task runs counts as that task. A variable only the kernel writes is therefore
marked in the capture's table, and its changes are always reported as `W` (outside writes), never `Z`.

**Result: all 72 entries pass all 23 modes** (1,656 checks). The `timeout` mode matched 1,171
actions (289 writes by other tasks applied), the `stop` mode 81. An AddressSanitizer build passes all five host-side
modes on eight entries (the timeout and stop entries among them).

The new entry **`host_timeout`** sends a clause with no end (`[:np] This clause has no end`) and then waits 10 s.
The clause is spoken after the timeout's CTRL-K, 5 s after the last host byte. The timeout also fires in
`host_phone` and `host_reset`, which pause the host for 5 s. The stop task runs in `stop`, `stop_early` and
`stop_drop`.

**Not reached:** the XOFF branch of the timeout task. In the emulator the host is held off only briefly, between two
one-second ticks. The emulator's host feed delivers bytes far faster than a real line (§13, caveat 3), and a text long
enough to hold the host off overruns the ROM's input ring. The branch is three lines and reads plainly from the ROM.

**Where they belong** (§13 item 15, the library API):
- The host timeout is host-terminal policy: it acts on the host line's silence.
- The stop task carries out DT_STOP inside the synth (it syncs the pipeline while `stop_pending` makes klsyn drop
  speech). In the speech library it corresponds to dapi's `TextToSpeechReset`, and DT_SYNC to `TextToSpeechSync`. For
  now it sits with the host C, as the task list in §3 has it.

**The host side is now C, apart from the kernel.** That is the `host`, `phone`, `main` (SETUP), `host timeout` and
`stop` tasks, §15.35-15.38. Still to do: the RTOS and the line backends (§13 item 15).

## 16. DSP (TMS32010) program — first disassembly (2026-09-23)

The user reopened the DSP on 2026-09-23 (it had been deferred, AGENTS §0.1). Ghidra has no TMS320C1x processor
module, so the DSP is disassembled outside Ghidra:
- **`dsp/tmsdis.py`**: a recursive-descent disassembler. Its opcode map comes from `native/tms32010.c`, the MAME
  core the emulator runs. It carries the label/comment tables for everything identified so far.
- **`dsp/dsp_v1.8.lst`**: its annotated output. Regenerate after editing the tables:
  `python dsp/tmsdis.py > dsp/dsp_v1.8.lst`.

**Image** [V]: `merges/dsp_v1.8_A.bin` = 2048 big-endian words; chip **E70 (23-166F4) = high byte**, **E69 (23-165F4)
= low byte**. Code reached from the two vectors = **1180 words, no illegal opcodes**; the rest is tables.

### 16.1 Hardware interface (from `native/dtc01.c`, a port of MAME's DECtalk driver) [V]
| DSP side | 68000 side | Meaning |
|---|---|---|
| `IN x,PA1` | write `0x9C002` | pop the next word of the 68000→DSP input FIFO (32 deep) |
| `OUT x,PA1` | — | push a DAC sample (top 12 bits used; offset-binary at the DAC) into the 16-deep output FIFO |
| `OUT x,PA0` | read `0x9C000` bit 7 (bit 5 = error) | raise the semaphore (IRQ 5 if `0x9C000` bit 6 is set); bit 0 of the value = error latch |
| `BIOZ` | write `0x9C000` bit 1 | BIO = semaphore; the 68000 clears it (acknowledge). The DSP polls BIO to see whether a frame is waiting |
| `INT` → vector `0x002` | — | asserted when the output FIFO has room, i.e. once per 100 µs DAC tick |
| reset → vector `0x000` | `0x9C000` bit 0 | the 68000 holds the DSP in reset between utterances; bit 0 also clears both FIFOs |

### 16.2 Program memory map
| Words | Contents |
|---|---|
| `000` / `002` | `B COLD_START` (`0x2A7`) / `B DAC_ISR` (`0x694`) |
| `004-013` | `INIT_CONSTS`: copied to data RAM `0x00-0x10` at reset (`data[n] = prog[n+3]`), listed below |
| `014-020` | `RESET_COEFFS`: loaded at reset into `data[25 58 59 6A 6B]` and `data[80-87]` (roles in §16.10: the noise seed, the fixed B/C of the parallel F6 and the nasal pole, the FNZ index offset 31, the 4× low-pass; `data[80 82 83 86 87]` are never read) |
| **`021-089`** | **nasal anti-resonator tables** `FNZ_TAB_A/B/C` (3 × 35 words): coefficients `a, b, c` (with a fixed gain) for FNZ ≈ 240 + 8k Hz, BW ≈ 80 Hz; indexed by `FNZ/8 − 31` (fitted 2026-09-23, §16.6) |
| **`08A-0E1`** | **Klatt's `amptable[88]`** (dB → linear), identical to `klsyn/parwav.h`; indexed as `0x8A + dB` |
| `0E2-1C6` | cosine table: `0x1FBF` (≈ 8192·cos(2π·200 Hz/10 kHz)) down to `0xE000` (−8192) |
| **`1C7-2A6`** | **`klsyn/parwav.h` `B0[224]`** (= 1920000/nopen², the "natural" glottal-source table), byte-identical [V]; indexed by `nopen − 40` |
| `2A7-73E` | code |
| `73F-7FF` | never reached; looks like leftover EPROM contents [I] |

Data-RAM constants set at reset: `data[02] = data[0C] = 0x1000` (1.0 in Q12); `data[05] = 0x7FFF` (checksum
mask); `data[06] = 0x6000` (speech header); **`data[07] = 50`** (the frequency lookups use breakpoints
4·50 … 64·50 = 200/400/800/1600/3200 Hz); `data[08] = 0x21EA` (×2 = the 19-word frame trailer `0x43D4`);
`data[0E] = 0x1FFF` (header must-be-zero mask); `data[10] = 1`; **`data[04] = 0x2AAB`** (1/3 in Q15, for parwav's
`a = b·nopen·.333`); **`data[09] = 0x00D2`** (`0xF5 + 0xD2 = 0x1C7`, the `B0` base); `data[0D] = 12`. The rest were resolved by the C translation (§16.10): `data[01] = 0x0118` (280, the parallel F3
bandwidth term), `data[03] = 0x0641` (the noise feedback), `data[0F] = 0xE000` (the noise's sign bits);
`data[0A] = 0x21FC` and `data[0B] = 0x7850` are never read.

### 16.3 Control flow
- **`COLD_START` `0x2A7`**:
  1. set saturation mode;
  2. semaphore with no error (tells the 68000 "ready");
  3. clear data RAM `0x00-0x80`;
  4. copy the constants, plus a few more `TBLR` loads (`0x2BD-0x2D8`);
  5. spin on BIO until the 68000 acknowledges;
  6. enter `MAIN_LOOP`.
- **`MAIN_LOOP` `0x2F9`**: if BIO says a frame is waiting, read it; then compute **one output sample** (`SAMPLE`
  `0x4A5`). The sample goes to `data[11]`, and the DSP spins on `EINT`/`DINT` until the DAC interrupt
  (`DAC_ISR` `0x694`: `OUT 11h,PA1`) clears the wait counter. If 20 tries pass without a tick (`DAC_TIMEOUT`
  `0x685`) it flags an **error** to the 68000. This is the v1.8 timing sensitivity that `native/dtc01.c` works
  around. With no frame arriving, `NO_FRAME` `0x49E` counts frames and `IDLE_DECAY` `0x2E7` ramps state down.
- **Frame header** (first word, `data[2C]`):

  | Header | Frame |
  |---|---|
  | bit 15 set | **tone mode**: a two-sinusoid generator at `0x698` (phase accumulators `2F`/`30`, steps `2D`/`2E`, sign flips `31`/`32`), DTMF dialing and the power-up self-test tones; the 68000 sends `0x8000 | Hz` / `0x9000 | Hz` (§15.32) [V]. Each further header word updates the tones; a header with bit 15 clear leaves via `COLD_START` |
  | `0x6000` | **24-word speech frame** (below) |
  | `0x2000` / `0x4000` | **19-word frame** (`READ_FRAME_19` `0x3D3`) |
  | anything else | error (`header & 0x6000` must be non-zero, `header & 0x1FFF` zero) |

### 16.4 The 24-word frame — **the speaker definition, not the speech frame** (§16.9 corrects the roles below)
The DSP adds every word into `data[73]`, and the last word must make `(sum & 0x7FFF) == 0`; otherwise it sets
the error latch. Accepted frames are acknowledged with `OUT 07h,PA0` (semaphore, no error).
- **Words 1-6 are formant frequencies.** Each goes through a cosine lookup (`COS_LOOKUP_*` `0x709`/`0x721`/`0x72A`).
  - Words 1/2 (`64`/`65`) and 3/4 (`67`/`68`) are frequency plus a bandwidth-like second word, so two resonators
    are fully specified.
  - Words 5 (`52`) and 6 (`55`) are frequency-only; their "bandwidth" is derived from `data[07]` (8·50, 10·50).
- **Ten words are dB amplitudes** converted through `amptable`: words 9 (`66`), 10 (`63`), 11-13 (`2B 2A 29`),
  16 (`79`), 19 (`75`) and 20-22 (`69 76 77`).
- **The rest are passed as raw values**: word 7 (`8B`, data page 1), 8 (`7F`: at each glottal pulse `T0 += 7F·T0`
  and then `7F = −7F`, i.e. parwav's alternating skew), 14/15 (`28`/`27`: **open phase** `nopen = 28·T0 + 27`,
  §16.6; corrected, they are not the pitch period), 17 (`1A`) and 18 (`74`, the blend weight used at
  `FRAME_COEFFS` `0x456`).
- **Word 23** is the checksum.

**Next step:** map words to Klatt parameters (F0, AV, AF, AH, F1-F3, B1-B3, A2-A6, AB …). Use the 68000 side:
find who fills the 24-word queue items that `dsp_send_speech_frame` sends, which is presumably fed from
`phsettar`/`pht0draw` output, and read them against `klsyn/parwav.c` `gethost()`.

### 16.5 The 19-word frame — **the per-frame (6.4 ms) speech data** (§16.9 has the confirmed word map)
The 68000 sends it inline from `dsp_command_queue_isr` (header `0x2000`/`0x4000`, 18 more words). The DSP reads:
- 9 raw words into `30 22 21 20 1F 1E 1D 1C 1B`;
- 7 words through `amptable`, scaled by `data[75]`/`data[77]`, into `16 6F 70 51 54 57 17`;
- 1 raw word (`2F`, minus `data[0D]` = 12);
- the trailer `0x43D4`.

What the DSP does with them (traced 2026-09-23 against `parwav.c`, §16.6):

| Word | Data | Use | Tag |
|---|---|---|---|
| 1 | `30` | **T0, the pitch period** in 4×-rate samples (the period counter `14` is compared with it; parwav `T0 = 40·samrate/F0`) | [V] use |
| 2-4 | `22 21 20` | frequencies (cosine lookups) of three resonators set up at `0x54D-0x59F`, with gains `29 2A 2B` from the speech frame | [V] use, identity [I] |
| 5 | `1F` | **FNZ, the nasal-zero frequency** in Hz: indexes the anti-resonator tables as `1F/8 − 31` | [V] use |
| 6-8 | `1E 1D 1C` | bandwidth-type words of the three resonators | [I] |
| 9 | `1B` | **AV** (dB); converted `amptable[1B + 4]` once per glottal period (parwav: AV changes only at a pulse) | [V] use |
| 10-16 | `16 6F 70 51 54 57 17` | dB amplitudes (`16`/`17` = parallel-voicing and bypass gains) | [I] |
| 17 | `2F` | raw, minus 12; feeds the noise generator constant `7B` | [I] |

**Correction:** the earlier guess that this frame carries the per-voice constants is withdrawn. It carries the
pitch period, AV, FNZ and three resonators, so it is frame-rate (or pitch-rate) data. How often each frame type is
sent is still open; the emulator FIFO log (§16.8) settles it.

### 16.6 Synthesis, compared with `klsyn/parwav.c`
Same architecture as Klatt's `parwav()` (`klsyn/`, the priority source):
- **Glottal source run 4× per output sample** (`GLOTTAL_4X_LOOP` `0x4C2`, counter `data[72]` = 4), followed by a
  low-pass filter before decimation (`GLOTTAL_LOWPASS` `0x5BA`, 2-pole, state at `0x88/0x89`) — cf. parwav's
  `for (n4 = 0; n4 < 4; n4++) … resonlp()`.
- **The voicing source is parwav's "natural" source**, not the 1980 paper's impulse + RGP/RGZ (traced
  2026-09-23; the `B0` table is byte-identical):
  - open phase (`nper` `14` < `nopen` `15`): `a −= b; vwave += a` at `0x4CB` (`a` `13`, `b` `2E`, `vwave` `78`), then
    `glotout = vwave·amp` (`76`);
  - closed phase: `vwave = 0` until `nper` reaches `T0` (`30`);
  - **pitch-synchronous reset** `PITCH_SYNC_RESET` `0x4DF` = parwav `pitch_synch_par_reset()`: AV latched from
    `amptable`; alternating skew (`7F`); `nmod = T0`, halved when voiced (`31`; noise is halved after `nmod`, `0x4BA`);
    `nopen = 28·T0 + 27` clamped to **40…263** (parwav's limits exactly; `263 = 0xD5 + data[07]`) and below `T0`;
    `b = B0[nopen − 40]`; `a = b·nopen/3` (`0x2AAB`).
- **Noise** (`NOISE_AND_MIX` `0x5CD`, state `7B`/`7C`) mixed with voicing.
- **Nasal zero**: coefficients come precomputed from `FNZ_TAB_A/B/C` (`0x5A0`) rather than from a cosine lookup.
  A fit of the 35 entries gives an anti-resonator at FNZ ≈ 240 + 8k Hz with a constant ≈ 80 Hz bandwidth and a fixed
  gain (Klatt 1980's `SETABC` for a negative F: `A' = 1/A`, `B' = −B/A`, `C' = −C/A`). That covers FNZ ≈ 240-510 Hz,
  inside Klatt's Table I range (200-700, default 250).
- **Cascade branch** `0x5E8` (states read downwards from `0x4A`, coefficients from `0x6E`; corrected, §16.10), then a
  **parallel branch**
  `0x632` whose resonator outputs are **summed with alternating signs**, minus a **bypass** term
  (`data[17]` × frication) — exactly Klatt 1980 (§14.10).
- Output: `data[11]` → DAC.

All arithmetic is Q12/Q15 fixed point (`MPY`/`PAC`/`SACH …,4`). The coefficient math per frame is at
`FRAME_COEFFS` `0x456-0x49C`, and the voice-frame coefficient setup at `0x4DF-0x5B9`.

### 16.7 68000 side (renamed 2026-09-23)
- `dsp_command_queue_isr` `0x12258` drains the queue. Bit-15 items go out as 2 words, then it waits for the
  semaphore (tone mode). `0x6000` items go out through **`dsp_send_speech_frame` `0x122a4`**: 24 words, then
  `0x9C000 = 0x42` (IRQ enable + acknowledge). Other headers go out inline as 19 words.
- The whole handler, the SPC and the tone hooks are C now (`dsp_link.c`, §16.11): every speaker frame resets the
  DSP, the last one is sent again after each reset, and an error is handled at the next post.
- **Correction:** `kl3_push_event` `0xa6e2` is **not** part of this path. It is dapi's `make_f0_command`: it fills
  the F0 command list for `pht0draw` (§15.14, §12 item 33).

### 16.8 Open items
1. ~~Map the frame words to Klatt parameters~~: **done from the emulator log, §16.9** (2026-09-26).
2. ~~Decode the tables at `0x014-0x089` and `0x1C7-0x2A6`~~: `B0` and the FNZ tables are done (2026-09-23);
   `RESET_COEFFS` and the reset constants are done by the C translation (§16.10, 2026-09-27).
3. ~~Confirm the tone mode~~: done (§15.32). `phone_dial` `0xeb54` posts the bit-15 items; the words are Hz with
   bit 12 selecting the oscillator. The power-up self-test sends them directly (`0x580`).
4. ~~Dynamic check of the input FIFO~~: done (§16.9): lengths, headers, checksum and trailer are all confirmed.
5. Optional: a Ghidra SLEIGH module for the TMS32010 would allow decompiling it. The listing is enough for now.
6. **The whole program is C** (`src/speech/dsp_synth.c`, §16.10), sample-exact on the corpus, and plain C
   since the same day. Open: the DAC-timeout paths, which never ran.

### 16.9 Frame map from the emulator log (2026-09-26) — step 3 of the C plan [V]
**Tool.** `native/dtc01.c` has a diagnostic tap, `dtc01_set_spc_tap()`, which reports every 68000 write to the SPC
control register and the DSP FIFO with the writer's PC and the cycle count. The tap sits outside the snapshot
region and does nothing when unset. `native/spclog.c` uses it:
- build: `native/build_spclog.bat` (MSVC 2022 Build Tools) → `native/build/spclog.exe`;
- run: `spclog [-o prefix] "text"` boots, waits until the power-up announcement is done, speaks the text, and
  writes `<prefix>.words.tsv` (every write) and `<prefix>.frames.tsv` (frames decoded and checked).
The log is **deterministic**: two runs are byte-identical, cycle stamps included.

**Reference corpus.** `decomp/scripts/make_reference.py` → `decomp/reference/*.frames.tsv`. It covers:
- hello, fricatives, stops, nasals;
- all voices, `[:dv sex f]`, `[:ra 250]`;
- phonemic input, sung notes, and money numbers.

Across the corpus there are no bad checksums or trailers, no stray or dropped words, and no tone frames. **This is
the equivalence test for the C rebuild**: the rebuilt speech path must reproduce these files.

**Correction [§12.37]:** §16.4/§16.5 had the roles backwards. The **24-word `0x6000` frame is the speaker
definition**: it is sent once when speech starts and once per voice command (`[:n…]`, `[:dv]`): "hello" has 2, `dv_sex` 3, `voices` 7.
The **19-word `0x4000` frame is the per-frame speech data**, every **6.4 ms** (median interval exactly 6.40 ms;
209 of 233 intervals in "Hello world" within ±0.05 ms). No `0x2000` header was seen.

**19-word speech frame (`0x4000`)** — the DSP names are from §16.5:

| Word | DSP | Parameter | Evidence |
|---|---|---|---|
| 1 | `30` | **T0 = 40000 / F0** (pitch period at the 4× glottal rate) | `[d<100,102>aa<250>]` → 392 = 102.0 Hz; notes 13/17/20/25 → ≈128/161/192/256 Hz with vibrato |
| 2-4 | `22 21 20` | **F1, F2, F3** (Hz) | formant tracks, e.g. /h/→/ɛ/ in "hello" |
| 5 | `1F` | **FNZ** (Hz): 300 at rest, 527 in /m n/, ≈345 in nasalised vowels | nasal sentence |
| 6-8 | `1E 1D 1C` | **B1, B2, B3** (Hz) | 60/110/160 in vowels, 300-600 in /h/ and at pauses |
| 9 | `1B` | **AV** (dB) | 0 in voiceless sounds, 54-66 in vowels |
| 10 | `16` | **AH** aspiration | /h/, aspirated /p t/ releases (55-58) |
| 11 | `6F` | **A2** | velar and dental bursts, /θ/ |
| 12-15 | `70 51 54 57` | **A3, A4, A5, A6** | /ʃ/ 50 67 59 50; /s/ only A6 55 |
| 16 | `17` | **AB** bypass | /f/ /v/ 43-47, /b/ burst |
| 17 | `2F` | **TLT** spectral tilt: 12 in plain voicing, up to **28** (the cap) in voiceless frames | matches `pht0draw`'s tilt value capped at 28 (§15.14) |
| 18 | — | trailer `0x43D4` | |

AF, A1 and AN are not sent. Frication in the DSP comes from the parallel amplitudes, and the nasal zero from FNZ.

**24-word speaker frame (`0x6000`)** — every relation holds exactly for all seven ROM voices (`voice_t`, §8.1):

| Word | DSP | Content |
|---|---|---|
| 1, 2 | `64 65` | f4 × (200 − hs)/100, b4 |
| 3, 4 | `67 68` | f5 × (200 − hs)/100, b5 (females: 2500 / 2048, not scaled: the "unused" sentinel) |
| 5, 6 | `52 55` | p4, p5 |
| 7 | `8B` | ap × 10 = dapi **`f0minimum`**: used by `pht0draw` for F0; the DSP only checksums it (§15.17) |
| 8 | `7F` | **la** = dapi `t0jit` (Frank, Ursula, Rita: 100, 100, 6; the others 0). The DSP uses it as the alternating period skew, i.e. laryngealization. (An earlier "Harry sends 40" misread word 15, §12.38.) |
| 9-13 | `66 63 2B 2A 29` | g1 … g5 |
| 14 | `28` | 4000 + 160 × ri → open-phase slope (`nopen = 28·T0 + 27`, §16.6) |
| 15 | `27` | nf × 4 → open-phase offset |
| 16 | `79` | br + 9 → breathiness amplitude |
| 17 | `1A` | 41 × pr = dapi **`f0scalefac`** (Q12): used by `pht0draw` for F0; the DSP only checksums it (§15.17) |
| 18 | `74` | 41 × (200 − hs): the **head-size formant scale** in Q12, applied to F1-F3 at `FRAME_COEFFS` |
| 19-22 | `75 69 76 77` | gf, gn, gv, gh |
| 23 | — | checksum |

sm, as, ft, bf, ef and sex are not sent. They act on the 68000 side (prosody, tilt, and sex → hs/f4/f5).

**Open:** (Harry's word 8 and speaker words 7/17 are resolved in §15.17.)
- Text without a clause terminator was never spoken in the emulator, even after 30 s, although the manual
  promises a 5-second host timeout. This may be an emulator timer issue or a firmware condition; check it with
  `host_timeout_task_main` and `clock_tick`.
- Tone frames need a phone command, or the power-up self-test: `spclog -t -b` runs the emulator's self-test mode
  and logs from reset on (§15.32).


### 16.10 The DSP program in C (2026-09-27) — §17.5 [V: every sample of the corpus]
**Files.**
- **`src/speech/dsp_synth.c/.h`**: the whole DSP program as plain C (about 400 lines). The state is a
  struct with named fields (`dsp_t`): the frame values, the speaker values, the twelve resonators (`dsp_res_t res[]`:
  the nasal zero, the cascade, the parallel branch, in the order the program runs them), the filter histories
  (`hist[]`, in the program's order, because the program clears prefixes of it), the source, the noise and the two
  tone oscillators. Each block carries its label and address from the listing. The arithmetic is still exactly the
  program's:
  - every store keeps 16 bits (the `int16_t` assignments wrap, as SACL does), and the right shifts are the SACH
    stores;
  - the three-product sums of the filters (`mac3`) saturate at 32 bits, as the chip's accumulator does in overflow
    mode, which the program sets and never clears. Every other sum is bounded well inside 32 bits (two products, or
    shifts up to 14), so it needs no saturation.

  The first version (2026-09-27, the same day) translated the listing one instruction per call on a model of the
  accumulator, with the data RAM as an array. It gave the same samples and was replaced by this one.
- **`src/speech/dsp_rom.c`**: the tables, named and typed, in ROM order from word 0x000 to 0x2A6:
  `dsp_vectors[3]`, `dsp_init_consts[17]`, `dsp_reset_coeffs[13]`, `dsp_fnz_a/b/c[35]`, `dsp_amptable[88]`,
  `dsp_cos_table[229]`, `dsp_b0[224]`. They are generated once by `decomp/scripts/gen_dsp_rom.py` and kept as source.
  `table_word()` maps a TBLR address onto them. One read falls outside the obvious tables: before the first speech
  frame FNZ is 0, so the nasal-zero lookup reads program word 2, the `B DAC_ISR` opcode `0xF900` (hence
  `dsp_vectors`).
- **The interface.** `dsp_init()` is power-up (data RAM 0). `dsp_reset()` is the 68000 letting the DSP out of reset,
  which runs `COLD_START`. `dsp_step()` does what the program does between two DAC samples and returns the sample. The
  program's ports are three callbacks (`dsp_io_t`):
  - `frame_waiting`: the BIO line, i.e. has the 68000 acknowledged;
  - `read_word`: the input FIFO;
  - `signal`: the semaphore.
- **The check.**
  - `native/dspcapture.c` (`build_dspcapture.bat`) logs, per DSP instruction, what the ROM's program does at its
    ports: resets, the acknowledge it waited for, the BIO polls at `MAIN_LOOP` and `TONE_LOOP`, every word read,
    every semaphore and every sample. It hooks the TMS32010 core through `TMS_STEP_HOOK`, which only this build
    defines (`dspcap_hook.h`); `-d N` also dumps the data RAM at each sample, for debugging.
  - `decomp/test/test_dsp.c` (`build_dsp_test.bat`) replays the log through the C and compares every semaphore and
    every sample. (The first version's `-r`, which compared the data RAM with a `dspcapture -d` dump, went with the
    RAM array; `dspcapture -d` still writes the dumps.)
  - `check_frames.py` runs it as mode `dsp` on all 72 corpus entries, plus a DSP-only entry, `selftest`: the power-up
    self-test's 16 DTMF digits in tone mode (`dspcapture -t -b`). `--mode dsp` runs only that mode.
- **Result.** All entries pass: 6,856,731 samples, every one equal to the ROM's, in speech and tone mode. The
  full `check_frames.py` run passes too: all 72 entries in all 24 modes, plus `selftest` (1,729 checks).
  The captures showed no DAC timeouts, so the C does not model them. `test_dsp` fails if a capture has one.
  An AddressSanitizer build is clean. The plain C gives the same result, and builds without warnings with MSVC `/W4`
  and gcc `-Wall -Wextra -pedantic`; a gcc build with AddressSanitizer and UndefinedBehaviorSanitizer is clean.

**What the translation showed** (new, [V] by the check):
- **The DSP paces the 68000.** `FRAME_TIMEOUT_CHECK` sends the program back to `MAIN_LOOP` every 64 samples. Only
  there does it poll BIO, so a speech frame is taken at most once per 64 samples (6.4 ms). The 68000's queue fills
  it on the semaphore interrupt. After a speaker frame the program polls again at once, so the speech frame that
  follows it starts in the same pass.
- **Idle.** A missed poll adds 1 to `data[71]`. At the third missed poll in a row the program stops producing samples
  (`IDLE_DECAY`) and only polls, until a frame comes. If a speech frame came since the last idle (`data[8C]`), it
  first raises the semaphore with the error bit set: that is the "error" the 68000 sees at the end of every
  utterance. Then it clears the cascade's states (`CLEAR_STATE`).
- **The signal chain of a sample:**
  - **Noise.** `data[24]` shifts left each sample, XOR `0x0641` when it was ≤ 0; it is reseeded with `0x2D61` at
    every speaker frame. The noise is its low 13 bits, sign-filled, then low-passed (`y = x + y1/8`) and halved once
    `nper` passes `nmod`.
  - **Voicing.** parwav's natural source at 4× rate, through a 2-pole low-pass: C, B = `data[84]`/`[85]`, input gain
    700.
  - **Tilt.** A one-pole filter on the voicing. Its feedback coefficient is `1094 × (TLT − 12)` in Q15 (1094 = 4 ×
    280 − 26), set at each period. Its input coefficient is `0x7FFF` minus that, updated only while TLT ≥ 12.
  - **Breathiness.** `br × T0 × 8` times the noise generator, added in the open phase.
  - **The source.** AV × voicing + AH × noise.
  - **The cascade.** A nasal zero (3-tap FIR, coefficients from `FNZ_TAB` by FNZ/8 − 31), then the nasal pole (B, C
    fixed at reset, gain gn), then F5, F4, F3, F2 and F1. The output is doubled.
  - **The parallel branch.** It runs on the noise: F6 (B, C fixed at reset, gain A6), F5 (p5, A5), F4 (p4, A4),
    F3 (A3) and F2 (A2). The outputs are summed with alternating signs, then AB × noise is subtracted.
- **Update rates.**
  - The cascade's F1-F3 coefficients, AV, the tilt filter, the open phase and the nasal zero are set at each glottal
    period (`PITCH_SYNC_RESET`).
  - The parallel F2/F3 coefficients are set at each speech frame (`FRAME_COEFFS`).
  - F4, F5, p4 and p5 are set at each speaker frame.
- **Head size.** `FRAME_COEFFS` scales F1 about 256 Hz, F2 about 512 Hz and F3 about 0: `F' = pivot + (F − pivot) ×
  hs/4096`.
- **Resonator coefficients.**
  - B = cos(f) × (1 − bw), C = 2bw − 1, both Q12, with `bw` the frame's bandwidth word. With `bw` = (1 − r²)/2, C is
    −r² and B about 2r·cos θ (the cosines are Q13).
  - A = gain × (1 − B − C) >> 11, normalized; F1's A is also × 8. SACH shifts only by 0, 1 or 4, so the program gets
    the shift of 11 by storing the product's two halves and adding them back doubled.
  - The nasal zero's A, B and C are the `FNZ_TAB` words unchanged (the program multiplies them by `0x1000` and shifts
    back by 12).
  - The speaker's F4/F5 take their gains (speaker words 10 and 9) unnormalized.
  - The parallel F2 and F3 have fixed bandwidth terms: 210 and 280.
- **`cos_lookup`** maps Hz onto the 229-entry cosine table on a piecewise scale:
  - below 200 Hz → entry 0;
  - 200-400 Hz in 4 Hz steps; 400-800 in 8; 800-1600 in 16; 1600-3200 in 32; above that in 64.

  The entries for F3 and for the speaker's frequencies skip the lower tests.
- **Cascade gains.** Speaker words 9-13 are the cascade gains of F5, F4, F3, F2 and F1, in that order (the voice
  record's g1 … g5, §16.9). g5 is dapi's "loudness", the gain into F1 [I: dapi's naming].
- **Amplitudes.** The speech frame's dB words go through `amptable[dB + 10]` × gf (A2-A6, AB) or × gh (AH), and are
  0 unless dB > −10. AV goes through `amptable[AV + 4]` at each period. The speaker frame's dB words (the cascade
  gains, br, gf, gn, gv, gh) go through `amptable[dB]`.
- **Resets of the filter state.** `CLEAR_STATE` (at idle and on every error) clears the 4× low-pass and 24 history
  words: all of them but the last, the parallel F2's y[n−1]. A speaker frame clears only the first 12: the nasal
  zero's, the nasal pole's, F5's, F4's and F3's, and F2's y[n−2].
- **The emulator core's arithmetic quirks never occur.** `native/tms32010.c` follows MAME: ADDH tests overflow
  against the operand's high word, which is always 0 (so a sum whose high word turns negative saturates although it
  did not overflow), and MPY turns −32768 × −32768 into `0xC0000000`. The first C version modelled both and counted
  them: neither happened in the corpus, and no sum saturated anywhere. The plain C uses the true arithmetic: ADDH as a
  32-bit add, MPY as the plain product [I: whether the chip's MPY does as MAME has it].
- **Constants that nothing reads.** `data[0A]` (`0x21FC`), `data[0B]` (`0x7850`), and reset words 0x19, 0x1B, 0x1C,
  0x1F and 0x20 (`data[80 82 83 86 87]`).
- **Tone mode.** Each tone word sets one oscillator (bit 12: oscillator 1):
  - its step is the word's low 12 bits in Hz;
  - its amplitude is 2 at 1000 Hz and above, 1 below;
  - its phase restarts at 0.

  The phase counts Hz per sample and turns over at 5000, half a period at 10 kHz, where the sign flips. The sample is
  the sum of the two `cos_lookup` values times their signs. A word with bit 15 clear leaves through `COLD_START`
  without a reset.
  The oscillators live in words of the speech state. Entering tone mode zeroes both steps and phases, but the
  amplitudes keep what those words held (`nmod`, and the parallel F2's y[n−1]) until a tone word sets them. So a
  single tone sent right after speech adds a constant, `cos(0)` times that leftover, to the other oscillator. In the
  self-test's log the second word of each digit comes one sample after the first, so there it lasts one sample (and
  after `COLD_START` the leftover is 0 anyway). The dialer after speech is not checked [I].

**The DAC word.** `dsp_step` returns the DSP's word, as it goes into the output FIFO. The DAC uses its top 12 bits,
so the PCM sample is `(int16_t)(word & 0xFFF0)` (`native/dtc01.h`).

**Not yet done.**
- ~~**Simpler C.**~~ Done (2026-09-27): `dsp_synth.c` is plain C now (see Files).
- ~~**A library-side driver**~~: done (§16.11): `dsp_link.c`, the 68000's side of the link and the DAC clock.
- **The DAC-timeout paths** (`DAC_TIMEOUT`, the tone loop's lost sample) are not modelled.

### 16.11 The DSP link and the DAC clock in C (2026-09-27) — §17.5 [V: every word, reset and sample of the corpus]
**What it is.** The part of the library between klsyn and the audio: the 68000's side of the DSP link, the SPC
between the two processors, and the DAC clock that pulls samples from `dsp_synth.c`.
- **`src/speech/dsp_link.c/.h`**:
  - **the 68000's side**, from the ROM:
    - `dsp_link_init` `0x7aec` + `dsp_queue_init` `0x12226`: the pool of 48 messages of 26 words, the queue, and a
      DSP reset;
    - `dsp_post_frame` `0x7b56`: its queueing half, with the checksum or trailer (`ph_frame.c` builds the words);
    - `spc_irq_enable` `0x12216`: the queue's notify, called on every post;
    - `dsp_command_queue_isr` `0x12258` with `spc_reset_wait` `0x122fa` and `dsp_send_speech_frame` `0x122a4`;
    - the tone hooks `dsp_tone_timeout_hook` `0x124a2` and `dsp_tone_done_hook` `0x124fe`.
  - **the SPC**, as `native/dtc01.c` has it:
    - the 32-word input FIFO (an empty FIFO gives its last word again);
    - the semaphore and its error bit;
    - the interrupt enable (flags bit 6);
    - the reset line (bit 0, which also clears the FIFO and the semaphore);
    - the acknowledge (bit 1).
  - **the DAC clock**: `dsp_link_run(l, pcm, max)` makes up to `max` samples (`(int16_t)(word & 0xFFF0)`). It
    returns fewer when the DSP waits for the 68000. `dsp_link_post()` returns 0 when the pool is empty: klsyn waits
    there in the ROM. `dsp_link_tick()` is the 10 ms clock tick that times the tones; `dsp_link_run` calls it every
    100 samples.
  - **Hooks** report the DSP's ports, resets, samples and returned messages. The dialer waits on `returned()`, and
    index marks at audio time use `mark()` (§16.13).
- **When the 68000 answers.** The interrupt handler runs between DSP passes, at the end of the pass in which the
  semaphore rose, or when a post turns the interrupt on. The ROM's 68000 answers within microseconds, which fixes two
  details:
  - the DSP's re-poll right after a speaker frame always misses, because the handler has not run yet;
  - a reset by the handler kills the pass in progress, so that pass's sample never reaches the DAC. Any other effect
    (words into the FIFO, the acknowledge) only shows at the DSP's next poll, so the sample goes out first.
- **The check: `decomp/test/test_link.c`** (`build_link_test.bat`; mode `link` of `check_frames.py`). It feeds the
  link klsyn's posts as the ROM made them (`<name>.posted.tsv`, from `phcapture`) and compares everything with the
  ROM's DSP port log (`<name>.dsp.tsv`): every reset, acknowledged poll, FIFO word, semaphore and sample. The ROM's
  timing comes from the capture:
  - a post is released when the ROM's DSP found one (an acknowledged poll, or a reset, while the link's semaphore is
    still up);
  - a post during a pass (the `mid_pass` hook) is how a reset that killed a sample is replayed;
  - the phone dialer's tone items do not go through `dsp_post_frame`, so they are rebuilt from the capture's tone
    words;
  - the tone timer is ticked when the capture shows its work.

  Everything else is the link's own doing: which words go into the FIFO, the resets, and the re-sent speaker frames.
  The capture starts with the DSP idle after the power-up announcement, so the harness starts the link in that
  state.
- **Result.** All 72 corpus entries pass, every post taken; `host_phone` includes two dialled DTMF digits with their
  timeout and done hooks. (`selftest` has no posts: the self-test drives the SPC itself.) MSVC `/W4` and gcc
  `-Wall -Wextra -pedantic` are clean, and so is a gcc AddressSanitizer + UndefinedBehaviorSanitizer build.

**What the 68000's side does** (new, [V] by the check):
- **Every speaker frame resets the DSP first.** The handler keeps the last speaker frame (one pool message stays
  held) and sends it again after every other reset.
- **After an error the next post restarts the DSP.** The DSP raises its semaphore with the error bit when it goes
  idle after speech. The handler only sees it when the next post turns the interrupt on. It then resets the DSP and
  sends the kept speaker frame, and the post waits for the next round. So an utterance in a new voice starts with two
  resets: the old voice sent again, then the new speaker frame.
- **After a speaker frame the DSP always misses one frame time**, which it spends as silence from a cleared state.
- **The queue is never flushed.** Only `dsp_command_queue_isr` and `dsp_tone_done_hook` take messages off it;
  DT_STOP acts in klsyn (§15.23), not on frames already queued.
- **Tones.** A tone item goes out as two words, each waited for. The item stays queued while the tones play:
  - 16 ticks of tone;
  - `0x8000` `0x9000` (both oscillators at 0 Hz), then 6 ticks;
  - the item comes back, which `phone_dial` waits for.

  The DSP stays in tone mode after that. At 0 Hz it gives a constant, `cos(0)` × 2 oscillators = `0x3F7E`, until the
  next speech frame's handler resets it (`tone_active`).
- **The ROM's 68000 barely keeps up at the start of an utterance.** When the queue is empty at a poll, the DSP
  repeats the last frame for 64 more samples. "Hello" does this at sample 1024, in the middle of the word.
  How often it happens depends on the 68000's speed, so the library cannot reproduce it, and it is not to: the late
  frames are a byproduct of the ROM's hardware, and the library is for low latency, e.g. screen readers (user,
  2026-09-27).

**The library's way (`test_link --wav out.wav <name>`).** Every post goes in as soon as the pool has room, and
`dsp_link_run` pulls samples into a 10 kHz 16-bit wave file. No capture and no ROM are involved. Klsyn is never late
there, so the result equals the ROM's audio wherever the ROM's 68000 kept up: `voices` has r = 0.9992 against the
ROM's samples at a lag of 64 (one frame time less of leading silence), and 88 % of the samples are identical. After
one of the ROM's late frames the two drift apart in phase: `hello`'s pitch periods are shifted after sample 1024.

- **The library turns on the DSP fixes** (§16.12): `l->dsp.fixes = DSP_FIX_FIRST_PERIOD | DSP_FIX_ROUND` after
  `dsp_link_init`.

**Not yet done.**
- ~~**Index marks at audio time**~~: done (§16.13): tags ride with the frames, and `mark()` reports each at its
  frame's first sample.
- ~~**The kernel around it**~~: done (§17.9): klsyn waits on a semaphore for room in the pool, and the link's
  `returned()` hook wakes it and sets `yield`, so `dsp_link_run` returns and klsyn posts the next frame at once.
- **The outputs** (§17.2): the audio device, a wave file and memory buffers, fed by `dsp_link_run`.
- **The DAC-timeout paths** of the DSP program (§16.10).

### 16.12 Three flaws in the DSP program, and the library's fixes (2026-09-27) [V]
All three are in the ROM's own output (the emulator's captures) and in the C that reproduces it. The fixes are
options in `dsp_t.fixes`, off by default, so every ROM check (`dsp`, `link`) still runs the ROM's behaviour. The
library turns them all on (user, 2026-09-27: its goal is good output and low latency, e.g. for screen readers, not the ROM's
timing).

**1. The thump at the start of an utterance (`DSP_FIX_FIRST_PERIOD`).** A low, smooth click of about 3 ms, heard
when speech starts after a DSP reset: in "hello" the samples go 80, 288, 688, 1248, 1920, 1536, 528, −80, then
settle (the ROM's capture has the same bump, peak 2112).
- **Why.** The cascade's F1-F3 and nasal-zero coefficients are set only when a glottal period begins
  (`PITCH_SYNC_RESET`, when `nper` reaches T0).
  - Until the first speech frame after a reset, T0 is 0. So every 4× step "begins a period" and sets the
    coefficients from the empty frame: F = 0 and bandwidth 0, which give C = 2·0 − 1 = −1 and B = 2·cos 0. That is
    a pole pair on the unit circle at 0 Hz, an undamped double integrator.
  - The first speech frame overwrites T0 (364 in "hello"), and the next period begins only T0/4 ≈ 90 samples later.
    Until then the degenerate poles stay.
  - The first frames' aspiration (AH, only ±1 at the cascade's input) is integrated twice into a ramp (to 2130 in 26
    samples). When the real coefficients arrive, it rings out through F1 as the thump.
  - Only utterances that start with aspiration excite it, e.g. /h/. In the corpus it happens in 15 of 72 entries,
    once per reset, peaking at 2114-4178 (`dv_cmds`, whose voice changes reset the DSP mid-text).
- **The ROM's slip.** parwav keeps the period's T0 apart from the frame's, and it is 0 until the first period: its
  first sample begins a period with the frame's own values. The DSP program stores the frame's T0 straight into the
  word it compares with, so the first period waits a whole T0.
- **The fix.** At the first speech frame after a reset (T0 was 0), set `nper = T0`, so the next 4× step begins the
  period with that frame's values (`read_speech_frame`). The glottal periods of that utterance then start about
  T0/4 samples earlier; nothing else changes. With it, no entry of the corpus ever runs the cascade on the
  degenerate poles with a signal (a counting build of `dsp_synth.c`: 0 samples, against 18-125 per entry without).

**2. A DC offset (`DSP_FIX_ROUND`).** The output rides on a small constant: "hello" averages +82, and the ROM's
capture ends at +32 in the closing silence (about −60 dBFS). It is a tiny step wherever the output starts or stops,
e.g. at a reset or when the DSP goes idle.
- **Why.** Every filter sum is shifted right by 12, which truncates: on average −½ LSB per section and per sample.
  In narrow low resonators (F1, the nasal pole) the feedback multiplies that into a steady offset of tens of LSBs.
- **The fix.** Add `0x800` before the shift in the filter sums (`mac3`), i.e. round. The mean falls to 2 ("hello")
  and −0.3 (`voices`), and the sound is otherwise the same (r = 0.99992 against the truncating version on `voices`).
  What is left, 0 or −16, is the DAC's own 12-bit step (`& 0xFFF0`).

**Hearing it:** `test_link --wav out.wav <name>` renders an entry the library's way, and `--fix` turns both fixes
on.

---

**3. The tones' clicks and DC (`DSP_FIX_TONE`, found 2026-09-27 with `TextToSpeechPlayTones`, §17.11).**
- **What.** The tone generator's oscillators are cosines (`TONE_SAMPLE` `0x6D2`), and a tone word sets the phase to 0,
  the cosine's peak. So:
  - a tone starts with a step to its peak;
  - a 0 Hz word, which ends every tone (the 68000's timeout hook sends `0x8000` and `0x9000`), freezes each
    oscillator at its peak.
- **Effect.** After a DTMF digit the DAC holds 8,120 per oscillator, 16,240 in all, about half the scale, through
  the silence and until the next reset. The real unit's audio path was presumably AC-coupled, so the DC went
  unheard, but a file or a sound card gets a DC step and two clicks.
- **The fix.** A tone starts a quarter period in, at the cosine's zero, and a 0 Hz oscillator has amplitude 0.
- **The check** (`test_lib api`): a 100 ms DTMF 0 is loud until sample 1,000, then silent; before the fix it held
  16,240 to the end.

### 16.13 Index marks at audio time (2026-09-27) — §17.2 [V: every speech frame of the corpus]
**Why.** v1.8 acts on an index mark (DT_INDEX, DT_INDEX_REPLY, `[:in n]`) when `phsettar` starts the marked phone
(`index_mark_reached` `0xfaaa`, §15.23). That is when klsyn computes the frame, which can be up to the DSP queue's
depth ahead of the audio: 48 frames, 307 ms. The library reports marks when the audio reaching them is played or
delivered (user, 2026-09-27; dapi's timing). A screen reader uses marks to follow the speech, so they have to match
what is heard.

**How.**
- **klsyn** (`ph_task.c`). `index_mark_reached` now hands the mark to `ph_mark_hook` when one is set. The mark's
  action is its own function, `index_mark_spoken`: set `last_index`, and for a `0x67` mark answer the host (R2 = 31).
  With no hook, `index_mark_reached` calls it at once, as the ROM does, so all the ROM checks still apply. The sync
  mark `0x68` is unchanged.
- **The frame it goes with.** `phsettar` runs just before that frame is drawn and posted (`phclause_draw_frames`:
  `phsettar`, `phdraw`, `pht0draw`, `dsp_post_frame`). So the mark belongs to the next post, and every mark is
  followed by one.
- **The link** (`dsp_link.c`). A post carries a tag (not 0). The tag goes into the FIFO with the frame's first word
  (`fifo_tag[]`). When the DSP reads that word, the tag waits for the next sample the DSP makes, and `mark(tag,
  sample)` reports it with that sample's number, counted from 0 at `dsp_link_init`. That is the first sample made
  from the frame, since the DSP reads a frame at the start of a pass. Tags are never lost:
  - if a reset kills that pass (§16.11), the tag goes out with the next sample made;
  - a reset that clears the FIFO turns any unread tags into pending ones.

  Resent speaker frames carry no tag.
- **The library's part** (to build with the outputs):
  - `ph_mark_hook` keeps the mark for the next post;
  - the post carries the mark's id as its tag;
  - `mark()` gives the mark's sample number;
  - the output calls `index_mark_spoken` and the API's `TTS_MSG_INDEX_MARK` when that sample is played (the
    device's position) or delivered (a buffer's `TTS_INDEX_T.dwIndexSampleNumber`, a file's write).

**The checks.**
- `test_link` (mode `link`) tags every post. Each speech frame's tag must come at the first sample the ROM's DSP made
  after reading that frame: **104,430 frames across the corpus, all at the right sample**, each tag reported once. A
  tag one sample late fails.
- `test_frames --task` now runs with `ph_mark_hook` set. Every mark must come just before the post the ROM made next
  (the ROM's post count at the mark, `marks.tsv`), and `last_index` and the replies still match the ROM.
- The ROM's own marks: `index` (`00660005`, `00670007`) and `clause_log` (`00660005`).

**Which phone.** In the ROM a marker goes with the allophone before it, so it comes one phone early. The library puts
it on the first phone of its word (§17.13).

**What it changes.** `test_link --wav` runs the library's way: klsyn posts whenever the pool has room. There,
`index`'s DT_INDEX 5 is heard at 288 ms and its DT_INDEX_REPLY 7 at 422 ms. Their frames were posted 288 ms and
218 ms earlier, which is when v1.8 would have acted. `clause_log`'s mark comes 262 ms after its post. That early
time is also when `last_index` changed for DT_INDEX_QUERY. (These leads are measured on runs of 1,024 samples, so
they are approximate.)

## 17. The speech library's API (draft, 2026-09-27)

**Status.** The draft is two files: `src/api/ttsapi.h` (the header) and `src/api/dectalk.def` (the
exports). They were first drafted as `dtc01tts.h`/`.def` and renamed after the user's answers (17.6). **Built
(2026-09-27, 17.10):** `DECtalk.dll` and `libtts_us.so` speak through the thread and the three outputs; the user
dictionary, v1.8's voices, the tones, the console and the log file are still to come. The header compiles as C and C++, with the Windows types and with its own typedefs. dapi's
SAY and speak samples compile against it unchanged (17.8).

The user's decisions (§13 item 15):
- the speech synth becomes a DLL (Windows) or a shared object (Linux);
- the host terminal emulator becomes a program that uses it;
- the exports follow dapi's;
- the API is drafted before the library is built;
- it has an index callback;
- dapi's SAY and speak become the project's own speaking programs, on Windows and Linux (17.8).

### 17.1 What follows dapi

- **dapi's names:**
  - the header `ttsapi.h`;
  - the library `DECtalk.dll` (`LIBRARY DECTALK`, import library `dectalk.lib`) on Windows and `libtts_us.so` on Linux
    (dapi's Linux name for US English);
  - the functions `TextToSpeech…`, `LPTTS_HANDLE_T`, `MMRESULT` with the `MMSYSERR_…` codes.
- **dapi's types, constants and structures** (`dapi/src/API/TTSAPI.H`):
  - `TTS_BUFFER_T`, `TTS_INDEX_T`, `TTS_PHONEME_T`, `TTS_CAPS_T`, `struct dic_entry`, `VERSION_INFO`, `LANG_ENUM`,
    `SPDEFS`;
  - the speaker numbers (`PAUL` … `RITA`; v1.8's voice table `0x16146` has the same order). Slot 8 is dapi's `WENDY`,
    which is Val in v1.8;
  - `TTS_NORMAL`/`TTS_FORCE`, `LOG_TEXT`/`LOG_PHONEMES`, the status identifiers and the error codes 1-13.
- **Every export of `dapi/src/dectalk.def`, with its ordinal.** The ones v1.8 has no counterpart for are stubs (17.4).
  A Windows program imports its DLL functions by name and does not start if one is missing, so a missing export
  would stop every dapi program that uses it, even for a menu item it never uses. The additions dapi does not have
  are marked `[DTC01]` and numbered from 100.
- **Both of dapi's startup forms:**
  - `TextToSpeechStartup` is dapi's per-system form. On Windows it takes an `HWND` and posts three registered window
    messages ("DECtalkErrorMessage", "DECtalkIndexMessage", "DECtalkBufferMessage"; `wParam` = the code, `lParam` =
    the value), as dapi's DLL does. On Linux it is the callback form, as dapi's Linux library has it.
  - `TextToSpeechStartupEx` is the callback form on both.
- **dapi's callback**, `(lParam1, lParam2, instance parameter, message)`, with its three messages:
  - `TTS_MSG_BUFFER`: a buffer is full;
  - `TTS_MSG_INDEX_MARK`: `lParam1` is `TTS_INDEX_MARK`, or `[DTC01]` `TTS_INDEX_REPLY` (31) for `[:re n]`, and
    `lParam2` is the value;
  - `TTS_MSG_STATUS`: audio start and stop, errors.

  No message carries a pointer in a `LONG`, which is 32 bits even in 64-bit builds; a full buffer is taken with
  `TextToSpeechReturnBuffer`. The window message's `lParam` does carry the buffer's address, as in dapi.
- **The version:** `TextToSpeechVersion` returns the string "DECtalk v1.8" (user, 2026-09-27). Its number keeps dapi's
  layout: DECtalk version 1.8 in bits 16-30, DLL API 1 in bits 8-15, and the library's revision in bits 0-7.
  - DLL API 1 is dapi's "DECtalk DLL", which speak's `CheckVersion` requires.
  - speak turns highlighting on for a revision of 3 or more, and shows its control-panel menu for 20 or more. So the
    revision should stay between 3 and 19.
  - `TextToSpeechVersionEx` gives `DTalkVersion` 0x0108, `VerString` "DECtalk v1.8" and `Language` "US".
- **The calling convention is dapi's:** the C default, `__cdecl`, named by the `.def`. A 32-bit build of `DECtalk.dll`
  can therefore replace dapi's under an old 32-bit Windows program; a 64-bit build is for programs built now.
- **Text is v1.8's.** `TextToSpeechSpeak` takes what v1.8 reads from its host line: `[:np]`, `[:ra n]`, `[:in n]`,
  `[:dv …]` (§8). dapi's later spellings (`[:name paul]`, `[:index mark n]`, `[:phoneme on]`) are not translated;
  programs written for dapi need v1.8's (17.8).

### 17.2 The model

- **One text queue,** like the firmware's text pipe. `TextToSpeechSpeak` writes the bytes the host task writes today:
  8-bit text, CTRL-K to end a clause, STX/ETX for phonemic text, `[: ]` commands and index marks. **It never waits**
  (user, 2026-09-27), as dapi's does: the queue grows. v1.8's 64-byte pipe, which made the host task wait and the
  host line send XOFF, becomes the host program's own wait: it passes text on while
  `TextToSpeechGetStatus(INPUT_CHARACTER_COUNT)` is under 64 (17.3, 17.10).
- **One thread inside the library** (user, 2026-09-27). It runs `dttask`, `klsyn` and a C model of the DSP program,
  with the kernel's mailboxes and pipe. A cooperative scheduler keeps the firmware's order of events. Callbacks come
  from that thread. (Built: the engine and its kernel, §17.9.)
  - `[DTC01]` `TTS_MANUAL_CLOCK` starts the library without a thread instead: the caller runs it with
    `TextToSpeechRun(n)`, so runs are repeatable, as the emulator's are. This is for tests.
- **Output to one place at a time, as in dapi** (user, 2026-09-27):
  - **the audio device**, by default (`uiDeviceNumber`: `WAVE_MAPPER` or a device number). `OWN_AUDIO_DEVICE`,
    `REPORT_OPEN_ERROR` and `DO_NOT_USE_AUDIO_DEVICE` mean what they mean in dapi. Without a device, `Startup`
    returns `MMSYSERR_NODRIVER`; SAY then starts again with `DO_NOT_USE_AUDIO_DEVICE`.
    - Windows: `waveOut` (winmm), as dapi.
    - Linux: ALSA, loaded when needed (`dlopen("libasound.so.2")`), so the library needs no sound library to link or
      to run with files and buffers. PulseAudio and PipeWire systems play through ALSA's plugins. dapi's own Linux
      code used OSS (`/dev/dsp`, `NT/linux_audio.c`), which current systems rarely have.
  - **a wave file**, from `OpenWaveOutFile` until `CloseWaveOutFile`, in place of the device;
  - **memory buffers** (`OpenInMemory`, `AddBuffer`, `ReturnBuffer`), in place of the device.

  Synthesis runs as fast as the output takes it. That is the role the DSP's DAC had: real time into the device, as
  fast as possible into a file, as fast as buffers come back into memory. `Pause`/`Resume` pause the device.
- **Only 10,000 samples a second** (user, 2026-09-27: no resampling). dapi's format constants choose the sample
  encoding only:

  | Constant | Encoding | dapi's rate |
  |---|---|---|
  | `WAVE_FORMAT_1M16` | 16-bit linear (the 12-bit DAC value, shifted up 4 bits) | 11,025 Hz |
  | `WAVE_FORMAT_1M08` | 8-bit linear, unsigned | 11,025 Hz |
  | `WAVE_FORMAT_08M08` | 8-bit mu-law | 8,000 Hz |

  The data is always at 10,000 Hz, and a wave file's header says so. Any other format is `WAVERR_BADFORMAT`.
  `GetCaps` reports 10,000 Hz.
- **The phone line is an emulation for now** (user, 2026-09-27). While the simulated line is off hook, the host
  terminal emulator switches the output to memory buffers and passes them to its line model, where the DTMF receiver
  hears the unit's own tones (§15.34). A real line or VoIP would take the same buffers later.
- **Index marks** are reported when the audio reaching them is played or delivered (user, 2026-09-27: dapi's
  timing). The buffers carry each mark's sample number. v1.8 reports a mark when `phsettar` computes its frame, up to
  the DSP queue's depth earlier (about 220-290 ms in the library's timing), so an R2 = 31 reply moves by that much.
  The mechanism is done (§16.13): `ph_mark_hook`, tags on the posts, and `dsp_link`'s `mark()` with the sample number.
- **A handle, but one instance for now.** The C keeps the firmware's globals, so a second `Startup` returns
  `MMSYSERR_ALLOCATED`, even after `Shutdown` (the globals cannot yet go back to power-up; 17.10). Several instances would need the globals moved into the handle.
- **Startup is v1.8's speech initialization:** the DSP link, the two tasks, Paul at 180 wpm, MODE SQUARE and an empty
  user dictionary (the speech part of `speech_init` `0x30c8`). The host part stays in the host program: the settings
  and NVRAM, the other tasks, and the `[:np :ra 180]` that `main_task` writes.

### 17.3 How the host terminal uses it (the firmware's coupling points → calls)

| Firmware (host side) | Call |
|---|---|
| host-line and local-terminal text into the pipe (`stream_printf`/`stream_putc` on `cur_stream`) | `TextToSpeechSpeak`; the host program first waits while `GetStatus(INPUT_CHARACTER_COUNT)` is 64 or more (v1.8's pipe size), which keeps the host line's XOFF where v1.8 had it |
| CTRL-K: SUB on the host line, the 5 s host timeout, an empty EDITED line | `Speak("\x0b")`, or `TTS_FORCE` |
| DT_SYNC (`emit_sync_marker`: the `0x1A` marker, then `sem_wait(&sync_sem)`) | `TextToSpeechSync`, which is dapi's: it returns when the audio has been heard, a little later than v1.8, whose DT_SYNC returned when klsyn had taken the marker (17.10) |
| DT_STOP (`dt_stop` resumes the stop task: `stop_pending`, a sync) | `TextToSpeechReset(h, FALSE)`, called from the host program's own stop thread, so the host task goes on as in v1.8 (user, 2026-09-27) |
| DT_INDEX, DT_INDEX_REPLY (`\n STX :in n ETX \n` into the pipe) | unchanged: in-text commands through `Speak` |
| the `:re` reply (`ph_index_reply_hook` → `send_dcs_reply(31, n)`) | the callback: `TTS_MSG_INDEX_MARK`, `TTS_INDEX_REPLY` |
| DT_INDEX_QUERY (`last_index`) | `TextToSpeechGetStatus(STATUS_LAST_INDEX)` |
| DT_DICT (`dict_hash_insert_or_delete`) | `TextToSpeechAddUserEntry` (an empty substitution deletes, v1.8's rule); `MMSYSERR_NOMEM` = R3 1 |
| RIS and power-up (`dict_hash_clear_all`) | `TextToSpeechUnloadUserDictionary` |
| DT_MODE, DT_LOG (the flag words the speech side reads) | `[DTC01]` `TextToSpeechSetMode`, `TextToSpeechSetLog` |
| what the speech side prints on the console (`kprintf`: `[:dv list]`, "Illegal voice", the logs) | `[DTC01]` `TextToSpeechSetConsole` |
| DSR error 25 (bit `0x08` of `dt_error_flags`, set by `parse_phonemic_text`) | `TextToSpeechGetStatus(STATUS_ERRORS)`, which reads and clears |
| DT_PHONE dialing (`phone_dial`: tone messages straight to `dsp_link_queue`, waiting for each) | `[DTC01]` `TextToSpeechPlayTones(high, low, 160, 60)` |
| the self-test's DTMF loopback (`reset_entry`) | `TextToSpeechPlayTones`, and the program's own receiver |
| off hook / on hook (the audio goes to the line) | `TextToSpeechOpenInMemory` / `CloseInMemory` (17.2) |
| DT_SPEAK | nothing: the host program stops calling `Speak` |

The speech side reads no other host variable. `speak_enabled`, the character sets, the phone state and the lines stay
in the host program.

### 17.4 dapi functions v1.8 has no counterpart for (stubs)

All are exported, so dapi programs load (17.1). Each does the least that is true:

| Function | Stub |
|---|---|
| `…ControlPanel` @28, `…Typing` @29 | nothing (no control panel, no typing mode in v1.8) |
| `…Reserved1-3` @31, @32, @39, `…Tuning` @51 | return 0 (dapi's header gives no prototype) |
| `…StartLang` @34, `…SelectLang` @35, `…CloseLang` @36, `…EnumLangs` @38 | one language: "US" is 1 and selects; anything else `TTS_NOT_SUPPORTED` |
| `…GetFeatures` @37 | 0 |
| `…GetSpeakerParams` @40, `…SetSpeakerParams` @41 (`SPDEFS`) | `MMSYSERR_NOTSUPPORTED`: dapi's later 33 parameters are not v1.8's 28. `[DTC01]` `TextToSpeechGetVoice`/`SetVoice` use v1.8's (`DTC01_VOICE_T`) |
| ~~`…DictionaryHit` @42, `…DumpDictionary` @43~~ | done (§17.11): the built-in trie, pronunciations as phonemic text |
| ~~`…ConvertToPhonemes` @50~~ | done (§17.11): the pipeline's phoneme log (LOG_PHONEMES), dapi's `TTS_SILENT` |
| `…GetPhVdefParams` @52 | `NULL` |

Kept, but with no v1.8 counterpart:
- `TextToSpeechPause`/`Resume` pause the audio device.
- `TextToSpeechLoadUserDictionary` reads a text file of DT_DICT pairs (v1.8 has no dictionary file).
- `SetRate`, `SetSpeaker` and `SetVoice` queue `[:ra]`, `[:n…]` and `[:dv]` commands, as a host would.
- `OpenLogFile` writes v1.8's console log lines (LOG_TEXT, LOG_PHONEMES) to a file. LOG_SYLLABLES (dapi's) is
  `MMSYSERR_INVALFLAG`.

### 17.5 Before the library can be built

- ~~**The DSP in C.**~~ Done (§16.10, 2026-09-27): `dsp_synth.c` gives every sample of the corpus as the ROM's
  DSP does. ~~The DAC clock~~: done (§16.11): `dsp_link.c` is the 68000's side of the link, the SPC and the DAC
  clock, checked word for word and sample for sample on the ROM's timing.
- ~~**The kernel for the speech side:**~~ Done (§17.9, 2026-09-27): `kernel.c` (the pipe, the mailboxes and the
  semaphore; the tasks as threads that take turns by priority) and `engine.c` (`dttask`, `klsyn`, the DSP link and
  the DAC clock), checked from power-up on the whole corpus.
- ~~**The library's thread and outputs:**~~ Done (§17.10, 2026-09-27): `DECtalk.dll` / `libtts_us.so` speak to the
  device, a wave file or memory buffers, with the events at their sample.
- **The shared variables behind the API.** The speech C now reads DT_LOG, DT_MODE, `dt_error_flags`, `last_index`
  and `cur_stream` as host globals, and `ph_index_reply_hook` is a bare function pointer. (2026-09-27: the library
  reaches them through `engine_get`/`engine_set`, under the kernel's lock, §17.10.)
- **A harness for the library** that the emulator can check, as `test_host` checks the tasks.
- **A build for both systems: CMake** (user, 2026-09-27). One CMake project builds the library, the host terminal
  emulator, SAY and speak, with MSVC on Windows (x64, and x86 for old DECtalk programs) and gcc on Linux. CMake comes
  with the installed Visual Studio Build Tools; the machine also has WSL (Ubuntu) for the Linux build. The `.bat`
  files and the test tools (the emulator, the captures, `check_frames.py`) stay as they are: they were for debugging
  the decompilation and are not part of the product (user, 2026-09-27).

### 17.6 The user's answers (2026-09-27)

| # | Question | Answer | Where |
|---|---|---|---|
| 1 | a thread inside the library, or only caller-driven | the thread (`TextToSpeechRun` stays for tests) | 17.2 |
| 2 | an audio device | as dapi: the device, memory buffers or a wave file; the phone line is an emulation for now | 17.2 |
| 3 | DT_STOP | a blocking `Reset` from the host program's stop thread | 17.3 |
| 4 | names | dapi's: `ttsapi.h`, `DECtalk.dll`; a version string "DECtalk v1.8" from dapi's `TextToSpeechVersion` | 17.1 |
| 5 | Windows `Startup` | also dapi's window-message form, for existing Windows DECtalk programs; find the Linux counterpart | 17.1, 17.7 |
| 6 | sample rates | 10 kHz only, never resampled | 17.2 |
| 7 | index timing | at audio time | 17.2 |

### 17.7 The Linux counterpart of window messages

A window message does two things. It carries the event, and it delivers it in the program's own GUI thread, so the
program needs no locking. The callback does only the first: it runs on the library's thread.

Linux has no system-wide message queue:
- X11's `ClientMessage` works only under X, not under Wayland.
- GTK, Qt and the others each have their own loop.
- What every Linux event loop can wait on is a **file descriptor**: `poll`/`select`, GLib's `g_unix_fd_add`, Qt's
  `QSocketNotifier`.

So the counterpart is an **event queue with a waitable handle**, `[DTC01]`:
- `TTS_EVENT_QUEUE` (a `Startup` option) sends the messages to a queue instead of the callback.
- `TextToSpeechGetEventHandle` @109 gives the handle. On Linux it is a descriptor that polls readable while the queue
  is not empty (a pipe or `eventfd`). On Windows it is an event object, for `MsgWaitForMultipleObjects`.
- `TextToSpeechGetEvent` @110 takes the oldest message (`TTS_EVENT_T`: message, `lParam1`, `lParam2` and the sample
  number), and never blocks.

It works on both systems, so a portable program can use it everywhere, and a Windows program can keep using `HWND`.

### 17.8 dapi's SAY and speak samples

The user supplied two dapi SDK samples in `samples/` (2026-09-27). They are to become the project's own speaking
programs, on Windows and Linux, next to the host terminal emulator. The project then has four parts:
- the speech library;
- the host terminal emulator;
- SAY;
- speak.

SAY and speak speak without emulating the host side.

**What they are:**
- **`samples/SAY/say.c`** (530 lines): a console program. It speaks its arguments or standard input. Its options are:
  - `-w` a wave file;
  - `-l[t|p|s]` a text, phoneme or syllable log;
  - `-d` a user dictionary;
  - `-pre`/`-post` text before and after the input.

  Its only Windows parts are the console handles and the CTRL-C handler.
- **`samples/speak/Speak.c`** (4,545 lines, DEC/SMART 1996-99): a Win32 editor that speaks. It has:
  - nine voice buttons with pictures, and a rate slider;
  - highlighting of the word being spoken, done with index marks;
  - open and save for text, load and unload for the user dictionary, and conversion to a wave file in three formats;
  - find, a control panel and a typing demo;
  - language menus, registry lookups (`coop.h`, not in the folder; `dapi/src/API/coop.h`) and a help file.

  `speak.mak` builds only `SPEAK.C` and `SPEAK.RC`. Several other files are not part of the build:
  - `speakmul.c`/`.h`, `speakorg.rc` and `speak_gr`/`_sp`/`_us.rc` are other variants;
  - `DECTALK.PH` (a later control panel's voice file) and `PHDATA.OUT` (a later DECtalk's synthesizer parameter dump)
    are not used;
  - `build/`, `.aps`, `.ncb`, `.opt` and `.plg` are IDE output.

**Compile check (2026-09-27, MSVC 2022).** Both compile against `ttsapi.h` with no change to their API use:
- SAY compiles with no error as x86 and as x64.
- speak compiles as x86, apart from `DTALK_HELP_FILE_NAME`, which comes from the missing `coop.h`. The four
  wave-file error codes it tests (8-11) were added to the header for it.
- As x64, speak also needs the usual Win64 fixes: `GWLP_WNDPROC`, and pointers kept in `LONG`.

**What they would do with v1.8, and the change each needs:**

| Sample | With v1.8 | Change |
|---|---|---|
| speak, highlighting | inserts dapi's `[:i m n]` after each word; v1.8's index mark is `[:in n]` (what v1.8 does with `[:i m n]` is unchecked, [I]) | write `[:in n]` |
| speak, Wendy button | `[:nw]`; v1.8 has no `:nw` | Val, `[:nv]`, with its own picture and label |
| speak, rate slider | 75-600 wpm; v1.8 clamps to 120-350 | 120-350 |
| speak, wave menu | says 11.025 kHz and 8 kHz | the files are 10 kHz (17.2); relabel |
| speak, typing demo, control panel, language menus, registry, help | no v1.8 counterpart | remove (the stubs keep the original loading) |
| speak, version check | DLL API 1 passes; highlighting needs revision 3 or more | keep the library's revision 3-19 (17.1) |
| SAY, `-ls` | LOG_SYLLABLES: `MMSYSERR_INVALFLAG` | remove the option |
| SAY, `-w` | writes 10 kHz files | update the help text |
| SAY, no sound device | needs `MMSYSERR_NODRIVER`, then starts again with `DO_NOT_USE_AUDIO_DEVICE` | none (as drafted) |

**Plan (user, 2026-09-27):**
1. **Keep `samples/` untouched** as the reference, as `dapi/` is.
2. **SAY becomes portable C,** `src/apps/say/`:
   - standard I/O in place of the console handles;
   - `signal(SIGINT)` in place of `SetConsoleCtrlHandler` (it exists on both systems);
   - `TextToSpeechStartupEx` on both systems.
3. **speak: two native front ends over one portable core** (user's choice, option (a) of three; the others were one
   GTK 3 program for both systems, or the Win32 program under Wine), `src/apps/speak/`:
   - the core, portable C: marking the text with `[:in n]` for highlighting, finding the word a mark belongs to, the
     voice table (Val in place of Wendy), the rate range 120-350, opening and saving text, the user dictionary, wave
     output;
   - Windows: the Win32 program, ported onto the core. It starts with the window-message form, which it tests;
   - Linux: a GTK 3 program onto the same core. It uses the event queue (17.7).
4. **Nothing in the product loads the ROMs**, not even for a while (user, 2026-09-27; §0.10 of AGENTS). The programs
   wait for the C library (17.5). A stand-in `DECtalk.dll` running the ROM in the emulator was proposed and dropped.
5. **No licensing concern** (user, 2026-09-27: DEC no longer exists). The ports may reuse the samples' code.

**Done (2026-09-27): §17.12.** One finding changes the table's highlighting row: v1.8's `[:in n]` marks change the
speech around them (§17.12). The library now keeps them neutral (§17.13), and speak's highlighting is on by default,
with an on/off switch.

### 17.9 The speech engine and its kernel (2026-09-27) — §17.5 [V: every klsyn message and frame of the corpus, from power-up, on Windows and Linux]

**What it is.** The speech side, runnable on its own: what the library's thread will drive.
- `src/kernel/kernel.c` / `.h`: the kernel, i.e. the `rtos.h` calls for real, on Win32 threads or pthreads.
- `src/speech/engine.c` / `.h`: the engine: `dttask` and `klsyn` on that kernel, the DSP link (§16.11), the DSP
  program (§16.10) and the DAC clock.
- `decomp/test/test_engine.c` (build: `build_engine_test.bat`; mode `engine` of `check_frames.py`): the check.

**The kernel.**
- **Why threads.** The tasks wait deep in their call stacks, so they cannot be turned into plain function calls without
  giving up the word-for-word match with the ROM:
  - `dttask` waits in `dev_getc` inside `clause_readin`, and for a free message inside `newclause`;
  - `klsyn` waits in `mbox_get`, and in the ROM inside `dsp_post_frame` when the DSP pool is empty, which is in the
    middle of a clause (`phclause_draw_frames`).

  So each task keeps its own stack: it is an OS thread.
- **One task at a time, as on the 68000.** The running task holds the kernel's mutex, and hands it on only in a
  kernel call. `kernel_run()`, on the caller's thread, gives the turn to the highest-priority ready task, and gets it
  back when every task waits.
  - A task that wakes a higher-priority one is preempted at that call. In the ROM too, only kernel calls and
    interrupts switch tasks.
  - So the firmware code needs no locks, and a run is deterministic: the same text gives the same messages, frames
    and samples.
- **The calls:**
  - `mbox_init`, `mbox_init_pool`, `mbox_get`, `mbox_put`;
  - `sem_wait`, `sem_signal` (a signal hands the count straight to the first waiter);
  - `pipe_open`, `dev_getc`, `dev_putc` (the console goes to a hook), `dev_control` (nothing to do: only one task
    runs), `dev_rx_held`;
  - `panic`, which reports and ends the task.

  The speech side calls nothing else. `event_wait`, `task_suspend`/`resume`, the heap and `system_restart` belong to
  the host side's tasks, which the library does not run.
- **The pipe grows.** The ROM's holds 64 bytes, and then the host task waits (which makes the host line send XOFF).
  `engine_write` never waits, and `engine_pending()` gives the bytes not yet read, so a caller can hold back at a limit
  of its own. §17.2's draft has `TextToSpeechSpeak` block "while the queue is full": that limit is now the API
  layer's choice.
- **Shutdown.** Each task leaves its wait (a `longjmp` back to the start of its thread); the threads are joined and the
  memory freed. The firmware's globals are not reset, so there is one engine per process lifetime (§17.2: one
  instance for now).

**The engine.**
- **`engine_init` is the speech part of `speech_init` `0x30c8`.** In order:
  - `dsp_link_init`;
  - the tasks, at the ROM's priorities (klsyn 50, dttask 10); they start, and wait;
  - then what the `main` task (priority 0) does once they wait: `load_voice_definition` (Paul), `save_voice_params`
    (Val = Paul) and `newclause(0)`. That last one sends `dttask`'s first clause, still empty, to klsyn as a one-word
    message.

  `settings_reset` and the greeting (`[:np :ra 180]`, "DECtalk version ... is running") stay in the host program.
- **klsyn's posts.** `ph_frame_sink` waits on a semaphore while the link's pool is empty; in the ROM this is
  `dsp_post_frame`'s `mbox_get` on the DSP pool. Then it posts the frame, with the tag of the mark that goes with it.
  - The link's `returned()` hook (the ROM's ISR giving a message back) signals the semaphore. It also sets the new
    `dsp_link_t.yield`, which makes `dsp_link_run` return after the current sample.
  - `engine_run` then runs klsyn, which posts the next frame at once. So the queue stays full while klsyn has frames,
    and there are no late frames (§16.11).
- **`engine_run(pcm, max)`** takes turns: the tasks, then the DAC clock, then the tasks again. It returns fewer than
  `max` samples when the DSP waits for the 68000 and the tasks post nothing, because everything is said or a clause
  is not finished yet.
- **Index marks** (§16.13):
  - `ph_mark_hook` numbers each mark, and the next post carries the number;
  - the link reports it with the first sample made from that frame;
  - `hooks.mark(mark, sample)` goes to the library, which calls `engine_mark_spoken` when that sample is played; with
    no hook, the engine calls `index_mark_spoken` itself, at the sample.
- **The console** (DT_LOG text, `[:dv]` output, `log_error` under DT_LOG `0x20`) goes to `hooks.console`.
- **Four `.data` values had no C initializer,** because every harness until now imported them from the ROM's RAM:
  - `sprate` = 180 (`0x82296`);
  - `hatstate` = 1 (`0x822a0`);
  - `hatsize` = 200 (`0x822a2`);
  - `spdef_dirty` = 1 (`0x8229e`, the first clause posts the speaker packet).

  They come from the `.data` image at ROM `0x1d408`, and are now in `ph_timing.c` and `ph_frame.c`.

**The check** (`test_engine`, mode `engine`).
- **The run.** From C power-up, `test_engine` feeds the characters `dttask` read in the ROM (`scan.tsv`), boot
  greeting included, one at a time as `dttask` asks for them (the kernel's `call` hook). The other tasks' changes
  (DT_MODE, DT_LOG, DT_DICT) are applied where `test_dttask` applies them, and `stop_pending` where
  `test_frames --task` replays it. Everything else runs as the library runs it: the engine makes samples until it has
  nothing more to say.
- **What it compares:**
  - every message klsyn takes after the greeting: its words, `stop_pending`, and the posts so far;
  - every frame posted, word for word;
  - the index marks, in order.
- **Result: 72 of 72 entries pass,** built with MSVC on Windows and with gcc (pthreads) on WSL Ubuntu. That is 873 s
  of speech, and includes the three DT_STOP entries and the marks of `index` and `clause_log`.
- **What it found:** Val's power-up copy. Without `speech_init`'s `save_voice_params`, `voices2` and `phon_misc`
  went wrong at their first `[:nv]`, whose speaker packet was all zeros.

**Speed and latency** (MSVC `/O2`, this machine, DSP fixes on):
- about 400× real time;
- 0.1-0.2 ms of CPU from `engine_write` to the first 64 samples;
- the first frame with sound is the clause's second frame, about 13 ms into the audio (for "Hello world." it is
  the /h/ fading in).

**Not yet:**
- ~~the library's thread and the outputs; `Sync` at audio time; `TextToSpeechReset`~~: done (§17.10);
- the rest of the API layer (§17.10's "not yet").

### 17.10 The library: its thread and its outputs (2026-09-27) — §17.5 [V: the same samples and marks through every output, on Windows and Linux]

**What it is.** The speech library now builds and speaks:
- `DECtalk.dll` on Windows (`decomp/build_lib.bat`, with `dectalk.def`);
- `libtts_us.so` on Linux (gcc, pthreads, `-fvisibility=hidden`: its 63 exports are the API's and nothing else).

It is four files in `src/api/`:
- `ttsapi.c`: the handle, the thread, the outputs, and the text calls (Startup, Shutdown, Speak, Sync, Reset,
  Pause/Resume, GetStatus, the wave file, memory buffers, the manual clock, the event queue);
- `tts_audio.c`: the audio device;
- `tts_os.h`: a mutex, a condition variable and a thread, on Win32 or pthreads;
- `tts_stubs.c`: every other export.

**The thread.**
- **It drives the engine** (§17.9), 256 samples (25.6 ms) at a time, only as fast as the output takes them:
  - the device: as it plays, about 200 ms ahead;
  - a wave file: as fast as it can;
  - memory buffers: as fast as the program adds them;
  - no output (`DO_NOT_USE_AUDIO_DEVICE`, and no file or buffers): as fast as it can, and the samples are dropped.
- **It sleeps when there is nothing to do.** While the device plays, it wakes every 5 ms to follow the playing
  position. `Speak`, `AddBuffer`, `Resume`, `Reset` and `Shutdown` wake it.
- **Locks:** the handle's mutex, then the engine's (the kernel's), never the other way round. The engine's hooks run
  inside `engine_run`, on the thread that called it, and take no lock.
- **`TTS_MANUAL_CLOCK`:** there is no thread. `TextToSpeechRun` does the thread's work in the caller's thread, and so
  do `Sync` and `Reset` while they wait. Runs are then repeatable.

**Events at the sample they belong to.** Every sample has the engine's number, counted from startup. An index mark
belongs to the first sample of its frame (§16.13), and the audio's start to its first sample. An event goes out when
the output reaches its sample:
- the device: when the device has played it (its position: `waveOutGetPosition`, or ALSA's written minus
  `snd_pcm_delay`);
- a file: when the sample is written;
- a buffer: when the sample is in the buffer.

That is also when the library acts on a mark (`engine_mark_spoken`: `last_index`, and the answer to a
DT_INDEX_REPLY).
- **Messages:** `TTS_MSG_INDEX_MARK` (`TTS_INDEX_MARK`, or `[DTC01]` `TTS_INDEX_REPLY` for `[:re n]`), and
  `TTS_MSG_STATUS` with `TTS_AUDIO_PLAY_START` and `TTS_AUDIO_PLAY_STOP`, the latter once everything made has been
  played.
- **In memory, as in dapi,** the marks go into the buffers instead (`TTS_INDEX_T`: value, sample number, and in
  `dwReserved` the kind).
- **Delivery:** to the callback, to the window (Windows' `TextToSpeechStartup`, dapi's registered messages), or to the
  event queue (`TTS_EVENT_QUEUE`: an event object on Windows, a pipe's reading end on Linux). They are collected
  under the lock and sent after it is released, so a callback may call `AddBuffer` or `ReturnBuffer`.

**The calls, and where they follow dapi or the draft:**
- **`Speak` never waits** (user, 2026-09-27; this is also dapi's behaviour, whose `Speak` puts the text on an
  unbounded list). `TTS_FORCE` adds a CTRL-K. For the host terminal, v1.8's 64-byte pipe and its XOFF become the
  host program's own wait, on `TextToSpeechGetStatus(INPUT_CHARACTER_COUNT)` (dapi's identifier): it passes text on
  while fewer than 64 characters are unread (§17.3).
- **`Sync` is dapi's:** it returns when everything queued before it has been heard (played, or written to the file or
  the buffers). It ends the clause (`TTS_FORCE`) and resumes a paused device. v1.8's DT_SYNC returned earlier, when
  klsyn had taken the sync marker; the host program's DT_SYNC now waits a little longer, until the audio has ended.
  (Decided while building, 2026-09-27; the draft had v1.8's point.)
- **`Reset`** first does v1.8's stop (`engine_flush`: `stop_pending`, the sync marker, the clause cut after its
  phone). Then the rest is dropped:
  - the text not yet read;
  - klsyn's queued messages;
  - the DSP queue (the new `dsp_link_flush`: the DSP is reset and gets the speaker frame again);
  - the device's unplayed audio;
  - the marks not yet heard.

  `bReset` also writes `\x02:np :ra 180\x03` with MODE SQUARE (the startup state), and closes a wave file.
- **The device** opens when audio comes and closes when it has gone quiet, unless `OWN_AUDIO_DEVICE` (dapi).
  `Startup` opens it once to check it is there (`MMSYSERR_NODRIVER` if not). `REPORT_OPEN_ERROR` sends
  `ERROR_OPENING_WAVE_OUTPUT_DEVICE`.
  - Windows: `waveOut`, 8 buffers of 256 samples.
  - Linux: ALSA through `dlopen("libasound.so.2")`, with the device `default` (or `plughw:n`), 100 ms of buffer,
    and started at once instead of when its buffer is full.
- **The wave file** (`WAVE_FORMAT_1M16`, `1M08`, `08M08` mu-law) always says 10,000 Hz. As in dapi, opening and closing
  it first wait for what is queued (`Sync`).
- **Memory buffers:**
  - A buffer goes to the program when it is full, or when its index marks are (`TTS_MSG_BUFFER`).
  - `ReturnBuffer` is dapi's: the buffer being filled, as it is.
  - `[DTC01]` In 64-bit builds a `LONG` cannot carry the buffer's address, so the message carries 0 and
    `ReturnBuffer` hands over the full buffers first, oldest first. 32-bit builds are exactly dapi's.
  - The phoneme array is not filled yet: the engine does not report phoneme times.
- **One instance per process.** A second `Startup`, even after `Shutdown`, returns `MMSYSERR_ALLOCATED`: the
  firmware's globals cannot yet go back to power-up (§17.2).
- **`tts_stubs.c`:**
  - Real, small: the version and `VersionEx` ("DECtalk v1.8", revision 3), `GetCaps`, the language calls,
    `SetRate`/`SetSpeaker` (queued as `\x02:ra n\x03`, `\x02:np\x03` …, which work in any DT_MODE),
    `GetRate`/`GetSpeaker` (what is spoken now), `SetMode`/`GetMode`, `SetLog`/`GetLog`, `GetLastError`.
  - §17.4's stubs.
  - ~~**Not yet**: the user dictionary, `GetVoice`/`SetVoice`, `PlayTones`, `SetConsole` and the log file~~:
    done (§17.11).

**The check** (`decomp/test/test_lib.c`, `decomp/scripts/check_lib.py`). One scenario per process:
- **`file`** (the thread, a wave file, the event queue) and **`memory`** (the manual clock, five buffers of 3001
  bytes, taken back 777 samples at a time) speak the same text with three marks. They give **the same 77,376 samples,
  byte for byte, and the same marks at the same sample numbers** (15,232, 20,736, 42,432 since §17.13; 14,656, 20,288, 41,920 before it). `Sync`, `STATUS_SPEAKING`,
  `INPUT_CHARACTER_COUNT` and `STATUS_LAST_INDEX` are checked, and so are the start and stop messages.
- **`reset`**: 25,000 samples of a long text, `Reset`, then "Done.". The cut is exact, and the marks after it are
  never heard.
- **The Linux build gives the same samples as the Windows one**, in all three scenarios.
- **`device`** (`--device`) plays the text and times the events. On this Windows machine:

  | | audio starts after `Speak` | marks vs their sample times | `Sync` returns |
  |---|---|---|---|
  | device opened for the utterance (dapi's default) | 60 ms | within 20 ms | 7,807 ms (7,738 ms of audio) |
  | `OWN_AUDIO_DEVICE` | 26 ms | within 5 ms | 7,754 ms |

  Under WSL, whose sound goes over a PulseAudio bridge to Windows, the marks came up to 0.7 s later than their
  sample times while `Sync` returned on time. That looks like the bridge's reported delay, but it needs a check on a
  real Linux machine before ALSA's position can be trusted.

**Not yet:**
- ~~the rest of `tts_stubs.c`'s "not yet" list; the phoneme array in memory buffers~~: done (§17.11);
- a second instance (the globals into the handle);
- the speak and SAY ports (§17.8), and CMake.

### 17.11 The rest of the API (2026-09-27) — §17.10 [V: every call, on Windows and Linux]

Every export of `dectalk.def` now does what `ttsapi.h` says. Only §17.4's stubs are left, for dapi features v1.8
has no counterpart for (`ControlPanel`, `Typing`, `Reserved1-3`, `Tuning`, `Get`/`SetSpeakerParams`,
`GetPhVdefParams`).

**The user dictionary** (`src/api/tts_dict.c`, on `engine_dict_*`: v1.8's DT_DICT table under the engine's
lock).
- **An entry is v1.8's.** The name has no blanks. The substitution is **phonemic text** (spoken by
  `parse_phonemic_text`, as DT_DICT's is: `zorp z'aorp`, not plain words). An entry of the same name is replaced,
  and an empty substitution deletes. The name plus the substitution may be 255 bytes at most (DT_DICT's text).
  `MMSYSERR_NOMEM` is v1.8's R3 = 1.
- **The calls:**
  - `AddUserEntry`;
  - `DeleteUserEntry` and `ChangeUserPhoneme` (`MMSYSERR_ERROR` when the name is not there);
  - `UserDictionaryHit` (an exact name; `[DTC01]` it copies the substitution after the name);
  - `UnloadUserDictionary`.
- **Files are text**, one entry per line as DT_DICT takes it: the name, blanks, the substitution.
  `SaveUserDictionary` writes them in name order; `LoadUserDictionary` replaces the dictionary with a file's
  entries. `DumpUserDictionary` writes dapi's listing ("Total user dictionary entries: n", then "name,
  substitution"). dapi's compiled dictionaries are not read.

**The built-in dictionary** (the ROM's trie, 6,508 words, §15.25), which §17.4 had as "possible later":
- `DictionaryHit` looks a word up exactly (not through the user dictionary, no suffix stripping), and `[DTC01]`
  copies its pronunciation after the name.
- `DumpDictionary` walks the whole trie: "Total dictionary entries: 6508", then "word, pronunciation" in the
  trie's order (e.g. `hello, hxehl'ow`, `$, d'aalrr`; the punctuation words begin with a blank, since the trie's
  first root is the space).
- Pronunciations come out as phonemic text, with the names of v1.8's phoneme log (`phoneme_name`), so they can be
  spoken in `[ ]` or put in a user entry.

**`ConvertToPhonemes`** follows dapi's:
- It waits for what is queued (`Sync`), then speaks the text with the DT_LOG phoneme log (`0x02`) captured, and
  `Sync`s again.
- The result is the log's text, each clause by name, joined by blanks: "Good morning." gives `g'uhd m'owrnixnx.`.
- With dapi's `TTS_SILENT` (0x2, added to the header) klsyn drops the clauses (v1.8's DT_STOP flag), so nothing is
  heard. `[:dv]` commands in the text still take effect.
- dapi's alphabet flags are ignored: v1.8 has one alphabet.
- `*dwBufSize` goes in as the buffer's size and comes out as the bytes written, with the NUL. The text is cut to
  fit.

**v1.8's voices** (`tts_stubs.c`):
- `GetVoice` gives a voice's 28 `[:dv]` values (§8.1 order): the built-in records `PAUL` … `RITA`, `VAL`, or
  `[DTC01]` `TTS_CURRENT_VOICE` (the voice being spoken, with its `[:dv]` changes).
- `SetVoice` queues all 28 as two `[:dv]` commands, each small enough for the clause buffer's 200 words. `sex` comes
  first, since it moves `hs`, `f4` and `f5`, which follow. v1.8 clamps each value to its range.

**The tones.**
- `PlayTones(high, low, on, off)` waits for what is queued, then queues a tone item for the DSP (`engine_tone`),
  0-4095 Hz each, in 10 ms steps. It returns when the tone has been heard, as `Sync` does. v1.8's dialer uses
  160/60 ms.
- The link's tone items now carry their own lengths: `dsp_link_post_tone_ticks`; the ROM's
  `dsp_link_post_tone` keeps 16 and 6 ticks.
- The DSP stays in tone mode, making silence, until the next frame resets it. So the engine counts a finished tone
  with nothing queued as the end of what it has (`dsp_link_tone_done`), and the returned tone item makes
  `dsp_link_run` yield.
- **A third flaw of the DSP program, and its fix, `DSP_FIX_TONE` (§16.12).** The oscillators are cosines that
  start at phase 0, their peak. So a tone starts with a step, and a 0 Hz oscillator (the end of every tone) holds
  its peak. The "silence" after a tone was a constant 16,240, half the scale, until the next reset. With the fix,
  tones start at the zero a quarter period in, and a 0 Hz oscillator is silent. The library turns it on with the
  other two.

**The console and the log file.**
- The engine's console bytes (DT_LOG text, `[:dv list]`, "Illegal voice", errors under DT_LOG `0x20`) are collected
  as the engine runs, then passed on under the handle's lock:
  - to `SetConsole`'s routine, from the library's thread, after the lock is released;
  - to the log file of `OpenLogFile`, without the firmware's CR before each LF.
- `SetLog` and `OpenLogFile` each own their DT_LOG bits: `LOG_TEXT`, `LOG_PHONEMES`, `[DTC01]` `LOG_DEBUG`;
  `LOG_SYLLABLES` is `MMSYSERR_INVALFLAG`. `CloseLogFile` drops the file's bits.

**Phonemes in memory buffers.**
- A new hook in the frame loop, `ph_phone_hook` (after `phsettar`; NULL in every ROM check), reports each phone
  with its duration.
- The engine sends it with the next post, as it does a mark (one event ring for both).
- The buffer's `TTS_PHONEME_T` then holds the code, the phone's first sample, and its duration (frames × 64).
- A buffer also goes to the program when its phoneme array is full.

**The check** (`test_lib api`, in `check_lib.py`; the same results on Windows and Linux):
- **Voices:** Paul is the startup voice; `SetVoice(Betty)` makes the current voice Betty's; `[:dv list]` reaches
  the console routine.
- **User dictionary:** add, hit, a name with a blank refused, save, unload, load, change, dump, and delete (twice).
  "zorp." with the entry gives the phonemes of "[z'aorp]." in the phoneme log; the audio cannot be compared, since
  the DSP's noise generator runs on from one utterance to the next.
- **Log file:** `LOG_PHONEMES` writes `g'uhd m'owrnixnx.` for "Good morning.".
- **Tones:** 1336 + 941 Hz (DTMF 0), 100/50 ms: 1,500 samples, 251 sign changes in the first 100 ms, and silence
  from sample 1,000.
- **Phoneme array:** "Hello world." gives 11 phones, back to back (each starts where the one before ends).
- **Built-in dictionary:** the dump has 6,508 words, and `DictionaryHit` gives the dump's pronunciation.
- **`ConvertToPhonemes`:** `TTS_SILENT` gives `g'uhd m'owrnixnx.` and no audio, and an 8-byte buffer gets 7
  characters and the NUL.
- The ROM checks all still pass (`check_frames.py`, 1,874 lines).

**Not yet:** a second instance (the globals into the handle). The speak and SAY ports and CMake: §17.12.

### 17.12 SAY, speak and the CMake build (2026-09-27) — §17.8 [V: built and run on Windows (MSVC) and Linux (gcc, WSL)]

The plan of §17.8, done. The project's programs now speak through the library, on both systems, and one CMake
build makes them all without the ROMs.

**Files.**
- `CMakeLists.txt` and `CMakePresets.json` at the top of the project: the build (below).
- `src/apps/say/say.c`: SAY, portable C.
- `src/apps/speak/`: speak.
  - `speak_core.c/.h`: the portable core.
  - `speak_win.c`, `speak_win.rc`, `speak_res.h`, `speak.ico`: the Win32 front end.
  - `speak_gtk.c`: the GTK 3 front end.
  - `speak_pics.c`: the button pictures, generated once by `decomp/scripts/make_speak_pics.py` from `samples/speak` (removed on 2026-09-27, so the file
  is now kept as source and the script cannot run again).
- `samples/` is untouched; it stays the reference.

**SAY** (`say.c`) keeps the sample's options, texts and flow. What changed:
- **Standard I/O** in place of the console handles.
- **CTRL-C:**
  - On Windows, `signal(SIGINT)` and `SIGBREAK`. The handler runs on a thread of its own, as `SetConsoleCtrlHandler`'s
    did, and calls `TextToSpeechReset(TRUE)`.
  - On POSIX a handler must not take the library's locks. So SIGINT and SIGTERM are blocked in every thread (the
    library's thread inherits the mask), and a thread waits for them in `sigwait`. It resets, then interrupts the
    main thread's read with SIGUSR1, installed without `SA_RESTART`.
  - After CTRL-C, the `-post` text is not spoken.
- **Standard input:**
  - From a terminal, each line is forced out at RETURN, as in the sample.
  - From a pipe or a file, the text is sent as it comes (`TTS_NORMAL`), so a sentence that runs over a line end (or
    the sample's 2,048-byte read) keeps its intonation. The final `Sync` forces the rest.
  - SAY holds back while `INPUT_CHARACTER_COUNT` ≥ 16K, since `Speak` never waits (§17.2).
- **Options:**
  - `-ls` is an error: v1.8 has no syllable log.
  - `-w` writes 16-bit files at 10 kHz.
  - The help text's examples use v1.8's commands (`[:nb]`, `[:np :ra 200]`; dapi's `[:phoneme on]` is not v1.8's).
    `-lp` explains v1.8's singing (`[l'aa<400,24>]`), and `-d` the text dictionary format (§17.11).
  - `/` starts an option only on Windows.
- **Startup:** `TextToSpeechStartupEx` on both systems. With `-w` it starts without the device; without a device, it
  starts again with `DO_NOT_USE_AUDIO_DEVICE` (the sample's BATS #233 logic).

**speak.** The core has what is not window-system code:
- the button table: dapi's order, with **Val (`[:nv]`) in Wendy's slot**;
- the rate range 120-350 (steps of 5 and 20, default 200, set at startup, since v1.8 starts at 180);
- the highlighting marks and their lookup;
- find (case, whole word, up or down);
- reading and writing files, and line-end conversion;
- the wave conversion, the version check and the command line (`speak [file] [dictionary]`).

The two front ends keep the sample's window:
- the nine voice buttons (the pressed picture while held);
- the text, the rate slider with its "n WPM" label, and play / pause / stop;
- File: new, open, save, save as, close, load and unload the user dictionary, and convert to a wave file in the three
  formats, relabelled "Mono 10 kHz, 16-Bit / 8-Bit / µ-Law" (§17.2);
- Edit: cut, copy, paste, delete, select all, find, Highlighting;
- About;
- right-click: speaks the selection, or "what?" without one;
- the sample's error texts for the library's status messages.

**Removed**, with no v1.8 counterpart (§17.8): the language menus, the control panel, the typing demo, the help file,
the licence lines, the palettes and 16-colour pictures, the debug-only Reload button (a second `Startup` is not
possible, §17.2), and the "easter egg" (the DECtalk 4.5 team's photo).

**Small changes** on both front ends:
- Stop resumes a paused device first. In the sample, speech queued after Stop stayed silent until Resume.
- A wave conversion stops what is being spoken first, so it does not wait for it to be heard (`OpenWaveOutFile`
  syncs).
- A conversion speaks the text without marks.

**Per system:**
- **Windows** (`speak_win.c`):
  - The library's messages come as **window messages** (`TextToSpeechStartup`'s `hWnd` form).
  - The buttons are owner-drawn from the BMP bytes in `speak_pics.c` with `SetDIBitsToDevice`, as the sample drew
    its DIB resources. Each has a tooltip.
  - Common Controls 6 for the menus and scrollbar.
  - The window's place and size and the last file go in `HKCU\Software\DTC-01\Speak` (the sample used DEC's key).
  - The text is ANSI, which is what the library reads.
- **Linux** (`speak_gtk.c`, GTK 3):
  - `TTS_EVENT_QUEUE`, with its descriptor watched by `g_unix_fd_add`, so messages are handled in the GUI thread
    (§17.7).
  - GTK's text is UTF-8. What is spoken is one byte per character: Latin-1 as it is; typographic quotes, dashes and
    the ellipsis as `' " - .`; anything else a blank. So a mark's offsets are the buffer's character offsets.
  - Files are read as UTF-8, else as Windows-1252 (the sample's files), and written as UTF-8.
  - Not in the GTK version: Edit > Undo (GTK 3's text view has none) and dropping a file on the window.

**Val's picture.** The other eight are photographs of the people the voices were modelled on. Val is whoever the
user designs (`[:dv …]` then `[:dv save]`), so `make_speak_pics.py` draws a grey figure with a "?" in Paul's frame,
in the same palette.

**Highlighting, and what index marks do to v1.8's speech [V: measured with the phoneme arrays].**
- speak puts `[:in n]` (v1.8's spelling; dapi's `[:i m n]` is not v1.8's) before each word. n runs 1, 2, … and wraps
  after 32,767. Each index message selects that word; the audio stop clears the selection.
- In v1.8, a mark in the text is not neutral:
  - **A `[` ends v1.8's one-word lookahead.** At the bracket, `clause_readin` calls `token_dispatch(NULL)` (§15.30),
    so the word before a mark is spoken as the last word of its run.
  - A function word before a mark is lengthened as if phrase-final: "go for [:in 2]it" makes "for" 1,600 → 2,240
    samples. So is "are [:in 5]you" ("are" 1,920 → 2,560), and "thank" in "thank [:in 2]you" (its `k` 320 → 832).
    "the [:in 2]dog" and "how [:in 4]are" are unchanged.
  - Where the mark sits makes no difference: before the space, after it, or with spaces round it.
  - The phonemes stay the same: only durations change. In the test sentence the marked version was 3,712 samples
    (0.37 s) longer.
  - **A mark just before a voice command adds an empty clause:** `[:in 10][:np]` costs 1,664 samples of silence.
- So speak puts no mark before text in `[ ]` (commands, phonemes).
- **Since §17.13** the library's marks leave the speech as it is. Highlighting is now on by default, and the user
  turns it on and off with Edit > Highlighting or a "Highlight words" box.

**A library fix found on the way (`ttsapi.c`, `engine.c`, `dsp_link.c`).**
- **The bug:** with memory output on the library's thread and no buffer added, `Sync`, and `CloseInMemory`, which
  syncs, waited forever even when nothing was left to say. The thread only saw "said everything" when
  `engine_run` made fewer samples than it had room for, and without a buffer it had no room.
- **The fix:**
  - Without room, the thread now runs the tasks (no samples) and asks `engine_quiet()`: are all bytes read, is
    nothing queued for the DSP, and is the DSP waiting or idle (`dsp_link_quiet`)? If so, the text so far counts
    as said.
  - If sound is still pending, it waits for a buffer as before (dapi's rule).
- `test_lib file` now checks it: memory output on the thread, the buffer taken back, then `CloseInMemory`.
- A second `Sync` hang, a wake-up lost when text came while the thread ran the engine (seen on Linux), was fixed
  later in `thread_main`; `test_lib sync` checks it (§17.14).

**CMake** (`CMakeLists.txt` at the top of the project, CMake ≥ 3.16; its presets need 3.21. It was `decomp/CMakeLists.txt`
until the move of 2026-09-27, below):

| Target | What |
|---|---|
| `dectalk` | `DECtalk.dll` with `dectalk.def` (winmm, user32), or `libtts_us.so` (hidden visibility: only the 63 `TextToSpeech` functions are exported; pthreads, dl) |
| `say` | SAY |
| `speak` | Win32 (with the `.rc`) on Windows; GTK 3 through pkg-config on Linux. Without GTK 3 it is skipped with a message ("libgtk-3-dev"). Option `DTC01_SPEAK`. |
| `test_lib` | the library's check. Option `DTC01_TESTS`. C11, for `timespec_get`. |

- Programs go to `build/bin`. On Linux the library goes to `build/lib`, with an `$ORIGIN/../lib` rpath. The same
  folders are used by every configuration, also with the Visual Studio generator.
- `install` puts the library, `ttsapi.h`, SAY and speak in the usual places.
- The ROM checks (`build_*_test.bat`, `check_frames.py`) and `build_lib.bat` are unchanged.
- **Windows:** the CMake and Ninja that come with the VS 2022 Build Tools, from a developer prompt (`vcvars64.bat`).
- **Linux:** gcc and cmake, and `libgtk-3-dev` for speak.

**The build folder and 64-bit (user, 2026-09-27).** `CMakeLists.txt` moved from `decomp/` to the top of the project,
next to `src/`, and the build goes in `build/` there (`decomp/build/` keeps the ROM checks' harnesses and captures).
`CMakePresets.json`:

| Preset | Build | Folder | Prompt |
|---|---|---|---|
| `x64` | Windows 64-bit, the default: MSVC, Ninja, Release | `build/` | x64 developer prompt (`vcvars64.bat`) |
| `x86` | Windows 32-bit: sets `DTC01_32BIT` | `build/x86/` | x86 developer prompt (`vcvars32.bat`) |
| `linux` | Linux 64-bit: gcc, Release | `build/` | any shell |

- Use: `cmake --preset x64`, then `cmake --build --preset x64` (likewise `x86`, `linux`). Without presets,
  `cmake -S . -B build` works too; the build type defaults to Release.
- **64-bit is the default:** a 32-bit compiler (an x86 prompt, or `CC="gcc -m32"`) is refused with a message unless
  `DTC01_32BIT` is on. The presets' `architecture` is `external`: the prompt picks the compiler, and Visual Studio
  or VS Code set up the matching one themselves.
- A configure in the source folder itself is refused, so a build never lands among the sources.
- **Checked:**
  - `x64` gives an x64 `DECtalk.dll` (machine 8664) and `x86` gives an x86 one (machine 14C). Both build with no
    warnings, and `test_lib file` and `test_lib api` pass on both.
  - The 32-bit build's wave file is byte-identical to the 64-bit one.
  - An x86 prompt without `DTC01_32BIT` stops at configure with the message.
  - `linux` (with `-B` pointing outside the Windows `build/`) builds all four targets with no warnings, and SAY's
    file is still byte-identical to the Windows one.

**Checked (2026-09-27):**
- **Builds:**
  - Windows (MSVC 19.44, Ninja): all four targets, no warnings.
  - Linux (gcc, WSL Ubuntu 24.04): all four targets, with `-Wall -Wextra` on the programs; no warnings.
  - speak's GTK front end was compiled against GTK 3.24.41's headers, unpacked without root into a scratch folder.
    The WSL install has no `libgtk-3-dev`.
- **SAY:**
  - The command line and a pipe give byte-identical files.
  - The Linux file is byte-identical to the Windows one.
  - A `-d` entry `zorp z'aorp` gives the same audio as `[z'aorp].`.
  - `-lp` logs `g'uhd m'owrnixnx.`, and `-lt` logs the text with `^K`.
  - `-ls` exits with an error.
  - CTRL-BREAK on Windows ends a 30 s text 0.07 s after the signal.
  - On Linux, SIGINT while SAY waits on an open pipe ends it, and the wave file is complete.
- **speak:**
  - The Win32 window was captured while it spoke with Highlighting on: "over", then "dog.", in step with the audio.
  - A scratch harness around `speak_gtk.c` did the same under WSLg: "jumps", then "lazy".
- **The library:** `check_lib.py` ALL PASS, with the new memory check.

**Not yet:**
- a second instance;
- the host terminal emulator as a program on the library (§17.3).

### 17.13 Index marks that leave the speech as it is, and the highlight option (2026-09-27) — §17.12 [V: 29 texts, 921 words in the longest four, phone for phone; the ROM checks unchanged]

The user asked for marks that do not change the speech (§17.12 found that they do in v1.8) and for a highlight option in
speak. Measured with the memory buffers' phoneme arrays, v1.8's in-text marks change the speech in five ways. Each has a
library-only fix, and each is off in every ROM check.

| # | What v1.8 does with `[:in n]` in the text [V] | Where | The fix |
|---|---|---|---|
| 1 | A `[` ends the run of words: `clause_readin` calls `token_dispatch(NULL)` at the bracket (§15.30), so the word before a mark is said as a run's last word | `tx_scan.c` | A top-level `[` waits: at the first phonemic text read, or at the `]`, it ends the run as before, unless the brackets held only index marks. Then they are as if not there: no run break, no word boundary, the scanner's previous-character class restored. |
| 2 | The marker stands in the phoneme stream as two words, `0x66` and its value. `phalloph`'s stress rule looks back at `phonemes[n-1]` to stress the onset consonants, finds the value, takes it for a phone, and stresses the previous word's last phone: "are [:in 5]'you", "go for [:in 2]\`it", "thank [:in 2]'you" | `ph_alloph.c` | Look-back and look-ahead skip markers, and a marker leaves `phalloph`'s running state alone |
| 3 | A mark takes two of the clause's 199 words; a long sentence with a mark per word is cut elsewhere (the comma at 174 words) | `tx_clause.c` | Marks are kept out of the clause (below) |
| 4 | A mark in a clause that is otherwise empty makes klsyn say it: "brackets) [:in 7]-- and" (the `)` and the `--` each end a clause; the second, empty, is posted anyway, and skipped only when truly empty) adds a pause. And a clause whose last word is a marker's value gets a second clause end ("said [:in 4]--") | `tx_clause.c`, `ph_clause.c` | Such a clause is kept, its marks going with what follows; `klclause` checks the last symbol that is not a marker |
| 5 | A mark goes with the allophone *before* it (`at = nallo`), so it is reported when the previous word's last phone starts: one phone early | `ph_alloph.c` | A mark goes with the next allophone stored: the first phone of its word, or the pause there. Marks with nothing after them go with the clause's last. |

**How the marks travel with the fix (`TX_FIX_MARKS`, `PH_FIX_MARKS`).**
- **Waiting for their word.** A bracket of marks puts each mark on a list with the number of the next token (`tx_tokens`,
  counted in `token_dispatch`).
- **Tracking where words come from.** The words spoken carry the number of the token they came from, through the
  three places v1.8 holds words back:
  - money waits for the next token (`money_origin`);
  - `out()` keeps its last word until the next one (`out_word_origin`);
  - a lone Dr./St. waits for the next word (`abbrev_origin`).
- **Out of the clause.** `pronounce_word_or_abbrev` hands the due marks over just before a word's first symbol.
  `token_dispatch(NULL)`, at the end of a run, hands over the rest. They go into the clause's side list with their
  position, not into the clause. `newclause` writes the list after the clause's words, in the same message
  (position, code, value), with the count in `msg->pad6`: klsyn messages hold 400 words and use at most 200. A clause
  that holds only marks gets them as symbols, as in the ROM, so klsyn says it (a short silence) and reports them.
- **Back in place.** `klsyn_dispatch` takes the list. `parse_phoneme_param_stream` moves each position through its
  compaction and its splits at `:vo`/`:ra`. `phalloph` gives each mark to the next allophone.
- **Two marks on one phone.** The ROM keeps one mark per allophone (`index_marks[]`); the fix keeps the others in
  `ph_extra_marks` and reports them after it, in order.
- **An empty sub-clause** reports its marks at once; they go with the next frame posted.

**A sixth flaw, found by the same test: user durations after a mid-text voice or rate change (`PH_FIX_SPLIT`).**
- **What v1.8 does.** A `[:n.]` or `[:ra]` in the middle of a work item makes `parse_phoneme_param_stream` speak the
  part before it (`klclause`) and carry on in the same arrays. `phtiming` has meanwhile written that part's durations
  into `allodurs[]`, and the rest of the item reads them as user durations for its first phones.
- **Example.** "to [:nb] Betty," gives "Betty" a 1,728-sample tap and a short final vowel. Without the voice change
  the tap is 192 and the vowel 1,600.
- **The fix** clears `allodurs[]` and `f0tar[]` after the split, as at the item's start.

**The switches.**
- `engine_init` takes `ENGINE_FIX_MARKS` (0x100: `TX_FIX_MARKS` and `PH_FIX_MARKS`) and `ENGINE_FIX_SPLIT` (0x200:
  `PH_FIX_SPLIT`), next to the DSP fixes.
- The library turns all five on (`ttsapi.c`). The ROM checks turn none on.

**The checks.**
- **A scratch comparison** (the phoneme arrays of each text with and without a mark before every word):
  - 7 test sentences (function words, money, dates, times, Dr./St., phonemic text, voice changes, quotes, dashes);
  - the 18 local-terminal texts of the corpus;
  - the four texts of dapi's speak sample (921 words);
  - result: the same phones with the same durations, every mark reported, in order, each at a phone's start.
- **Where the marks land.** "[:in 1]The [:in 2]quick [:in 3]brown …" reports each mark at its word's first phone (`k`,
  `b`, `f`, …); the ROM's rule gave the phone before it.
- **`test_lib api`** now checks it on a sentence with all six effects: 62 phones either way, 18 marks in order, each
  at a phone's start.
- **The library's own marks moved.** `test_lib file` reports its three marks one phone later than before: 15,232,
  20,736 and 42,432 (they were 14,656, 20,288 and 41,920), with the same audio.
- **The ROM checks:** `check_frames.py` ALL PASS (1,874 lines) with the fixes off, `check_lib.py` ALL PASS, and
  Windows = Linux.

**What stays v1.8's.**
- A mark inside a word ("Hel[:in 2]lo") still splits it, since the scanner hands the word on at the `[`.
- A mark before punctuation alone ("world [:in 4].") makes the punctuation a word of its own, and v1.8 says "period",
  as it does for "world ." without the mark.

**The highlight option (speak).**
- Edit > Highlighting and a "Highlight words" check box by the rate slider show the same setting. It is **on by
  default** (`SPEAK_HIGHLIGHT_DEFAULT`), since the marks no longer change the speech.
- Turned off while speaking, the selection goes at once, and later marks are ignored.
- The choice is kept: `HKCU\Software\DTC-01\Speak\Highlight` on Windows, `$XDG_CONFIG_HOME/dtc01/speak.ini`
  (`[speak] highlight=`) on Linux.
- **Checked:**
  - The Win32 window, captured, highlights "lazy" with the box ticked from the start. The menu toggle unticks it,
    clears the selection and stores 0.
  - The GTK harness shows the same, and writes `highlight=false`.

### 17.14 `dtc01term`, the host terminal emulator on the library (design, 2026-09-27) — §17.3, §13 item 15

**Status: built (2026-09-27), on Windows (x64, x86) and Linux; results at the end of this section.** The design
was agreed with the user on 2026-09-27. The user's answers:

| Question | Answer |
|---|---|
| scope of the first version | serial first: the host line and the local terminal, with SETUP, the host timeout and DT_STOP/DT_SYNC; the phone task runs on a line that never rings. The simulated phone line (rings, caller keys, audio to the line, the self-test loopback) is a second step with its own design; its options are to be worked out next. |
| the lines at start with no options | local terminal = the program's console, host line = TCP on `127.0.0.1:2001` |
| line speeds and formats | real on a COM port only; kept and shown, but not paced, on TCP and the console (low latency) |
| how the host C meets the library | approach A: the host C linked unchanged, a glue layer, `kernel.c` extended |
| the name | `dtc01term`, to tell it from the speaking programs |

**Units** (`src/term/`, CMake target `dtc01term`):

| Unit | What it does | Depends on |
|---|---|---|
| `src/host/*.c`, `src/kernel/stream.c`, `console.c` | the five tasks (host, main/SETUP, phone, host timeout, stop), **unchanged**: the code `test_host` checks against the ROM | `rtos.h`, and the symbols the glue supplies |
| `src/kernel/kernel.c` (extended) | its scheduler (tasks as threads taking turns) gains a 10 ms clock, `event_wait`, `task_suspend`/`task_resume`, device input timers and a device-driver interface. The library calls none of them; the program links its own copy. | OS threads |
| `src/term/term_speech.c` | what the host C expects from the speech side and the board, through the library's public API only (below) | `ttsapi.h` |
| `src/term/term_dev.c` | the four devices: the host line (the ROM's input ring and XON/XOFF points), the local terminal, the text pipe, the phone (idle) | `term_line.c` |
| `src/term/term_line.c` | the byte-only "line" interface and its backends: the console, stdio, TCP, a COM port (Win32 COM; a POSIX tty on Linux) | OS |
| `src/term/dtc01term.c` | options, the boot (then `main_task`), shutdown | all |

**Lines.** `--host` and `--local` take `console`, `stdio`, `tcp:[addr:]port`, `com:NAME` or `none`.
- Defaults: `--local console --host tcp:127.0.0.1:2001`. TCP takes one client at a time and is raw bytes (no telnet
  negotiation: PuTTY "Raw", `nc`); when the client goes, the unit carries on with its host line idle.
- A COM port applies SET HOST/LOCAL SPEED and FORMAT, DECNVR, MODEM, BREAK and LBREAK for real. The other backends keep
  and show these settings and move bytes as fast as they come; BREAK does nothing there.
- The host line has the ROM's input ring and XON/XOFF points (to be read from the DUART interrupt code, §13 item 4). The
  local line has no flow control, as in v1.8. DECTC1/DECAC1 and the character sets are the host C's.
- **The console** is put in raw mode: keys go one by one to the ROM's line editor, Ctrl+C is the character 0x03, and
  the console is restored at exit.
- **The escape key is Ctrl+]** on the local line, whatever its backend: then `b` = a BREAK (enters SETUP), `q` = quit,
  `]` = a Ctrl+] itself. Ctrl+Break on the Windows console is a BREAK too. On stdio the program also ends at the end of
  its input, once the speech is done.
- **Audio:** the device by default; `-w FILE` a wave file (tests), `-d N` a device number.
- **Power-up:** the settings from the in-memory NVRAM (the factory record, §15.33), then `main_task`, which writes
  `[:np :ra 180]` and, with the self-test jumper open, the banner. `-q` closes the jumper: no banner. The self-test's
  DTMF loopback waits for the phone step.

**The speech hooks** (`term_speech.c`):

| The host C | `dtc01term` |
|---|---|
| characters into the text pipe (`cur_stream`, one `dev_putc` each) | collected, and handed to `TextToSpeechSpeak` whenever the writing task waits. While `INPUT_CHARACTER_COUNT` is 64 or more (v1.8's pipe) `dev_putc` waits as a kernel wait, so the host task stops reading and XOFF comes where v1.8 sent it. |
| `emit_sync_marker`: `0x1A`, then `sem_wait(&sync_sem)` | `0x1A` sends the text before it with `TTS_FORCE` (it ends a clause in v1.8); the wait is `TextToSpeechSync`, or `TextToSpeechReset(h, FALSE)` while `stop_pending` is set |
| DT_STOP: `dt_stop` wakes the stop task (`stop_pending`, a sync) | the ROM's stop task, unchanged: its sync is the `Reset` (§17.6 answer 3); the host task reads on |
| `last_index` (DT_INDEX_QUERY) | kept by the index callback (at audio time) |
| `send_dcs_reply(31, n)` for `[:re n]` | the callback queues it; a "reply" task sends it |
| `dt_error_flags` bit `0x08` (DSR 25) | ORed in from `GetStatus(STATUS_ERRORS)` on the 10 ms clock |
| `dt_log`, `dt_mode` | passed on with `SetLog`/`SetMode` before the next text goes to the library |
| the speech side's console output | `SetConsole` → the local terminal |
| DT_DICT, RIS | `AddUserEntry` (no room = R3 1), `UnloadUserDictionary` |
| DT_PHONE tone dialing (`dsp_link_queue`, waiting for the message back) | `PlayTones(high, low, 160, 60)`; the message comes back when the tone has played |
| `speech_init` | the ROM's order: `settings_reset(3)`, then the other tasks |
| `duart_input_port` | the self-test jumper open (the banner), or closed with `-q` |
| `system_restart` (DECTST 1, TEST POWER) | the tasks stop; the library is `Reset`, its user dictionary unloaded, mode and log set back; then the boot again |
| DECTST 2-4, HISTOGRAM | the data loopback (2, 4) as the ROM's driver does it (§17.14.1: it passes with a loopback connector or a host that echoes); the control-signal loopback (3) fails; an empty histogram (no profiler) |
| `heap_free_total` (DECTST 5) | 17,486, the emulator's figure |
| the phone device | never rings, hears no keys; goes off hook when asked, so dialing plays its tones on the speaker; speech stays on the speaker |

Kernel waits that call the library (`Sync`, `Reset`, room in the pipe, a tone) are made from a helper thread while
the task waits, so the other tasks (the local terminal, the host timeout) run meanwhile, as in v1.8.

**Differences from v1.8** (from earlier decisions): DT_SYNC returns, and index replies come, when the audio is heard
(§17.2). DECTST 1 cannot take the library back to power-up, so a voice changed with `[:dv]` (Val) keeps its changes.

**Checks** (test tools, not the product; `dtc01term` loads no ROM):
1. `check_frames.py` and `check_lib.py` pass (the kernel changed); `test_host` is unchanged.
2. `decomp/scripts/check_term.py`: the host-line corpus entries through `--host stdio --local none -w`; what the
   program writes on the host line equals what the ROM wrote (`host.tsv`): DA, DSR (error 25), DECID, the
   DT_INDEX_QUERY values, the `:re` replies in the same order, XON/XOFF. The phone entries wait for the phone step.
3. Plain-text entries fed on the host line give the same wave file as SAY with `[:np :ra 180]` first.
4. The typed SETUP entries (`setup_show`, `setup_edit`, `setup_cmds`, `setup_spoken`) through `--local stdio`, with `\B`
   as Ctrl+] `b`: the terminal output equals the ROM's (`main.tsv`).
5. By hand: PuTTY Raw on TCP 2001 (speak, DT_STOP, DT_SYNC, an index reply, a long paste for XOFF/XON); com0com
   (`--host com:COM10`, PuTTY on the pair's other end at 1200 baud, then SET HOST SPEED 9600; a BREAK from SETUP); the
   console (SETUP by Ctrl+] `b` and Ctrl+Break, the line editor, Ctrl+] `q`).
6. Linux (WSL): the build, checks 2-4, the tty backend on a pseudo-terminal pair.
7. No warnings: Windows x64 and x86, Linux.

**Built (2026-09-27).** Files as in the table above, plus `src/term/term_os.h` (threads, a mutex, sleeping).
Usage: `dtc01term [--host LINE] [--local LINE] [-w FILE] [-d N] [-q]`.

**From the ROM, for the devices [V]:**
- **Rings and flow control:**
  - The host and console input rings hold 304 bytes (`0x130`, set up at `0x15d0`); the output rings hold 64.
  - The host line sends XOFF when a received byte finds more than 64 waiting (`duart_rx_char` `0x192c`), and XON
    when a read leaves fewer than 16 (the device's after-read hook `0x1996`). XON/XOFF go out ahead of queued output
    (the pending byte at `dev + 0x40`).
  - The host's own XON/XOFF are data on the host line. The terminal's hold and release output on the local line
    (`0x18ea`).
- **Received bytes:** a BREAK is read as 0, and a plain NUL is dropped.
- **The loopback tests** (the DUART's ops table `0x19d6`, found for §17.14.1):
  - **Op 4 (`0x1a44`), the data loopback (DECTST 2 and 4).**
    - It returns −1 at once if a test is already running (`dev + 0x43` bits 0-1).
    - Otherwise it sets the channel to 9600 baud, MR1 `0x03` and MR2 `0x07` (8 bits, even parity, 1 stop bit, normal
      mode), and sends `0x00`-`0xFF` from the transmit interrupt.
    - The receive interrupt (`0x187e`) compares each byte with the next one expected (`dev + 0x44`). A match restarts
      a 500-tick timer (`dev + 0x76`, handler `0x195a`); an error, a BREAK or a wrong byte fails the test.
    - All 256 in order give 0; the timer running out gives −1. The calling task waits on `dev + 0x4a`.
    - The bytes really go out on the line: the manual asks for a loopback connector.
  - **Op 5 (`0x1ad4`), the control-signal loopback,** drives the DUART's output port and reads its input port back.
    On the console device it returns 0 without testing.
- **Speeds and formats:** op 1 is the speed (`0x11 ×` code, `0x60` for code 0), and op 2 is the DUART's MR2/MR1
  word:

  | Word | Format |
  |---|---|
  | `0x702` | 7E1 |
  | `0x706` | 7O1 |
  | `0x713` | 8N1 |
  | `+0x800` | 2 stop bits |

  Code 0 ("75/1200", split speeds) is 1200 on a COM port.
- **`speech_init`** (`0x30c8`) is `settings_reset(3, 0)`, the DSP link, then the task table `0x12bac` in order
  (phone, host, klsyn, dttask, host timeout, stop), then the voice. `dtc01term` spawns the same tasks without klsyn
  and dttask, plus a "reply" task at klsyn's priority 50 for the `[:re]` replies.

**Choices made while building:**
- **The NVRAM starts with the factory record,** encoded as `nvram_save_settings` would, so the power-up reads a
  good record and says nothing about an NVR fault (the emulator's first boot did).
- **Into a wave file (`-w`) the text pipe does not hold at 64.** A file is made far faster than real time, and a held
  pipe let the library run out of text between two refills. The DSP then added its pause frames, so the file
  depended on timing (a 77-character sung text came out 256 samples longer than SAY's). With the device, the
  64-character pipe and its XOFF stay as in v1.8.
- **Library callbacks never take the kernel's lock:** they queue events that the 10 ms loop hands to the tasks.
  Calls into the library are made with the kernel's lock held, which is safe because the library's thread never
  waits for it.
- **DT_SYNC runs `Sync` on a helper thread,** so the other tasks keep running. **DT_STOP runs `Reset` at once on the
  stop task,** so text the host sends after DT_STOP cannot slip in ahead of the reset.
- **`host.h` now includes `<stddef.h>`:** the host C had only been built with MSVC, and gcc wants it for `NULL`.
  Under gcc the host sources are built with `-Wno-implicit-fallthrough -Wno-cast-function-type`, since they keep
  the ROM's structure.

**A library bug found on the way (fixed, `ttsapi.c` `thread_main`).** On Linux, `TextToSpeechSync` sometimes never
returned when text came while the library's thread was running the engine. `dtc01term`'s first DT_SYNC after the
power-up's `[:np :ra 180]` hit it every time.
- **The cause:** `step` saw a newer `text_gen` than it started with, so it did not mark the text as said and returned
  "idle". The thread then waited on its condition variable with no timeout (file and memory output have no poll),
  and the wake-up from that `Speak` had come before the wait, so it was lost.
- **The fix:** the thread does not wait while `idle_gen != text_gen` after an idle step.
- **The check:** a new `test_lib` scenario, `sync` (run by `check_lib.py`, now with a time limit). Before the fix it
  hung one run in three on Linux, and a standalone reproduction hung every time. After the fix: 0 in 30 for both.
  Windows was not seen to hang, but it has the same race.

**Checks:**
- `check_frames.py` ALL PASS (1,874 lines) after the kernel additions. `check_lib.py` ALL PASS, with the new `sync`
  scenario.
- **`test_kernel`:** 28 checks, ALL PASS on Windows x64, x86 and Linux. It covers the clock, `event_wait`,
  suspend/resume, driver devices, the input timer, `DEV_POST`, `kernel_wait_until`, and `term_dev.c` on fake lines.
- **`test_line`:** ALL PASS on Windows x64, x86 and Linux. It covers TCP, the refused second client and the escape
  key. With `COM7 COM5` (a com0com pair) it also covers bytes both ways, 1200 8N1 and a BREAK. com0com sometimes
  misses the receive event, so the Windows reader also looks at the queue every 20 ms while it waits.
- **`check_term.py`: ALL PASS on the 69 corpus entries it runs** (the 3 phone entries are skipped):
  - **46 host-line entries:** every byte `dtc01term` writes on the host line equals the ROM's. That covers DA,
    DECID, DSR with errors 25 and 26, the index queries, the `[:re]` replies, the dictionary replies (R3) and the
    replies after RIS/DECSTR. The XON at start is checked too.
  - **19 plain entries:** the wave file equals SAY's.
  - **4 SETUP sessions:** the terminal output equals the ROM's, with `setup_show`'s text log (13 bytes, from the
    speech side) mixed in.
  - **On Linux (WSL)** 13 of them were run, the SETUP sessions among them: all pass.
- **By hand:**
  - TCP: DSR, an index reply and an index query; a second client is refused and a new one after a disconnect is
    served.
  - A com0com pair: `--host com:COM7`, with a host on COM5 at 1200 baud. SETUP's SET HOST SPEED 9600 went through,
    and DSR was answered at both speeds.
  - A Linux pseudo-terminal (`com:/dev/pts/N`): XON, DSR and a `[:re]` reply.
  - The x64 and x86 builds give the same bytes and an identical wave file.
- **Builds:** no warnings on MSVC x64 and x86 (`/W4`) or gcc (`-Wall -Wextra`).

**The Windows console backend (checked afterwards, 2026-09-27).** A scratch driver started `dtc01term` in a hidden
console window, typed into it with `WriteConsoleInputW` and read the screen back. It ran every SETUP command, each
compared with the same keystrokes typed on the ROM (`phcapture -T`): SHOW in all forms, SET LOG/LOCAL/HOST/MODE,
SET INTERRUPT (then the character entered SETUP), SAVE and RECALL (USER, FACTORY), ONLINE, OFFLINE, spoken SETUP,
BREAK, LBREAK, HELP, TEST, EXIT, and LOCAL HOST, whose typed text reached the TCP host line. Everything behaved as
on the ROM. Things that look like faults but are v1.8's (the user confirmed):
- TEST HDATA, HCONTROL and LDATA say "Failed.", as without the manual's loopback connectors. (Since §17.14.1 the
  data tests send their bytes and wait 5 s for them, as the ROM does.)
- TEST POWER restarts the unit and so leaves SETUP.
- The manual's `SET LOG PHONEMIC` is a bad command: the keyword is `PHONEMES`. `sh ho sp` is too short for `SPEEd`.
- SHOW HISTOGRAM prints nothing, in the emulator too (dtc01term has no profiler behind it).

Ctrl+Break was not tried.

#### 17.14.1 The simulated phone line (design, 2026-09-27)

**The user's answers (2026-09-27):**
- The line connects to a **simulated line** first. Asterisk AudioSocket, a built-in SIP client and a USB voice modem
  (AT+V) were the other options; they can come later behind the same interface (below).
- **You play the caller with escape keys on the local terminal**, as with BREAK. There is no separate control port.
  Scripts and `check_term` type the same bytes on a stdio local terminal.
- **No beep for the caller's keys.** The library plays tones in turn with the speech, so a beep would wait behind
  what the unit is saying, and the DTC01 does not play them either (the caller hears them on their own handset).
- **The line's state is shown in the console window's title**, never on the local terminal, whose output stays the
  ROM's.

**What the user sees and does:**
- The line is always there; `--phone none` keeps the line that never rings.
- **Ctrl+] `r`: one ring** in the US cadence, 2 s of ring signal and then 4 s without. More presses queue more rings.
  Ringing stops when the unit goes off hook, as the exchange does. The unit decides whether to answer:
  - in stand-alone mode (power-up, before the host's first byte) it answers the first ring and speaks the DTMF
    diagnostic menu (§15.34, §15.36);
  - otherwise only after DT_PHONE 10 (answer on n rings).
- **Ctrl+] then `0`-`9`, `*`, `#`, or capital `A`-`D`: the caller presses that key.** Lower-case `b` stays BREAK and
  `q` quit; capital `B` is the key (it was a second BREAK before). A key reaches the unit only while it is off hook with
  the keypad interrupt on, as on the hardware.
- **Audio:** the user is the caller, so the call's audio is the speaker, as now: the unit's speech and its tone dialing
  (DT_PHONE 40).
- **The DTMF receiver hears the unit's own tones** while off hook, as the hardware's does (§15.34). `phtask_main`
  does not send those digits to the host, but they still reset its idle-second count.
- **Status:** the console backend's window title shows `on hook`, `ringing` or `off hook` (Windows: `SetConsoleTitle`;
  POSIX console: the xterm title sequence). stdio, TCP and COM local terminals show nothing.
- **Not simulated:** the caller hanging up (the DTC01 cannot sense it and waits for its timeout), and the power-up
  DTMF self-test (`dtc01term` does not run the 68000's reset code, §15.32).

**Units:**

| File | Role |
|---|---|
| `src/host/hs_phonedev.c` | **The ROM's phone driver in C** (§15.34): `phone_init_impl` `0x22ba`, the ops table `0x2632` (keypad on/off, hook release/seize, set idle, go off hook, start answer), `phone_tlc_isr` `0x214e` and the tick hook `phone_ring_poll`. Ported like the rest of `src/host/`: ring counting (a falling edge after at least two 40 ms polls high), off hook at the ring's end and `0x82` 2.5 s later, `0x86` after 250 polls without a change. It sees the hardware only through a TLC interface: read `0x9c004` (bit 15 ring, bit 7 tone present), read the receiver's code (`0x9c007`), write `0x9c004` (bit 14 ring interrupt, bit 8 hook, bit 6 tone interrupt). |
| `src/term/term_phone.c` | **The simulated line behind that interface:** the ring cadence and queued rings, the hook relay, the DTMF receiver with a latched interrupt (requested when "tone present and bit 6" or "ring and bit 14" becomes true, cleared by the read of `0x9c004`, as the emulator's), the caller's keys (150 ms of tone), the unit's own tones, and the window title. A VoIP or modem backend later replaces only this file. |
| `src/kernel/kernel.c` | A per-device **tick hook** (`kdev_ops_t.tick`), run by `kernel_tick` under the kernel lock: `phone_ring_poll` is the ROM's tick hook on `phone_dev`. |
| `src/term/term_line.c` | The escape keys: Ctrl+] `r` and Ctrl+] followed by a key arrive as new negative codes, as `LINE_BREAK` does, and `term_dev_rx` hands them to `term_phone.c`. |
| `src/term/term_speech.c` | When a tone command for a DTMF pair starts playing, it tells `term_phone.c`, so the receiver hears it for the tone's length. |
| `src/term/term_dev.c` | `phone_dev` gets the driver's ops instead of today's stand-in. |

**Checks:**
- **`test_kernel`**, the driver and the simulated line on a fake clock: the answer after n rings and its 2.5 s, the
  `0x86` after 10 s, keys counted only off hook with the keypad on, the unit's own tones heard while dialing.
- **`test_line`:** the new escape keys.
- **`check_term`: the three phone entries** (`host_phone`, `phone_menu`, `phone_tests`) are no longer skipped. They
  run with the host line on TCP (the script connects) and the local terminal on stdio, where `\g`/`\gN` become N ×
  Ctrl+] `r` and `\kKEYS;` one Ctrl+] key every 300 ms (hostfeed's 150 ms tone and 150 ms gap). Two comparisons:
  - the host-line bytes (R3 replies, the forwarded digits) with `<name>.hostline.tsv`;
  - the text the phone task speaks (the menu, the test results) with the ROM's pipe bytes, the `O P` lines of
    `<name>.phone.tsv`, through a new test option that logs what `dtc01term`'s tasks write to the speech pipe. The
    spoken menu sends nothing on the host line, so it needs this second comparison.
- `check_frames`, `check_lib` and the other `check_term` entries still pass.

**Built (2026-09-28).** As designed, with one addition.
- **Files:** `src/host/phonedev.h` (the TLC interface), `src/host/hs_phonedev.c` (the ROM's driver),
  `src/term/term_phone.c/.h` (the simulated line) and the check `decomp/test/test_phone.c`. Changed: `kernel.c/.h`
  (`kdev_ops_t.tick`, `arg_op`, and `control`'s argument), `term_line.c/.h` (the keys, `line_set_title`),
  `term_dev.c`, `term_speech.c` (the unit's own tones, `--log-pipe`) and `dtc01term.c` (`--phone`, the title).
- **The addition: the data loopback test (DECTST 2 and 4) now runs as the ROM's driver runs it** (above, "From the
  ROM"), where it used to fail at once. It sends `0x00`-`0xFF` on the line at 9600 8E1 and waits up to 5 s for each
  byte back. On a COM port with a loopback connector, or with a TCP host that echoes, TEST HDATA passes.
  `phone_tests` needed it: those bytes include an XOFF (`0x13`, after the XON `0x11`). The emulator's host feed
  honoured it, so the entry's closing CR never reached the unit, and stand-alone mode did not end. check_term's feed
  honours it the same way.
- **What a capture holds:** `phcapture` stops 1 s after the last DSP frame once its feed is done. So the ROM's
  `host_phone` ends inside the 2 s wait after the last hang-up, before the R3 = 0 reply that follows it. `phone_tests`
  ends before the menu's 30 s input timer, whose end writes the task's sync marker. `check_term` stops the same way:
  once the feed is done (or held by an XOFF) and 1.5 s pass with nothing on the host line or in the pipe log. It
  compares what had come by then.

**Checks:**
- **`test_phone`:** 22 checks of the line on its own, then 12 of the ROM's driver on the line and the kernel. Among
  them: answering on 2 rings goes off hook at tick 804, as the second ring ends, and `0x82` follows exactly 250 ticks
  later; 250 polls without a ring give `0x86`.
- **`test_kernel`:** the tick hook and `arg_op`, and the data loopback test (sent at 9600 8E1, passed when all 256
  come back, failed 500 ticks after the last byte or on a wrong byte).
- **`test_line`:** the new keys.
- **`check_term`: the three phone entries pass.**
  - `host_phone`: 98 host-line bytes, the ROM's.
  - `phone_menu`: the 630 bytes of the spoken menu, the ROM's.
  - `phone_tests`: 254 host-line bytes (DECTST 2's bytes, less XON/XOFF) and 914 bytes the phone task spoke, the
    ROM's.
  - The same three pass on Linux (WSL, gcc).
  - **Once, `host_phone` failed:** the ring after DT_PHONE 10 was not answered (R3 = 0 where the ROM has 1). That run
    was during the checker's development, before its last two changes. It has not come back in 18 runs since (a loop
    of 10 kept the pipe log of any failure). The cause is not known; if it returns, the pipe log and host line of the
    failing run are what to look at.
- **The whole suite (2026-09-28):** `check_frames` (with the harnesses rebuilt on the new `kernel.c`), `check_lib`,
  and all 72 `check_term` entries (the 69 before, and the three phone entries) pass. The x64, x86 and gcc builds have
  no warnings.
- **By hand, in a hidden console window** driven through `WriteConsoleInputW`: at power-up Ctrl+] r rings, the title
  goes "on hook", "ringing", "off hook", the unit answers with the spoken menu, and Ctrl+] 5 and Ctrl+] * get "You
  pressed five", "You pressed star", "Using factory settings".

**Next, after this:** a second instance; a real phone backend (AudioSocket, SIP or a modem) behind `term_phone.c`'s
interface, with the call audio at 8 kHz (the library stays at 10 kHz, so the backend resamples).

## Appendix A — `docs/`: files, OCR caveats, table status

**Current sources** (all in `docs/`): `EK-DTC01-OM-002_Owners_Manual.html` (2nd ed., May 1984),
`EK-DTC01-RM-003.html` (3rd ed., Mar 1985; covers firmware **1.8 and 2.0**), `mitalk.html` (the MITalk
book, §14), and **`hunnicutt_lts.pdf`** (Hunnicutt 1976, AJCL microfiche 57, 82 pages; PDF only, JBIG2 page images
with an OCR text layer; §14.9 — the full letter-to-sound rule listing is on PDF pp. 64-72, and PDF page = printed
page). Also **`Klatt_1980_CascadeParallelFormantSynthesizer/`** and **`Klatt_1982_KlattalkTTS/`**: Markdown/YAML
study notes on those two papers by a third party, not the papers themselves; plain text, read directly (§14.10).
**`klatt1980.pdf`** (JASA 67, 971-995, 25 pages, with the FORTRAN appendix) and **`klatt1982.pdf`** (ICASSP 1982,
4 pages) are the papers themselves; both have a text layer, but read the listing and tables as images (§14.10). They are **ABBYY FineReader 15 HTML**: text in `<p><span class="fontN">`, real `<table>` markup
where the table survived, and `<img src="<doc>_files/<doc>-N.png">` where it did not (figures and some
tables). The earlier DjVu XML/`.txt` OCR (`docs/old/`) was deleted — it was **superseded** (§12.11); the HTML is
markedly better, especially for tables.

**Converter used** (scratchpad, Python stdlib; produces `[TABLE]`/`| cell |` text you can grep — the HTML
files are 0.35-1.0 MB with line noise, do not read them raw):
```python
import sys, re
from html.parser import HTMLParser
class P(HTMLParser):
    def __init__(s): super().__init__(convert_charrefs=True); s.out=[]; s.skip=0
    def handle_starttag(s,t,a):
        if t in('style','script','head'): s.skip+=1
        elif t in('p','div','h1','h2','h3','h4','h5','h6','br','ul','ol'): s.out.append('\n')
        elif t=='li': s.out.append('\n- ')
        elif t=='tr': s.out.append('\n| ')
        elif t=='table': s.out.append('\n[TABLE]')
        elif t=='img': s.out.append(' [IMG %s] '%dict(a).get('src','').split('/')[-1])
    def handle_endtag(s,t):
        if t in('style','script','head'): s.skip-=1
        elif t in('td','th'): s.out.append(' | ')
        elif t=='table': s.out.append('\n[/TABLE]\n')
        elif t in('p','h1','h2','h3','h4','h5','h6'): s.out.append('\n')
    def handle_data(s,d):
        if not s.skip: s.out.append(re.sub(r'\s+',' ',d))
p=P(); p.feed(open(sys.argv[1],encoding='utf-8-sig').read())
t=re.sub(r'\n{3,}','\n\n',re.sub(r'[ \t]+\n','\n',''.join(p.out)))
open(sys.argv[2],'w',encoding='utf-8').write(t)
```
Then compact table rows with e.g. `tr '\n' '~' | sed 's/~ | ~/ | /g; s/~|~/\n|/g' | tr '~' ' '`. (In the
converted text: OM ≈ 7 000 lines, RM ≈ 21 000, MITalk ≈ 12 400; the RM DECTLK.H listing and C/BASIC programs
are chapters 6-7 and are large but low-value — `xtras/dtlib/dectlk.h` is the same header.)

**Images worth knowing (RM, `docs/EK-DTC01-RM-003_files/EK-DTC01-RM-003-N.png`; view with the Read tool):**
`-4` = **Table 1-1** (control chars) · `-30` = **Table 2-2** (LS0/LS1/SS2/SS3/LS2/LS3/LS1R-3R) · `-39` = **Table 4-2**
(phone reply codes) · `-74` = Table A-3 (MODE_SQUARE 1 / ASKY 2 / MINUS 4) · `-75` = **Table A-4** (TERMINAL 1/2/4/8/16/32)
· `-76` = **Table A-6** (DECTST 1-5) · `-77` = LOG mnemonic column of Table A-7 (values 1 2 4 8 16 32 64 128 =
TEXT PHONEME RAWHOST INHOST OUTHOST ERROR TRACE DEBUG). Other numbers are protocol figures (Fig. 1-5 XON/XOFF
`-14/-15`, 7/8-bit mapping `-16`, character-set diagrams `-20…-29`, DT_SYNC/INDEX_QUERY timing `-32…-36`,
phone-session timeline `-40…-43`, logging/terminal data paths `-44…-47`, module tree `-49`, Appendix A
page images `-68…-73` (not read); figure descriptions other than the seven above are inferred from the
surrounding text). OM images `-1…-16` are figures (not read).

**OCR quirks (HTML version):** the `|` character is treated as a table border, so it vanishes from the
1-character phoneme alphabet (`ix` = `|`); `^` may come out as `*` (RM dial text, §5.3) or `A` (`ah`, §8.3);
`0`↔`O`/`o`, `1`↔`I`/`l`, `5`↔`S`, `.`↔`_` in mnemonics (`DT. STOP` = `DT_STOP`); `-`/`—` for `_`; `ESC \`
may appear as `ESC N`; boldface `z` may be lost; Tables 5-1/5-2 (feature defaults/reset actions) are photographed
text that OCR turned into noise (`Tahlo i auic u`); digits under example escape sequences (`027 080 048 …`)
remain a reliable cross-check. Stray Cyrillic glyphs and `MA-####-##` figure IDs are noise. **Numbers in tables
must still be verified against the ROM when possible** — e.g. the HTML got every value of Paul's `[list]`
right, but OM Table 5-3 quotes firmware-2.0 ranges (§8.1).

**Table status:**
- *Recovered from the HTML/PNGs:* RM 1-1 (§5.6), 2-1, 2-2, 4-1 (via A-5), 4-2 (§5.3), 5-3 (DECTST), 5-4/A-7 (LOG),
  5-5/A-4 (TERMINAL), **5-6/A-8 (MASK bit map, §5.2)**, A-1/A-2 (escape commands, replies, DSR extended = `ESC [ n`),
  A-3, A-5, A-6; OM 3-1…3-3 (SETUP), 4-1/4-2 (phonemes + 1-char symbols, §8.3), 4-3/4-4 (stress symbols, tones),
  5-1…5-3 (voice commands, `:n?` voices, `[:dv]` parameters), A-1/A-2 (numeric abbreviations / dictionary
  abbreviations), B-1 (alternates), C-1 (phonemic alphabets).
- *Recovered from the PDF scans (second pass):* **RM 5-1 and 5-2** (§5.5, RM PDF p. 65), RM Appendix A tables
  (PDF pp. 222-230, all confirmed), RM 5-3/5-4 pages; **OM Table B-1** (two strings fixed + Morphology column),
  **C-1 and 4-1/4-2** (1-char alphabet verified, §8.3), **4-4** (tones), **A-1/A-2** (full expansions, §7.3),
  quantity-word list; **MITalk** Table 9-1, rule 7 and rule 10 percentage tables, **Table 10-1** (§14.3-14.4), and
  Tables 11-1/11-2/11-3 (legible on MITalk file-order pp. 132, 134, 135 but not transcribed here — copy from the PDF
  when decoding the parameter tables).
- *Still not transcribed (readable in the PDFs if ever needed):* OM Table 1-1 (keyboard keys / terminal control
  chars; local line editing: CTRL-R repeat, CTRL-U kill line, DELETE); MITalk Appendix B (Klatt symbol tables B-1/B-2:
  the HTML is garbled, the scan is fine), Appendix C (C-1…C-7 PHONET targets and rules), Table 11-1/11-2
  numeric values; RM/OM figures.
- *No user action needed for page images any more:* the PDFs are in `docs/`; use the extractor below.

**Easiest way to view any PDF page: Ghostscript** (installed at `C:/Program Files/gs/gs10.02.0/bin/gswin64c.exe`;
tested on the RM and on `hunnicutt_lts.pdf`, about 0.2 s per page, handles JBIG2 too):
`gswin64c -q -dNOPAUSE -dBATCH -sDEVICE=pnggray -r100 -dFirstPage=65 -dLastPage=65 -sOutputFile=out.png <pdf>`.
Use `-r100` for a whole manual page and `-r150` for the dense Hunnicutt rule pages, then view the PNG with the Read tool.
Pages are numbered in page-tree order, which matched file order in the RM test. `pdftotext -layout` is also on
the path (`/mingw64/bin`), but it only helps for `hunnicutt_lts.pdf`, the one PDF with a text layer.
The Python extractor below is the fallback.

**PDF scans** (the three manuals in `docs/*.pdf`, 48-61 MB): pure page images, with no text layer (so `pdftotext`
returns nothing), and the Read tool cannot open them directly. Each page is one image XObject — RM 1-bit Flate
4300×5336; OM 4-bit Flate 2092×2700; MITalk 1-bit Flate plus a few JPEG (DCT) pages, **often with inverted
polarity** (auto-flip when the mean is dark). The images appear in the file in reading order (blank verso pages
included). Extract with Python (`zlib` + PIL + numpy, all available here), downscale to ~1300 px wide and view
the PNG with the Read tool; a contact sheet of 4-12 pages at 400-500 px is the cheap way to find a page, then crop
and enlarge the table region. **Printed page = PDF/file-order page − offset:** RM 12, OM 13, MITalk 13 (e.g. RM
Table 5-1 printed p. 53 = PDF p. 65; OM Table B-1 printed p. 86-87 = file-order pp. 99-100; MITalk Table 9-1
printed p. 96 = file-order p. 109). Core of the extractor:
```python
import re, zlib, io, numpy as np
from PIL import Image
b = open(pdf, 'rb').read(); items = []
for m in re.finditer(rb'(?<![0-9])(\d+) 0 obj\s*<<', b):
    h = b.find(b'stream', m.start(), m.start() + 3000)
    if h > 0 and b'endobj' not in b[m.start():h] and re.search(rb'/Subtype\s*/Image', b[m.start():h]) and b'/Width' in b[m.start():h]:
        items.append((m.start(), h))            # items[k-1] = image of page k (file order)
def page(k):
    s, h = items[k-1]; d = b[s:h]
    w = int(re.search(rb'/Width\s+(\d+)', d)[1]); ht = int(re.search(rb'/Height\s+(\d+)', d)[1])
    ln = int(re.search(rb'/Length\s+(\d+)', d)[1]); st = h + 6; st += 2 if b[st:st+2] == b'\r\n' else 1
    data = b[st:st+ln]
    if b'DCTDecode' in d: return Image.open(io.BytesIO(data)).convert('L')
    raw = zlib.decompress(data); bpc = int(re.search(rb'/BitsPerComponent\s+(\d+)', d)[1])
    if bpc == 1:                                   # RM/MITalk
        a = np.frombuffer(raw, np.uint8)[:((w+7)//8)*ht].reshape(ht, -1)
        px = np.unpackbits(a, axis=1)[:, :w]; inv = b'/Decode[1 0]' in d.replace(b' ', b'')
        im = (px if inv else 1 - px) * 255
    else:                                          # bpc 4 (OM): 2 nibbles/byte
        a = np.frombuffer(raw, np.uint8)[:((w+1)//2)*ht].reshape(ht, -1)
        im = np.stack([a >> 4, a & 15], axis=2).reshape(ht, -1)[:, :w] * 17
    im = np.asarray(im, np.uint8)
    return Image.fromarray(255 - im if im.mean() < 110 else im, 'L')
```
(The exact polarity rule for 1-bit pages is empirical — RM pages come out right with the `Decode[1 0]` rule, others
need the mean-brightness flip. The 1-bit RM/MITalk pages are ~23 MP; resize before saving.)

**Where to look (search the converted text for the heading):** RM ch. 1 "DECtalk-Computer Communication",
"Speech Timeout", "Data Synchronization", "Control Characters", "Control Character Logging"; ch. 2
"Selecting Alternate Character Sets"; ch. 3 DT_* voice/index/dict; ch. 4 telephone (Tables 4-1/4-2, Fig. 4-1);
ch. 5 "Device Attributes", "Device Status Request", "Reset to Initial State", "Nonvolatile memory (DECNVR)",
"DT_LOG", "DT_TERMINAL", "Keypad Mask Command (DT_MASK)", "Determining firmware revision level"; ch. 6-7 the
C/BASIC library. OM ch. 3 SETUP, ch. 4 phonemics, ch. 5 voices, App. A numbers/abbreviations/spell-out
strategies, App. B alternate pronunciations, App. C phonemic correspondence.

---

## Appendix B — `xtras/`: the 1980s sample code and how to read it

The `xtras/` folder was removed on 2026-09-27; the format notes and test vectors below are what was taken from it.

### B.1 File format: RMS variable-length records (this is *not* plain text)
Files were copied off VAX/PDP-11 media with their **RMS record structure** intact:

```
[u16 little-endian length N][N data bytes][1 NUL pad byte if N is odd]  … repeated
0xFFFF length  = end-of-file marker (absent in most of these files)
```
- **No newlines are stored between records**; each record *is* a line. Some records also carry
  their own `CR LF` (e.g. `readme.1st`), some don't (`dectlk.h`, `.bas`).
- **NUL bytes** are pad bytes (odd-length records) and, in `.doc`, long runs of empty records;
  a 0-length record is a blank line. Strip NULs.
- **Form feeds (`0x0C`)** appear inside/at the start of a record as **page delimiters**
  (`dtlib.doc` has 66; `dectlk.h`, `readme.1st` several). Keep or replace with a page marker.
- Tabs are literal; BASIC-PLUS sources use leading line numbers and `&` continuation;
  `.rno` files are DEC RUNOFF markup (`.hl`, `.s`, `.lm`, `.i`, `#`=nbsp) — the `.doc` is the
  rendered version.
- Verified: every text-like file under `xtras/` (122 of them: `.h .c .bas .cob .com .cfg .mms .lib
  .mem .user .txt .doc .rno .cmd .mak .1st .cld .script .fil`) decodes with **zero leftover
  bytes** using the reader below. Binary VAX/PDP artifacts (`.exe .obj .olb .tsk .bck`) are
  not text and are irrelevant to the ROM. `cookie.fil` is RMS too, but has a binary index up
  front followed by ~9000 lines of `%%`-separated fortune text (a large English test corpus).

```python
import struct, sys
def rms_records(path):
    b = open(path, 'rb').read(); i = 0
    while i + 2 <= len(b):
        n = struct.unpack_from('<H', b, i)[0]
        if n == 0xFFFF: break
        yield b[i+2:i+2+n]
        i += 2 + n + (n & 1)
for rec in rms_records(sys.argv[1]):
    sys.stdout.buffer.write(rec.rstrip(b'\r\n').replace(b'\0', b'') + b'\n')
```
(Use `C:/…` paths with native Windows Python; write decoded copies to the scratchpad, not the repo.)

### B.2 What is in `xtras/dtlib/` (DEC "DECtalk Support Library", Martin Minow)
`dectlk.h` (**authoritative numeric values** for P2/P3/R2/R3, DSR, LOG/TERM/MODE flags, control
chars — §5), `dtlib.doc`/`dtlib.rno` (library manual: `dt_open dt_init dt_answer dt_hangup
dt_talk dt_phone dt_msg dt_dcs dt_cmd dt_dial dt_drain dt_eol dt_gesc dt_pesc dt_get/ioget/ioput
dt_inkey dt_iskey dt_isvalid dt_keypad dt_timeout …`; **no C sources for the library itself** —
they were on separate tape directories; only the RM ch. 6 listing shows some), `sample.bas`
(BASIC-PLUS library + keypad-number demo: best short example of the wire protocol),
`readme.1st/readme.rno/decus.txt`, build scripts (`*.com *.mak makefile*`), `cookie.fil`, and
binaries (`*.exe *.olb *.tsk`).

Facts learned there (all [M]):
- No **call-progress detection** on dial; hosts speak a prompt and wait for a keypress
  (`dt_dial`). Tone chars `0123456789*#ABCD`, `!` 1 s pause, `^` 250 ms hook flash; pulse: digits only.
- `dt_init` turns "local mode" off (`DT_TERMINAL` = 0) so a logging terminal does not answer the
  DA query, then sends DA, checks for `CSI ? 19 c`, sends DECSTR and `MODE SQUARE`.
- Reads from DECtalk are 7-bit; NUL and DEL are ignored; an escape sequence can arrive between
  keypad characters; keypad timeouts (R3=2) can arrive unsolicited and must be tolerated
  while waiting for other replies; hosts should keep a small type-ahead buffer.
- DTC03/VMS `DTK$` constants (`xtras/guide/dtkdef.h`): keypad codes `0-9 A-D # *` = ASCII;
  modes add `EUROPE 8` and `SPELL 16` (DTC03-only), device types `DTC_01=1`, `DTC_03=2`;
  VMS "WINK" = brief caller on-hook flash; `DTK$_NOROOM`/`_TOOLONG` = dictionary status.

### B.3 What is in `xtras/guide/`
VAX/VMS **application demos** (BASIC, C, COBOL) built on the `DTK$` run-time library: banking,
insurance, health-care, telecom, "ACT" (field service/distribution) demos with `.script`/
`*_script.txt` call transcripts, `.cfg` (terminal line → dictionary file), `dictionary.user`
(user-dictionary sample, §7.2), `errata.mem`, `kitinstal.com`. Low RE value for the ROM itself
but good for (a) realistic *input text* (menus, dollars, dates, part numbers, phonemic
inserts like `[wuhdax<60>v]`, `[:nh +]`), and (b) confirming host behavior (1200 baud setting for
DTC01; DTK$ autostop mode broken with terminal servers on DTC01). Note the `[:nh +]`/`[+]` usage
is DTC03-era; on v1.8 it triggers the phoneme-error flag.

### B.4 Wire-level sequences from the samples (7-bit forms)
| Purpose | Bytes | Expected reply |
|---|---|---|
| flush clause | `0B` | – |
| who are you | `1B 5B 63` | `1B 5B 3F 31 39 63` (`ESC [ ? 1 9 c`) or 8-bit CSI form |
| soft reset | `1B 5B 21 70` | – |
| no local→host (`DT_TERMINAL` 0) | `1B 50 30 3B 38 32 7A 1B 5C` | – |
| MODE SQUARE on | `1B 50 30 3B 38 30 3B 31 7A 1B 5C` | – |
| speak a line | `"Hello. "` + `0D 0A` | speech |
| sync + confirm | `0B` `1B 50 30 3B 31 31 7A 1B 5C` `1B 50 30 3B 32 32 7A 1B 5C` | `ESC P ; 32 ; [n] z ESC \` (zero omitted) |
| phone status | `1B 50 30 3B 36 30 7A 1B 5C` | `ESC P ; 70 ; [R3] z ESC \` |
| answer after 1 ring | `… 3B 36 30 3B 31 30 3B 31 7A …` (`60;10;1`) | on-hook reply, later off-hook reply |
| keypad on / off | `60;20` / `60;21` | status |
| keypad timeout 10 s | `60;30;10` | R3=2 unsolicited on expiry |
| hang up | `60;11` | reply after on-hook |
| load dictionary | `1B 50 30 3B 34 30 7A "DEC d'ehk" 1B 5C` | `ESC P ; 50 ; [0\|1\|2] z ESC \` |

(Original BASIC-PLUS code sends `ESC` as `chr$(155)` = ESC with parity bit; the wire form is
plain `1B`. DECtalk's reply may be 7- or 8-bit C1 depending on S7C1T/S8C1T.)

---

## Appendix C — behavior test corpus (from the manuals; use with `LOG PHONEME` / emulator)

| Input | Expected speech |
|---|---|
| `The sum of $45.98 is too much to pay for a pair of shoes.` | "…forty-five dollars and ninety-eight cents…" |
| `$ 12.45` / `$12.45` | twelve dollars and forty-five cents |
| `$` + index + `12.45` | "dollar twelve point four five" |
| `$1.23 million` | one point two three million dollars |
| `123`, `123,456`, `12.34`, `12.34E56`, `+1.2E-4`, `12%` | cardinals / decimals / scientific / percent |
| `1(2)3` | one left-parenthesis two right-parenthesis three |
| `01234` | digit string (zip-code style) |
| `1234,56` | digits with "comma" (bad grouping) |
| `5000` / `1984` | five thousand / nineteen eighty-four |
| `12345678901` | digit groups "123, 456, 78901" |
| `1st 23rd` (ok) / `2th` (not ordinal) | ordinal / spelled |
| `1/2 2/3 44/100% 2/3rds` | fractions |
| `23-Sep-1983`, `23-Sep`, `23-Sep-83` | dates |
| `11:04:03.01`, `12:00` | VMS time; "twelve, oh oh" |
| `(617) 493-8255` | "six hundred seventeen, four nine three dash eight two five five" |
| `10-15` | dash vs minus per MODE MINUS |
| `3 cm.`, `2 lbs.`, `1 [ ]ft. 3` | centimeters, pounds; last one blocks the abbreviation |
| `Dr. Zhivago Dr.` / `St. Louis St.` | doctor zhivago drive / saint louis street |
| `A.P.O.` / `a.p.o.` | "aye pea oh" / "aye period pea period oh period" |
| `sys$system`, `(foo.)`, `Hello...` | spelled; abbreviation retry; ≡ `Hello.` |
| `naive` with accent, BS-overstrike samples | accent ignored; `a BS _`→a, `ab BS BS de`→de |
| `)read`, `)insert` (MODE SQUARE on) | alternate pronunciations (Table B-1) |
| `[+]` | DSR error 25 on v1.8 (silent phoneme only in 2.0) |
| `[:np] Hello. [:dv sex f] Good-bye.` | Paul then female-shifted Paul |
| `[:ra 250]`, `[:cp 250]`, `[:pp 2000]` | rate 250 wpm; +250 ms comma pause; +2000 ms period pause |
| `[d<100,102>aa<250>]` | sung note (pitch 102 Hz, 250 ms) |
| `1B 5B 6E` (extended DSR) twice, fresh power-up | 1st: `ESC [ 0 n ESC [ ? 21 n`; 2nd: `ESC [ 0 n ESC [ ? 20 n` |
| `1B 5B 6E`, then `1B 50 30 3B 30 7A 2B 1B 5C` (`[+]`), then `1B 5B 6E` | 3rd reply on v1.8: `ESC [ 3 n ESC [ ? 25 n` (firmware-revision probe) |
| `1B 50 30 3B 38 33 3B 33 30 37 32 7A 1B 5C` (`DT_MASK` 3072), then any reply-producing command | reply followed by `0D` (CR); keypad `*`/`#` (phone) also followed by CR |
| `1B 50 30 3B 38 33 3B 30 7A 1B 5C` (`DT_MASK` 0) | CR-after-reply behavior off |
| `1B 5B 35 6E` (brief DSR) | `ESC [ 0 n` (or `ESC [ 3 n` after an error; does not clear the error flags) |
| load `DEC d'ehk` (`DT_DICT`), speak `DEC`, send **DECSTR** (`1B 5B 21 70`), speak `DEC` again | still "dek" — DECSTR keeps the user dictionary (Table 5-2) |
| same, but send **RIS** (`1B 63`) instead of DECSTR | `DEC` back to the built-in reading (RIS deletes the dictionary) |
| `DT_TERMINAL` after power-up/RIS (no query command; observe local echo/speech) | flags = 6 (`TERM_SPEAK`+`TERM_EDITED`), `DT_LOG` = 0 (Table 5-1) |
| Paul: local terminal off-line `[:np :dv list]` | 28 rows matching §8.1's Paul column (not observable via host TX — reads the console queue `0x80328`) |
