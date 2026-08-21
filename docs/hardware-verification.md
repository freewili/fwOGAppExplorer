# Hardware verification — FreeWili OG App Explorer

Status: **the display-bootloader install, `LegacyDirect`, and the CPU prober
have all been run against a physical board on Linux.** Detection was verified
earlier on Windows. What has *not* been run is the everyday App Explorer
`OgApp` path — there is still no known-good OG app UF2 on this machine.

**HOW TO READ THIS DOCUMENT.** It is written in the order things happened, and
that order matters more than usual here, because the later work overturned some
of the earlier work's stated limits. Sections 1 through "CPU prober" record the
**Windows pass**, when no write had been authorised; "Identification on Linux"
records a **later Linux pass in which writes were authorised and performed**.
Where the two disagree the Linux section is the current state, and every
superseded claim above it now says so at the point of the claim rather than
leaving a reader to notice the contradiction on their own. Nothing has been
deleted: what a pass believed at the time is part of the record.

---

## Why the flash steps were not run *in the Windows pass*

> **Superseded by the Linux pass**, in which DISPLAY writes were authorised and
> performed: `bl_display.uf2` (the bootloader install) and
> `FreeWiliDisplayV67.uf2` (the `LegacyDirect` restore) both reached the DISPLAY
> CPU. What did *not* happen even then is a standalone `flash_nuke` to DISPLAY —
> on that run `dropRedundantErases()` removed the erase step. This section is
> kept because it is the reasoning that held while the images below were the
> only ones available, and because the risk it describes did not go away when
> the authorisation changed.

The remaining steps write firmware to a physical board. Running them
unattended was the wrong call, for a reason that is about the images
available rather than about caution in general:

Every UF2 currently in `firmware/` that can legally target the MAIN CPU is
one that **replaces or erases whatever the board is running now**:

| Image | Target | Effect if flashed |
|---|---|---|
| `FreeWiliMainV92.uf2` | MAIN | Overwrites MAIN with the deprecated legacy firmware |
| `flash_nuke.uf2` | MAIN | Erases MAIN, leaving it blank (also erases DISPLAY as step 1 of `LegacyDirect` — see Step 5) |
| `bl_display.uf2` | DISPLAY | **Out of scope — DISPLAY writes are not authorised** |
| `FreeWiliDisplayV67.uf2` | DISPLAY | **Out of scope — DISPLAY writes are not authorised** |

There is no "known-good app UF2" on this machine that would leave the board
in the state it started in. Choosing which of the above to accept — and
accepting the loss of whatever MAIN currently holds — is the board owner's
call, not something to decide on their behalf. So the write stops here.

MAIN has a reachable BOOTSEL button, so every step below is recoverable by
hand. That is true of MAIN only.

---

## ✅ Step 1 — Identification (VERIFIED)

This was the spec's **stated assumption**, and the most important thing to
confirm, because the entire CPU-role assignment rests on it.

Observed on a connected FreeWili OG:

```
FREE-WILi [OG]  serial FW4788
MAIN:    COM24  (by hub position)
DISPLAY: COM59  (by hub position)
```

**Result: the assumption holds.** fwfinder reports usable hub port
locations, and the primary identification path — `USBHubPortLocation`
(`Main=1`, `Display=2`) — is what actually runs.

The USB-product-name fallback (`"FWOG main "` / `"FWOG display "` prefixes,
read from `DEVPKEY_Device_BusReportedDeviceDesc`) is also live and reachable
on Windows, but it is the fallback, not the primary. No revision to the
Recovery tab text is needed.

> **Qualified by the Linux pass — see "Identification on Linux" below.** Two
> things about that paragraph turned out to need narrowing. The prefix test was
> anchored at position 0 against a string that is only the USB product string on
> Windows, so it could never match on Linux or macOS; and on a FreeWili 1-OG the
> fallback is not merely "not the primary", it is **unreachable for this
> hardware on every platform**, because fwfinder always resolves these CPUs'
> USB identities to `SerialMain`/`SerialDisplay` and `identifyCpus()` skips any
> record carrying a structural signal. Both are measured, below.

One real-world observation worth keeping: **this board has been seen
reporting its serial as the literal string `"Unknown"`**, which is what
fwfinder emits for a flashable OG board when it finds no FTDI child. It has
since turned out (2026-08-18 pass, below) that under OG firmware the FTDI
never enumerates at all, so `"Unknown"` is the normal state and the app no
longer refuses on it: boards are told apart by the RP2040 chip ids their CDC
ports report (`BoardFingerprint`, `src/device/fwDeviceModel.h`), and a refusal
needs a positive contradiction, never a missing serial.

---

## ✅ Step 2 — `DisplayBootloader` — DONE, on Linux

