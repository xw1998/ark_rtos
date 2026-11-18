# arkvim Editor Display Conventions

> Condensed English edition. The Chinese page (arkvim编辑器显示约定.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

The `vim` command is backed by `kernelsrc/components/arkvim`, a vendored copy of the upstream repository `D:/工作/git_project/arkvim` (gitee: `xw19981010/ark-vim`).

This document covers only **what should appear on screen** and how each convention is proved to hold. Upstream keybindings, commands and search implementation are not here.

## 1. Screen layout

| Row (0-based) | Content |
|---|---|
| `0 .. 21` | edit area, 22 rows |
| `22` | status line: `-- COMMAND --  <filename>  <n>L`, or the insert-mode hint |
| `23` | command line: `:`, `/` and subsequent input are all drawn here |

`ARKVIM_SCREEN_ROWS=24`, `ARKVIM_SCREEN_COLS=80`. The screen is a fixed 80×24 grid; window resizing is out of scope.

The gutter (line-number column) is **on** by default, width = digits of the largest line number + 1 separator space, at least 3 digits (so even a small file takes 4 columns and the text does not shift sideways as the file grows); from 1000 lines it becomes 5 columns, from 10000 lines 6. Column 0 of the edit area is the gutter and text starts at `gutter_width`; for a short file you see 4 columns like `  1 `.

## 2. Five display conventions

### 2.1 If a frame did not change, not one byte is sent

Refresh uses double buffering plus dirty-line detection: only cells that really changed become output. Cursor state (`ESC[?25l` / `ESC[?25h`) is accounted for the same way: a position and visibility the terminal already knows are not re-sent.

**Why**: previously every refresh unconditionally re-sent "hide cursor + show cursor", which on MobaXterm made the cursor blink a dozen times per second — that is exactly the "cursor blinking abnormally" the user reported.

**Criterion**: the serial line must see **zero bytes** while idle. Measured on board (`rawcap.py`, every byte timestamped): opening `/mnt/nor/main.c`, the full screen finished at `t=1.569`, and from then until `t=5.309`, when `:` was pressed, there were **3721 ms of zero bytes**.

### 2.2 The command line draws its prefix once

`cmd_buf[0]` in the command buffer already is the triggering `:` or `/`, `cmd_len` includes it, and `exec_ex()` also does `if (cmd[0] == ':') cmd++;`. So the prefix-drawing code **may draw it only once** — before upstream `a8fd82e` it was "draw the prefix once, then copy the whole line including the prefix", which showed `::q` on screen.

**Criterion**: pressing `:` on board sends only `ESC[24;1H ESC[0m :` (one colon), pressing `q` sends only `ESC[24;2H q`; host gate 2 directly requires that `::` never appears in the output.

### 2.3 Line numbers on by default, gutter grows with digits

The gutter is shown by default so line numbers from compiler diagnostics can be matched; above `1000` lines it goes from 2 to 3 digits automatically and the text does not shift because "the numbers got wider".

### 2.4 Control bytes are never drawn as glyphs

`0x00..0x1F` and `0x7F` are always drawn as `?`. That includes **CR** and **TAB**.

**Why**: the on-board `echo` writes files with CRLF, and in the gap buffer CR is just an ordinary byte. Sending it to the terminal pulls the cursor back to column 1, tearing the line apart, and the following `ESC[y;xH` cursor move then lands on the gutter — looking like "display corruption".

Measured on board (file content `line one\0x0D line two` / `x\0x09y` / `ctl\0x01z`): the terminal received CR 0 times, TAB 0 times, `0x01` 0 times, all three places showed `?`, and the following 1.5 s idle produced 0 bytes.

### 2.5 The cursor is hidden/shown only when the state really changes

The cursor is hidden during a write (so it never sits on a half-drawn cell) and handed back right after drawing. The terminal's own blinking (usually 1–2 Hz) is terminal behaviour, not bytes from the editor.

## 3. Known limitations

* **TAB is not expanded**: per 2.4 it is drawn as `?`. Makefiles need TAB indentation as a hard requirement; in this editor such a file shows `?` where the TAB is, and `make` still works normally.
* **No wcwidth**: the edit area lays out one byte per cell, so UTF-8 multibyte characters (Chinese) take several cells. Files containing Chinese misalign in this editor; that is the existing implementation and has not been changed.
* **No line wrapping**: long lines are truncated at the right edge instead of wrapping.

## 4. How to verify

### 4.1 Host side (no board)

```bash
cd D:/工作/git_project/arkvim && make test
```

Three gates, all must PASS:

| Gate | Requirement |
|---|---|
| 1 idle zero output | open a file, press nothing, output byte count must be 0 |
| 2 command line drawn once | the output must contain no `::` (and no NUL) |
| 3 control bytes not emitted | for a file with CR/TAB/LF, `0x0D/0x09/0x0A` in the output are all 0 and `?` ≥ 3 |

### 4.2 Board side

```bash
# idle zero bytes + single colon on :q + exit
python C:/Users/14905/AppData/Local/Temp/rawcap.py \
  "vim /mnt/nor/main.c" CR "wait=2.0" "wait=1.5" ":" "q" CR "wait=1.0"
```

Watch three things: whether there is a long silent stretch (zero bytes) between the full screen and the next keypress, whether `:` appears exactly once in the segment where `:` is pressed, and whether pressing Enter after `q` returns to `ark@arkrtos:~$`.

## 5. How to change this component

`kernelsrc/components/arkvim/src/` is a vendored copy and **must not be hand-edited** — edits get overwritten at the next sync, and the upstream gates never see them.

Process:

```bash
cd "D:/工作/git_project/arkvim"        # edit source + add/change tests/ gates + make test
git commit && git push origin master

cd "D:/工作/git_project/Ark RTOS"
python tools/sync_arkvim.py --apply    # copies only src/*.c src/*.h, rewrites VENDOR.md
```

One trap that lies silently: `shutil.copy2` carries mtime along. A file's last change in the source repo can be barely newer than the target `.o`, so `make` decides "no rebuild needed", the artefact's text/bss stay byte-identical, and it looks like "the sync took effect". `sync_arkvim.py` now calls `os.utime(dst, None)` after copying; after a sync it is still recommended to check build sizes once (see the table below).

Current sync point: source commit `0c4cb1c`, FLASH 461800 B (88.08%), RAM 285900 B (63.45%), SDRAMFS 5849888 B (92.98%).

## 6. Related cases

| Case | Coverage |
|---|---|
| `tools/final_app.py`, 6 items | does not touch the editor; guarantees cc / make / mini were not broken by this sync |
| upstream `tests/test_render_idle.c` | the three gates above, run on host, no board needed |
