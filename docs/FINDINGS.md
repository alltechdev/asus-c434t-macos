# What it took: root causes and fixes

Everything below was found on a real C434T (rammus/shyvana, m3-8100Y, 4 GB,
64 GB eMMC, MrChromebox UEFI 2408.1) running macOS Sonoma 14.8.

## 1. eMMC invisible to macOS: HPET took its interrupt

**Symptom.** Recovery boots, but `diskutil list physical` is empty. EmeraldSDHC
loads and attaches to the empty SD-card slot (`SDXC@1E,6`) instead of the eMMC.
`ioreg` shows the eMMC (`EMMC@1E,4`) with `IOInterruptSpecifiers = ()`.

**What it was not.**
- Not the interrupt pin. The OpenShell `pci 00 1E 04` dump shows
  `Interrupt Pin = 0x02` (INTB), and coreboot programs the eMMC as INTB/IRQ 21.
  (The ASUS C302CA, on older firmware, reportedly had pin 0; this board does not,
  and the eMMC has no MSI capability, so the C302CA "enableMSI" patch does not
  apply here.)
- Not `_PRT`. macOS uses the APIC table (`PICP`): the SD slot on the same device
  gets its GSI 23 from it.

**What it was.** A probe-only diagnostic kext (`kext/`) showed
`ResolvePCIInterrupt` succeeding with IRQ 21, then
`SetDeviceInterrupts(21) -> kIOReturnNoInterrupt`. Dumping `ioreg` showed the
HPET device holding GSIs 20, **21**, 11 and 12 as exclusive edge interrupts:
coreboot's `HPET._CRS` lists no IRQs, so macOS allocates the HPET timers itself
from the hardware's route capabilities.

