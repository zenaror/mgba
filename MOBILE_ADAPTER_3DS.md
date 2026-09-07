# Mobile Adapter GB on the 3DS

Notes from porting mGBA's Mobile Adapter GB support to the Nintendo 3DS, on
branch `feature/3ds-magb`. Written mostly for whoever picks this up next,
including the bugs that cost the most time and why they hid for so long.

Status: **working on hardware.** A game reaches the DNS server, resolves a
name, and connects. Tested on a New 3DS XL against a local REON server.

## What this adds

- Mobile Adapter GB support in the 3DS build, which upstream disables.
- A native configuration screen, since the Qt dialog obviously does not exist
  there: enable, adapter type, unmetered, DNS 1 and 2, P2P port, relay server,
  relay token, mail port redirection, and a DNS test.
- The library's own chatter on the bottom screen while a game runs.
- Reaching that screen before a game is loaded, on the 3DS and on the desktop.
- The vendored libmobile swapped for the `feature/custom-mail-port` fork.

## Two build gates, not one

Getting the adapter compiled into the 3DS binary needed two unrelated things
lifted, and finding the second one took a while because the first appeared to
be the whole story:

1. `CMakeLists.txt` turns `USE_LIBMOBILE` off for every embedded platform.
2. `MINIMAL_CORE` — set for any toolchain reporting a `Generic` system, which
   devkitARM does — drops the whole `SIO_FILES` list, so `gb/sio/mobile.c` and
   `gba/sio/mobile.c` are never compiled even with the flag on.

Both are lifted for the 3DS only. Turning `MINIMAL_CORE` off wholesale is not
worth it: it also pulls in the scripting socket bindings, which do not build
against newlib.

## How it hangs together

The core already drives the adapter: `GBSIOMobileAdapterUpdate()` runs at the
end of every video frame from `gb.c` and `gba.c`. The frontend never has to
pump `mobile_loop()` itself, so the port is an attach plus a screen.

`struct mGUIMobileAdapter` hangs off `mGUIRunner`. Creating and freeing it maps
to a game being loaded and unloaded, not to the config screen opening and
closing — the screen can be closed with the adapter left plugged in, which
matters on a console where any menu takes over the screen and pauses
emulation.

Wanting the adapter is remembered in `runner->mobileEnabled` for as long as the
emulator runs and never reaches disk, so it always starts off. The adapter's
own settings persist to `mobile_config.bin` as before.

With no game there is no serial port to attach to, so that screen builds an
adapter that is never started, purely to read and edit the stored config.
Starting one calls into emulation timing that does not exist yet.

### Where the config lives

`sdmc:/mGBA/mobile_config.bin`, from `mCoreConfigDirectory()`, next to
`config.ini`. The directory is created by that same call. There is no portable
mode on the 3DS — `mCoreConfigPortableIniPath()` returns nothing there, so a
`portable.ini` beside the executable does nothing.

The blob is the library's own 0x200 bytes and is identical across platforms, so
a desktop config can be copied straight over. The one caveat is IPv6: the 3DS
has none, and the config screen rejects it there.

## Bugs found

Roughly in the order they were hit. The last one is the interesting one.

### Uninitialised struct members (pre-existing, desktop)

`GBSIOMobileAdapter` embedded in `CoreController` was read before being
initialised. Fixed by zeroing it in the constructor.

### No network service on the 3DS

Nothing ever called `SocketSubsystemInit()`, so the console's socket service
was never started and no socket could have worked. This produces no compile
error and no runtime message. It is now started when an adapter is plugged in
rather than at boot, so the 1MB service buffer costs nothing to anyone not
using it.

### Uninitialised error paths without IPv6

`SocketOpen()` and `SocketConnect()` left their error variable unset when
handed an IPv6 address on a platform built without IPv6, which the 3DS is.
Reachable in practice by copying a desktop `mobile_config.bin` holding an IPv6
DNS server.

### Menus drawn through a null core

