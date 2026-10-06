# Net de Get flash maintenance regression

## Scope and provenance

These checks use ordinary joypad input in the owned, unchanged original ROM
(SHA-256 `9fb1e6e4a637796b8624bd2de6c9abaa9e758546b620cb5dc8441b07c288bc63`),
public synthetic accounts on the disposable localhost REON harness, and copies
of saves produced by natural downloads. No original save, personal adapter
configuration, production account or ROM image is included here.

The initial Linux runtime was `ed393f522`. The corrected core was tested as a
dirty build from that commit, library SHA-256
`0122452480576dd1942e932523f30a3e95ddc288fc94906184e1fe2b3fe8fbbc`.
The independent disassembly/trace evidence is in zenaror/net-de-get (checkpoint `3ff7e91`),
`docs/research/minigame-maintenance.md` and `fixtures/maintenance/`.

Addresses written as `20:xxxx` below use the trace's decimal A selector 20
(hexadecimal `$14`), not a physical 16 KiB disassembly bank. Flash bank numbers
prefixed with `$` are hexadecimal 8 KiB selectors.

## Failure and correction

Two natural downloads installed G001 at flash offset `$00000` and G002 at
`$20000`. Moving G001 from BOX1 to BOX2 through `いれかえ`, then deleting G002
through `けす`, produced a white screen on the old core. BOX membership is
catalog metadata; both games still occupied different physical sectors.

The first bad read was captured before the crash. ROM0 `$138D` erased the spare
sector through window B, polled completion, sent `$F0`, and disabled flash access
at `$1359`. Later `20:446A` selected B bank `$10` and re-enabled flash to read the
selected game's ID. The old core still redirected array reads to the erased
spare bank `$70`; the ID passed at `20:447E` and ROM0 `$0D50` was `FFFFFFFF`.

The correction releases the erase-bank latch when flash access is disabled
**after completion in array mode**. `$F0` or bank selection alone retains the
latch. Disabling during busy or status mode retains it as well. This models the
access boundary required by the original software; it is not a hardware
measurement. An independent replay of the same save and macro read ID `47303032`
(G002) and returned normally to the menu after the correction.

## Natural maintenance results

- Delete a single downloaded game: removed its catalog entry, persisted across
  a fresh core; the complete flash file remained unchanged. This is logical
  deletion, not secure erasure of its executable bytes.
- Delete G001 with G002 in another BOX: G002 remained launchable in a fresh core,
  all eight input counters reached one, and exit returned to the host.
- Move both games into BOX2, then delete G002: corrected core returned to the
  normal menu without invalid mapper accesses. Remaining G001 passed fresh-core
  launch, all eight inputs and exit; the entire flash file stayed unchanged.
- Download modified G001 again while sectors were free: the host appended a
  second copy in a different sector. Same game ID did not cause in-place overwrite.
- Seven natural acquisitions occupied the active sectors. The next acquisition
  exercised the host's erase/copy/program path: G001 was copied byte-identically
  to `$E0000`, new content was programmed at `$E2000`, and the catalog changed
  G001 index `$10` to `$80`. All seven original sectors, hidden data and protection
  metadata stayed unchanged. Both relocated G001 and new `$81` passed fresh-core
  launch/input/exit. The old core had skipped the copy and left G001's index stale.
- The following acquisition reused occupied sector 0. G002 was copied exactly
  to `$00000`, new content written at `$02000`, and the rest of sector 0 erased
  to `$FF`. Every other sector, hidden data and protection metadata was preserved.
  Relocated G002 and new `$11` passed fresh-core launch, all eight inputs and exit.

Each successful acquisition checked both authenticated catalog/download response
bodies against their independently supplied hashes and compared full payloads.
Some fixed macros instead encountered story/options screens and made no HTTP
request; those runs are excluded from acquisition results. The eight-game story
was acknowledged normally before subsequent fresh-core navigation.

No user-visible whole-chip/BOX format command was found in the inspected menus.
The held-Select list-entry shortcut rebuilds the catalog from retained flash
headers; it must not be described as formatting or erasing the chip.
The diagnostic game exits immediately while Start+Select is still held. An
instruction trace confirmed `$FF96=0C` at `20:4010`, `20:401D` and ROM0 `$3E00`
after exit: the held Select activates that shortcut, potentially restoring
logically deleted entries. This is an input/host interaction, not autonomous
undeleting by the core.

## Regression coverage and local evidence

`src/gb/test/mbc.c` contains four synthetic erase-latch tests: continuous access,
completed array-mode access cycle, busy cycle, and status cycle. They issue real
window-B erase commands and invoke the scheduled completion callback in a
controlled fixture. Enable the suite with `BUILD_SUITE=ON`; `BUILD_TEST=ON`
selects the separate fuzz harness. All 30 core CTest targets passed.

The MBC6 Test ROM's separate observational M6FL fixture distinguished the cycle:
continuous B reads stayed `FF/FF` on both cores; after disable/re-enable, old B
still read `FF/FF`, corrected B read source `A5/C3`. Opposite-window controls and
new-command reset controls agreed. Both runs passed all 21 fixture checks.
The frozen offline Test ROM also passed six scenarios and 56 checks on this core.
Four legacy Test ROM cases passed: safe WP0/WP1 each `15/0/0/5`; destructive
marker `22/0/0/10`; no-marker `21/0/1/10`, with TD6 deliberately skipped.

Local evidence directories retained on the test host:

- Natural two-game baseline: `/tmp/mgba-netdeget-local-hihkiz03`.
- Old failing move/delete: `/tmp/netdeget-two-games-v2-3ceisrfx/natural-two-same-box-delete`.
  This directory name is historical; its input came from natural downloads,
  unlike the earlier artificial dual fixtures, which are excluded.
- Corrected delete: `/tmp/mgba-maintenance-candidate-delete-z5wlm03w`.
- Independent ID trace: `/tmp/netdeget-candidate-idtrace-nh5yb2_t`.
- Corrected copy/program: `/tmp/mgba-netdeget-local-1hqcsm7h/report.json`.
- Corrected occupied-sector reuse: `/tmp/mgba-netdeget-local-2cw0297h/report.json`.
- Relocated G002 input checks: `/tmp/mgba-maintenance-candidate-neighbor-ril0pyyg/run.log`,
  stages 28/44/46; new game: `/tmp/mgba-maintenance-candidate-neighbor-rq279isg/run.log`, stages 14/30/32.
- Observational Test ROM: `/tmp/mbc6-latch-9ekuuow8/report.json`.
- Offline Test ROM: `/tmp/mbc6-offline-dccldju3/report.json`.

Screenshots, WRAM dumps and public fixture TCP captures accompany these runs.
The original ROM remains outside Git. This is emulator integration evidence for
these maintenance routes and diagnostic games, not proof for arbitrary games or
physical cartridges.
