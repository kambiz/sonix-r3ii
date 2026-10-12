"""Reads the ISO 9660 .upt firmware image of the pre-2025 HiBy R3 II.

    upt.py extract IMAGE DIR   unpack every file into DIR
    upt.py list IMAGE          print each file with its size and md5

The image is plain ISO 9660 with no Rock Ridge or Joliet names, as
genisoimage writes it from a directory of lower-case names: "uimage.bin"
is stored as "UIMAGE.BIN;1" and ".gitignore" as "_GITIGNO.;1". extract
gives the files back their original names, so the directory can be handed
to genisoimage again unchanged.
"""

import hashlib
import io
import os
import sys

import pycdlib

ORIGINAL_NAMES = {"_GITIGNO.": ".gitignore"}


def files(iso):
    for root, _dirs, names in iso.walk(iso_path="/"):
        for name in names:
            yield root.rstrip("/") + "/" + name


def plain_name(iso_path):
    name = os.path.basename(iso_path).split(";")[0]
    return ORIGINAL_NAMES.get(name, name.lower())


def main(argv):
    if len(argv) < 3 or argv[1] not in ("extract", "list"):
        sys.exit(__doc__)
    iso = pycdlib.PyCdlib()
    iso.open(argv[2])
    try:
        if argv[1] == "extract":
            if len(argv) != 4:
                sys.exit(__doc__)
            os.makedirs(argv[3], exist_ok=True)
            for path in files(iso):
                if os.path.dirname(path) not in ("", "/"):
                    sys.exit("unexpected subdirectory in image: " + path)
                iso.get_file_from_iso(os.path.join(argv[3], plain_name(path)), iso_path=path)
        else:
            for path in files(iso):
                buf = io.BytesIO()
                iso.get_file_from_iso_fp(buf, iso_path=path)
                data = buf.getvalue()
                print(f"{len(data):>10} {hashlib.md5(data).hexdigest()} {path}")
    finally:
        iso.close()


if __name__ == "__main__":
    main(sys.argv)
