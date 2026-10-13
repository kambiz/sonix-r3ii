# Sonix on the pre-2025 HiBy R3 II

This branch is the start of a port of Sonix Player to the **original HiBy R3 II**
(sold 2023-2024). It is **not** the R3 II 2025 and **not** the R3 Pro II, which
use a different processor; nothing here applies to them, and nothing built for
them may be flashed on this one.

Status: **hardware survey**, and a first R3 II profile in Sonix that runs in
the simulator. No Sonix code runs on the device yet.

## What the stock 1.4 firmware says about the hardware

Read out of `r3ii.upt` (1.4, built 2024-12-20, 60 973 056 bytes,
sha256 `4da41665...0f54b2c`).

| | R3 II (pre-2025) | Sonix's R3 Pro II / R1, for comparison |
|---|---|---|
| SoC | Ingenic **X1000** (U-Boot: "Board: RS2 (Ingenic XBurst X1000 SoC)") | Ingenic X1600E |
| RAM | **32 MB** (`mem=32M` on the kernel command line) | more |
| Kernel | Linux **3.10.14**, Ingenic gcc 4.7.2 | 4.4.94 |
| Userland | Buildroot 2014.05, **glibc 2.16**, BusyBox 1.25 | glibc 2.22 |
| Storage | SPI NAND, UBI; rootfs is UBIFS on `ubi.mtd=4`, data on `ubi.mtd=5` | |
| DAC | ES9219 (ALSA codec driver `es9219`, "ES9219 Soft Mute" control) | CS43198 / CS43131 |
| Touch | Goodix GT9xx (`gt9xx`, `goodix_wq`) | Goodix / Hynitron |
| PMIC | AXP2101 (`axp2101_charger`, `axp2101-pek`) | AXP2101 |
| Display | Ingenic SLCD (smart panel), `/dev/fb0` | |
| Keys | `gpio_keys` and `adc_key` | |
| Wi-Fi/BT | CYW43438 (`cyw43438-7.46.58.35.bin`) | |

The CPU is the same MIPS32r2 little-endian ABI as the X1600E (ELF flags
`0x70001005` on both the stock player and a static musl build), so user-space
code ports; kernel modules do not.

### The firmware image

`r3ii.upt` is an ISO 9660 image (genisoimage, volume `CDROM`, no Rock Ridge)
of `version.txt`, `update.txt`, `uimage.bin` (kernel, gzip uImage),
`uboot.bin` and `system.ubi`. `update.txt` lists only the kernel and the
rootfs, each with an md5; U-Boot is shipped but not flashed. `system.ubi` is a
bare UBIFS image: 2048-byte pages, 126 976-byte LEBs, 449 of at most 480 LEBs
used, LZO, format version 4.

### How an update is installed

The stock player looks for `/mnt/sd_0/r3ii.upt`, writes `recovery` to
`/proc/jz/reset/reset` and reboots. U-Boot then starts its separate recovery
mode, which flashes the kernel and the rootfs from the card. The recovery
system is not part of the `.upt`, so a test image cannot overwrite it.

### How the player is started

`/etc/init.d/S92_03_start_music_player` runs `/ui_data/player` if it exists,
otherwise `/usr/bin/hiby_player.sh`, which starts `/usr/bin/hiby_player` and
reboots when it exits. The stock player draws to `/dev/fb0` through SDL 1.2
and plays through ALSA (`libasound`).

## What this means for Sonix

- Sonix's kernel, kernel modules and packer overlays are X1600E-only. The port
  keeps HiBy's 3.10 kernel and drivers and replaces only the player.
- The player has to run against glibc 2.16 (`DEVICE_GLIBC_MINOR` would be 16,
  not 22), or be linked static.
- **32 MB of RAM** is the biggest open question: the stock player, the kernel
  and everything else share it. Sonix's footprint has to be measured and cut
  (no Game Boy emulator, smaller caches) before it is worth a full build.
- The ES9219 mixer controls, the panel size and orientation, the touch range
  and the key codes are what `r3ii_probe` is for.

## What Compás's R3 II 2025 board tells us

