# MBC6 Flash protocol notes

The flash command implementation follows the more detailed buffer behavior
documented by Iceboy. Pan Docs describes programming as writing 128 aligned
bytes and then writing the final address again, while Iceboy describes a
128-byte buffer that may receive up to 128 bytes in any order. During fill,
A6-A0 select the buffer slot and higher address lines are ignored. Writing the
most recently written slot again consecutively triggers programming; the
trigger write's A19-A7 select the destination. Rewrites of older slots update
their buffered data without triggering unless that slot is currently the most
recent one. In either mode, F0 on that repeated slot aborts instead of
triggering; F0 written to a fresh slot is buffered as data.

Sources:

- [Pan Docs: MBC6](https://gbdev.io/pandocs/MBC6.html) — command summary and
  128-byte program sequence.
- [Iceboy: Nintendo Power Game Boy Memory cartridge documentation](https://iceboy.a-singer.de/doc/np_gb_memory.html)
  — Flash commands, hidden-map sequence, status bits, and detailed program
  buffer transfer behavior (sections “Flash commands” and “Flash program
  operations”). Iceboy notes its hardware documentation is based on observed
  cartridges and may contain errors.
- [Dan Docs: Net de Get, MBC6 Flash Operation](https://shonumi.github.io/dandocs.html#ndg)
  — game-specific status and remembered-bank behavior after sector erase.

## Busy status and address mapping

Iceboy says status can be read during and after a program or erase operation,
regardless of the address read from the flash chip. The emulated bus can reach
the chip only through a ROM/flash window currently mapped to flash, so while
busy, any such mapped window returns status; a window mapped to ROM continues
to return ROM.

Dan Docs' Net de Get analysis is more specific about sector erase: after the
game issues F0, the erase-selected bank remains latched in the operation window
until another flash opcode is accepted; the other 8 KiB window continues to use
its current array bank. Accordingly, F0 does not clear the remembered bank.
Accepting the next opcode releases that exception and makes the new chip mode
visible through both mapped flash windows. A focused differential harness
reproduces this transition: after erase/F0/hidden-map entry, the old core reads
the hidden byte as `66/FF` through A/B, while the updated core reads `66/66`.
The MBC6 Test ROM's TD9 run used a controlled fixture with only hidden-map byte
5 set to `66`; all other flash and hidden-map bytes were `FF`. TD9 erased sector
7 via flash bank 112 in window A, ran the game's F0 reset, entered hidden-map
mode, then XOR-read all 256 hidden bytes through each window. Both windows
returned checksum `99`;
an array read would return `00` for this fixture. This directly validates this
post-erase/F0/77 transition for the sentinel fixture, while the XOR checksums do
not prove arbitrary byte-for-byte equality or hardware behavior. This game-
specific observation qualifies Iceboy's general “any address” description and
adds a window detail not covered by Pan Docs' brief status summary.

The cross-window array exception is specific to sector erase. After a buffered
program or chip erase completes, status remains visible from both flash windows
until F0 or another opcode. A disposable-core harness observed program polling
as `00/00`, then ready status `80/80` with the programmed byte changed to `66`;
chip-erase ready status was also `80/80`. The pre-fix core returned `80/FF` in
the other window for both operations. The implementation now applies the
remembered-array exception only when the completed operation was sector erase,
matching the narrower Net de Get observation while preserving Iceboy's
operation-wide status behavior for other operations. These timings are emulator
fixture results, not exact hardware timing claims.

The MBC6 Test ROM's disposable TD10–TD12 fixture also passed against the
updated core: JEDEC ID reads after commands in either window returned `C2/81`
through both windows; buffered-program and chip-erase status snapshots were
`00/00` while busy and `80/80` when ready; and the first/last buffer slots in
the first/last 8 KiB bank pages retained their distinct `A5/5A` sentinels. The
runner reported 21 pass, 0 fail, 1 skip, 10 informational results and a valid
checksum. The ROM and `.sav.flash` were disposable fixtures. A separate core
harness saved/restored a one-slot, not-yet-triggered program buffer, then added
a second slot and triggered programming; both bytes (`66/33`) were programmed
and both windows returned ready status (`80/80`). This exercises emulator
state restoration only; no hardware behavior is inferred.

Additional disposable-core cases cover buffer slots 0 and 127, changing the
selected flash bank between buffer fill and trigger, and a fill that crosses a
128-byte page. All four cases passed. A separate sweep read both endpoints of
all 128 8 KiB selectors with A and B mapped independently and found zero
mismatches. These tests exercise emulator address mapping and buffering; they
do not establish physical-chip electrical behavior.

## Persistence and savestates

The host save is split into SRAM in `.sav` and flash-chip contents in
`.sav.flash`. The flash sidecar includes the 1 MiB array, the 256-byte hidden
map, and the protection metadata byte. An emulator harness loaded a synthetic
legacy combined `.sav`, synchronized it, confirmed that mGBA split it into an
SRAM-only `.sav` and a `0x100101`-byte `.sav.flash`, then reopened both through
a fresh core and recovered the written SRAM and flash sentinels. A separate
raw-sidecar fixture confirmed that a short flash image is expanded with erased
(`FF`) bytes while preserving the SRAM section.

Savestates retain volatile flash command/buffer/operation state; the flash
array itself remains in savedata. Disposable-core checks restored a pending
program, a partially filled program buffer, a pending sector erase, and a
pending chip erase, hidden-map erase, and sector-0 protection operation. Each
resumed operation completed with the expected data and status; after the
protection operation, the ready-status protected bit was set (`82`). These
checks validate mGBA state serialization with fixture contents, not hardware
behavior.

Reset behavior also passed a focused array-mode harness. `$F0` exited JEDEC ID
and hidden-map reads when written through either mapped window, after which the
independently selected flash banks returned their original `5A/A5` array
sentinels. A separate pending-program case wrote `$F0` through the opposite
window while busy; the operation remained busy and completed normally. This
matches Iceboy's description that `$F0` exits command/status modes and has no
effect while an operation is still in progress.

The flash write-protect input does not protect the whole chip: Iceboy documents
that it blocks programming and erase of sector 0 and the hidden map region,
while sectors 1-7 remain writable and erasable. The emulator's write-enable
checks intentionally preserve that behavior.

## Net de Get runtime checks (2026-10-06)

A fresh Linux Qt/SDL/shared-library build of commit `4c8066be4` passed the
complete 33-target CTest suite with `QT_QPA_PLATFORM=offscreen` (including
`gb-mbc`). The initial run without a display failed to initialize two Qt
tests; both passed with the offscreen backend. The default MBC6 Test ROM
reported 15 PASS, 0 FAIL,
0 SKIP and 5 INFO; the disposable destructive fixture with the TD6 marker
reported 22 PASS, 0 FAIL, 0 SKIP and 10 INFO. These are emulator results.

The original Net de Get writer also completed four 4 KiB fixture writes:
selector 0 at `$4000`, selector 1 at `$5000`, and selector 127 at both
`$4000` and `$5000`. Each completed 32 program operations, matched all target
bytes in memory and on disk, and matched after reopening a fresh core.
Bytes outside the target, including hidden storage, remained unchanged.
Those tests enter the writer with synthetic WRAM input; they do not prove
the preceding network acquisition path.

The independent Net de Get reconstruction project provides an original
PAD TEST homebrew fixture and `fixtures/input-tester/run_natural.sh`.
With an isolated SRAM catalog and flash sidecar, joypad input alone opened
the fixture from BOX2. All eight held/released masks and press counters
passed, and the complete flash sidecar remained byte-identical. Start+Select
returned to the host; the host moved the catalog entry to BOX1, where the
fixture reopened both in the same core and after reopening the save.
The tested 8 KiB payload SHA-256 is
`0e42875ef2569905d056f895ab5d6998e4f17709875dd27f13cbd9b20c2158b0`.

Two fixture errors were exposed by natural execution: the header must include
the entry offset word at bytes 3–4, and minigame state must not overwrite host
WRAM at `$C700`. The corrected fixture uses bank 1 `$D800`. A repeated sample
at the host's VBlank wait PC does not establish a freeze; input and the actual
catalog location must be checked.

Natural execution with a disposable Mobile GB Adapter configuration reached
the local REON HTTP menu and authenticated catalog. Test instrumentation
resolves DNS locally and remaps port 80 to loopback port 8088. No personal
adapter configuration is used. The SDK performs GET followed by an empty
authenticated POST to the same `RomList.cgb` URL. A GET-only restriction in
the local server harness caused error `32-404`; the server project corrected
the harness to use its existing handler for POST.

The natural integration run then exposed a catalog encoder error. The original
ROM reads blocks at record offset `$04`, category at `$05`, ID at `$06`,
levels at `$0C`, hidden requirements at `$10`/`$12`, and title length at `$14`.
Four leading reserved bytes were missing from the server's custom record.
After the server project corrected these offsets, PAD TEST appeared naturally.
The tested GET and POST catalog bodies were identical (434 bytes, SHA-256
`8f072d41c39146380053abe0281835b4a639511a523cd5963bc67ef222aec043`).

The original ROM downloaded the Maker mode-5 body (1,014 bytes, SHA-256
`a8f6e181ddedf0f5d0b1b8e164d9e41edcddaadd14cf0c9f4730ede455560a24`).
Starting from an erased flash fixture, it wrote the payload only after BOX2
was selected, then confirmed storage completion. The resulting first 8 KiB
matched the expected payload byte-for-byte; the remaining array, hidden map
and protection metadata were unchanged. The complete sidecar SHA-256 was
`cf6d32c72339ee34bc0029142a7cf0a8e7b35935544302a779a0a52c10bc511b`.
Joypad input alone then launched the downloaded game from BOX2, validated all
eight controls, exited, and reopened it from BOX1 with counters reset. A new
core reopened the resulting save, launched the game, accepted input and exited
without changing the flash sidecar. No CPU registers or PC were redirected in
this acquisition/launch sequence. This validates the complete local fixture
path; it does not establish production REON deployment or arbitrary payload
compatibility.

### Production REON acquisition (2026-10-06)

The original Net de Get ROM also completed the C PAD TEST route against the
production REON service using a dedicated temporary opt-in account and disposable
SRAM/flash. The test used real DNS on port 53 and the normal mGBA socket callbacks;
there was no loopback DNS or HTTP redirection. The runtime remained
`358230c82773aec67dff2995eb77fbd84f8ea8a2` (the Linux test release).

Joypad input alone entered the account password, downloaded the game, selected
BOX2, launched it, exercised all eight held/released inputs exactly once, exited,
and relaunched it from BOX1 with cleared counters. The resulting first 8 KiB
matched the C PAD payload SHA-256
`ab49fffb02e1b918d442a876508ed83c75ffc32fbbfa482cb8a3b9e46d70381c`;
the remaining array, hidden map and protection bytes were unchanged. A fresh
core reopened the save, launched the game, accepted input and exited with the
flash unchanged. Both assertions reported PASS.

The server owner independently confirmed two successful authenticated catalog
responses and two successful game-body responses in the production logs, with
no PHP errors. The server's separate byte checks matched the previously tested
437-byte catalog and 1,114-byte C PAD body. The emulator run did not capture raw
HTTP traffic or WRAM; credentials were provided only to the temporary process.
Local assertion logs are `/tmp/mgba-netdeget-production-zqeg16la/run.log` and
`reopened/run.log`; the gameplay image is `c-pad-controls.png` in that directory.

The Maker sources and guides are published on `codex/maker-guides-gbdk` at
`zenaror/net-de-get-maker`, commit `727b06b607adea93e5fbe10b6890b45739be1070`.
Its original additions have an MIT license. This production run validates this
free C PAD content and the tested host/compiler path; broader host APIs, ordinary
GBDK libraries and arbitrary minigames require their own checks.

### Natural flash maintenance (2026-10-06)

Additional original-ROM tests found a completed erase latch surviving a flash
access disable/re-enable cycle. After a natural move/delete, this redirected
the selected game's ID read to the erased spare sector and caused a white screen.
The correction releases the latch only when access is disabled after completion
in array mode; continuous access, busy and status modes retain prior behavior.

[Maintenance evidence](../tools/mbc6/MAINTENANCE.md) records the independent
first-bad-read trace, four core regression cases, natural deletion with neighbor
preservation, and occupied-sector erase/copy/program checks. Relocated games and
new content passed fresh-core gameplay/input/exit. Re-downloading the same ID
with free space appended a copy; physical reuse was exercised separately after
seven natural acquisitions. No whole-chip/BOX format menu was confirmed.

### ROM bank zero and Maker exit compatibility

MBC6 permits ROM bank zero in either 8 KiB window. The existing ROM-overflow
wrap policy previously left the bank pointer at zero but recorded bank one.
Switching ROM to flash and back then selected the wrong ROM data. The correction
keeps the recorded bank consistent with the pointer for MBC6; other mappers and
invalid-bank warnings retain their existing behavior. This does not establish
the electrical meaning of selector bit 7. The same regression executable passes
13/13 mapper cases with the fix, versus 12/13 with `61f28d126`; all 30 CTest
targets pass with the corrected core.

The Maker release-wait examples (C PAD, assembly PAD and C REACTION) were each
downloaded by the original Net de Get through the authenticated local REON
fixture, then stored in BOX2. Each complete 8 KiB payload matched its sealed
Maker artifact and every byte outside the payload remained unchanged. Fresh
cores launched the downloaded saves using only joypad inputs. Both PAD examples
passed all eight controls. REACTION passed WAIT, early response, GO, scored
response and reset states. Start+Select followed by holding Select kept control
in the minigame; releasing both returned to the host with the BOX2 catalog and
flash unchanged. These are three tested examples, not proof for arbitrary games.

REON does not implement billing. A game's price remains historical metadata;
charging is not a required compatibility test. Rafael confirmed the admin panel
test passed. Missing original downloadable minigames do not block validation of
the emulator and the available Maker examples.
