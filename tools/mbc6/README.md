# Net de Get local integration check

This fixture-only check exercises the original Net de Get ROM through ordinary
joypad input: boot, public test-account login, authenticated HTTP catalog and
Maker download, BOX2 storage, gameplay inputs, exit, and BOX1 relaunch. It then
reopens the downloaded save in a new core without an adapter and checks input
and exit again. It copies input saves into a fresh `/tmp` directory; it never
changes the supplied ROM, SRAM, flash or payload files.

## Requirements

- A Linux shared mGBA build with libmobile enabled, matching this source tree.
- A C compiler and Python 3; the helper binds loopback UDP port 8053.
- The owned original ROM matching the SHA-256 checked by the script.
- A validated empty SRAM fixture and a `0x100101`-byte erased flash fixture.
  These are intentionally outside Git. The SRAM must reach the host menu with
  the fixture's expected settings and empty slot, and retain the host's valid
  SYS1 checksum. Erasing an existing save by hand is not sufficient.
- The Net de Get project's `fixtures/input-tester/input_tester.flash` D800
  payload, SHA-256 `0e42875ef2569905d056f895ab5d6998e4f17709875dd27f13cbd9b20c2158b0`.
- The REON project's disposable BMVJ HTTP harness on loopback port 8088, serving
  the matching 1,014-byte Maker body and 434-byte corrected catalog. See that
  project's `docs/NET_DE_GET.md` and `web/tests/bmvj_local_router.php`. This check
  does not start, deploy, or change the server. It uses only the harness's public
  synthetic account and password, never a personal `mobile_config.bin`.

Run from this repository:

```sh
python3 tools/mbc6/run_netdeget_local.py ROM BUILD EMPTY_SAV EMPTY_FLASH PAYLOAD
```

For another diagnostic payload with the same D800 ABI, G001 ID, 8 KiB block
and menu route, supply its independently verified hashes explicitly:

```sh
python3 tools/mbc6/run_netdeget_local.py ROM BUILD EMPTY_SAV EMPTY_FLASH PAYLOAD \
  --payload-sha256 PAYLOAD_SHA --catalog-sha256 CATALOG_SHA --body-sha256 BODY_SHA
```

The defaults retain the original PAD TEST regression. Overrides change only
the expected byte identities; input, exit, relaunch, fresh-core persistence,
ROM identity and unchanged chip-region checks still apply. The example in C
from the Maker's GBDK compiler integration uses this diagnostic ABI; these
options do not make the macro a generic gameplay test for arbitrary games.

On 2026-10-06 the Maker's first C PAD TEST, compiled with GBDK 4.5.0 and
its own host-compatible startup (without the normal CRT/libraries), passed
this complete natural download/gameplay/fresh-core sequence using mGBA
`358230c82`. The payload, HTTP body and catalog identities were respectively:

- 8,192 bytes: `ab49fffb02e1b918d442a876508ed83c75ffc32fbbfa482cb8a3b9e46d70381c`.
- 1,114 bytes: `f46337ae8627482742511c5d508aaa0ac66930de93c9fe2814fd0c00a02324ba`.
- 437 bytes: `d5323f206466632a447ceb68168175af5d8c6de66e441c9a58f7868c65e679e6`.

This demonstrates the compiler/startup/example path. It does not establish
compatibility with the standard GBDK runtime or arbitrary GBDK programs.

The adapter's socket callback redirects requested HTTP port 80 exclusively to
`127.0.0.1:8088`. DNS answers are exclusively loopback. The helper closes its
DNS socket after the run; stop the disposable REON harness separately.

The command prints both PASS checkpoints and the temporary evidence directory.
It verifies held/released masks, press counters, exact flash payload and unchanged
remaining chip contents, catalog/download response hashes, and fresh-core
persistence. `run.log` records the loaded library's version and commit. PPM
screenshots, WRAM dumps and TCP captures are retained locally in that directory.
These contain only the synthetic test session; do not use this capture driver
with personal credentials or personal save data.

The byte layout at catalog offsets `$04/$05/$06/$0C/$10/$12/$14` was derived from
the local original ROM and confirmed by natural visibility/download execution.
The host moves the game from BOX2 to BOX1 after exit in this fixture. The macro
therefore selects BOX1 item 16 for relaunch. The macro and hashes are deliberately
specific to this fixture; changing payloads or initial save settings requires
updating the assertions and reproducing the route.

This is an emulator/local-server integration check. It does not establish
production deployment, physical cartridge behavior, or arbitrary minigame
compatibility. The C socket capture helpers are adapted from `src/core/mobile.c`
(MPL-2.0); the Mobile Trainer tracing work provided the adapter attachment pattern.
