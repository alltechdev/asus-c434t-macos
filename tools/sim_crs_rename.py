"""Approximate OpenCore's Base-scoped _CRS -> XCRS rename on a DSDT binary, for
offline load testing with acpiexec.

For each device NameSeg given, finds `Device (<name>)` in the AML (ExtOpPrefix
0x5B, DeviceOp 0x82, PkgLength, NameSeg) and renames the first `_CRS` NameSeg
after it, as OpenCore does when Base points at <device>._CRS.

usage: python3 -I sim_crs_rename.py IN.aml OUT.aml HPET TIMR
"""
import sys

data = bytearray(open(sys.argv[1], "rb").read())
for name in sys.argv[3:]:
    seg = name.encode().ljust(4, b"_")
    pos = -1
    for i in range(len(data) - 8):
        if data[i] == 0x5B and data[i + 1] == 0x82:
            lead = data[i + 2]
            nbytes = lead >> 6
            nseg = i + 3 + nbytes
            if data[nseg:nseg + 4] == seg:
                pos = nseg
                break
    if pos < 0:
        sys.exit(f"device {name} not found")
    crs = data.find(b"_CRS", pos)
    data[crs:crs + 4] = b"XCRS"
    print(f"{name}: device at {pos:#x}, _CRS renamed at {crs:#x}")

# Fix the table checksum (byte 9) so acpiexec accepts it.
data[9] = 0
data[9] = (-sum(data)) & 0xFF
open(sys.argv[2], "wb").write(data)
