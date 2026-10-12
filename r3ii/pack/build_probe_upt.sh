#!/usr/bin/env bash
# Builds a test r3ii.upt for the pre-2025 HiBy R3 II: the stock 1.4 firmware
# with r3ii_probe added and run once per boot before the stock player.
#
#   build_probe_upt.sh STOCK_UPT PROBE_BINARY OUTPUT_UPT
#
# Needs root (to keep the owners of the stock files), python3 with
# ubi_reader and pycdlib, mkfs.ubifs (mtd-utils) and genisoimage.
#
# The .upt of this model is an ISO 9660 image (genisoimage, volume "CDROM")
# holding:
#
#   version.txt   name=r3ii and a build date
#   update.txt    what the recovery system flashes, with an md5 for each
#   uimage.bin    the kernel, Linux 3.10.14 (left untouched)
#   uboot.bin     U-Boot 2013.07 (not listed in update.txt, not flashed)
#   system.ubi    the root filesystem, a bare UBIFS image written into UBI
#                 volume "rootfs" (ubi.mtd=4)
#
# Only system.ubi is rebuilt. It is made with the stock image's geometry
# (2048-byte pages, 126976-byte LEBs, at most 480 LEBs, LZO, format 4: Linux
# 3.10 reads nothing newer), and the result is unpacked again and compared
# with the tree it came from before the .upt is written.

set -euo pipefail

[[ $# -eq 3 ]] || { echo "usage: $0 STOCK_UPT PROBE_BINARY OUTPUT_UPT" >&2; exit 2; }
stock=$(realpath "$1")
probe=$(realpath "$2")
output=$(realpath -m "$3")
here=$(cd "$(dirname "$0")" && pwd)

[[ $EUID -eq 0 ]] || { echo "run as root: the stock files are owned by uid 1009 and must stay so" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "== unpack $stock"
python3 -I "$here/upt.py" extract "$stock" "$work/iso"
( cd "$work/iso" && grep -A3 -E '^(kernel|rootfs)=' update.txt )
grep -q 'name=r3ii$' "$work/iso/version.txt" || { echo "not an R3 II image (version.txt)" >&2; exit 1; }
for part in kernel:uimage.bin rootfs:system.ubi; do
	want=$(awk -v p="${part%%:*}={" '$0==p{f=1} f&&/md5=/{sub(/.*md5=/,"");print;exit}' "$work/iso/update.txt")
	have=$(md5sum "$work/iso/${part#*:}" | cut -d' ' -f1)
	[[ "$want" == "$have" ]] || { echo "${part#*:}: md5 $have, update.txt says $want" >&2; exit 1; }
done

echo "== unpack system.ubi"
ubireader_extract_files -k -o "$work/rootfs" "$work/iso/system.ubi" >/dev/null
root="$work/rootfs"
[[ -x "$root/usr/bin/hiby_player" && -f "$root/usr/bin/hiby_player.sh" ]] || { echo "unexpected rootfs layout" >&2; exit 1; }

echo "== add r3ii_probe"
install -m 0755 -o 0 -g 0 "$probe" "$root/usr/bin/r3ii_probe"
# The probe goes in just before the stock player starts. It has its own
# 90-second alarm; whatever it does, the player starts after it.
python3 -I - "$root/usr/bin/hiby_player.sh" <<'EOF'
import sys
path = sys.argv[1]
s = open(path).read()
line = "\n/usr/bin/hiby_player\n"
assert s.count(line) == 1, "hiby_player.sh changed shape"
hook = (
    "\n# r3ii_probe: one-minute hardware survey, logs to /mnt/sd_0/r3ii_probe/\n"
    "[ -x /usr/bin/r3ii_probe ] && /usr/bin/r3ii_probe >/dev/null 2>&1\n"
)
open(path, "w").write(s.replace(line, hook + line))
EOF
tail -n 4 "$root/usr/bin/hiby_player.sh"

echo "== build system.ubi"
geometry=$(ubireader_display_info "$work/iso/system.ubi")
for want in "min_io_size: 2048" "leb_size: 126976" "max_leb_cnt: 480" "fmt_version: 4" "default_compr: 1"; do
	grep -q "$want" <<<"$geometry" || { echo "stock system.ubi no longer has $want" >&2; exit 1; }
done
stock_lebs=$(grep -m1 -oP 'leb_cnt: \K[0-9]+' <<<"$geometry")
mkfs.ubifs -r "$root" -m 2048 -e 126976 -c 480 -x lzo -o "$work/system.ubi"
built=$(ubireader_display_info "$work/system.ubi")
grep -q "fmt_version: 4" <<<"$built" || { echo "mkfs.ubifs wrote a format Linux 3.10 cannot read" >&2; exit 1; }
built_lebs=$(grep -m1 -oP 'leb_cnt: \K[0-9]+' <<<"$built")
echo "LEBs: stock $stock_lebs, built $built_lebs"
# The stock image fills 449 of its 480 LEBs; the probe adds about one. A much
# bigger image means something went wrong, and might not fit the volume.
(( built_lebs <= stock_lebs + 4 )) || { echo "image grew from $stock_lebs to $built_lebs LEBs" >&2; exit 1; }

echo "== check system.ubi against the tree"
ubireader_extract_files -k -o "$work/check" "$work/system.ubi" >/dev/null
diff -r --no-dereference "$root" "$work/check"
( cd "$root" && find . -printf '%p %m %U %G %y\n' | sort ) >"$work/meta.src"
( cd "$work/check" && find . -printf '%p %m %U %G %y\n' | sort ) >"$work/meta.chk"
diff "$work/meta.src" "$work/meta.chk"

echo "== write $output"
cp "$work/system.ubi" "$work/iso/system.ubi"
python3 -I - "$work/iso/update.txt" "$(md5sum "$work/iso/system.ubi" | cut -d' ' -f1)" <<'EOF'
import re, sys
path, md5 = sys.argv[1], sys.argv[2]
s = open(path).read()
s, n = re.subn(r"(rootfs=\{[^}]*?md5=)[0-9a-f]{32}", r"\g<1>" + md5, s)
assert n == 1
open(path, "w").write(s)
EOF
mkdir -p "$(dirname "$output")"
( cd "$work/iso" && genisoimage -quiet -V CDROM -sysid LINUX -o "$output" . )
python3 -I "$here/upt.py" list "$output"
md5sum "$output"
