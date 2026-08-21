# FreeWili OG App Explorer

**A single-executable desktop app for loading OG apps onto the FreeWili 1-OG.**

An OG app ships as one `<name>_main.uf2`. You pick it, you press Flash, and both
CPUs end up running it — the app finds the board, works out which RP2040 it is
talking to, and writes to the right one. No drag-and-drop into `RPI-RP2`, no
guessing which of the two identical mass-storage volumes is MAIN and which is
DISPLAY.

![FreeWili OG App Explorer](assets/OGexplore.png)

## ⚠️ First: the board needs the OG display bootloader

**OG apps will not work without it.** An OG app's display half is *embedded
inside the main UF2* and travels to the DISPLAY CPU over the inter-CPU link —
and the OG display bootloader is the thing on the other end that receives it.
On a board that does not have it, flashing an OG app leaves you with a MAIN CPU
running the app and **a display that never comes up**.

It is a one-time, per-board install, and this app does it for you:

> **OG Bootloader Installer tab → "Install FreeWili OG Bootloader"**

Do that once. After that, every OG app is a single file and a single click.

Two things worth knowing:

- Installing the bootloader **erases the MAIN CPU first**. That is deliberate, not
  collateral: a running MAIN app talks continuously on the inter-CPU link, and
  the DISPLAY bootloader's console only enumerates after ~10 seconds of MAIN
  silence. It also leaves the board wanting an app, which is the next thing you
  flash.
- Installing the **original (deprecated) FreeWili 1 firmware removes the display
  bootloader again.** Coming back to OG apps means reinstalling it from the same
  tab.

The app warns you about this at flash time rather than assuming — it cannot
prove a bootloader's absence without interrogating the board, and a false
refusal would be worse than a false warning.

## Download

