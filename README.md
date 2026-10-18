# Sonix for the HiBy R3 II (pre-2025)

A work-in-progress port of [Sonix Player](https://github.com/Jepl4r/sonix-player)
to the **original HiBy R3 II**, the model sold in 2023 and 2024.

> **Status: hardware survey.** Sonix does not run on this device yet. The only
> image so far is a test build: the stock HiBy 1.4 firmware with a small
> program that records how the hardware behaves. The music player is still
> HiBy's.

## Is this your player?

This fork is for one device only. Check yours before you install anything.

| | This fork | Not this fork |
|---|---|---|
| Model | HiBy R3 II, 2023-2024 | HiBy R3 II **2025**, HiBy R3 Pro II, HiBy R1 |
| Processor | Ingenic X1000 | Ingenic X1600E |
| Firmware file | `r3ii.upt` | `r3ii_2025.upt`, `r3proii.upt`, `r1.upt` |

If HiBy's update file for your player is `r3ii.upt`, you are in the right
place. For the R3 Pro II and the R1, use
[Sonix](https://github.com/Jepl4r/sonix-player) itself. For the R3 II 2025, see
[Compás](https://github.com/Starnished66/compas-player), which has an
experimental target for it.

**Never install a file built for another model.** The recovery system flashes
whatever it is given.

## Why a separate port

The original R3 II uses older hardware than every player Sonix supports.

| | R3 II (pre-2025) | R3 Pro II / R1 (Sonix) |
|---|---|---|
| Processor | Ingenic X1000 | Ingenic X1600E |
| Memory | 32 MB | more |
| Linux kernel | 3.10.14 (HiBy's) | 4.4.94 (custom) |
| C library | glibc 2.16 | glibc 2.22 |
| DAC | ESS ES9219 | Cirrus Logic CS43198 / CS43131 |
| Touch | Goodix GT9xx | Goodix / Hynitron |

Sonix replaces HiBy's kernel and drivers, and those are built for the X1600E.
This port keeps HiBy's own kernel and drivers and replaces only the player
program. The processors share an instruction set, so Sonix's interface and
playback code can be rebuilt for this device. The hard parts are fitting it in
32 MB of memory, and finding the right screen, touch, button and DAC settings.

[R3II.md](R3II.md) has the full technical notes on the stock firmware.

## Roadmap

1. **Hardware probe.** A test image that records the screen, touch, buttons
   and sound card. *Built; waiting for results from a real device.*
2. **Device profile.** Add the R3 II to Sonix's model table: panel size, touch
   range, key codes and ES9219 mixer controls. *Started: the 320x480 panel,
   the volume ring and the DAC's card, checked against Compás's R3 II 2025
   board, run in the simulator. See [R3II.md](R3II.md).*
3. **Memory budget.** Measure Sonix's footprint and cut it to fit in 32 MB
   next to the kernel and system services.
4. **First Sonix build.** Run the player against HiBy's 3.10 kernel and
   glibc 2.16, from a test image that falls back to HiBy's player.
5. **Daily use.** Playback, library, settings, Bluetooth and battery life.

## Trying the test image

Test images are built automatically and published under
[Releases](https://github.com/kambiz/sonix-r3ii/releases) as `r3ii-probe-N`
pre-releases. Each has a single file, `r3ii.upt`.

**What it does:** for about a minute after it boots, the screen shows colour
bands. Touches draw white squares, and button presses change the colour of the
border. A quiet tone plays for two seconds. Then HiBy's player starts as usual.
Nothing else in the firmware is changed. The kernel, HiBy's player and all
other files are identical to stock 1.4.

**What you need:** a microSD card, and a way to copy files to it. From an
iPhone, use a USB-C or Lightning SD card reader with the Files app.

**Steps:**

1. Charge the player above 50 %.
2. Download `r3ii.upt` from the latest `r3ii-probe` release and copy it to the
   **root** of the microSD card.
3. Install it the way you would install an official update: the firmware
   update option in the player's system settings. Let it reboot.
4. **Keep headphones off your ears for the first minute.** The tone is quiet,
   but it plays at whatever volume the player was last set to.
5. While the colour bands are showing:
   - take a photo of the screen;
   - touch all four corners and the middle;
   - press every button once, slowly, and turn the volume knob one click
     each way; write down the order you did them in.
6. When HiBy's player is back, delete `r3ii.upt` from the card.

The results are in the `r3ii_probe` folder on the card: `probe.log`,
`sysinfo.txt`, `events.txt` and `aplay.txt`.

**Sharing the results:** fill in the
[R3 II probe results](https://github.com/kambiz/sonix-r3ii/issues/new?template=r3ii_probe.yml)
form and attach the four files, the photo and your button order. This works
from Safari on a phone. Even "the update was refused" is worth reporting.

## Going back to stock

Install the stock HiBy 1.4 `r3ii.upt` the same way: put it in the root of the
card and use the firmware update option. A copy is in the
[1.4 release of kambiz/hiby](https://github.com/kambiz/hiby/releases/tag/1.4).
HiBy also publishes it in its own firmware folder on Google Drive.

The update only rewrites the kernel and the system partition. The recovery
system that installs updates lives outside them, so a test image cannot damage
it. That is how HiBy's update mechanism is built. It has not yet been tested on
this device with a modified image.

## Building

Everything is built by GitHub Actions in
[.github/workflows/r3ii-probe.yml](.github/workflows/r3ii-probe.yml) on every
push to `r3ii-port`. The workflow:

1. Compiles `r3ii/probe/r3ii_probe.c` as a static MIPS32r2 binary with
   `zig cc` (musl), so it needs nothing from the device's C library.
2. Downloads the stock 1.4 firmware and checks its SHA-256.
3. Runs `r3ii/pack/build_probe_upt.sh`, which:
   - unpacks the image and its UBIFS root filesystem;
   - adds the probe;
   - rebuilds the filesystem with the stock settings (2048-byte pages,
     126976-byte erase blocks, LZO, format 4);
   - unpacks the result again and compares every file, mode and owner with
     the tree it came from;
   - writes the new `r3ii.upt`.
4. Publishes the image as a pre-release.

To build locally on Linux, you need root, `mtd-utils`, `genisoimage`, and
Python with `ubi_reader` and `pycdlib`:

```sh
python3 -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -static -Os \
    -o r3ii_probe r3ii/probe/r3ii_probe.c -lm
sudo r3ii/pack/build_probe_upt.sh stock-r3ii.upt r3ii_probe r3ii.upt
```

No HiBy firmware or binaries are stored in this repository.

## Repository layout

| Path | What it is |
|---|---|
| `README.md` | This file |
| `R3II.md` | Technical notes on the R3 II and the stock firmware |
| `r3ii/probe/` | The hardware probe |
| `r3ii/pack/` | Unpacking and building `r3ii.upt` images |
| `r3ii/sim/` | Simulator screenshots of Sonix as the R3 II |
| `SONIX.md` | Sonix Player's own README: building, features, source tree |
| `sonix-player/`, `sonix-packer/` | Sonix Player 1.3.0, with the R3 II model added (see R3II.md) |

## Credits and license

- **[Sonix Player](https://github.com/Jepl4r/sonix-player)** by Jepl4r and
  contributors. This repository carries its full history.
- **[hiby_os_crack](https://github.com/hiby-modding/hiby_os_crack)** for the
  groundwork on HiBy's firmware format.
- **[Compás](https://github.com/Starnished66/compas-player)**, another
  open-source player for HiBy's X1600E models.

Licensed under the GPL-3.0, like Sonix. See [LICENSE](LICENSE).

This project is not affiliated with HiBy. Modifying your player's firmware is
at your own risk.
