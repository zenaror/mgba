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
game issues F0, status and the erase-selected bank remain active in the
operation window until another flash command is issued; the other 8 KiB window
continues using its current bank. Accordingly, F0 does not clear the remembered
bank; accepting the next flash opcode does. This game-specific observation
qualifies Iceboy's general “any address” description and adds a window detail
not covered by Pan Docs' brief status summary. The cross-window status policy
after completion remains open pending ROM/test-ROM evidence.

The flash write-protect input does not protect the whole chip: Iceboy documents
that it blocks programming and erase of sector 0 and the hidden map region,
while sectors 1-7 remain writable and erasable. The emulator's write-enable
checks intentionally preserve that behavior.