**[Download FwOGExplorerV2.zip →](https://github.com/freewili/fwOGAppExplorer/releases/latest)**

Windows x64. Unzip and run `fwOGExp.exe` — one statically linked executable with
the firmware images baked in, so there is nothing to install and no
redistributable to chase. `fwogcli.exe` ships beside it and drives the same
flash engine from a terminal.

The `catalog/` folder holds nothing but a note explaining itself, and that is
the v2 change worth knowing: the app now fetches the published FreeWili catalog
on first launch, so the apps are there without anything being bundled. Drop a
`.uf2` into `catalog/` and the App Explorer tab lists it alongside them.

**Linux** works and has flashed a real board, but there is no prebuilt download
— build it from source, and read [Linux](#linux) first: it needs a few
development packages, it has one shared-library dependency Windows does not, and
serial-port permissions usually need a one-time setup step.

**macOS** works and has flashed a real board, also from source — see
[macOS](#macos). It needs nothing beyond CMake and Ninja, and it has the
smoothest permission story of the three platforms.

## What is new in v2

| | |
|---|---|
| **A catalog that fills itself in** | A fresh install fetches `https://docs.freewili.com/og-apps/apps.json` with no configuration — apps arrive without a bundled `catalog/`. Settings can point it elsewhere or clear it. See [The app catalog](#the-app-catalog). |
| **Flashing from any board state** | Both CPUs running, either or both in `RPI-RP2`, a blank CPU, the display on its bootloader alone, the original firmware installed — every state was exercised on hardware rather than assumed. |
| **`fwogcli` is a full front-end** | The same engine as the GUI: `list`, `flash`, `install`, `entries`, `info`, `bootsel`, with board selection and real exit codes. See [Command line](#command-line). |
| **The second tab says what it installs** | It is the **OG Bootloader Installer** now, not a generic firmware tab, because installing the display bootloader is the thing a new board needs first. |
| **The old FreeWili 1 firmware is named as deprecated** | Restoring it removes the display bootloader, so it sits at the bottom of the Danger zone under a name that says so. |

Publishing to the catalog is [its own section](#publishing-to-it); the tooling
that builds it ships in `tools/build_catalog.py`.

## What it does

| Tab | Purpose |
|---|---|
| **App Explorer** | **The main event: load OG apps.** Browse the catalog — embedded, a local `catalog/` folder, or a remote `apps.json` URL — and flash any app in one click. Requires the display bootloader (above). |
| **OG Bootloader Installer** | **Install the OG display bootloader here first.** Its Danger zone also erases either CPU, or restores the deprecated OLD FreeWili 1 firmware. |
| **Recovery** | Documentation for getting a board back when it will not enumerate. |
| **Settings** | Theme, remote catalog URL, window state. |

The parts that make it more than a file copier:

- **The two CPUs are told apart.** In BOOTSEL both MAIN and DISPLAY present an
  identical `RPI-RP2` volume, and writing an app to the wrong one is not a
  recoverable mistake on every board. The app resolves the ambiguity from serial
  enumeration where it can, and where it cannot it flashes a tiny probe image
  that reports which CPU it woke up on. See [`probe/README.md`](probe/README.md).
- **Flash plans, not single writes.** Installing the display bootloader means
  erasing MAIN first (a running MAIN keeps DISPLAY's bootloader console from ever
  enumerating), then writing DISPLAY. That ordering is data in
  `firmware/manifest.json`, not scattered through the UI.
- **Catalog entries declare their target CPU**, and the flash scheme is what
  decides which CPU an entry can reach — a `DisplayBootloader` entry cannot
  address MAIN even if it asks to.
- **UF2s describe themselves.** A FwOGapp image carries its own name, version,
  description and build identity, so dropping an unknown `.uf2` into `catalog/`
  shows you what it is and what it will do to both CPUs — with nothing
  downloaded and no catalog entry written. See below.
- **It flashes from whatever state the board is in.** Both CPUs running, either
  or both sitting in `RPI-RP2` (by button, by an earlier flash, or because a CPU
  is blank), the display running only its bootloader, the original firmware
  installed — every state was exercised on hardware. Each CPU is written by its
  position on the board's own USB hub, the board is re-identified live at every
  step of a plan, and a CPU that is mid-reboot is waited for rather than
  refused. Before a MAIN install the DISPLAY is parked in BOOTSEL so a running
  display app cannot disturb the write; the new MAIN firmware brings it back.
- **The board does not need a serial number.** A FreeWili OG under OG firmware
  never enumerates its FTDI, so its serial reads `Unknown` for life; the app
  tells boards apart by the RP2040 chip ids their serial ports report, and only
  refuses when a board *contradicts* the one it was working with.

## Command line

`fwogcli.exe` sits beside the GUI and drives the identical flash engine:

```
fwogcli list                                  every connected board and its CPUs
fwogcli flash <file.uf2> [--cpu main|display] flash a UF2 (default: MAIN)
fwogcli install <slug>                        an embedded entry's plan (see `entries`)
fwogcli entries                               the embedded entries and their plans
fwogcli info <file.uf2>                       what a UF2 says about itself
fwogcli bootsel main|display                  reboot a running CPU into BOOTSEL
   --device <n|serial|chip>  pick a board when several are connected
   --keep-display            do not park the DISPLAY before a MAIN install
   --yes                     confirm a drive the engine cannot place by hub port
```

Exit codes: 0 ok, 1 flash failed, 2 usage/selection, 3 needs `--yes`.

## The FwOGapp Contract

Firmware for the FreeWili OG follows the **FwOGapp Contract**, a set of seven
build-enforced rules defined in the BSP
([`AGENTS.md` in wiliOGBsp](https://github.com/freewili/wiliOGBsp/blob/main/AGENTS.md)).
The point of it is that an OG image is identifiable, versioned, self-describing
and recoverable without anyone opening the case. In short:

| # | Rule | Enforced by |
|---|---|---|
| 1 | Every app declares a three-digit `VERSION` and a `DESCRIPTION` | configure error |
| 2 | DISPLAY apps declare a power policy — `FWOG_POWER_DEFAULT()` or `FWOG_POWER_CUSTOM()`; the default handles the 6-second red-button shutdown hold | link error |
| 3 | 1200-baud USB BOOTSEL must not be built away — the DISPLAY CPU has no BOOTSEL button, so this is its only way back | configure error |
| 4 | The FPGA bitstream SHA-256 is pinned | automated test |
| 5 | USB identity is declared: VID/PID `093C:2054` (MAIN) / `093C:2055` (DISPLAY), product string `FWOG <cpu> <name> <version>` | build-time check |
| 6 | Every image embeds an `fwog_uf2_info_t` metadata record — magic, name, description, version, CRC | build-time check |
| 7 | MAIN apps declare a watchdog policy; the default kicks an 8.3-second hardware watchdog each loop | link error |

**Rules 5 and 6 are what this app reads.** Rule 5's `FWOG main ` / `FWOG display `
product-string prefix is one of the signals that identifies a connected board
down to which CPU it is, before anything is written — second in line behind USB
hub port location, which is authoritative, and ahead of the probe image as a
last resort. Rule 6's record is how an image explains itself: a MAIN UF2 carries
*two* records — its own, plus one describing the DISPLAY image embedded inside
it — which is what lets a single `<name>_main.uf2` be shown, and flashed, as the
one file that provisions both CPUs.

That second record is also the clearest statement of why the display bootloader
is a prerequisite rather than a nicety: the DISPLAY image genuinely is inside the
MAIN UF2, and the bootloader is what unpacks it across the link. It decides
whether to transfer by comparing CRC32, which is why a display record carries no
build identity of its own — there is nothing for it to disagree with. With no
bootloader on the board, nothing ever reads that record and the DISPLAY CPU
keeps whatever it had.

One caveat worth stating plainly: the record layout this app parses was
**derived from real images rather than from a published header** (see
[`src/catalog/fwOgAppInfo.h`](src/catalog/fwOgAppInfo.h), which documents the
offsets and the evidence). Every field is bounds-checked and a record that does
not fit is rejected rather than read past.

## The app catalog

App Explorer merges three sources into one list:

| source | where it comes from |
|---|---|
| **Embedded** | compiled into the executable — the display bootloader, the erase actions, the deprecated original firmware |
| **Local** | any `.uf2` dropped in the `catalog/` folder beside the executable, plus an optional `catalog.json` describing them |
| **Remote** | an `apps.json` fetched over HTTPS |

The remote catalog ships pointed at the published FreeWili one:

```
https://docs.freewili.com/og-apps/apps.json
```

A fresh install fetches it at startup with no configuration; the Settings tab
can point it somewhere else, or clear it to stop fetching. Clearing it sticks —
the default is seeded once, not re-applied on every launch — and **Use the
FreeWili catalog** puts it back.

Only `https://` is accepted, and a redirect from `https://` down to `http://`
is refused. This is not general caution: a catalog entry is authoritative over
which of the board's two CPUs each image is written to, and a main-CPU image
written to the DISPLAY CPU drives GPIO 29 against the PDM microphone's own
output. Nothing downstream can catch a rewritten catalog — `sha256` is
self-attested by the same document, and a UF2 header cannot distinguish a MAIN
image from a DISPLAY one.

A failed fetch is a status line and never a modal, and never clears entries the
app already had: an offline launch shows the previous catalog from
`apps-cache.json`.

### Publishing to it

`publish/` holds the authored input and `tools/build_catalog.py` builds the
tree that gets uploaded:

```powershell
python tools/build_catalog.py     # -> publish/out/og-apps/{apps.json, uf2/*.uf2}
```

Almost nothing in the published `apps.json` is written by hand. Every OG main
image carries `fwog_uf2_info_t` records (the FwOGapp Contract, above), so the
tool reads each app's name, description, version and build identity straight
out of the bytes it is publishing, then computes `sha256` and `size` from them.
`publish/sources.json` carries only what an image cannot know about itself:
which apps to publish, and their category, author, repository and tags.

Copy `publish/out/og-apps/` into the documentation site's `static/` and deploy.
Full procedure, including what the server has to get right, in
[`publish/README.md`](publish/README.md).

## Firmware for the board itself

The RP2040-side firmware — the display bootloader, the CPU prober this app
embeds, and the OG board support package — lives in the BSP repo:

**[github.com/freewili/wiliOGBsp](https://github.com/freewili/wiliOGBsp)**

## Building from source

Requires CMake 3.28+, Ninja, and a C++23 compiler. SDL3, Dear ImGui,
nlohmann/json, doctest and `freewili-finder` are fetched automatically by CMake.
On Windows nothing else needs to be installed by hand; on Linux a handful of
development packages do — see [Linux](#linux).

```sh
cmake --preset win-msvc-release
cmake --build --preset win-msvc-release
```

Output lands in `build/win-msvc-release/`. On Windows, run this from a shell
with the MSVC environment loaded (the "x64 Native Tools Command Prompt") — the
preset fails loudly rather than silently falling back to a GCC on `PATH`.

Presets: `win-msvc-debug`, `win-msvc-release`, `linux-gcc-release`,
`mac-clang-debug`, `mac-clang-release`,
`wasm-release`.

`linux-gcc-release` is a real target and has its own section below — it builds,
runs, and has flashed a board, but it has build prerequisites the Windows preset
does not and it does not deliver "one exe" in the same way. See
[Linux](#linux). `wasm-release` has still never been configured or built — see
[`web/README.md`](web/README.md), which is explicit about what that means.

### Tests

```sh
cmake --preset win-msvc-debug
cmake --build --preset win-msvc-debug
ctest --preset win-msvc-debug
```

### Release builds need the firmware images

The `.uf2` payloads in `firmware/` are **not** committed — 20+ MB of binaries do
not belong in git history. A build without them still succeeds, with an empty
firmware manifest and a warning; only a release build needs the real files.
[`firmware/README.md`](firmware/README.md) lists each image and where it comes
from.

The one committed binary is `probe/probe.uf2` (49 kB), and its provenance is
checked at configure time and re-verified at run time before it is ever written
to a board. `probe/README.md` explains why that trade was made.

## Linux

Linux builds, runs, and has installed the display bootloader on a real board.
There is no prebuilt Linux download — build it from source as below.

Three things about Linux are genuinely different from Windows, and each has its
own subsection: the build needs packages installed by hand, the "no
dependencies, one exe" claim is **not** true here in the way it is on Windows,
and serial-port permissions are not something you get for free.

### Building on Linux

```sh
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
ctest --preset linux-gcc-release
```

Measured on GCC 16.1.1, glibc 2.44, CMake + Ninja, an Arch-derived distribution:
warning-clean at `-Wall -Wextra` for this project's own sources, and `ctest`
green at 551 cases / 2170 assertions.

CMake still fetches SDL3, Dear ImGui, nlohmann/json, doctest and
`freewili-finder`. It does **not** fetch the system libraries those need
headers for, so install your distribution's development packages for:

| Needed for | pkg-config modules |
|---|---|
| `freewili-finder`'s USB enumeration | `libudev` |
| SDL3's X11 backend | `x11`, `xext`, `xcursor`, `xi`, `xfixes`, `xrandr`, `xrender`, `xscrnsaver`, `xtst` |
| SDL3's Wayland backend | `wayland-client` (≥1.18), `wayland-egl`, `wayland-cursor`, `egl`, `xkbcommon` (≥0.5.0) |

(Module names rather than package names on purpose: the package names differ per
distribution and this was written on one of them. These are the names SDL3's own
`cmake/sdlchecks.cmake` and `freewili-finder` actually look for.)

The X11 and Wayland rows are the ones worth care. SDL3 defines
`SDL_VIDEO_DRIVER_X11` and `SDL_VIDEO_DRIVER_WAYLAND` only inside the branch
where its configure step found those headers, so a machine missing both still
**configures, builds and links without complaint** and produces an executable
with no way to open a window. Both are `1` in this build's generated
`SDL_build_config.h`; if you are debugging a binary that starts and immediately
fails to create a window, check there first.

### The dependency truth

On Windows, "no dependencies, one exe" is delivered by a static CRT and is
true. On Linux it is **not** true, and here is exactly how untrue:

```
$ readelf -d build/linux-gcc-release/fwOGAppExplorer | grep NEEDED
  libudev.so.1   libm.so.6   libc.so.6   ld-linux-x86-64.so.2
```

Most of the promise does hold. SDL3 is linked statically — it builds to
`libSDL3.a` and appears in no `NEEDED` entry — and `-static-libstdc++
-static-libgcc` mean neither `libstdc++.so.6` nor `libgcc_s.so.1` is a direct
dependency. The firmware images are still baked in. What is left is **one**
shared library this project cannot remove — plus a set of libraries that are
`dlopen`ed and therefore do not appear in `readelf` output at all, which is the
part most likely to surprise someone:

- **`libudev.so.1` — a hard, direct dependency.** It comes from
  `freewili-finder`, whose Linux backend uses it to walk sysfs for USB devices:
  over a hundred `udev_*` references in a single file, which is also why
  replacing it is not a small change. If it is missing the app does not start at
  all. It is provided by systemd's own runtime library package, so on a
  systemd-based distribution it is already installed — but "already there" is
  not "no dependency", and this paragraph exists so that nobody has to find out
  from an `ldd` on a machine where it is not.
- **A display stack, loaded at run time.** SDL3 opens
  `libwayland-client.so.0` / `libwayland-cursor.so.0` / `libwayland-egl.so.1`
  or `libX11.so.6` / `libX11-xcb.so.1` / `libxkbcommon.so.0`, plus
  `libGL.so.1` and `libdecor-0.so.0`, by `dlopen` rather than by linking them.
  That is why they are absent from `readelf -d` and why the ELF header
  understates what the app needs. On a desktop you already have them.
- **`libcurl.so.4`, optionally.** The remote-catalog feature `dlopen`s libcurl
  precisely so that its absence is not fatal: with no libcurl the remote catalog
  reports itself unavailable and everything else works. See `fwHttp.cpp`.

**Why libudev is not statically linked.** Three reasons, in order of how
decisive they are. First, there is no static libudev to link: there is no
`libudev.a` anywhere on the machine this was written on, and
`pkg-config --libs --static libudev` answers plainly `-ludev` — the systemd
package that provides the shared library provides no static one. Linking it
statically would mean building systemd from source as a step in building this
app, which is a worse dependency than the one it removes. Second, libudev is
LGPL-2.1-or-later, and static linking carries an obligation to let a recipient
relink against a modified libudev; satisfying that means shipping object files
or equivalent alongside the "one exe", which is the opposite of the point.
Third, the remaining alternative — replacing `freewili-finder`'s libudev use
with direct sysfs reads — is a rewrite of an upstream dependency's platform
backend rather than of this project's code, and it would fork the finder this
app shares with the rest of the FreeWili tooling.

So: one dependency, documented here, satisfied by the package that ships udev
itself. That is a worse answer than Windows gets and a better one than a false
claim of none.

**glibc itself is deliberately dynamic.** Statically linking glibc breaks
`dlopen()`, and `dlopen()` is how the libcurl branch above works at all.

### Will the binary run on another machine?

**A binary built here needs glibc 2.43 or newer.** That is measured, not
estimated: the highest symbol version the executable requires is `GLIBC_2.43`
(`acosf`, `asinf`, `atan2f`, `log10f`, `sqrtf`, from `libm`), with `GLIBC_2.42`
close behind it (`cfsetispeed`, `cfsetospeed`, from the serial code).

```sh
objdump -T build/linux-gcc-release/fwOGAppExplorer | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
```

glibc's symbol versioning is backward compatible and **not forward** compatible,
so a binary built against a newer glibc does not start on an older one — the
loader refuses it by name before `main()` runs. The floor is therefore a property
of the machine that built it, not of this source: it is 2.43 because this machine
runs 2.44. **If you intend to distribute a Linux build, build it on the oldest
distribution you intend to support**, which is the only thing that actually
lowers the floor. Nothing here has been tested on a second machine.

### Device permissions

The app needs write access to exactly two things, and it is worth being precise
because the usual advice is broader than necessary:

1. **The board's CDC serial ports** (`/dev/ttyACM*`), opened `O_RDWR` — for the
   1200-baud BOOTSEL touch and to read the CPU prober's reply.
2. **A mounted, writable `RPI-RP2` volume** — the UF2 is copied into the
   filesystem, so this is ordinary file permission on a mount point.

It does **not** need raw USB access. `freewili-finder` enumerates through
libudev, which reads sysfs, and sysfs is world-readable — so a rule that opens
up `/dev/bus/usb` is granting something this app has no code to use.

**You will probably need to do something about item 1.** Left to the default
rules, a USB serial port belongs to a group you are not in: measured here,
`/usr/lib/udev/rules.d/50-udev-default.rules` sets `GROUP="uucp"` for tty
devices and a port left to it alone comes out `root:uucp 0660`. Debian and
Ubuntu use `dialout` for the same job. Either way `open()` returns `EACCES`, and
what you see in the app is a touch that failed rather than a permission that is
missing. Two ways to fix it:

```sh
# Either: join the group your distribution already assigns.
sudo usermod -aG uucp $USER        # dialout on Debian/Ubuntu; log out and back in

# Or: install the narrower rule shipped in this repo.
sudo install -m 0644 packaging/60-fwog-app-explorer.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

[`packaging/60-fwog-app-explorer.rules`](packaging/60-fwog-app-explorer.rules)
covers the three USB IDs that publish a port — `093c:2054` (MAIN),
`093c:2055` (DISPLAY) and `2e8a:000a` (the CPU prober) — and uses
`TAG+="uaccess"`, so systemd-logind gives an ACL to whoever is logged in at the
machine. No group to join, no re-login, and no other account on the box gains
the ability to reflash an attached board. Its syntax is checked with
`udevadm verify`; the file itself explains each choice, including why
`0403:6014` (the board's own FTDI) and `2e8a:0003` (the bootrom) are
deliberately absent.

> If everything already works on your machine without any of this, do not
> conclude the rule is unnecessary — find out which of two things is carrying
> you, because they cover different ground. On the machine this was written the
> ports were `crw-rw-rw-` and nothing needed setting up, and there were **two**
> independent reasons: `/etc/udev/rules.d/99-freewili.rules` was already
> installed from other Intrepid tooling setting `MODE="0666"`, *and* the user
> was already a member of `uucp`, which the stock
> `50-udev-default.rules` grants the port to anyway.
>
> Neither is a property of Linux, and neither covers everything: that vendor
> rule matches `ATTRS{idVendor}=="093c"`, so it does not reach the CPU prober's
> own port, which enumerates as `2e8a:000a` while it is running. On that machine
> the prober was readable because of the group membership, not the vendor rule.

**The `RPI-RP2` volume.** A udev rule cannot grant the right to *mount* a
filesystem, so there is nothing to install for this. On a desktop with udisks2 —
which is to say almost any desktop environment — the volume is auto-mounted for
the logged-in user under `/run/media/$USER/RPI-RP2` and is writable, which is
what was observed during the hardware runs. Without an automounter, mount it by
hand and make sure it is writable by you (for the FAT filesystem the RP2040
presents, that means mounting with your own uid). The app finds the volume by
reading `/proc/mounts` and checking `INFO_UF2.TXT`, so it will only see volumes
that are actually mounted.

### Running it, and the release layout

There is no installer and no system install, deliberately: the app looks for its
catalog at `catalog/` **beside the executable** (`catalogDir()` is
`exeDir()/catalog`), so it is a portable directory rather than something that
belongs in `/usr/bin`. The Linux equivalent of the Windows `FwOGExplorerV2.zip`
is the same layout in a tarball:

```
FwOGExplorerV2-linux-x86_64/
├── fwOGAppExplorer
├── fwogcli
├── catalog/                        # empty; the tab picks up any .uf2 dropped here
├── 60-fwog-app-explorer.rules      # optional, see Device permissions
├── fwOGAppExplorer.desktop         # optional, see The .desktop file
└── fwOGAppExplorer.png             # the icon that .desktop names
```

```sh
mkdir -p FwOGExplorerV2-linux-x86_64/catalog
cp build/linux-gcc-release/fwOGAppExplorer build/linux-gcc-release/fwogcli \
   packaging/* FwOGExplorerV2-linux-x86_64/
tar czf FwOGExplorerV2-linux-x86_64.tar.gz FwOGExplorerV2-linux-x86_64
```

The three `packaging/` files are all optional at run time. They travel in the
tarball so that somebody who downloads only the tarball already has everything
that "Device permissions" above and "The `.desktop` file" below tell them to
install. `catalog/` ships empty on both platforms as of v2 — the remote catalog
is what fills the list on first launch, so nothing has to be bundled and the
same app no longer appears twice once the fetch lands.

**The build still does not produce this**, and since v2 that is only because
nothing has automated it — not because it cannot be. The old reason was that
`catalog/*.uf2` is excluded from git (for the same reason `firmware/*.uf2` is),
so any packaging target would still have needed a manual copy and would only
have looked like it had automated it. With an empty `catalog/` that argument is
gone: the layout above is the build output plus `packaging/` plus one `mkdir`.
The Windows zip is assembled by the same three steps by hand, and if one
platform ever gets a packaging target the other should get it in the same
change — two release processes that drift is how the two stop matching.

macOS does ship a script — [`packaging/make_mac_app.sh`](packaging/make_mac_app.sh)
— and that is not the drift this rule forbids. The script does not assemble a
release layout; it builds the `.app` wrapper that macOS *structurally requires*
(Gatekeeper's stapler refuses to attach a notarization ticket to a bare Mach-O,
and Finder drags a Terminal window up behind one — see [Signing and
distribution](#signing-and-distribution)). Windows and Linux have no equivalent
requirement, so there is no sibling target to keep in step. The macOS release
zip around that bundle is still assembled by hand, by the same steps as the
other two.

### The `.desktop` file

[`packaging/fwOGAppExplorer.desktop`](packaging/fwOGAppExplorer.desktop) is not
needed to run the app. It is needed to give it an **icon on Wayland**.

The window icon the app sets itself is an X11 mechanism (`_NET_WM_ICON`), and it
was confirmed working — the app's own icon data is on the window. Wayland's
xdg-shell has no equivalent: a compositor finds a window's icon by taking the
surface's `app_id`, looking for `<app_id>.desktop`, and reading `Icon=` from it.
No desktop file, no icon, however good the one compiled into the binary is.

The name is load-bearing. SDL derives both the X11 `WM_CLASS` and the Wayland
`app_id` from the same `SDL_GetAppID()`, which — with no app metadata identifier
set, and this app sets none — falls back to the executable's own name. Measured
on the running app: `WM_CLASS = "fwOGAppExplorer", "fwOGAppExplorer"`. So the
file must be `fwOGAppExplorer.desktop`, and **renaming the executable in a
release would silently break the icon** without breaking anything else.

```sh
install -Dm644 packaging/fwOGAppExplorer.desktop \
  ~/.local/share/applications/fwOGAppExplorer.desktop
install -Dm644 packaging/fwOGAppExplorer.png \
  ~/.local/share/icons/hicolor/256x256/apps/fwOGAppExplorer.png
```

Then edit `Exec=` to wherever you unpacked the binary, or put it on your `PATH`.
The icon is the 256×256 frame extracted byte-for-byte from the same
`resources/product.ico` the Windows build uses, so the two platforms show the
same artwork rather than two drawings of it.

### What is and is not verified on Linux

**Verified against the real board:** device detection and CPU identification by
USB hub position; the 1200-baud touch; `RPI-RP2` volume discovery including both
CPUs in BOOTSEL at once; the CPU prober end to end; the **display bootloader
install**; and the `LegacyDirect` restore. Timings and serial numbers are in
[`docs/hardware-verification.md`](docs/hardware-verification.md).

**Not verified:**

- **The App Explorer `OgApp` flash** — the everyday path. There is no known-good
  OG app UF2 on the machine this was done on, so the one flow most users will
  use is the one flow that has not been run on Linux.
- **Wayland.** Every launch of the app on Linux has been on a private `Xvfb`
  display, because the machine's owner was on a Wayland session that had to be
  left alone. The X11 backend is exercised; the Wayland backend is compiled and
  has never had a window on screen. The `.desktop` icon behaviour above follows
  from the protocol and from SDL's source, and has not been watched happening.
- **Any other machine.** Everything here is one build on one distribution. The
  glibc floor above is the honest way to reason about the rest.

## macOS

macOS builds, tests green, runs, and **has flashed a real board** — including
the one path Linux never exercised. What that claim rests on, and its one
caveat, is spelled out in [What is and is not verified on
macOS](#what-is-and-is-not-verified-on-macos) at the end of this section.

### Building on macOS

Requires Xcode command line tools plus CMake and Ninja (`brew install cmake
ninja`). Then:

```sh
cmake --preset mac-clang-release
cmake --build --preset mac-clang-release
ctest  --preset mac-clang-release
```

Measured here: Apple clang 21.0.0 (clang-2100.1.1.101), arm64, macOS 26.6
(SDK 26.5), CMake 4.4.2, Ninja 1.13.2. The build **configures, builds and
links without complaint** — warning-clean, both `mac-clang-release` and
`mac-clang-debug` — and the complete test suite passes: **580 cases / 2337
assertions**. `fwogcli` builds and runs from the same presets.

Output lands in `build/mac-clang-release/`. `freewili-finder` has a native IOKit
backend, so unlike Linux there is no `libudev` equivalent to install — the USB
enumeration, serial identity reads (`fwSerialPorts.cpp`) and BOOTSEL device
counting (`fwVolume.cpp`) all go through IOKit and CoreFoundation, which ship
with the OS.

### What macOS gets for free that Linux does not

- **No serial-port permission setup.** `/dev/cu.*` nodes are world-writable by
  default; there is no `dialout`/`uucp` group to join and no udev rule to write.
- **The `RPI-RP2` volume auto-mounts**, under `/Volumes/RPI-RP2` (a second board
  mounts at `/Volumes/RPI-RP2 1` — the space is handled). No `udisksctl` step.
- **libcurl ships with the OS** (`/usr/lib/libcurl.4.dylib`), so the remote
  catalog works without installing anything. It is still `dlopen`ed, never
  linked, same as Linux.
- **A leaner dependency truth.** `otool -L` reports only OS-provided libraries
  and frameworks — there is no third-party shared-library dependency at all,
  which makes the "one executable" claim closer to the Windows truth than the
  Linux one.

### Where things land

Settings and the catalog cache go to `~/Library/Application Support/
fwOGAppExplorer/` — the platform's convention, where Linux uses
`~/.local/share`. An absolute `$XDG_DATA_HOME`, if you set one, still wins on
both. The `catalog/` folder is looked for beside the executable, same as every
platform.

### Signing and distribution

The build output is a plain executable, ad-hoc signed by the linker as arm64
requires — fine for local use. For distribution, wrap it in the minimal `.app`
bundle (a bare Mach-O double-clicked in Finder drags a Terminal window up
behind it, and `stapler` refuses to staple anything that is not a bundle):

```sh
packaging/make_mac_app.sh                 # bundle + Developer ID signature
ditto -c -k --keepParent build/mac-clang-release/fwOGAppExplorer.app /tmp/fwog.zip
xcrun notarytool submit /tmp/fwog.zip --keychain-profile <profile> --wait
xcrun stapler staple build/mac-clang-release/fwOGAppExplorer.app
```

This flow has been run end to end: the bundle notarizes (`status: Accepted`),
staples, and `spctl --assess` answers `accepted, source=Notarized Developer
ID`. It needs a **Developer ID Application** certificate — an Apple Development
certificate signs a binary that runs locally but will not pass Gatekeeper on
another machine — and a `notarytool store-credentials` profile.

One consequence of the bundle worth knowing: `catalog/` sits beside the
executable on every platform, and inside a bundle "beside the executable"
means `fwOGAppExplorer.app/Contents/MacOS/catalog/`. The "Open catalog folder"
button opens the right place; finding it by hand takes Finder's "Show Package
Contents".

And one consequence worth stating before someone hits it: in a **signed**
bundle that folder is sealed resources, so dropping a `.uf2` into it after
signing breaks the code seal (`codesign --verify` fails from then on), and a
quarantined app run straight from `~/Downloads` executes under App
Translocation, where the folder is read-only entirely. The drop-a-file-next-
to-the-exe model, which is exactly right on Windows and Linux, does not
transfer to a signed mac bundle. Since v2 the remote catalog covers the
everyday case without touching the bundle; local `.uf2`s work fine with the
unbundled binary. Moving the local catalog to per-user data on macOS is the
platform-correct future answer, and is recorded here rather than smuggled
into a port.

`fwogcli` stays a bare executable — a terminal program gains nothing from a
bundle, and codesigning a bare Mach-O for local use is exactly what the linker
already did. A macOS release ships it beside the `.app`, the same way the Linux
tarball seats it beside the GUI binary.

### What is and is not verified on macOS

**Verified against the real board** (an attached FreeWili 1-OG, serial FW4300,
2026-08-14): device detection and CPU identification by hub position; product
strings read from the IO registry; and the **App Explorer `OgApp` flash end to
end** — the 1200-baud touch, `RPI-RP2` volume discovery, the copy with
`F_FULLFSYNC`, MAIN provisioned, and the display bootloader carrying the
embedded DISPLAY image across the inter-CPU link, confirmed by both CPUs
re-enumerating with the new app's product strings. The signed `.app` notarized
and stapled. Timings and serials are in
[`docs/hardware-verification.md`](docs/hardware-verification.md).

**Dates matter here, so:** that board pass ran on the pre-v2 codebase, and
this branch was then rebased onto v2 — which rewrote the flash sequencing
(`fwFlashPrep`) and the catalog around the same platform code. The rebase was
re-verified in two stages, both in the ledger: first the board-free evidence
(warning-clean build on both presets, the full suite — **580 cases / 2337
assertions** — `fwogcli` on an empty bus, the `.app` packaging, a first-launch
remote-catalog fetch over the `dlopen`ed libcurl), and then **the flash was
re-run against v2's sequencing on the same board**: `fwogcli flash` — its
first board flash ever driven from macOS — with an image downloaded and
hash-verified from the published catalog, both bootrom volumes mounted at once
(`/Volumes/RPI-RP2` and `/Volumes/RPI-RP2 1`, the space case, on real
hardware), and both CPUs re-enumerating as the new app.

**Not verified:**

- The display-bootloader install and `LegacyDirect` restore flows (the attached
  board already had its bootloader), the CPU-prober recovery flow, and two
  boards at once.
- The GUI flash **on the rebased build** — the post-rebase flash above went
  through `fwogcli`, which drives the identical engine; the GUI's own
  end-to-end run is the 2026-08-14 (pre-rebase) evidence.
- **Any other machine.** Everything here is one arm64 Mac; no Intel build has
  been run. The bundle declares macOS 12.0 as its floor
  (`LSMinimumSystemVersion` in `make_mac_app.sh`), and every API the port
  uses predates that by years — but 12.0 is a declaration, not a measurement,
  for the same reason the Linux section's glibc floor is one.

## Hardware verification status

[`docs/hardware-verification.md`](docs/hardware-verification.md) records exactly
what has been confirmed against a physical board and what has not. As of the
2026-08-18 pass every flash path — OG app, display bootloader, the original
firmware and back, both erase actions — has been run on hardware from every board
state listed there, through both the GUI and `fwogcli`.

## License

MIT — see [LICENSE](LICENSE).

Bundled third-party code: [miniz](third_party/miniz) (public domain) and Brad
Conte's [SHA-256](third_party/sha256) (public domain). Dependencies fetched at
build time keep their own licenses: SDL3 (Zlib), Dear ImGui (MIT), nlohmann/json
(MIT), doctest (MIT).