**Was NOT AUTHORISED in the Windows pass — this writes to the DISPLAY CPU.**

It has since been run end to end on Linux and it succeeded: see
"`DisplayBootloader` install, end to end" below for the plan, the timings and
the USB identity the board came back with. What that run does **not** settle is
this step as originally written — a re-run on a board that *already* has a
bootloader. The write-up below does not record what DISPLAY was holding
beforehand, so whether that run was idempotent-over-a-bootloader or a first
install cannot be read back out of it. The owner has confirmed the re-run is
safe; confirmation is not a measurement.

---

## ☐ Step 3 — `OgApp` with a known-good app UF2  ← **start here**

This is the highest-value remaining test and the only one that exercises the
normal, everyday flash path.

1. Obtain a known-good OG app UF2 (an app built against `wiliOGBsp`).
   Place it in a `catalog/` directory beside the executable, or point the
   remote catalog URL at a store that serves one.
2. Launch the app. Confirm the device bar shows the board with both CPUs.
3. Select the app in **App Explorer** and press **Flash**.
4. Expect: the app performs a 1200-baud touch on MAIN, one `RPI-RP2` volume
   appears, the UF2 is copied, and the volume disappears as the board
   reboots.
5. Confirm the display image transfers over the inter-CPU link and the
   display comes up.

**Windows note:** the 1200-baud touch works by opening the CDC port at 1200
baud, which triggers `rom_reset_usb_boot_extra()`. On Windows the open then
*fails* with "A device attached to the system is not functioning." **That
failure is the success indicator**, not an error — the app is written to
expect it.

---

## ☐ Step 4 — Refusals (mostly non-destructive; run these)

These test that the app declines to act. Step 4a writes nothing.

**⚠ 4b DOES WRITE. Do not run it expecting a refusal.** This entry said the
opposite until the Linux pass measured it, and a tester who trusted the old
wording would have got a real write to a real CPU while believing nothing could
happen. What changed is not the checklist's honesty but the engine: two mounted
`RPI-RP2` volumes are no longer indistinguishable, because hub position
identifies both of them. `classifyVolumes()` resolves that case *before* the
ambiguity check ever runs — see its own comment, "the case this whole file was
built around as unresolvable, resolves" — `test_fwVolumeState.cpp:172` pins it,
and the Linux run recorded further down this document measured exactly that:
`RPI-RP21` → MAIN and `RPI-RP2` → DISPLAY, cross-checked against
`/sys/block/sd*`.

- **4a. Unplug mid-plan.** Start a flash, pull the cable partway through.
  Expect a failure message that names exactly which steps completed. Confirm
  it does not claim success.
- **4b. Both CPUs in BOOTSEL by hand. THIS WRITES.** Two `RPI-RP2` volumes
  mount. On a FreeWili — where both CPUs sit on the board's own internal hub —
  expect the app to identify each volume by hub position and **write
  immediately, with no prober and no prompt**. That is correct behaviour, not a
  missing guard: guessing is what is forbidden, and hub position is not a guess.
  The refusal path still exists and is what you get when the identity is
  genuinely absent — two bootrom volumes with no hub identity behind them, which
  a single FreeWili cannot produce.
  Confirm the write lands on the CPU the hub says it should.
- **4c. One CPU in BOOTSEL by hand.** Expect the typed-confirmation prompt
  to appear. Type a wrong answer and confirm it is rejected. (Typing the
  correct answer *will* proceed to a write — stop before that unless you
  intend the write.)

---

## ✅ Step 5 — `LegacyDirect` (LAST, and destructive) — DONE, on Linux

**Reordered after a real hardware failure — see below before running it.**

Run to `Success` on Linux as the restore between phases; see "`LegacyDirect`
also verified on Linux" below. One caveat that section carries and this one
must not lose: on that run `dropRedundantErases()` removed the leading DISPLAY
erase, because DISPLAY was already in its bootrom. **The erase-then-write-the-
same-CPU race described at the end of this document was therefore not
exercised.** The plan shape was verified; the hardest step in it was skipped.

The plan is now three steps, and the order is enforced by
`buildFlashPlan()` rather than by the manifest's array position:

```
1. ERASE  DISPLAY   (flash_nuke.uf2)
2. FLASH  DISPLAY   (FreeWiliDisplayV67.uf2)
3. FLASH  MAIN      (FreeWiliMainV92.uf2)
```

**Why it changed.** The old plan was `[MAIN, DISPLAY]`. Run on the owner's
board, step 1 wrote MAIN successfully; step 2 then touched the DISPLAY CPU,
waited the full 30 s for an `RPI-RP2` volume, and timed out. The DISPLAY
image was never written. The cause is the same 10-second rule the Recovery
tab documents: the display bootloader's USB console only enumerates after
~10 s of MAIN-CPU silence, and MAIN running the freshly written legacy
firmware chatters continuously. **MAIN-first does not risk the DISPLAY
step — it makes it unreachable.**

