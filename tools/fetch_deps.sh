#!/bin/sh
# Download the pinned OpenCore and kext releases into deps/<zip name>/.
# build_efi.py reads them from there. Re-running skips what is already present.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS=$ROOT/deps
mkdir -p "$DEPS"

GH=https://github.com
while read -r url; do
  [ -n "$url" ] || continue
  zip=$(basename "$url")
  dir=$DEPS/${zip%.zip}
  [ -d "$dir" ] && continue
  echo "fetching $zip"
  curl -fsSL "$url" -o "$DEPS/$zip"
  mkdir -p "$dir"
  unzip -qo "$DEPS/$zip" -d "$dir"
  rm "$DEPS/$zip"
done <<EOF
$GH/acidanthera/OpenCorePkg/releases/download/1.0.8/OpenCore-1.0.8-DEBUG.zip
$GH/acidanthera/Lilu/releases/download/1.7.2/Lilu-1.7.2-RELEASE.zip
$GH/acidanthera/VirtualSMC/releases/download/1.3.8/VirtualSMC-1.3.8-RELEASE.zip
$GH/acidanthera/WhateverGreen/releases/download/1.7.1/WhateverGreen-1.7.1-RELEASE.zip
$GH/acidanthera/EmeraldSDHC/releases/download/0.1.2/EmeraldSDHC-0.1.2-RELEASE.zip
$GH/acidanthera/VoodooPS2/releases/download/2.3.7/VoodooPS2Controller-2.3.7-RELEASE.zip
$GH/acidanthera/BrcmPatchRAM/releases/download/2.7.2/BrcmPatchRAM-2.7.2-RELEASE.zip
$GH/1Revenger1/ECEnabler/releases/download/1.0.6/ECEnabler-1.0.6-RELEASE.zip
$GH/VoodooI2C/VoodooI2C/releases/download/v2.9.1/VoodooI2C-2.9.1-RELEASE.zip
$GH/OpenIntelWireless/itlwm/releases/download/v2.3.0/itlwm_v2.3.0_stable.kext.zip
$GH/OpenIntelWireless/IntelBluetoothFirmware/releases/download/v2.4.0/IntelBluetooth-v2.4.0.zip
EOF
echo "deps ready in $DEPS"
