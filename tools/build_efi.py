"""Assemble the OpenCore EFI partition contents for the ASUS Chromebook Flip C434T
(board rammus/shyvana, MrChromebox UEFI firmware) running macOS Sonoma.

usage:
  python3 tools/build_efi.py                 # -> ESP/ with placeholder SMBIOS (committed)
  python3 tools/build_efi.py --smbios FILE   # -> out/ESP/ with your own SMBIOS (git-ignored)
                            [--wifi FILE]  # and Wi-Fi networks for itlwm to join at boot

FILE is JSON: {"serial": ..., "mlb": ..., "uuid": ..., "rom": "<12 hex digits>"}
(generate serial/MLB with OpenCore's macserial for MacBookAir8,1; rom = Wi-Fi MAC).
--wifi FILE is JSON {"ssid": ..., "password": ...} or a list of them; it is only
written into out/ESP, never into the committed ESP/.
Run tools/fetch_deps.sh first.
"""
import argparse, json, os, plistlib, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, "deps")
OC_PKG = os.path.join(DEPS, "OpenCore-1.0.8-DEBUG")

PLACEHOLDER_SMBIOS = {
    "serial": "REPLACE_WITH_SERIAL",
    "mlb": "REPLACE_WITH_MLB",
    "uuid": "00000000-0000-0000-0000-000000000000",
    "rom": "112233445566",
}

ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument("--smbios", help="JSON file with your own SMBIOS values")
ap.add_argument("--wifi", help="JSON file with Wi-Fi networks for itlwm (only with --smbios)")
args = ap.parse_args()
if args.smbios:
    SMBIOS = json.load(open(args.smbios))
    ESP = os.path.join(ROOT, "out", "ESP")
else:
    SMBIOS = PLACEHOLDER_SMBIOS
    ESP = os.path.join(ROOT, "ESP")
OUT = os.path.join(ESP, "EFI")
OC = os.path.join(OUT, "OC")

# (source dir under deps/, kext bundle name)
KEXT_SOURCES = [
    ("Lilu-1.7.2-RELEASE", "Lilu.kext"),
    ("VirtualSMC-1.3.8-RELEASE/Kexts", "VirtualSMC.kext"),
    ("VirtualSMC-1.3.8-RELEASE/Kexts", "SMCProcessor.kext"),
    ("VirtualSMC-1.3.8-RELEASE/Kexts", "SMCBatteryManager.kext"),
    ("ECEnabler-1.0.6-RELEASE", "ECEnabler.kext"),
    ("WhateverGreen-1.7.1-RELEASE", "WhateverGreen.kext"),
    ("EmeraldSDHC-0.1.2-RELEASE", "EmeraldSDHC.kext"),
    ("VoodooI2C-2.9.1-RELEASE", "VoodooI2C.kext"),
    ("VoodooI2C-2.9.1-RELEASE", "VoodooI2CHID.kext"),
    ("VoodooI2C-2.9.1-RELEASE", "VoodooI2CELAN.kext"),
    ("VoodooPS2Controller-2.3.7-RELEASE", "VoodooPS2Controller.kext"),
    # itlwm, not AirportItlwm: on macOS 14.4+ AirportItlwm cannot do iMessage/
    # FaceTime (Apple Push never comes up); itlwm shows the card as Ethernet.
    ("itlwm_v2.3.0_stable.kext", "itlwm.kext"),
    ("IntelBluetooth-v2.4.0", "IntelBluetoothFirmware.kext"),
    ("IntelBluetooth-v2.4.0", "IntelBTPatcher.kext"),
    ("BrcmPatchRAM-2.7.2-RELEASE", "BlueToolFixup.kext"),
    # Built from kext/C434TAudio with kext/build.sh (audio DSP bring-up).
    (os.path.join(ROOT, "kext", "build", "C434TAudio"), "C434TAudio.kext"),
    # kext/C434TKeyboard: Chromebook keyboard layout on top of VoodooPS2.
    (os.path.join(ROOT, "kext", "build", "C434TKeyboard"), "C434TKeyboard.kext"),
]