So DISPLAY is dealt with while MAIN is still quiet, and MAIN — the CPU that
*has* a BOOTSEL button and is therefore always recoverable — goes last.
Erasing DISPLAY first is what makes this recoverable rather than a one-way
door: **an RP2040 with erased flash enumerates `RPI-RP2` by itself, with no
button**, so step 2 writes to a volume the erase brought back and never needs
the DISPLAY serial port at all. `flash_nuke.uf2` is safe on the DISPLAY CPU —
it only erases flash and reboots to BOOTSEL, and never drives GPIO 29, so the
PDM-microphone hazard that makes a main *application* image dangerous there
does not apply to it.

**Still destructive, and still writes to the DISPLAY CPU.** It destroys the
display bootloader from step 1 onward. If you run it, run it last, and
restore afterwards with Step 2.

**What to watch for on a real run:** step 2 should NOT prompt for a typed
`DISPLAY` confirmation. The engine credits the volume that appears after
step 1 to the CPU step 1 just erased (`VolumeState::ExpectedAfterErase`), so
a prompt there means either the erase did not take or something else was
mounted — stop and investigate rather than typing it.

---

## ☐ Erase MAIN

The `erase-main-cpu` action writes `flash_nuke.uf2` to MAIN, leaving it
blank. It is MAIN-only by two independent mechanisms (the `OgApp` scheme
permits only `Main`, and the display-retarget control is gated to
`Unlisted && schemeInferred` entries while this one is always `Embedded`).
Both are unchanged by the `LegacyDirect` reordering and by the addition of the
standalone DISPLAY erase below: the erase image reaching the DISPLAY CPU is
scoped by `eraseAllowsCpu()` to `LegacyDirect` plans plus the one
`erase-display-cpu` slug, neither of which the `erase-main-cpu` entry is. It is
still not retargetable — rewriting its asset's `cpu` to `Display` produces an
empty plan, which `test_fwFlashPlan.cpp` pins.

It requires typing **`ERASE MAIN`** — deliberately a *different* phrase from
the flash engine's `MAIN`, so the confirmation cannot be completed by
habit.

Recovery afterwards is by BOOTSEL button on MAIN plus flashing real
firmware, so only run this when you have a replacement image to hand.

---

## ☐ Erase DISPLAY — AWAITING HARDWARE VERIFICATION

**Status: shipped; never written to a board by this action.**

An earlier draft softened that by saying `flash_nuke.uf2` had already reached
the DISPLAY CPU as step 1 of the `LegacyDirect` plan, so the image and the CPU
were "not new to each other". **That reassurance does not survive checking.**
The one `LegacyDirect` run written up in this document is the Linux one, and on
that run `dropRedundantErases()` *removed* the DISPLAY erase because that CPU
was already in its bootrom — so it is not evidence that `flash_nuke` has ever
run on DISPLAY. No run recorded here erased the DISPLAY CPU.

So both halves are new: reaching it from a standalone entry, and the erase
itself. The safety argument below stands on the RP2040's documented behaviour
and on the erase-then-reappear property the Linux pass *did* observe on MAIN —
not on a DISPLAY precedent, because there is not one.

The `erase-display-cpu` action writes `flash_nuke.uf2` to DISPLAY. It is
DISPLAY-only by the same two independent mechanisms, mirrored: the
`DisplayBootloader` scheme permits only `Display`, and the display-retarget
control is gated to `Unlisted && schemeInferred` entries while this one is
always `Embedded`. A third, narrower mechanism keeps the scheme itself from
being widened: `eraseAllowsCpu()` admits a DISPLAY erase for this **slug**
specifically, not for `DisplayBootloader` entries in general, so the bootloader
install — and any future display entry, local or remote — still cannot acquire
an erase step by listing the erase image beside its own.

It requires typing **`ERASE DISPLAY`**, again deliberately different from the
flash engine's `DISPLAY`.

**Why this is offerable at all on the CPU with no BOOTSEL button:** an RP2040
with blank flash re-enumerates `RPI-RP2` unprompted, with nothing pressed and
nothing running. That is the same property the `LegacyDirect` plan's leading
erase already depends on. Erasing DISPLAY is destructive and recoverable, in
that order — reinstall the bootloader from the same tab straight afterward.

**What to watch for on a real run:** an `RPI-RP2` volume should appear on its
own within a few seconds of the copy completing, with no button held. If it
does not, stop: that is the one outcome this action's safety argument rests on.

---

## ✅ CPU prober (`probe/probe.uf2`) — REBUILT, AND SINCE VERIFIED ON HARDWARE

