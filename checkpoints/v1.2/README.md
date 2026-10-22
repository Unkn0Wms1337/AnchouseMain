# anchousechanger

**Version:** 1.2 (2026-10-22)

Open-source internal Dota 2 skin changer for Windows.

![Arcana with gems](assets/arcana.jpg)

![Unusual effect](assets/unusual.jpg)

## What's new in 1.2

- **Saved loadouts.** Equipped fake items, their styles, socket gems and
  unusual effects are written to `anchousechanger.sets` next to the DLL and
  restored on the next injection through `loader.exe`.
- **Early injection.** `loader.exe --launch` waits for the required game
  modules and injects before the client's first SOCache subscription, so the
  complete catalog replaces that subscription instead of arriving too late.
- **Auto-grant only through the loader.** Starting Dota 2 from Steam or
  injecting the DLL with another tool leaves the real inventory untouched.
- **No add button.** The catalog is granted automatically; the `+` button was
  removed.
- **No received-items popup.** The modal new-items dialog is hidden
  automatically and no longer blocks the dashboard.

See [CHANGELOG.md](CHANGELOG.md) for the full 1.1 → 1.2 release notes.

## Features

- Any item added to the inventory and equipped from the hero loadout
- Styles unlocked, sockets, prismatic and ethereal gems
- Per-item unusual effect picker
- Selected sets, styles, gems and effects survive a game restart when the
  next launch goes through `loader.exe`
- Items, arcana models and effects applied in matches
- Automatic full catalog grant only when the DLL is injected through
  `loader.exe`; there is no separate add button
- Menu launch modes: **Launch with mod** injects through `loader.exe`;
  **Without mod** starts Dota 2 through Steam and keeps the real inventory

## Files

All runtime files are created next to the DLL (`build/` in this repository):

| File | Purpose |
|---|---|
| `anchousechanger.dll` | the mod, injected into `dota2.exe` |
| `loader.exe` | waits for the game and injects the DLL |
| `anchousechanger.cfg` | optional; grant tuning only, no skin data |
| `anchousechanger.sets` | saved selection: equipped fakes, styles, gems, effects |
| `anchousechanger.log` | diagnostic log |

Deleting `anchousechanger.sets` resets the saved loadout; deleting
`anchousechanger.cfg` resets the grant tuning.

## Building

Requirements:

- Visual Studio 2022 or newer with the **Desktop development with C++** workload
- CMake
- Internet access on the first build

Run `build.bat`. It downloads and builds protobuf, fetches the Dota 2 protobuf
schemas, generates the message sources, and produces:

- `build/anchousechanger.dll`
- `build/loader.exe`

The first build takes several minutes because protobuf is compiled from source.
Later builds reuse `.deps/` and are much faster.

Useful commands:

```bat
build.bat            :: build DLL and loader
build.bat rebuild    :: regenerate protobufs and rebuild
build.bat clean      :: remove build output only
build.bat distclean  :: remove build, generated protos and .deps
build.bat nopause    :: build without the final pause
```

The Dota 2 protobuf revision is pinned in `build.bat` (`PROTO_SHA`). If a game
update changes the GC schema, update that hash and run `build.bat rebuild`.

## Usage

1. Close Dota 2.
2. Run `build\loader.exe --launch` to start the game through Steam and inject
   `anchousechanger.dll` from the loader's own directory. The loader waits for
   the required game modules and injects before the client's first item-cache
   subscription. Injecting into an already-running client (for example, after
   the main menu has loaded) may be too late for the complete catalog to
   replace that subscription.

Loader options:

```text
loader.exe [options] [dll_path]

--launch, -l      Start Dota 2 through Steam if it is not running
                 (close Dota first; injection happens before the first SOCache subscription)
--pid <pid>, -p   Inject into a specific process
--dll <path>, -d  Use a custom DLL path
--help, -h        Show help
```

`loader.exe` and `anchousechanger.dll` must be updated together. The new DLL
requires a marker written by the new loader; an old loader leaves auto-grant
disabled.

The desktop menu exposes the same choice as two buttons. **Launch with mod**
runs `loader.exe --launch`; **Without mod** opens `steam://rungameid/570`
without injecting anything, so Dota 2 uses the normal inventory. The second
mode does not load the DLL and therefore does not enable auto-grant. It also
does not unload a DLL from an already running game; close Dota 2 first if the
mod is active.

Every equippable cosmetic is added to the inventory automatically when the DLL
is injected by `loader.exe`, so all skins are available in the hero loadout
without pressing anything. Auto-grant is armed by a short-lived marker that the
loader writes next to the DLL before injection; starting Dota 2 from Steam alone
or injecting the DLL with another tool leaves it disabled. The complete catalog
is granted automatically; there is no separate add button.

## Saved loadouts

The complete catalog is rebuilt in memory on every injection and is never
written to disk. Only the items you selected are persisted: equipped fake items
and their hero/global slots, styles, socket gems and unusual effects. They are
stored in the compact binary file `anchousechanger.sets` next to the DLL.

Changes are written shortly after they are made (a 400 ms debounce) and are
flushed again from `DLL_PROCESS_DETACH` when Dota 2 exits. The next loader
injection restores the saved records before the complete cache snapshot is
served, so the saved sets are equipped immediately and no extra requests are
needed.

`anchousechanger.cfg` is optional and contains only the grant tuning keys
below; it does not store skins. On startup the mod removes the legacy
`skin.items` / `skin.unusual` keys from old configs, and if no settings remain
it deletes that file. The loader still removes Dota 2's persisted local SOCache
and fake item acknowledgements after the game process exits, so a later launch
from Steam without the DLL starts from the real inventory instead of the last
fake catalog; the saved `.sets` file is kept for the next modded launch.

