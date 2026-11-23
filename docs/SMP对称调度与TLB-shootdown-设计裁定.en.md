# SMP symmetric scheduling and TLB shootdown: design ruling

> Status: **ruling made, not implemented** (2026-11-23).
> The English page is a condensed edition. The Chinese page
> [SMP对称调度与TLB-shootdown-设计裁定.md](SMP对称调度与TLB-shootdown-设计裁定.md)
> is normative and carries the full wording, the source quotes and the check commands.
> This page adds no new claims and no new measurements.

## 0. Conclusion in three sentences

1. The kernel stays at **AMP (asymmetric multiprocessing)**: every secondary core has its own
   run queue, time slice and switch path, but only CPU0 is a "kernel core" - secondary cores issue
   no SVC, call no kernel API and do not advance the global tick.
2. **TLB shootdown is not implemented.** It only makes sense if user-mode tasks concurrently touch
   the same virtual address on two cores, and that premise does not exist here. Implementing it now
   would produce a function that sends an IPI and *assumes* the entry is gone - worse than nothing on
   real silicon.
3. **Symmetric scheduling is not done either.** It would rewrite the architectural premise of the
   "kernel core" itself (per-core kernel stack, interrupt path, SVC dispatch, shared ready structure),
   at a cost out of proportion to what can be verified. The reopening conditions are in §5.

## 1. Three rungs, the kernel stands on the first

| Rung | Meaning | This kernel |
|---|---|---|
| **AMP policy split** | Per-core run queues and switch paths; CPU0 explicitly moves tasks out and back; secondary cores never enter the kernel | **Yes** (manual migration, then automatic balancing) |
| **Symmetric scheduling** | Any core can take any task from one shared structure; the decision is not bound to a core | No |
| **Full SMP** | Symmetric scheduling + any core may enter the kernel (SVC / exception / locks) + cross-core coherence protocol (including TLB) | No |

The four boundaries are design constraints written into headers, not a temporary state:

- secondary cores may not issue SVC and may not call kernel API (a SYS from a secondary core is a
  fatal stop, never silently allowed);
- only READY tasks are accepted for migration (refuse code -3); a task currently running on CPU0 is
  refused with -6 so that two cores never share one stack frame;
- per-core timers exist but the global tick is advanced by CPU0 only;
- migration is "request + per-tick poll": `task_to` / `task_back` only register, the actual move
  happens at the secondary core's switch point, latency bounded by one tick.

Automatic balancing sits on top of this layer as pure policy and changed none of the four.

## 2. What symmetric scheduling would cost

In dependency order: (1) a globally consistent notion of time (one master clock plus drift handling,
or per-core local ticks with a global number used only when needed); (2) secondary cores entering the
kernel - per-core kernel stack, full save/restore on their interrupt entry, SVC dispatch usable in a
secondary context, kernel critical sections still correct with two cores inside at once; (3) the
scheduling structure itself (`ready_rebuild` / `sched_check` / the linear pick would all be rewritten);
(4) locks and memory ordering - cross-core ordering goes from "only at migration hand-off" to
"everywhere"; (5) verification cost - QEMU can prove both cores run and both receive IPIs and the
queue states agree, but it cannot prove real-silicon races or coherence protocol behaviour.

Any one of these five costs at least as much verification as the two balancing rounds together.

## 3. TLB shootdown: what this QEMU can even do

Measured readouts (see the QEMU page) on `invtlb` op:

| `invtlb` op | Behaviour on this QEMU |
|---|---|
| 0 / 1 / 2 / 6 | clears the whole table, after which kernel refill turns `KERNEL ... fatal` |
| 4 / 5 | no-op (no error, no invalidation) |
| 7 | illegal instruction |
| 3 | invalidates by VA and **ignores ASID** |

So there is no "invalidate one ASID" instruction on this carrier. Even with every prerequisite in
place, shootdown could only be "wake the target core and let it invalidate by VA", not a broadcast
flush. That caps both the shape and the cost.

## 4. Three prerequisites, none of them met

1. `arkrtos_port_send_ipi()` explicitly refuses core 0 (core 0 is the boot core and has no IPI
   handler path installed; it returns -1). A secondary core therefore cannot force the main core to
   invalidate an entry.
