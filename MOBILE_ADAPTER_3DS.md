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
  relay token and mail port redirection.
- The library's own chatter on the bottom screen while a game runs, off
  until asked for.
- The vendored libmobile swapped for the `full_server` fork, and the relay
  reporting that comes with it.

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

Anything the library writes into that config while a game runs reaches the card
within a frame, not when the adapter is put away. It has to: the library
reserves its relay-report counters in batches and hands the new ceiling over at
once, and a console is switched off mid-game far more often than it is shut
down cleanly. A ceiling that only reached the card when the adapter screen
closed was lost with the power, and the next session then replayed counters the
server had already accepted — which it refuses, so mail to the outside came back
with a bare 554 while every report claimed success. Found the hard way, from
the server's signatures matching the previous day's byte for byte. The per-frame
poll that does this writing (and puts relay reports in the log as they happen)
had been defined without ever being called; both were dead until this.

Those counters are kept per device on the server, and the library names the
device from whatever bytes the frontend hands it — never from the config, since
a name stored there would travel with a copied file and tell two consoles apart
by which one copied it. The console answers with its radio's MAC address
(`SOCU_GetNetworkOpt` with `NETOPT_MAC_ADDRESS`; `sceNetGetMacAddress` on the
Vita), which survives the config being wiped or downloaded again; the desktop
core answers with the machine id, or the host and user name failing that. The
library hashes it together with the frontend's name — `"mgba"`, a literal
agreed with the other implementations, since libmobile-bgb on the same PC
would otherwise read the same machine id and become the same device — and
only the hash is ever sent, as the `device` field of the report. With no callback there is no field, and the server files the report
under the account's one unnamed device, which is where every build before this
one sat.

A device that has lost its counter anyway — a config restored from a backup,
say — no longer has to be power-cycled until its batches overtake the server.
Once per session, before anything is authorized, the library asks the server
where the counter stands, through the same side channel: a query carries no
counter, and the reply (`<counter> <signature>`) goes back to the library byte
for byte, which verifies the signature and only ever moves its counter forward.
The query is asked as an HTTP/1.0 client so that the body can never arrive
chunked. It consumes a counter of its own, which the server echoes inside the
signed reply, so an old reply cannot be replayed at a device whose counter
never moves. The adapter screen shows it as "counter asked", then "counter
answered" or "counter unanswered"; an unanswered query costs only the
recovery, the counter still advances on its own.

The same reply is how the site tells a device it has been blocked: a signed
"blocked" verdict rather than a bare 403, which any DNS on the way could have
forged. The library then fails the session's network on purpose, so the game
shows its own error screen, and asks again next session — nothing about it is
ever persisted. The frontend only says so: "Blocked on the site" replaces the
status on the adapter screen and above the log, and only on a verified verdict,
never on a mere lack of answer. A report that was already on its way when the
verdict arrived is refused by the server; its reply is read for that, so the
log says "refused, device blocked" rather than blaming the network.

The screen opens only while a game runs, from the pause menu, on every port
alike. There was for a while a menu ahead of the file browser that led to it,
backed by an adapter that was never started; it went, on the user's decision,
so that the desktop, the 3DS and the Vita all behave the same way — the Vita
only has a menu once a game is running. Switching the adapter on is still
remembered until the emulator closes, so the next game loaded gets it plugged
in; at worst a game has to be reset after the adapter is switched on.

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
core. Safe while menus only exist during a game, which is again the case;
while a start menu led to the adapter screen with no game loaded, opening it
took a data abort on hardware. `_drawBackground()` checks for a core anyway.

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
Wii, Switch and Vita would hit it too, as would anything else running that
library on ARM.

**This fix lives only here, and every update to the library has silently
removed it.** It has been carried across twice already. Nothing about losing it
is loud: the build stays clean and every lookup simply fails, exactly as it did
before it was found. Whoever next pulls that library should check
`mobile_addr_compare()` first, and it would be better placed upstream than
guarded by whoever remembers.

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

Each image carries its own recipe as its default command — the toolchain file
it needs, and whether to build shared or static. Run it with nothing after the
image name and let it do that. Writing the `cmake` line by hand instead works
by luck on some targets and fails confusingly on others: doing it for Windows
produced a build that went looking for a `libmgba.so`, because the recipe that
was skipped is where `BUILD_SHARED=OFF` lives.

```sh
docker run --rm -v "$PWD":/home/mgba/src mgba/3ds            # mgba.3dsx, mgba.cia
docker run --rm -v "$PWD":/home/mgba/src mgba/ubuntu:noble   # mgba-qt, mgba, libs
docker run --rm -v "$PWD":/home/mgba/src \
  -e CMAKE_FLAGS="-DCMAKE_POLICY_VERSION_MINIMUM=3.5" mgba/windows:w64
```

Each writes its own `build-*` directory, and `$CMAKE_FLAGS` is passed through
to the configure step.

The flag Windows needs is not about anything here: that image ships CMake
4.2.3, which dropped support for `cmake_minimum_required` below 3.5, and the
vendored zlib still asks for 2.4.4. Without it the configure step fails before
compiling a line. The Linux image is on 3.28 and does not care; the 3DS image
is on 3.31, comfortably past the 3.25 libmobile itself asks for.

Windows links statically, so the executable needs nothing beside it, and comes
out around 100MB until `x86_64-w64-mingw32-strip` takes it down to about 40.