## How the catalog is served

After injection the mod asks the game client to re-subscribe its item cache the
same way it does when connecting to the GC, and merges the whole catalog into
that single cache snapshot. The inventory is rebuilt once, with every skin
already inside, so the items look like they were always there and no per-item
stream is sent to the UI.

If the GC first sends an empty cache subscription and immediately follows it
with the complete one, the mod holds the empty message instead of merging the
catalog into it. When the complete subscription arrives, the empty one is
discarded and only the complete snapshot is served with the catalog. If no
complete subscription arrives, the held empty message is replayed after 2
seconds; an indeterminate held message is discarded after 6 seconds. This
prevents a second inventory rebuild and stale item views that could freeze the
game when a skin or persona is equipped.

After a complete snapshot is served, the mod checks whether the client actually
accepted the catalog. If the probe item is still missing four seconds later, the
same complete snapshot is pushed once more with a higher cache version. This
recovery path never falls back to streaming thousands of individual items, so
it cannot reintroduce the multi-minute freeze.

Dota normally opens a modal "new items received" popup after the catalog is
granted. The mod hides that popup automatically (the `PopupItemReceived` /
`ItemReceived` panel), including the "1 из N" / "Accept all" dialog, so it does
not block the dashboard or the hero loadout.

Sizing note: the Dota 2 client spends CPU proportional to the current inventory
size on every item it applies from live GC updates (about 0.7 microseconds per
already-owned item), so feeding ~12.5k items in batches costs the client around
a minute of work no matter how the mod paces them. Delivering them inside one
cache snapshot is the only path that can rebuild the inventory in one pass. If
the GC never sends a complete subscription, the mod waits 12 seconds and then
falls back to a paced delivery: one small GC message per frame, sized so the
window stays responsive. That fallback is not used after a complete snapshot has
already been served.

## Configuration

The behavior is configurable in `anchousechanger.cfg` (add the keys on separate
lines). The file is optional and contains only the grant tuning below; it does
not store skins or saved loadouts.

```ini
skin.grant.snapshot=0  ; never merge the catalog into cache snapshots
skin.grant.probe=0     ; do not ask the client to re-subscribe, only stream
skin.grant.wait=12     ; seconds to wait for a snapshot before streaming
skin.grant.chunk=256   ; items per streamed message (0 = one message, freezes)
skin.autogrant=0       ; disable automatic catalog injection entirely (even via loader)
```

## Diagnostics

`build\anchousechanger.log` gets a `[milliseconds]` prefix on every line, so a
hang can be located by the last lines written. A background watchdog prints a
line every 15 seconds:

```text
[  19657] [+] Watchdog: frames=787 queued=0 delivery=12461/12461 mode=2
```

`mode` is `0` while waiting for a cache snapshot, `1` while streaming and `2`
when the catalog was merged into a snapshot.

Persistence writes its own lines at startup and after every change:

```text
[      0] [+] Persistence: loaded 40 selected item(s) from ...\anchousechanger.sets
[   4157] [+] Persistence: restored 40 of 40 selected item(s)
[   5407] [+] Persistence: saved 40 selected item(s) to ...\anchousechanger.sets
```

The first `AutoGrant:` line shows whether the loader marker was accepted. If it
says `no loader marker`, the DLL was injected by another tool and auto-grant is
disabled; no items are added.

Grant stages appear as:

```text
[   4141] [+] AutoGrant: enabled by loader marker (pid=22804)
[   4157] [+] Subscribe: 40 real items, 0 replaced, 12461 fakes added, 3436 -> 506212 bytes
[   4240] [+] Pump: 113 ms                                  <- one-time cache rebuild
[   6094] [+] Cache probe: fake 0x700000000000811e present
[   6100] [+] UI: hiding received-items popup id="ItemReceived"
```

If the probe stays missing, the recovery line looks like:

```text
[   6950] [+] Grant: cache still empty, pushed complete snapshot again (504134 bytes, version ...)
```

If the GC sends an empty subscription first, the log shows it being held and
then dropped when the complete subscription arrives:

```text
[   2641] [+] Subscribe: empty cache version ... held, waiting for the complete cache
[   2704] [+] Subscribe: staged empty version ... dropped, version ... served
```

When the GC ignores the refresh, the mod keeps probing (a cache check message
to the client at 4 s, a Dota cache-resend message at 6.5 s) and retries the
refresh every 4 seconds. If no snapshot arrives within `skin.grant.wait`
seconds, streaming starts and the log switches to `Grant: streamed` lines:

```text
[   9000] [+] Grant: no snapshot after 9000 ms, streaming 12461 items by 256
[   9010] [+] Grant: streamed 256/12461 (256 items, 9010 ms)
```

A clean shutdown ends with a detach line:

```text
[ 315422] [!] Detach, process exit 1
```

If the game freezes:

- the watchdog lines keep appearing with `frames=` stuck - the main thread is
  busy inside the game (usually while applying items); note the last `Grant:`
  and `Pump:` lines and the `mode=`,
- the whole log stops - the process is fully locked, note the very last line,
- `queued=1` with `frames=` stuck means the client did not consume an injected
  GC message; set `skin.autogrant=0` to confirm the source,
- to rule out the cache-snapshot path, set `skin.grant.snapshot=0` and
  `skin.grant.probe=0`; the mod then only streams batches and never resizes
  snapshots.

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for release notes (in Russian) and
[CHECKPOINT.md](CHECKPOINT.md) for the 1.2 checkpoint record.

## License

GPL-3.0