**Fix.** `acpi/SSDT-HPET.dsl`: under Darwin, HPET gets legacy IRQ 0 and 8 and
the 8254 timer drops IRQ 0 (same idea as SSDTTime's FixHPET). The DSDT's
`HPET._CRS` and `TIMR._CRS` are renamed to `XCRS` by OpenCore ACPI patches.
After that the eMMC gets GSI 21 by itself and EmeraldSDHC works unmodified.
The PixelbookOSX guide independently uses FixHPET for its eMMC.

**Gotcha.** OpenCore ACPI patches with `Base` set to a `Name()` object
(`\_SB.PCI0.LPCB.HPET._CRS`) fail with `failed to find base` and are skipped,
which also makes the replacement SSDT fail to load (duplicate `_CRS`). The
patches match `_CRS` plus the first bytes of each device's own resource buffer
instead; both patterns are unique in this DSDT.

## 2. Hang/reset when graphics acceleration starts

**Symptom.** With the iGPU accelerated (UHD 615 `8086:591C` presented as Amber
Lake UHD 617, `AAPL,ig-platform-id 0000C087`, `device-id C0870000`), boot freezes
right after `[IGPU] Render Compression for IOSurface is Enabled`, or resets with
a Machine Check panic: `IA32_MC4_STATUS 0xbe00000000800400` (bank 4, MCACOD
0x0400: internal timer error, i.e. a core stuck on a transaction that never
completes).

**Causes and fixes**, both needed:
1. `-igfxnotelemetryload` (WhateverGreen boot-arg). Same requirement as the
   Pixelbook on KBL-Y + coreboot.
2. The eDP panel reports `MAX_LINK_RATE` 0 (eDP 1.4 link-rate table). The
   framebuffer computes zero bandwidth (`AppleGraphicsDevicePolicy:
   doesStreamFitInDPLink: ASSERT(linkBW) would fail!!!`) and hangs. WhateverGreen's
   auto-probe was not enough; forcing `dpcd-max-link-rate = 0x0A` (HBR, 2.7 Gbps
   per lane, plenty for 1080p60) together with `enable-dpcd-max-link-rate-fix`
   fixed it.

Result: Metal 3, 1536 MB dynamic VRAM, working brightness, wallpapers.

## 3. Bluetooth (Intel 7265, `8087:0A2A`) crash loop

**Symptom.** `bluetoothd` faults every ~20 s, Bluetooth shows address NULL and
firmware `v0 c0`.

**Fix.** The NVRAM variables from the BlueToolFixup README:
`bluetoothExternalDongleFailed = 00` and
`bluetoothInternalControllerInfo = 00 x 14`. Afterwards: state On, real address,
firmware v4352.

## Other notes

- **coreboot CPU objects.** CPUs are `Device(\_SB.CPxx)` with `ACPI0007` and no
  `Processor` block. `SSDT-PLUG-CB` hides them under Darwin and declares
  `Processor` CPU0-3 with `plugin-type=1` on CPU0.
- **USB works without a map.** ACPI declares 18 ports; macOS keeps the first 15,
  which covers every real port (HS01/SS01 Type-C 1, HS05/SS02 Type-C 2,
  HS02/SS03 Type-A, HS03 Bluetooth, HS09 camera; from coreboot's rammus
  devicetree).
- **I2C.** Touchscreen is a SiS HID panel on I2C0 (GSI 31, VoodooI2CHID). The
  touchpad is ELAN0000 on I2C1 (GSI 51, above the 47 VoodooI2C can use, so it
  runs in polling mode, VoodooI2CELAN). I2C2-I2C4 are hidden; I2C5 carries the
  audio codecs and is driven by VoodooI2C for `C434TAudio`.
- **Audio.** Own driver: `kext/C434TAudio` (cAVS DSP, MAX98927 amps, DMICs) plus
  the HAL plug-in in `plugin/`. Two sleep-related lessons:
  - Closing the lid turns the display off ~10 s before the system sleeps, and
    CoreAudio stops writing during that window without stopping IO. A DMA ring
    with no erase head then loops its last 85 ms through the speakers. The kext
    zeroes everything behind the DMA position every 10 ms.
  - On real sleep the HDA/DSP and the amps lose state. The kext uses
    `registerPrioritySleepWakeInterest`: stop on `SystemWillSleep` (before any
    driver powers down), reboot the DSP and restart on `SystemHasPoweredOn`,
    keeping the plug-in's sample timeline running forward.
  Headset codec notes for the jack: `docs/DA7219.md`.
- **Kexts on Linux.** See `kext/README.md` for building a working kext with
  clang + Apple ld64 + MacKernelSDK, no Mac needed. Every symbol a kext uses must
  come from a bundle in its `OSBundleLibraries`: `C434TKeyboard` once referenced
  `IOACPIPlatformDevice::metaClass` (via `OSDynamicCast`) without declaring
  `com.apple.iokit.IOACPIFamily`; the kext still loaded, with that pointer left
  unbound, and panicked in `start()` on every boot. Check with `llvm-nm -u`.
- **Keyboard.** `kext/C434TKeyboard` sits in VoodooPS2's key notification path
  (`RM,deliverNotifications` in, `kPS2K_notifyKeystroke` out) for the Chromebook
  layout, and drives the keyboard backlight through the EC byte `KBLV` (coreboot's
  `\_SB.KBLT` has `_STA` 0, so `SSDT-PS2-KEYS` adds `KBLQ`/`KBLS` on `PS2K`).
- **iMessage/FaceTime.** Two separate problems:
  - Apple refused Apple ID creation ("used too many times") on the first generated
    serial, so the identity was regenerated: a fresh serial + MLB from OpenCore's
    `macserial -m MacBookAir8,1`, ROM = the Wi-Fi MAC, en0 built-in. apsd then logged
    "Certificate doesn't match host" until its old push certificate
    (`/Library/Keychains/apsd.keychain`, `/Library/Preferences/com.apple.apsd.plist`)
    was moved away. (IOPlatformUUID does not follow `SystemUUID` here.)
  - Even then the Apple Push connection never opened: AirportItlwm leaves en0's link
    quality metric at "off" (-2), configd publishes
    `State:/Network/Interface/en0/LinkQuality = -2`, and apsd's interface usability
    monitor will not connect over an "off" interface ("no closed interfaces are
    usable"). Writing 100 to that key brought push up, but registration still failed
    (AuthKit SRP / anisette loops, IDS errors 17/31). The itlwm project says plainly
    that AirportItlwm does not do iServices on macOS 14.4+; the build now uses
    **itlwm** (the card appears as a built-in Ethernet en0, networks come from
    `build_efi.py --wifi` or HeliPort), with which push connects on its own.
  - Remaining: Apple answers IDS registration with 6001/6005 for the Apple ID used,
    i.e. the account is blocked for iMessage/FaceTime (Dortania: "Customer Code",
    fixed by Apple Support), not a machine problem. NVRAM persistence was verified.
- **Driving Recovery remotely.** Recovery has no SSH; `tools/recovery-channel/`
  is a tiny HTTP command channel (`curl ... | sh` polling loop) that was used to
  read `ioreg`, swap `config.plist` on the ESP and reboot, all over Wi-Fi.