# Kernel/Add order. OpenCore does not inject PlugIns on its own: each plugin is
# listed separately, after its dependencies. VoodooInput comes from VoodooI2C
# only; the copy inside VoodooPS2 (and the PS2 mouse/trackpad plugins that need
# it) are left out because the touchpad is I2C.
ORDER = [
    "Lilu.kext",
    "VirtualSMC.kext",
    "SMCProcessor.kext",
    "SMCBatteryManager.kext",
    "ECEnabler.kext",
    "WhateverGreen.kext",
    "EmeraldSDHC.kext",
    "VoodooI2C.kext/Contents/PlugIns/VoodooI2CServices.kext",
    "VoodooI2C.kext/Contents/PlugIns/VoodooGPIO.kext",
    "VoodooI2C.kext/Contents/PlugIns/VoodooInput.kext",
    "VoodooI2C.kext",
    "VoodooI2CHID.kext",
    "VoodooI2CELAN.kext",
    "VoodooPS2Controller.kext",
    "VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Keyboard.kext",
    "itlwm.kext",
    "IntelBluetoothFirmware.kext",
    "IntelBTPatcher.kext",
    "BlueToolFixup.kext",
    "C434TAudio.kext",
    "C434TKeyboard.kext",
]

SSDTS = [
    ("SSDT-PLUG-CB.aml", "coreboot CPxx -> Processor CPU0-3 + plugin-type"),
    ("SSDT-EC-USBX.aml", "Dummy EC + USBX power properties"),
    ("SSDT-I2C-OFF.aml", "Hide I2C2-I2C4 under Darwin (I2C5: audio codecs)"),
    ("SSDT-PNLF.aml", "Backlight"),
    ("SSDT-HPET.aml", "HPET on IRQ 0/8 so GSI 21 is free for the eMMC"),
    ("SSDT-PS2-KEYS.aml", "VoodooPS2 RMCF: publish HIDFKeyMode for C434TKeyboard"),
]

DRIVERS = [
    ("OpenRuntime.efi", "OpenRuntime"),
    ("OpenHfsPlus.efi", "HFS+ (recovery BaseSystem.dmg)"),
    ("ResetNvramEntry.efi", "Reset NVRAM picker entry"),
]

BOOT_ARGS = "keepsyms=1 debug=0x100 -igfxnotelemetryload"
# Unaccelerated fallback, installed as config-vesa.plist and restored by startup.nsh.
BOOT_ARGS_VESA = "-v keepsyms=1 debug=0x100 -igfxvesa"

if os.path.exists(OUT):
    shutil.rmtree(OUT)
shutil.copytree(os.path.join(OC_PKG, "X64", "EFI"), OUT)
# Keep only what the config references.
for sub, keep in (("Drivers", {d for d, _ in DRIVERS}), ("Tools", {"OpenShell.efi"})):
    for f in os.listdir(os.path.join(OC, sub)):
        if f not in keep:
            os.remove(os.path.join(OC, sub, f))
for f in os.listdir(os.path.join(OC, "ACPI")):
    os.remove(os.path.join(OC, "ACPI", f))
for name, _ in SSDTS:
    shutil.copy(os.path.join(ROOT, "acpi", name), os.path.join(OC, "ACPI", name))

# The VoodooPS2 plugins left out of ORDER are removed from the bundle too, so
# nothing references a second VoodooInput.
for src, name in KEXT_SOURCES:
    shutil.copytree(os.path.join(DEPS, src, name), os.path.join(OC, "Kexts", name))
# itlwm joins the networks in its personality's WiFiConfig at boot (HeliPort can
# add more later). The release ships sample entries: replace them with ours, or
# with nothing for the committed placeholder build.
itlwm_plist = os.path.join(OC, "Kexts", "itlwm.kext", "Contents", "Info.plist")
info = plistlib.load(open(itlwm_plist, "rb"))
networks = []
if args.wifi and args.smbios:
    w = json.load(open(args.wifi))
    networks = w if isinstance(w, list) else [w]
info["IOKitPersonalities"]["itlwm"]["WiFiConfig"] = {
    f"WiFi_{i + 1}": {"ssid": n["ssid"], "password": n["password"]} for i, n in enumerate(networks)}
with open(itlwm_plist, "wb") as f:
    plistlib.dump(info, f, fmt=plistlib.FMT_XML)
ps2_plugins = os.path.join(OC, "Kexts", "VoodooPS2Controller.kext", "Contents", "PlugIns")
for p in ("VoodooInput.kext", "VoodooPS2Mouse.kext", "VoodooPS2Trackpad.kext"):
    shutil.rmtree(os.path.join(ps2_plugins, p))


def kext_entry(path):
    info = plistlib.load(open(os.path.join(OC, "Kexts", path, "Contents", "Info.plist"), "rb"))
    exe = info.get("CFBundleExecutable")
    return {
        "Arch": "x86_64", "BundlePath": path, "Comment": os.path.basename(path),
        "Enabled": True, "ExecutablePath": f"Contents/MacOS/{exe}" if exe else "",
        "MaxKernel": "", "MinKernel": "", "PlistPath": "Contents/Info.plist",
    }


