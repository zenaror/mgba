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

The flash write-protect input does not protect the whole chip: Iceboy documents
that it blocks programming and erase of sector 0 and the hidden map region,
while sectors 1-7 remain writable and erasable. The emulator's write-enable
checks intentionally preserve that behavior.
