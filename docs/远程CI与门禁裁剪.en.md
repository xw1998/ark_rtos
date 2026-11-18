# Remote CI and Gate Trimming

> Condensed English edition. The Chinese page (远程CI与门禁裁剪.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

This page says one thing only: after `tools/ci_gate.py` runs on a Gitee runner, **which checks actually ran, which did not, and why** — and how to read that log without fooling yourself.

In one line: the remote profile runs a **trimmed check set**, `skip` is not `pass`, and the log carries one line `NOT VERIFIED on this run: ...` listing everything that did not run.

## 1. Why trimming is needed

17 of the gate's forty-odd checks drive **toolchains that exist only on the development machine**:

| Toolchain | Who needs it | Used for |
|---|---|---|
| Keil `armclang` | `tickless` `rtstat` `eset-syntax` `waitmulti-syntax` `waitq` `smp` `seccheck` `swap` | compile the "switch-on" TUs with `-fsyntax-only` |
| `arm-none-eabi-gcc` | `arkmake` | syntax tier of the on-board make |
| LoongArch64 / RISC-V cross gcc | `paging` `elf` `modld` `payload-sign` `stack-guard` | `paging_gate.py` / `elf_gate.py` / `modld_gate.py` etc. really build two TU sets and run negative cases |
| RV32 cross gcc (needs GCC 10+, `-fanalyzer`) | `analyzer` | path-sensitive static analysis per TU (48 TUs + 2 poisoned self-checks) |

None of these exist on a Gitee Go runner. Three ways of faking it are unacceptable:

* **keep running them** on the runner ⇒ every build is a wall of false red (a missing toolchain reported as "the code is broken");
* **delete them** from the table ⇒ they stop running locally too, and real regressions go unseen;
* **call a missing toolchain a pass** ⇒ the worst of the three, turning the gate into decoration.

So the fourth option is used: under `--remote` these checks are **explicitly skipped and counted separately**, the list is printed at the end, and the exit code only reflects real failures.

## 2. Reading the remote profile

`.workflow/ci.yml` triggers on every push to master:

```
python3 tools/ci_gate.py --remote
```

The tail of the log looks like this (actual reading from a local run with `--remote` on 2026-11-18; a runner should match):

```
skip tickless                   needs armclang (Keil MDK) (not on the runner)
...
skip stack-guard                needs LA64 / RV32 cross gcc (not on the runner)
skip analyzer                   needs RV32 cross gcc with -fanalyzer (GCC 10+) (not on the runner)
ok   remote-ci                  workflow calls the gate with --remote; 17 skipped check(s) all named in the table

30 check(s) passed, 0 failed, 17 skipped (remote profile)
NOT VERIFIED on this run: analyzer, tickless, arkmake, rtstat, eset-syntax,
waitmulti-syntax, waitq, smp, seccheck, swap, paging, elf, shell-auth, modld,
payload-sign, stack-guard, sec-crc
```

Three conventions:

1. **A `skip` line is not an `ok` line**: skipped checks do not enter `PASSED` and do not change the exit code; a check that never ran can never be read as passing because it "did not fail".
2. **`remote-ci` is a real check**: it confirms `.workflow/ci.yml` exists, really points at `tools/ci_gate.py` with `--remote`, and that every name in `LOCAL_TOOLCHAIN` appears in `CHECK_TABLE` (a misspelt name would make the local check keep demanding a toolchain on the runner).
3. The local run is still the **full profile** (`python tools/ci_gate.py`); the remote profile is a supplement, not a replacement.

## 3. What the remote profile covers (and what it does not)

**Really runs on the runner**: structural checks (partition twins, single address source, encoding/line endings, doc index, API freshness, layout, no partition header, Kconfig output, DTS output, release consistency, softbus sync header) + host build and run checks (utest/sdk-stdio/sbuf/mempool/mq/sync/eset/waitmulti/loader/fifo/fault/coverage/warn-clean/sign-host) + `remote-ci` itself.

Note `warn-clean` uses host gcc (`-Wall -Wextra -Wshadow`), which the runner has; `analyzer` needs **GCC 10+** with `-fanalyzer`, which locally only exists as the cross compiler, so the remote profile skips it — that is a different thing from `warn-clean`, do not conflate them.

**The four groups that do not run, and where their evidence lives instead**:

| Not run | Substitute evidence |
|---|---|
| Keil syntax tier (tickless/rtstat/eset/waitmulti/waitq/smp/seccheck/swap/sec-crc) | full `python tools/ci_gate.py` on the development machine; the criterion is that line's `ok`/`FAIL` |
| `arkmake` | same as above |
| paging / ELF / shell-auth / modld / stack guard / payload signing (LA64 + RV32) | same as above; plus QEMU run readings (see QEMU模拟器移植与验证.md) |
| static analysis (`analyzer`, RV32 + `-fanalyzer`) | same as above; the criterion is `tools/analyzer_gate.py` printing `ANALYZER-GATE OK (48 TU, 0 diagnostic, 2 self-check probes fired)` |
| Keil project build (`--build`, never in the remote profile anyway) | `--build` on the development machine + hardware regression |

## 4. Status of this file (the unverified parts are pinned down)

* **The workflow file itself is unverified**: there is no Gitee Go runner here; `.workflow/ci.yml` is written against fields from the Gitee Go help centre ("trigger events", "task orchestration"), so the first time the pipeline is enabled in the web UI, the platform's validation decides. The fields used are only: `version` / `name` / `displayName` / `triggers.push.branches.precise` / `stages[].stage.name` / `displayName` / `strategy` / `trigger` / `steps[].step: build@python` / `pythonVersion` / `commands`.
* **Readings on the runner are unverified**: the **local** behaviour of `--remote` has readings (30/0/17 quoted above), but "can Python 3.9 + system gcc on the runner make all 30 pass" has never been run. Two known risks: `coverage-host` needs `gcov`; `api-doc` needs `git diff` readable in the work tree (a Gitee Go checkout is a plain clone, which should satisfy it).
* Therefore **the existence of the remote profile must not be read as "the gate has left the local machine"**: it is one more evidence chain elsewhere; real failures are still found mainly by the full local profile.

## 5. What remains after wiring it in

1. First real run on a runner, then record the `NOT VERIFIED` list and any failures into this page (this step can only be written after seeing a result, it is not pre-filled).
2. To get the Keil half into the cloud, either self-host a runner (with MDK installed) or rewrite the syntax tier into a form `arm-none-eabi-gcc` can compile — the latter changes the convention "which compiler represents the board's answer", so it must be decided first.
3. **Real static analysis is wired** (2026-11-18): the `analyzer` item runs 48 TUs through `-fanalyzer` of the local cross toolchain's GCC 10, with criteria and boundaries written into the static-analysis row of 欠账盘点.md. It parts ways with remote CI here: analysis runs only on machines that have the toolchain, and is explicitly skipped on the runner.