**Status: rebuilt against the board's own BSP after failing on hardware, then
run on a real CPU during the Linux pass and answered correctly.** The
static-only verification below was written before that run and is left as it
was; the checklist it ends with is now scored against the run, item by item.

### What happened the first time

The original prober was built standalone in this repository for
`PICO_BOARD=pico`. It **failed on real hardware**. Copied directly to `E:\`
(bypassing this app), with drives, USB PIDs and COM ports polled every 300 ms:

```
22:55:27.9  drives=[E:,G:]  pids=[VID_2E8A&PID_0003, &MI_00, &MI_01]  ports=[COM70]
22:55:30.9  drives=[G:]     pids=[VID_2E8A&PID_0003, ...]             ports=[COM70]
22:55:36.5  drives=[E:,G:]  pids=[VID_2E8A&PID_0003, ...]             ports=[COM70]
```

`E:` accepted the UF2, rebooted, ran ~5.6 s, then returned to BOOTSEL on its
own. **No CDC ever enumerated** — `PID_0003` is the RP2040 bootloader PID and
was the only PID seen throughout. (COM70 was the board's unrelated FTDI.) The
UF2 was well-formed; its *board configuration* was wrong.

### What changed

The prober is now `apps/cpuprobe` in **wiliOGBsp** and is built there, so it
inherits the FreeWili OG board header. The sharpest difference is one that the
`boot2_name` field hides: both board headers select `boot2_w25q080` *by name*,
but `PICO_FLASH_SPI_CLKDIV` is compiled **into** boot2 — `pico.h` sets 2,
`freewili_og.h` sets 4. The two 256-byte boot2 blocks differ at exactly one code
byte (offset 28, `0x02` → `0x04`) plus the 4-byte checksum. The failed image was
clocking XIP at `clk_sys/2`.

It links `fwog_common` only — never `fwog_main_bsp` or `fwog_display_bsp`, whose
`board_init()` would drive the wrong CPU's pins. Full provenance, the
`picotool` field-by-field diff, and the freshly re-derived GPIO-29 and
1200-baud evidence are in `probe/README.md`.

### What is established, and what is not

Statically verified (this is the entire verification budget — no hardware):

- compiles and links clean, warning-free, and produces a UF2;
- `picotool info -a` reports `pico_board: freewili_og`, `boot2_w25q080`, USB
  stdin/stdout, and `Fixed Pin Information: none`; the SDK's generated linker
  region for this configure is `LENGTH = (16 * 1024 * 1024)`;
- **GPIO 29 is never configured or driven** — re-derived from scratch against
  the new `.dis`, because linking BSP code made the previous proof stale. The
  exhaustive literal-pool sweep over IO_BANK0/PADS_BANK0 and all their atomic
  aliases finds only `gpio_init`/`gpio_set_function`, called from exactly four
  sites with constant pins 18/4/6/7, plus the SDK's known IE-disable on pads
  26–29. Every SIO GPIO write is masked to bit 18 alone;
- the 1200-baud BOOTSEL path is present in the instruction stream, and passes
  an activity-LED mask of 0, so it drives no pin either;
- reproducible: three independent build trees give byte-identical `.bin`/`.uf2`.

**The checklist, scored against the Linux run** (the run itself is written up
under "The CPU prober's first DISPLAY answer" below):

- [x] The image **boots on real silicon**. This is the specific thing the
      previous version failed at. It re-enumerated as `2E8A:000A` — a PID the
      failed image never reached, which stayed at the bootrom's `2E8A:0003`
      throughout. The boot2 `PICO_FLASH_SPI_CLKDIV` inference above is still an
      inference about the *cause*; that the rebuilt image boots is now measured.
- [x] It **enumerates a USB CDC port** and holds it, on the right hub port and
      carrying that CPU's own serial.
- [ ] It prints `main` or `display`, **once per second, and keeps doing so.**
      Only the first answer was read — that is all `CpuProbeController` needs,
      so nothing watched for the second. The repetition remains unverified.
- [x] The answer is **correct** on a known CPU. It said `display` on the CPU
      hub position independently placed at `3-4.1.2`.
- [x] The **1200-baud touch** returns it to BOOTSEL — the flow closed as a
      round trip.

**The suggested first run was on MAIN, and that is not what happened.** The
advice below was written to keep the first exercise on the CPU with a reachable
BOOTSEL button; the Linux pass instead ran it on **DISPLAY**, from the
two-volumes-mounted flow, which is the more exposed of the two cases in both
respects. It worked, and the 1200-baud touch brought that CPU back. The advice
is kept because it was the right advice for an unproven image, and because it is
still the right first step for anyone rebuilding the prober:

> Put MAIN in BOOTSEL by hand, copy `probe.uf2` to the single `RPI-RP2` volume,
> and watch for a new port saying `main`.

What is still **not** established for the prober: any run on **MAIN**. Every
observation of this image booting comes from one CPU, and it is the other one.

---

## ✅ Identification on Linux, and the display-bootloader install (VERIFIED)

Run against the attached board (`FW4852`, MAIN serial `E463A8574B251838` on hub
port `3-4.1.1`, DISPLAY serial `E463A8574B531838` on `3-4.1.2`), driving the
real `FlashController`, `CpuProbeController` and `identifyCpus()` through a
scratch program linked against the built libraries.

**Signal 1 — hub port location. Works, and is always the answer.** Every
identification observed in this pass, in every board state — both CPUs running
firmware, both in BOOTSEL, one of each — resolved with
`IdentitySource::HubLocation`. `ProductString` was never the source of any
answer.

**Signal 2 — the rule-5 product string. Was broken on Linux; the prefix test is
now fed the right string, but it remains unreachable on this hardware.** Two
separate facts, and they were being conflated:

- fwfinder's `USBDevice::name` is *not* the USB product string except on
  Windows. Measured: the board reports `product=MainCPU v92` /
  `manufacturer=FreeWili` in sysfs and fwfinder hands out
  `"FreeWili MainCPU v92"`. With the display bootloader installed — whose
  descriptors really do say `manufacturer="FreeWili OG"`,
  `product="FWOG display bl 001"`, `093C:2055` — fwfinder hands out
  `"FreeWili OG FWOG display bl 001"`, which begins with neither `"FWOG display "`
  nor `"FWOG "`. `productStringOf()` (`src/device/fwDeviceRecords.cpp`) now reads
  the descriptor from sysfs instead, and the field holds what its own comment
  always said it held.
- Even so, **`identifyCpus()`' pass 2 cannot fire for a FreeWili 1-OG on any
  platform.** `getUSBDeviceTypeFrom()` maps `093C:2054`/`093C:2055` straight to
  `SerialMain`/`SerialDisplay`, and refines the RP2040 CDC PID by hub port to the
  same two — so every CPU record carries a structural flag, and pass 2 skips
  those by design. The product string's remaining live consumer is
  `ogBootloaderState()`, which is not gated on pass 2.

**What the broken string actually cost, measured on the board with the
bootloader installed:**

```
fwfinder name   = "FreeWili OG FWOG display bl 001" -> ogBootloaderState Missing
kernel iProduct = "FWOG display bl 001"             -> ogBootloaderState Present
```

`Missing` is what drives the device bar's "no OG bootloader" banner, so before
this fix Linux told the owner of a correctly provisioned board to go and install
the bootloader it already had — the false negative `ogBootloaderState()`'s own
comment singles out as the expensive direction. Windows was never affected.

**Both CPUs in BOOTSEL at once — the two-volume case.** Confirmed with two real
CPUs rather than the loopback filesystems `fwVolume.cpp` was previously measured
against. Both bootrom devices published the *identical* USB serial
`E0C9125B0D9B`, udisks2 mounted them at `/run/media/drebbe/RPI-RP2` and
`/run/media/drebbe/RPI-RP21`, `findRpiRp2Volumes()` returned **both**,
`countBootselDevices()` returned 2, and `identifyCpus()` mapped
`RPI-RP21`→MAIN and `RPI-RP2`→DISPLAY. Independently checked against
`/sys/block/sd*` → `3-4.1.1`/`3-4.1.2`: correct.

**The CPU prober's first DISPLAY answer.** With both CPUs in BOOTSEL, the real
`CpuProbeController` wrote `probe.uf2` to the DISPLAY volume; that CPU
re-enumerated as `2E8A:000A` on hub port 2 carrying DISPLAY's own serial and
product `FWOG probe 002`, and answered `display` over its CDC. Whole flow took
3.1 s. `proberCpu=DISPLAY`, `remainingCpu=MAIN`, `outcome=Success`. The 1200-baud
touch then returned the prober's CPU to BOOTSEL, so the flow is a round trip.

**`DisplayBootloader` install, end to end (the headline feature).** Run through
`FlashController` from a normally-running board. Plan, timings and enumeration:

| t | Step | Board |
|---|---|---|
| 0.1 s | ERASE MAIN — touch `/dev/ttyACM0` at 1200 baud | MAIN `093C:2054` → absent → `2E8A:0003 RP2 Boot`, auto-mounts |
| 3.1 s | copy `flash_nuke.uf2` | MAIN volume releases |
| 4.1 s | WRITE DISPLAY — touch `/dev/ttyACM1` at 1200 baud | DISPLAY `093C:2055` → absent → `2E8A:0003 RP2 Boot`, auto-mounts |
| 6.8 s | copy `bl_display.uf2` | |
| 7.7 s | `Success`, 2/2 steps | DISPLAY re-enumerates as `093C:2055 "FWOG display bl 001"` |

Both steps went through `GuardAction::TouchThenWait`, and both copies landed on
the mount point spelled `/run/media/drebbe/RPI-RP2` — the *same* path, because
MAIN's volume released and DISPLAY's then took the name. The release wait
between steps is the only thing that makes that safe, and this run is a live
instance of the case its comment describes.

**`LegacyDirect` also verified on Linux**, as the restore between phases:
`dropRedundantErases()` correctly dropped the DISPLAY erase (that CPU was
already in its bootloader), the 8.2 MB display image took 128 s and the 2.5 MB
main image 41 s, `Success` 2/2, and the board came back to `MainCPU v92` /
`DisplayCPU v67` with unchanged serials.

**The state the board was left in, and how it got there**, because the sequence
above does not by itself produce it and a reader checking the board against this
document should not have to guess. The bootloader plan's first step erases MAIN,
so immediately after it MAIN held nothing. A further `WRITE MAIN` from
`FreeWiliMainV92.uf2` was run to restore it. Final state, read from live sysfs:

    3-4.1.1  093c:2054  product=MainCPU v92          manufacturer=FreeWili
    3-4.1.2  093c:2055  product=FWOG display bl 001  manufacturer=FreeWili OG
    3-4.1.3  0403:6014  product=FreeWili             serial=FW4852

Serials unchanged throughout on all three. That DISPLAY line is the evidence
that the bootloader install worked: `FWOG display bl 001` is the bootloader's
own USB identity, and only the bootloader publishes it.

**An observation worth checking before trusting a 30 s budget.** After
`flash_nuke.uf2`, the erased MAIN CPU took a long time to come back as
`2E8A:0003`. What was actually timed: it was still absent when checked
immediately after the plan reported success, and was still absent through a
further 18 s of polling, appearing on the next 2 s sample. The interval from the
write itself was **not** timed, so the honest bound is "well over 20 s", not a
figure.

Nothing in this pass depended on it — the bootloader plan's erase is the last
thing that touches MAIN, and the DISPLAY step that follows waits on a different
CPU. `GuardAction::WaitForEraseReboot` allows `kVolumeWaitMs` (30 s) for exactly
this event.

**A plan of that shape does ship, and this is the one thing in this document
that should worry someone.** An earlier draft of this section said no such plan
exists; that was wrong. `freewili-original-deprecated` — a `defaultFirmware`
entry, offered on the Default Firmware tab — is `LegacyDirect` with three
assets, and `buildFlashPlan` orders it **ERASE DISPLAY → WRITE DISPLAY → WRITE
MAIN**. The project's own test at `tests/test_fwFlashPlan.cpp:244` pins that
sequence against the real compiled-in catalog. So the erase-then-write-the-same-
CPU race is not hypothetical: it is a shipping path, and it runs on DISPLAY,
the CPU with no BOOTSEL button.

What happens there: `fwFlashEngine.cpp:388` sets `expectedFromPriorErase`,
`classifyVolumes()` returns `ExpectedAfterErase`, and step 2 waits under
`WaitForEraseReboot` — 30 s — for a `flash_nuke` reboot measured here at "well
over 20 s". If it loses that race the outcome is `FlashOutcome::Timeout`, on a
CPU whose firmware the app has just destroyed.

The run recorded above dodged it only by accident: `dropRedundantErases()`
removed the erase because DISPLAY was already sitting in its bootrom. On the
ordinary running board this entry exists for, the erase is kept and the race is
real.

**Still not timed properly**, and that is the gap: the interval was measured
from the wrong starting point, so the margin against 30 s is unknown — it could
be 8 s or it could be negative. Anyone touching this path should time the erase
reboot from the write itself before trusting `kVolumeWaitMs`.

**Not established here:** what the physical display panel shows in any of these
states (nobody looked at the screen), and whether the DISPLAY bootloader console
behaves differently under an OG main image than under `FreeWiliMainV92` — with
v92 running, the bootloader console enumerated within 10 s and stayed up for at
least 60 s, but v92 is not an OG image and may not speak the inter-CPU protocol
the "~10 s of MAIN silence" rule is about.

---

## Not verified anywhere

- **Linux** — the identification, probe and flash paths are verified against
  hardware; see the section above. `ctest --preset linux-gcc-release` is green
  at **551 cases / 2170 assertions**, warning-clean at `-Wall -Wextra`. The one
  path still unexercised on Linux is the App Explorer **`OgApp` flash** — the
  everyday one — because there is still no known-good OG app UF2 on this
  machine. Both halves of the remote catalog's libcurl `dlopen` *have* been run,
  including the libcurl-absent half, forced under a mount namespace with the
  sonames made unresolvable; `fwHttp.cpp`'s own VERIFICATION STATUS block lists
  exactly what was exercised and what was not.
- **Linux, and what the app looks like** — the app has been launched and driven
  on Linux, but only on a private `Xvfb` display, because the machine's owner is
  on a Wayland session that must not be disturbed. So the X11 path is exercised
  and the **Wayland path is not**: no window has been mapped on a real
  compositor. See README.md, "What is and is not verified on Linux".
- **Linux, as it stood before the hardware pass** — kept as a record of what was
  once claimed, not as a current statement. At that point the preset compiled
  and unit-tested only: no `RPI-RP2` volume discovered, no 1200-baud touch
  performed, no UF2 written, and the app never launched. Every line of that has
  since been overtaken by the section above.
- **Emscripten / web** — never compiled, by explicit decision. See
  `web/README.md` for what a person with emsdk should try first, including
  the COOP/COEP headers `-pthread` requires.

---

## 2026-08-18 — every flash path, from every board state (VERIFIED)

Board: FreeWili OG, MAIN chip `E463A8574B5D3D35`, DISPLAY chip
`E463A8574B183D35`, FTDI serial never enumerating under OG firmware
(`Unknown` for the board's whole working life — the reason the GUI could not
flash it at all before this pass; see `BoardFingerprint`, fwDeviceModel.h).

Everything below was run through **both** `fwogcli` and the GUI unless noted,
against `build/win-msvc-release`, and each ended with both CPUs running the
OG app and the display bootloader reported present.

| # | Starting state | Operation | Result |
|---|---|---|---|
| 1 | both CPUs running an OG app | flash an OG app | OK — DISPLAY parked in BOOTSEL first, MAIN touched, written by delta wait, DISPLAY brought back by the new MAIN firmware |
| 2 | MAIN in BOOTSEL (by touch), DISPLAY running | flash an OG app | OK — MAIN's drive chosen by hub port with the DISPLAY drive mounted beside it |
| 3 | DISPLAY in BOOTSEL, MAIN running | flash an OG app | OK — no prep needed, MAIN touched |
| 4 | both CPUs in BOOTSEL (no serial, no chip id) | flash an OG app | OK — MAIN's drive chosen by hub port; the fingerprint gate does not refuse a silent board |
| 5 | both running an OG app | install display bootloader | OK — ERASE MAIN, WRITE DISPLAY (both by touch) |
| 6 | MAIN blank, DISPLAY running the bootloader | flash an OG app | OK (GUI and CLI) — bootloader console touched into BOOTSEL first, MAIN's blank drive written by hub port |
| 7 | both running an OG app | install the ORIGINAL firmware (ERASE DISPLAY, WRITE DISPLAY, WRITE MAIN) | OK — 3 min 13 s (a 16 MB display image over the bootrom); chip ids unchanged under the legacy firmware |
| 8 | original firmware on both CPUs | install display bootloader | OK — legacy CDC ports touched, bootloader restored |
| 9 | DISPLAY erased (blank), MAIN running | install display bootloader | OK — step 2 waited ("waiting for CPU") until the blank DISPLAY's drive appeared, then wrote it by hub port |
| 10 | both CPUs erased | install display bootloader, then an OG app | OK — every drive resolved by hub port; MAIN's slow-to-appear blank drive was waited for |

Observations worth keeping:

- **The FTDI serial is not a usable board identity on this hardware.** It did
  not enumerate under the OG app, the OG bootloader, or the original v92/v67
  firmware in this session. RP2040 chip ids (the CDC serial) are stable across
  every firmware above and are what the app now uses.
- **A blank RP2040 does not always re-enumerate immediately.** After an erase
  the erased CPU's drive was seen to take several seconds to appear (state 10:
  "main not detected" straight after `erase-main-cpu`); the engine's identify
  wait (kIdentifyWaitMs) is what makes the following step succeed.
- **The freshly booted MAIN firmware resets the DISPLAY.** A DISPLAY parked in
  BOOTSEL ahead of a MAIN install came back running its app every time without
  any action from the host, so the parking needs no undo step.
- **Attribution by timing alone is not enough after an erase.** MAIN's blank
  drive re-enumerating a second after the DISPLAY was touched lands inside the
  "wait for the drive that appeared" window; the wait now checks each arrival
  against the live hub-position identity and skips a drive that belongs to the
  other CPU (`waitForNewVolume`, fwFlashEngine.cpp).

---

## 2026-08-14 — macOS: the OgApp flash, end to end (VERIFIED, pre-v2 code)

Board: the same FreeWili 1-OG, serial FW4300, attached to an arm64 Mac
(macOS 26.6, Apple clang, `build/mac-clang-release`). This is the pass the
macOS port's claims rest on, and it ran the one flow Linux never did:

- **App Explorer `OgApp` flash, end to end, through the GUI** — the 1200-baud
  touch on `cu.usbmodem*`, `RPI-RP2` discovered under `/Volumes`, the copy
  with `F_FULLFSYNC`, MAIN provisioned, and the display bootloader carrying
  the embedded DISPLAY image across the inter-CPU link — confirmed by both
  CPUs re-enumerating with the new app's product strings.
- Device detection and CPU identification by hub position, through IOKit.
- Product strings read from the IO registry (fwfinder_mac leaves `_raw`
  empty, so the registry is the only honest source on this platform).
- The clean DiskArbitration unmount-before-write: "Disk Not Ejected
  Properly" confirmed present before the fix and gone after it, on real
  flashes.

The session surfaced four defects, each fixed and re-verified on the board
in the same session; the `fix(macos)` commit carries the measurements
(interface numbers, the flush-is-the-trigger unmount finding).

Not run on macOS in that pass: the display-bootloader install and
`LegacyDirect` restore (the board's bootloader was already in place and
wanted alive), the CPU-prober recovery flow, and two boards at once.

## 2026-08-21 — macOS, rebased onto v2 (build and suite re-verified; flash NOT re-run)

The macOS branch was rebased onto v2 — which rewrote flash sequencing
(`fwFlashPrep`), added `fwogcli` as a full front-end, and made the remote
catalog the first-launch default — all after the 2026-08-14 board pass.
What a pass believed at the time is part of the record, so, explicitly:
**every board-flash claim above was measured against pre-v2 code.**

Re-verified on the rebased branch, this machine, 2026-08-21:

- `mac-clang-release` and `mac-clang-debug` both configure, build and link
  **warning-clean**; `ctest` green on both: **580 cases / 2337 assertions**.
- `fwogcli` — its first macOS build ever — decodes the embedded entries
  (`entries`) and walks the empty bus without error (`list`, no board
  attached).
- `packaging/make_mac_app.sh` produces a bundle that signs and passes
  `codesign --verify` (ad-hoc in this session; the Developer ID + notarize
  flow was proven 2026-08-14 and was not re-run).
- A first launch from a clean slate seeds the v2 default catalog URL and
  **fetches the remote catalog over the `dlopen`ed system libcurl**
  (`apps-cache.json` written and listed).

Not re-run against v2, because no board was attached to this machine at
rebase time: **any flash**. The platform arms v2's `fwFlashPrep` calls into
are byte-for-byte the ones the 2026-08-14 pass exercised, but this ledger
records runs, not reasoning — the first post-rebase flash belongs in a new
dated section here.

## 2026-08-21 — macOS: the post-rebase flash, run (VERIFIED, v2 code)

The section above ends by saying the first post-rebase flash belongs in a new
dated section. This is that section, run the same day against the branch tip
(the build the PR ships), board FW4300 attached to the same arm64 Mac -- MAIN
chip `E4622890471F4734`, DISPLAY chip `E4622890474B4434`.

The flow: `fwogcli flash ogvegas_main.uf2` -- v2's own front-end, its first
board flash ever driven from macOS -- with the image downloaded from the
published catalog (`docs.freewili.com/og-apps/uf2/ogvegas_main.uf2`, sha256
and size verified against `apps.json` before use; `fwogcli info` decoded its
embedded DISPLAY half first). Starting state: both CPUs running, display
bootloader present.

What v2's sequencing did on macOS, observed step by step: the prep parked the
DISPLAY CPU in BOOTSEL (its drive auto-mounted at `/Volumes/RPI-RP2`), MAIN
was touched at 1200 baud, and MAIN's drive mounted at **`/Volumes/RPI-RP2 1`**
-- both bootrom volumes mounted simultaneously, which means the
space-in-the-mount-path spelling escapeMount() exists for was exercised on
real hardware, not just in its round-trip test. The copy wrote to the
space-named volume; 24.8 s port-to-port for the 1,049,088-byte image. Both
CPUs re-enumerated as the new app -- IO registry product strings
`FWOG main ogvegas 001` and `FWOG display ogvegas 001` -- and the display
bootloader still reports present.

So the one caveat the two sections above carried is closed: the flash path has
now been run against v2's `fwFlashPrep` sequencing on macOS, through the
platform arms as this branch ships them (getmntinfo_r_np mount scan, the
statfs re-proof before the copy, the DiskArbitration unmount-before-write).
Run through `fwogcli`; the GUI drives the identical engine, and the GUI
end-to-end flash remains the 2026-08-14 entry's evidence.