[Compás](https://github.com/Starnished66/compas-player) has an experimental
`r3ii_2025` board. The 2025 model is the same body with an X1600E, and this
1.4 firmware ships 2025 resources too (`R3II_2025_fonts`,
`ot_devices_2025.json`). Each Compás value was checked against the 1.4
firmware before it was used:

| Compás `r3ii_2025` | Pre-2025 R3 II, from the 1.4 firmware | Used? |
|---|---|---|
| Panel 320x480 | Nine full-screen theme images are 320x480; `theme2/playing_plane` is a 320x310 cover over a 320x170 panel, Compás's 310/170 split | Yes |
| Volume ring on `sa-ring-keys`, KEY_RIGHT clockwise | The kernel registers `sa-ring-keys` ("ring left", "ring right"); the stock player opens it by that name | Yes |
| A Previous key | The kernel's keys: "play key", "next song key", "previous song key", "power key" | Yes |
| Buttons on `jz adc keyboard` and `md-gpio-keys` | Neither name is in the 3.10 kernel; it has `gpio-keys` and `adc_key` | No: own names, to confirm with the probe |
| DAC on ALSA card 0 | `ot_devices.json` puts the analog output on **card 1**; card 0 is S/PDIF, card 3 USB. Only `ot_devices_2025.json` says card 0 | No: card 1 |
| Charge voltage 4.40 V (AXP2101 enum 5) | No `axp2101.sh` here; the AXP2101 driver is built into the kernel and its setting is not known | No: Sonix's charge limiter stays off |
| One volume curve for every board (0.34 dB a step) | HiBy's HDB/MDB/LDB tables in `ot_devices.json` are byte-for-byte the R3 Pro II tables Sonix already uses | Sonix's own tables |

## The R3 II in Sonix

`sonix-player/src/system/device/sysinfo.c` has a "HiBy R3 II" row, picked
when `system-info.json` says `"device-name": "HiBy R3 II"`:

- **Panel:** 320x480.
- **Update file:** `r3ii.upt`.
- **Buttons:** `gpio-keys` and `adc_key`. With neither found, Sonix falls back
  to event0 and event2.
- **Volume ring:** `sa-ring-keys`. Its KEY_RIGHT and KEY_LEFT become volume
  up and down (`system.c`).
- **DAC controls:** on `hw:1` (`alsa-controls.c`).
- **ES9219 routes:** "Output Port Switch" is left where the driver puts it,
  because which value is which socket is not known yet.
- **No HBC3000:** the R3 Pro II's suspend and resume steps for it are
  skipped (`alsa_board_has_hbc3000()`).
- **Charge limiter:** off.
- **No serial prefix.**

The simulator runs it at 320x480 (`r3ii/sim/sonix-320x480.png`: home, track
list, Now Playing, clock setup). Everything works, but the interface was drawn
for 480 pixels wide:

- tile labels on the home and Music grids are cut off or hidden;
- titles are truncated to a few characters;
- the clock setup's day and year rollers are clipped;
- the Now Playing transport row is crowded.

Compás solved the same problem on the 2025 model by scaling its widths from a
480-pixel reference. Sonix needs the same treatment.

Still unknown, and asked of the probe:

- the input names of the buttons;
- the "Output Port Switch" values and the jack switches;
- which I2C bus has the AXP2101 and the ES9219;
- the charge voltage.

Not addressed yet:

- the default ALSA PCM: Sonix plays to "default", which on this board has to
  be card 1;
- glibc 2.16;
- 32 MB of RAM.

## Step 1: the hardware probe

`r3ii/probe/r3ii_probe.c` is a small static program. The test image is the
stock 1.4 firmware, unchanged except that `hiby_player.sh` runs the probe once
per boot before starting the stock player. For about a minute after boot:

1. The screen shows six colour bands with a white frame, a white square in a
   black box in the top-left corner, and a bar along the bottom that shrinks as
   time runs out.
2. Touching the screen draws white squares; every button press changes the
   colour of the border.
3. A quiet 1 kHz tone plays for two seconds.
4. Then the normal HiBy player starts.

It writes `probe.log`, `sysinfo.txt`, `events.txt` and `aplay.txt` to
`r3ii_probe/` on the microSD card.

### Getting the image

Each push to `r3ii-port` builds it in GitHub Actions
(`.github/workflows/r3ii-probe.yml`) and publishes a pre-release named
`r3ii-probe-N` with `r3ii.upt` attached. The build fetches the stock firmware
from the `kambiz/hiby` release, checks its sha256, rebuilds `system.ubi` with
the stock geometry, unpacks the result again and compares it file by file
(contents, modes, owners) with the tree it was made from before writing the
`.upt` (`r3ii/pack/build_probe_upt.sh`).

### Running it

1. Charge the player above 50 %.
2. Copy `r3ii.upt` from the pre-release to the **root** of the microSD card.
3. Install it the way an official update is installed (the firmware update
   option in the player's system settings), and let it reboot.
4. **Keep headphones off your ears** for the first minute: the tone is quiet
   (-30 dBFS), but the volume it plays at is whatever the mixer was left at.
5. While the bands are up: photograph the screen, touch all four corners and
   the middle, then press every button once, slowly, in a known order
   (for example: power, volume up, volume down, previous, play, next).
6. When the HiBy player is back, delete `r3ii.upt` from the card and collect
   the four files from `r3ii_probe/`.

### Going back to stock

Put the stock `r3ii.upt` from the `kambiz/hiby` 1.4 release on the card and
install it the same way.