2. The secondary entry `arkrtos_smp_secondary_entry` never enables paging (never writes `CRMD.PG`,
   never builds a TLB), so it has no entries that could go stale.
3. There is no user-mode task concurrently accessing one VA on two cores; a SYS from a secondary core
   is fatal, and the only `arkrtos_smp_sched_task_to()` callers are demo code.

Judgement = the first no longer returns -1, the second shows a `CRMD.PG` write, the third shows a
non-demo caller.

## 5. Ruling

### 5.1 Keep AMP and say so

The only permitted wording is "SMP support = two cores running in parallel plus explicit/automatic
task migration, secondary cores are compute-only". It must **not** be described as symmetric
multicore, load-balancing SMP, or full SMP support.

### 5.2 Shootdown is not implemented, and no look-alike substitute either

Three fake implementations are explicitly rejected: sending an IPI and assuming the target
invalidated; wrapping `invtlb` op 4/5 and claiming per-ASID invalidation (it is a no-op here); using a
full table clear in place of a targeted invalidation (it kills our own refill). The existing shape -
each core only changes mappings it can see, cross-core sharing only at the migration hand-off - is
the correct shape when no shootdown exists.

### 5.3 Reopening conditions (all of them, none optional)

| # | Condition | Test |
|---|---|---|
| R1 | Main core has an IPI path | **Met (2026-11-23, measured on QEMU LA64 `-smp 2`)**: `arkrtos_port_send_ipi(0)` returns 0; the INT branch of `la_trap_c()` has an `ESTAT.IS` IPI arm that reaches `arkrtos_smp_ipi_handler()`; `arkrtos_port_irq_init()` enables this core's `CORE_EN` and `ECFG.LIE[12]`. Reading `[smp] ping ipi0=2 self=1 back1=1 result=ok`; gate `tools/smp_gate.py` pin R1. See [QEMU emulator port and verification](QEMU模拟器移植与验证.en.md) §9.10 |
| R2 | Secondary cores enable paging | **Met (2026-11-23, measured on QEMU LA64 `-smp 2`)**: `la_mmu_init()` is split in three, static entries now come from a replayed snapshot (5 entries); `arkrtos_port_paging_enable_secondary()` writes its own `ASID`/`STLBPS`, replays the snapshot, repoints `TLBRENTRY` to `tlb_refill_entry_secondary` (one 8 KB stack per core) and finally goes DA->PG; after enabling it looks up an identity mapping through a kernel `.bss` address and returns -3 on mismatch. Reading `[smp] pg cpu1 on=1 rc=0 look=1 result=ok`; gate `tools/smp_gate.py` pin R2. See [QEMU emulator port and verification](QEMU模拟器移植与验证.en.md) §9.11 |
| R3 | A real cross-core shared-VA caller exists | **Met (2026-11-23, measured on QEMU LA64 `-smp 2`)**: the path used is the real `arkrtos_smp_sched_task_to` migration (not a hand-rolled simulation); the task's stack and mailbox both live in window VA at `0x02100000`, and both cores read and write the same physical frame through that VA. Three pieces on the secondary side: it installs the space's window itself before switching to the user task (`arkrtos_port_paging_install_space_window` drops the old entries first, then proves the first and last entry pairs by **indexed** read-back, with codes -1/-2/-3/-4/-5); the restore privilege comes from the frame's own return address (`la_frame_priv_auto` intersects "should be a user task" with "ERA inside app window VA", shared by all three restore paths); and task binding rebuilds the initial frame inside the target space (`relocate_frame` writes straight through the frame-pool physical address, no window involved). Readings `[shva] win inst=1 rc=0 space=0` / `[shva] ack=1 core1=1 data=0x00001235 loops=46896894` / `[shva] back rc=0 owned=0` / `[shva] result=ok`; gate `tools/smp_gate.py` pin R3. See [QEMU emulator port and verification](QEMU模拟器移植与验证.en.md) §9.12 |
| R4 | There is a receipt, not a timeout | **Met (2026-11-23, measured on QEMU LA64 `-smp 2`)**: `arkrtos_smp_tlb_shootdown()` fills a request slot per online core (`space/va/pages` first, `seq` published last), sends the IPI and **waits for the target to write `ack` equal to that `seq`** -- the receipt is a counter written by the target, not an initiator spin count. The target calls `arkrtos_smp_sd_target_poll()` at the tail of `arkrtos_smp_ipi_handler()`: when its own window is exactly the requested space it does "drop old -> rebuild from the ledger -> prove by indexed read-back", otherwise it records `sd_skip` but **acknowledges anyway**; failures also acknowledge and record a negative `last_rc`. Timeout is an iteration cap and honestly returns -2. Readings `[sd] win=0 pos rc=0 dack=1 dinval=1 dskip=0 dtimeout=0 dipi1=1 seq1=1 ack1=1 rc1=0`, negative control `[sd] neg rc=-2 dack=0`, recovery `[sd] rec rc=0 dack=1 dinval=1`, `[sd] result=ok`; default build zero footprint (byte-identical to the R3 baseline). Gate `tools/smp_gate.py` pin R4. **Limit**: it proves the protocol and the receipt, not that the stale entry really disappeared (`tlbsrch` and MTLB `tlbrd` are unusable on QEMU), and it is not a full shootdown (one slot per core, whole-window rebuild, no batching, no fine-grained page range). See [QEMU emulator port and verification](QEMU模拟器移植与验证.en.md) §9.13 |
| R5 | A real-board carrier exists | races and coherence protocol behaviour cannot be judged on QEMU alone |