def block(identifier, comment):
    return {"Arch": "Any", "Comment": comment, "Enabled": True, "Identifier": identifier,
            "MaxKernel": "", "MinKernel": "", "Strategy": "Disable"}


# Rename HPET._CRS and TIMR._CRS to XCRS so SSDT-HPET can replace them. A
# Base-scoped "_CRS" patch fails on these Name() objects ("failed to find base"),
# so match the NameSeg together with the start of each device's own resource
# buffer; both patterns occur exactly once in this DSDT.
def rename_crs(name, tail_hex):
    tail = bytes.fromhex(tail_hex)
    return {
        "Base": "", "BaseSkip": 0, "Comment": f"{name}._CRS -> XCRS",
        "Count": 1, "Enabled": True, "Find": b"_CRS" + tail, "Limit": 0, "Mask": b"",
        "OemTableId": bytes(8), "Replace": b"XCRS" + tail, "ReplaceMask": b"", "Skip": 0,
        "TableLength": 0, "TableSignature": b"DSDT",
    }


cfg = plistlib.load(open(os.path.join(OC_PKG, "Docs", "Sample.plist"), "rb"))

# ACPI
cfg["ACPI"]["Add"] = [{"Comment": c, "Enabled": True, "Path": p} for p, c in SSDTS]
cfg["ACPI"]["Delete"] = []
cfg["ACPI"]["Patch"] = [rename_crs("HPET", "11110a0e86090001"),
                        rename_crs("TIMR", "11180a15")]
cfg["ACPI"]["Quirks"]["ResetLogoStatus"] = True

# Booter: coreboot/Chromebook. DevirtualiseMmio must stay off on MrChromebox.
cfg["Booter"]["MmioWhitelist"] = []
cfg["Booter"]["Patch"] = []
cfg["Booter"]["Quirks"].update({
    "AvoidRuntimeDefrag": True, "DevirtualiseMmio": False, "EnableSafeModeSlide": True,
    "EnableWriteUnprotector": False, "ProtectMemoryRegions": True, "ProvideCustomSlide": True,
    "RebuildAppleMemoryMap": True, "SetupVirtualMap": True, "SyncRuntimePermissions": True,
})

# iGPU: UHD 615 (8086:591C) presented as Amber Lake UHD 617, the MacBookAir8,1
# framebuffer. coreboot preallocates 32MB DVMT, hence the stolenmem/fbmem patch.
cfg["DeviceProperties"]["Add"] = {
    "PciRoot(0x0)/Pci(0x2,0x0)": {
        "AAPL,ig-platform-id": bytes.fromhex("0000C087"),
        "device-id": bytes.fromhex("C0870000"),
        "model": "Intel UHD Graphics 615",
        "framebuffer-patch-enable": bytes.fromhex("01000000"),
        "framebuffer-stolenmem": bytes.fromhex("00003001"),
        "framebuffer-fbmem": bytes.fromhex("00009000"),
        # The eDP panel reports MAX_LINK_RATE 0 (eDP 1.4 link-rate table), so the
        # framebuffer computed zero link bandwidth ("doesStreamFitInDPLink:
        # ASSERT(linkBW) would fail") and the GPU hung. Force HBR (0x0A, 2.7 Gbps
        # per lane), enough for 1080p60 on two lanes.
        "enable-dpcd-max-link-rate-fix": bytes.fromhex("01000000"),
        "dpcd-max-link-rate": bytes.fromhex("0A000000"),
    }
}
cfg["DeviceProperties"]["Delete"] = {}

# Kernel
cfg["Kernel"]["Add"] = [kext_entry(p) for p in ORDER]
cfg["Kernel"]["Block"] = [
    block("com.apple.driver.AppleIntelLpssI2C", "VoodooI2C owns the SerialIO I2C controllers"),
    block("com.apple.driver.AppleIntelLpssI2CController", "VoodooI2C owns the SerialIO I2C controllers"),
]
cfg["Kernel"]["Force"] = []
cfg["Kernel"]["Patch"] = []
cfg["Kernel"]["Quirks"].update({
    "AppleCpuPmCfgLock": True, "AppleXcpmCfgLock": True, "DisableIoMapper": True,
    "PanicNoKextDump": True, "PowerTimeoutKernelPanic": True, "ThirdPartyDrives": True,
    "XhciPortLimit": False,
})

# Misc. Auxiliary entries (recovery, Reset NVRAM) are behind the Space key.
cfg["Misc"]["Boot"].update({"ShowPicker": True, "Timeout": 5, "PollAppleHotKeys": True,
                            "LauncherOption": "Disabled", "HideAuxiliary": True,
                            "PickerMode": "Builtin"})