The menu backdrop is the running game's last frame, drawn straight from the
core. Safe while menus only existed during a game; opening the adapter screen
from the start menu took a data abort on hardware. `_drawBackground()` now
checks for a core. The start menu itself survived only because it passes no
background at all.

### Three ways the socket layer was not 3DS-shaped

All three confirmed against devkitPro's own socket example, which is worth
reading before touching this:

- `bind()` will not take the unspecified address. Already worked around where
  no bind address is given, but the adapter always gives one with the address
  left zero, which took the other branch. Outgoing sockets are no longer bound
  at all — sending names them anyway.
- `socket()` takes only the default protocol for the type. `IPPROTO_UDP` is
  rejected with `EINVAL`. The same workaround already existed for the Wii, one
  line above.
- `SO_REUSEADDR` is not accepted, and mGBA refused to open the socket when
  setting it failed. It is an optimization; it is skipped there now.

### Polling a socket that answers honestly anyway

`sock_recv()` asked `select()` whether the socket was readable, with an error
set alongside, before reading. On a non-blocking socket that is redundant — the
read already reports "nothing yet" — and it is exactly the sort of thing a
limited service gets wrong. It now reads directly.

Also in that path: the source address was decoded from a stack buffer that was
never initialised, and decoded even when the read had failed and written
nothing into it.

### Comparing struct padding across an ABI difference

This is the one that mattered, and it hid behind every symptom above.

A DNS reply is only accepted if it came from the server that was asked, and
`mobile_addr_compare()` compared the two addresses with `memcmp` over the whole
struct:

```c
struct mobile_addr4 {
    enum mobile_addrtype type;
    unsigned port;
    unsigned char host[4];
};
```

ARM stores an enum in one byte, so `type` is followed by three bytes of padding
before `port`. Nothing that fills an address in ever writes them. One side came
from a zeroed local, the other from config memory carrying whatever was there.
Two addresses agreeing in every field did not match, the reply was discarded
without a word, and the lookup timed out into a generic error.

On x86 the enum is four bytes wide and there is no padding, which is why the
desktop build was never affected and why this only appeared on hardware.

Fixed by comparing `port` and `host` directly. This is in vendored libmobile
and is not 3DS-specific — any ARM or otherwise short-enum target has it, so
Wii, Switch and Vita would hit it too.

## What made the difference when debugging

The failure looked like a network problem for a long time and was not one. Two
things ended the guessing:

- **The DNS server's own log.** `dnsmasq` showed the query arriving and being
  answered every time, which ruled out sending and the server long before the
  code did.
- **Instrumenting each layer until the rejection spoke.** A probe inside the
  emulator did a full DNS exchange successfully while the adapter, on the same
  console against the same server, failed. That contrast is what narrowed it to
  the library's own validation, and a single log line in `dns.c` named it.

Three hypotheses were wrong along the way — the errno mapping, the emulated
clock, and buffer corruption — each eliminated by measurement rather than
argument. The clock one is worth recording as a near miss: it is a real class
of bug in this library, and two independent reviewers endorsed it, but the
arithmetic did not support it here (`3000 * 8388608 / 1000` is exactly three
emulated seconds, and the console is frame-limited).

## Building

```
docker run --rm -v "$PWD":/home/mgba/src mgba/3ds
```

Produces `mgba.3dsx` and `mgba.cia`. The image carries devkitARM and CMake
3.31, comfortably past the 3.25 that libmobile asks for.

## Still open

- Only the GB path has been exercised on hardware. The GBA link port attach is
  written and compiles but is untested.
- The socket callbacks currently log every open, send and receive, which is
  noisy in normal play. That instrumentation is worth trimming now that it has
  done its job.
- The on-screen log keeps 48 lines but only shows what fits. A scrollable view
  of the rest would help, since opening any menu hides the live log.
- Nothing here is upstreamable as-is, but the socket fixes and the address
  comparison are bugs in their own right and worth reporting.