R1-R4 can be advanced on this machine; R5 needs silicon. Until then shootdown can at most be
"demonstrated on QEMU", never announced as supported.

### 5.4 If reopened, the minimal first step

The order is fixed: ~~R1 first (the only independent increment touching neither paging nor
scheduling)~~ **done 2026-11-23** (four changes, host and emulator readings, and the gate are in the
R1 row of §5.3 and [QEMU emulator port and verification](QEMU模拟器移植与验证.en.md) §9.10; next is R2),
~~then R2 (only then does a secondary core have entries to invalidate)~~ **done 2026-11-23**
(snapshot scope, per-core refill stack, the -3 self-verify and the two wrong predicates are in the R2 row
of §5.3 and [QEMU emulator port and verification](QEMU模拟器移植与验证.en.md) §9.11; next is R3), ~~R3~~ **done 2026-11-23** (the R3 row of §5.3 and
[QEMU emulator port and verification](QEMU模拟器移植与验证.en.md) §9.12; only R4's receipt and
R5's silicon are left for shootdown), then the protocol
~~(target core receives IPI, invalidates by VA at a safe point, writes a readable counter; the initiator
waits for the counter with a bound and reports failure honestly - never "timeout means success")~~
**done 2026-11-23 (R4)**: protocol, failure-code split, negative control and measured readings are in the R4 row
of §5.3 and [QEMU emulator port and verification](QEMU模拟器移植与验证.en.md) §9.13. Shootdown now only lacks
**R5 (real hardware)**, which needs silicon -- so until a board is available shootdown can still only be
"demonstrated on QEMU", never announced as supported.

### 5.5 Cost estimate (order of magnitude, no false precision)

| Change | Magnitude |
|---|---|
| Main-core IPI path (R1) | one trap branch + one handler + gates |
| Secondary paging (R2) | secondary boot sequence + its own mapping install + readout |
| Invalidation protocol (R3/R4) | new module + call sites + real-board verification (highest risk, no local precedent) |
| Full symmetric scheduling | rewrite clock / ready structure / locks / interrupt surface |

## 6. Relation to existing records

- `docs/QEMU模拟器移植与验证.md` §9.6.10, §9.6.11, §9.9, §5.28, §5.29 - the readouts and the
  verdicts this page relies on.
- Rows `SMP 多核`, `每空间 ASID / TLB 切换（U-7）` and `每进程地址空间（MMU 档）` in
  `docs/欠账盘点.md`.
- This page rules; it does not replace any of those readouts.

## 7. Boundary and unverified

- The `invtlb` semantics are **this QEMU carrier's** behaviour; whether real silicon offers targeted
  invalidation cannot be established here. §3 holds only for "QEMU as the verification carrier".
- "Secondary cores never enter the kernel" is deliberate, not an omission; changing it is an
  architectural change that needs a new pre-registration.
- No new experiment was run for this page, so it adds no "verified" item. The ruling is a trade-off,
  not a performance conclusion.