# Logging off for speed; set Target to 67 to get opencore-*.txt on the ESP.
cfg["Misc"]["Debug"].update({"AppleDebug": False, "ApplePanic": True, "DisableWatchDog": True,
                             "Target": 0, "SysReport": False})
cfg["Misc"]["Security"].update({"ScanPolicy": 0, "SecureBootModel": "Disabled",
                                "Vault": "Optional", "AllowSetDefault": True,
                                "ExposeSensitiveData": 6, "HaltLevel": 2147483648,
                                "AuthRestart": False, "ApECID": 0})
cfg["Misc"]["Entries"] = []
# OpenShell runs startup.nsh from the ESP root, which copies config-vesa.plist
# over config.plist and reboots: a one-click way back from a graphics hang.
cfg["Misc"]["Tools"] = [{
    "Arguments": "", "Auxiliary": False, "Comment": "OpenShell + startup.nsh", "Enabled": True,
    "Flavour": "OpenShell:UEFIShell:Shell", "FullNvramAccess": False,
    "Name": "Restore safe graphics", "Path": "OpenShell.efi", "RealPath": False, "TextMode": False,
}]

# NVRAM
guid = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
nv = cfg["NVRAM"]["Add"][guid]
# -igfxnotelemetryload: without it the Amber Lake framebuffer hangs the CPU
# (MCE bank 4, internal timer error) right after IGPU init (PixelbookOSX needs
# the same on KBL-Y + coreboot).
nv["boot-args"] = BOOT_ARGS
nv["csr-active-config"] = bytes(4)
nv["prev-lang:kbd"] = b"en-US:0"
# Intel Bluetooth on macOS 12+ (BlueToolFixup README): without these bluetoothd
# keeps faulting and the controller reports address NULL / firmware v0 c0.
nv["bluetoothExternalDongleFailed"] = bytes(1)
nv["bluetoothInternalControllerInfo"] = bytes(14)
cfg["NVRAM"]["Delete"][guid] = ["boot-args", "csr-active-config", "prev-lang:kbd",
                                "bluetoothExternalDongleFailed",
                                "bluetoothInternalControllerInfo"]
cfg["NVRAM"]["WriteFlash"] = True

# PlatformInfo
cfg["PlatformInfo"]["Automatic"] = True
cfg["PlatformInfo"]["Generic"].update({
    "SystemProductName": "MacBookAir8,1",
    "SystemSerialNumber": SMBIOS["serial"],
    "MLB": SMBIOS["mlb"],
    "SystemUUID": SMBIOS["uuid"],
    "ROM": bytes.fromhex(SMBIOS["rom"]),
    "SystemMemoryStatus": "Auto",
})
cfg["PlatformInfo"]["UpdateDataHub"] = True
cfg["PlatformInfo"]["UpdateNVRAM"] = True
cfg["PlatformInfo"]["UpdateSMBIOS"] = True
cfg["PlatformInfo"]["UpdateSMBIOSMode"] = "Create"

# UEFI. MrChromebox's security policy refuses third-party drivers unless disabled.
cfg["UEFI"]["Drivers"] = [
    {"Arguments": "", "Comment": c, "Enabled": True, "HideVerbose": False,
     "LoadEarly": False, "Path": p}
    for p, c in DRIVERS
]
cfg["UEFI"]["APFS"]["EnableJumpstart"] = True
cfg["UEFI"]["Input"].update({"KeySupport": True, "KeySupportMode": "Auto"})
cfg["UEFI"]["Output"]["ProvideConsoleGop"] = True
cfg["UEFI"]["Quirks"].update({
    "DisableSecurityPolicy": True, "RequestBootVarRouting": True,
    "ReleaseUsbOwnership": False, "IgnoreInvalidFlexRatio": False,
})

with open(os.path.join(OC, "config.plist"), "wb") as f:
    plistlib.dump(cfg, f, fmt=plistlib.FMT_XML)
# The fallback also leaves out the in-development audio driver.
nv["boot-args"] = BOOT_ARGS_VESA
for entry in cfg["Kernel"]["Add"]:
    if entry["BundlePath"] == "C434TAudio.kext":
        entry["Enabled"] = False
with open(os.path.join(OC, "config-vesa.plist"), "wb") as f:
    plistlib.dump(cfg, f, fmt=plistlib.FMT_XML)
# UEFI Shell script at the ESP root, CRLF line endings.
with open(os.path.join(ROOT, "tools", "startup.nsh")) as src, \
        open(os.path.join(ESP, "startup.nsh"), "w", newline="\r\n") as dst:
    dst.write(src.read())
print("built", ESP)