## Telling a relay about mail sessions

Not 3DS-specific: this lives in `src/core/mobile-auth.c` and works wherever the
adapter does.

The library signs a note whenever a game starts or stops using mail, for a
relay that wants telling out of band, and hands over the address to send it to
along with it. Carrying it there is all the frontend does: a report is queued
as it arrives and posted from the per-frame update, a step at a time — connect,
send, read the answer to its end — so nothing waits on a socket.

Reading the answer is not politeness. Hanging up as soon as the request is out
looks to a server like a client that gave up, and shows up there as a 499 or a
reset. Both other implementations of this hit that and fixed it the same way.

Finding the server is the library's business, not this file's. It resolves the
name against the same DNS a game's own lookups go through, on whatever port
that is configured with, and there is deliberately no way to point it elsewhere
from here. An earlier version of this did its own lookup; that is gone.

A failed report is dropped rather than retried: nothing in the emulated session
depends on it, and the game reopening mail produces another.

The socket used for this is its own. Never reach for one of the connection
slots the library hands out, even one that looks free — it may be about to
belong to a game.

Two things worth knowing before calling it broken:

- **Nothing is reported until an account has a key.** The key arrives through
  the library's own POP3 bootstrap, so a fresh account has to log in once with
  a real user and password before any report is ever sent. Until then this is
  silent by design, not failing.
- **Quitting without the game ending its session skips the closing report.**
  The server's own timeout covers that case; nothing here tries to catch it.
  The library says so in the log when it happens, which is the only trace that
  will ever exist here: the adapter is freed straight after it is stopped, so
  the queued report goes with it and no later start can pick it up.

This has been exercised end to end from the console: mail sent from a game,
through the relay, to an address outside it. Worth recording what that took,
because the failure it replaced was silent in the same way the DNS one was. A
report is raised while a game is connected to a mail port, and the library
would only send one while no session was in progress — so the opening report
could never be sent at all, and the closing one overwrote it. A server saw
only hangups, and refused to relay for a device it had never been told about.
Nothing in the emulated session could show this: the game got a plain refusal
from the mail server, with nothing to say why. It was found by noticing that
the two kinds of report failed differently, which rules out the channel they
share and leaves only the ordering.

## Tracing the sockets

The bottom screen stays clear while playing. **Log on bottom screen** puts the
library's own account of a session there, which is the first thing to reach for
when one goes wrong. When that is not enough — a session failing for reasons
the library's account does not explain — **Trace sockets**
on the adapter screen turns on a line per open, per send and per read. Turning
it on also reports the console's own address and whether a socket can be
created at all, so the state of the network is on screen straight away:

```text
<mGBA> console is 192.168.10.120
<mGBA> network ready
<mGBA> conn 0 open udp ok, port 0
<mGBA> conn 0 sent 42 to 192.168.10.80:53
<mGBA> conn 0 got 58 from 192.168.10.80:53
```

That is what those lines are for, roughly in the order they answer questions:

- `console is 0.0.0.0` — the console never joined a network, and everything
  after it will fail as a consequence rather than a cause.
- `no network: socket() failed` or `bind failed` with an errno — the service
  refused something before any traffic was attempted.
- `open ... failed` — the adapter could not get a socket for a connection,
  which the game only ever sees as a generic error.
- `sent` without a matching `got` — the request left and nothing came back.
  Worth checking against the server's own log before suspecting the console.
- `got` from an address, and a failure anyway — the reply arrived and something
  above the socket rejected it. This is what the padding bug looked like.

It is a runtime switch rather than a build option deliberately: rebuilding to
answer a question is cheap on a desktop, where there is a log file anyway, and
expensive when the thing misbehaving is a console across the room. Like
enabling the adapter, it lasts only as long as the emulator runs.

The callbacks it narrates through stand in for the core's own permanently, not
only while tracing, so they have to stay a faithful copy of `sock_open`,
`sock_send` and `sock_recv` in `src/core/mobile.c`. Changing those without
changing these would be a quiet way to break the console builds only.

## Still open

- Only the Game Boy core has been run against a game on a console. The attach
  for the GBA core is written and compiles, but no GBA title has been played
  through it, so nothing about that path is known to work beyond building.
- The on-screen log keeps 48 lines and shows only what fits, and opening any
  menu hides it. Scrolling the rest was considered and dropped: what matters
  arrives at the end, which is the part that stays on screen.
- Receiving does not implement the contract's "is this connection still
  alive" case, where the library passes no buffer and expects to be told
  whether the remote has gone. Nothing in the library asks for it today, so
  this costs nothing now; it would need a peeking read, which the socket layer
  does not offer and which every platform spells differently. Answering "alive"
  unconditionally would be worse than not answering, since it would hide a
  disconnect that had really happened.
- Loading a config file over a running adapter does not replace a device-auth
  key that is already in memory. The library refuses to reload one on purpose,
  since re-reading storage that a write has not reached yet would roll its
  replay counter backwards. Working around that from here would defeat what the
  refusal is for, so it stands; everything else in the file imports normally.
- Nothing here is upstreamable as-is, but the socket fixes are bugs in their
  own right and worth reporting. The address comparison no longer belongs on
  this list: it now lives in the library, so the copy here carries no local
  patch at all.
