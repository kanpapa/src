# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This is the NetBSD source tree (`src/`). The primary active work is a port of NetBSD/alpha to the **DEC AXPvme 230** (VMEbus Alpha SBC, 21066A CPU / LCA chipset, system type `ST_DEC_AXPVME_64 = 10`). Target is diskless boot.

## Build Commands

```bash
# Build the AXPVME kernel (run from /home/ocha/NetBSD/src)
./build.sh -U -u -j2 -O ~/obj -m alpha -a alpha kernel=AXPVME
```

The built kernel ends up under `~/obj/sys/arch/alpha/compile/AXPVME/netbsd`.

## AXPvme 230 Port – Key Files

| File | Role |
|------|------|
| `sys/arch/alpha/alpha/dec_axpvme_64.c` | Platform init (`dec_axpvme_64_init`, `dec_axpvme_64_cons_init`) |
| `sys/arch/alpha/pci/pci_axpvme_64.c` | PCI interrupt routing stub (not yet implemented) |
| `sys/arch/alpha/conf/AXPVME` | Kernel config (`options DEC_AXPVME_64`) |
| `sys/arch/alpha/alpha/cpuconf.c` | Registers `dec_axpvme_64_init` for system type 10 |
| `sys/arch/alpha/alpha/machdep.c` | Generic Alpha `alpha_init()`; contains AXPvme Bcache workaround block at end |
| `sys/arch/alpha/alpha/locore.s` | Boot assembly (`locorestart`); no printf between `alpha_init` return and `swpctx` |
| `sys/arch/alpha/pci/lca.c` | LCA (21066A) chipset: `lca_probe_bcache()` reads `MEMC_CAR` |

Reference platform (same LCA chipset): `dec_axppci_33.c` / `pci_axppci_33.c`.

## AXPvme 230 Hardware Constraints

### External Bcache (21066A / LCA)

`MEMC_CAR = 0x217f9c75` → `BCE=1, BCS=3` = **512 KB external Bcache enabled**.

- **Bcache FILL (L1 write-back)**: works correctly.
- **Bcache probe/invalidation**: hangs — the Bcache controller on this board does not respond to the invalidation protocol. This affects:
  - `K1SEG` writes (uncached superpage `0xfffffe0000000000|PA`) → triggers probe/invalidation → **HANG**
  - `PAL_cflush` → triggers probe/invalidation → **HANG**
- **K0SEG writes** (`0xfffffc0000000000|PA`, cached) work correctly via L1 write-back to Bcache on eviction.
- `swpctx` reads the new PCB via the **cached** path (L1 → Bcache), not directly from DRAM.

### SRM PROM callback scratch area

Physical `0x10a000` is used by the SRM PROM as scratch space during **every** `promcons` `printf` callback. Each callback writes stale bootstrap data (`0xde0a`) to `0x10a000` via an uncached physical write **and** invalidates the Bcache entry for that page.

lwp0's uarea (PCB) is allocated at physical `0x10a000`. `apcb_ksp` is at PCB offset 0, which lands exactly at physical `0x10a000`.

### Critical invariant: no printf between alpha_init() and swpctx

`machdep.c:alpha_init()` ends with a K0SEG block that writes correct PCB values (especially `apcb_ksp`) after the last `printf`. Any `printf` (= PROM callback) called **after** this block and **before** `call_pal PAL_OSF1_swpctx` will corrupt `0x10a000` and cause a `double error halt` (halt code 6).

`locore.s:locorestart` must not have any `printf`/`CALL` to debug-marker functions between `CALL(alpha_init)` and `call_pal PAL_OSF1_swpctx`.

### Console

`dec_axpvme_64_cons_init()` is intentionally empty: the SRM PROM callback console (`promcons`) set up by `init_bootstrap_console()` (`prom.c`) is used throughout. The on-board Z8530 SCC is not driven by NetBSD at this time.

## Current Status

- Platform registers as `AXPvme 64`, reaches `locorestart` in locore.s.
- K0SEG PCB write + removal of mark1/mark2 debug prints is the active fix for `swpctx` double error halt.
- Next milestone: `swpctx` succeeds → `mark3` prints → proceed to PCI interrupt routing.
- After boot: PCI interrupt routing implementation in `pci_axpvme_64.c`.

## 開発ジャーナル
タスクが完了したら、作業内容（追加・修正したファイル、直面したバグ、解決策）を必ずMarkdown形式で `journal.md` に追記してください。