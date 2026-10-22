# macOS Sonoma on the ASUS Chromebook Flip C434T

OpenCore EFI, ACPI patches and notes for running **macOS Sonoma** natively on the
ASUS Chromebook Flip C434TA (board **rammus / shyvana**) with MrChromebox UEFI
firmware, installed on and booting from the **internal eMMC**, no USB needed.

| | |
|---|---|
| Model | ASUS Chromebook Flip C434TA (C434T) |
| CPU / GPU | Intel Core m3-8100Y (Amber Lake) / UHD Graphics 615 |
| RAM / storage | 4 GB / 64 GB eMMC |
| Firmware | MrChromebox UEFI Full ROM (tested 2408.1) |
| macOS | Sonoma 14.8, SMBIOS `MacBookAir8,1` |
| Bootloader | OpenCore 1.0.8 |

> [!IMPORTANT]
> **Everything in this repo assumes the Chromebook's stock firmware has already been
> replaced.** Stock ChromeOS firmware cannot boot OpenCore. Before anything here:
>
> 1. **Disable firmware write protection.** The C434T uses CR50 write protection:
>    unplug the battery inside and run on the charger (see MrChromebox's
>    [write-protect guide](https://docs.mrchromebox.tech/docs/firmware/wp/disabling.html)).
> 2. **Flash MrChromebox's UEFI Full ROM** with the
>    [Firmware Utility Script](https://docs.mrchromebox.tech/docs/fwscript.html), from
>    ChromeOS in developer mode ([getting started](https://docs.mrchromebox.tech/docs/getting-started.html)).
>
> This replaces ChromeOS and is at your own risk. Let the script back up the stock
> firmware, and read [unbricking](https://docs.mrchromebox.tech/docs/support/unbricking/) first.

## Status

| Feature | Status | Notes |
|---|---|---|
| Internal eMMC (boot + install) | Working | EmeraldSDHC + `SSDT-HPET` (see [FINDINGS](docs/FINDINGS.md#1-emmc-invisible-to-macos-hpet-took-its-interrupt)) |
| Graphics acceleration | Working | Metal 3, `-igfxnotelemetryload` + forced eDP link rate |
| Brightness | Working | `SSDT-PNLF` (full range: `_UID` 16, PWM max set to 0x56C) |
| Wi-Fi (Intel 7265) | Working | itlwm (shows as Ethernet) + [HeliPort](https://github.com/OpenIntelWireless/HeliPort) to pick networks; joins the networks from `--wifi` at boot |
| Bluetooth (Intel 7265) | Working | IntelBluetoothFirmware + BlueToolFixup + NVRAM vars |
| iMessage / FaceTime | Untested | Apple Push connects (itlwm, valid SMBIOS, ROM = Wi-Fi MAC). The test Apple ID was blocked by Apple, so sign-in is not confirmed with a clean account. See [FINDINGS](docs/FINDINGS.md#other-notes) |
| Keyboard | Working | VoodooPS2 + `kext/C434TKeyboard`: every key as printed, Mac modifiers, search layer. See [Keyboard](#keyboard). |
| Touchpad | Working | VoodooI2CELAN, polling mode |
| Touchscreen | Working | VoodooI2CHID |
| USB-A, both USB-C | Working | No map needed |
| Webcam | Working | Standard UVC |
| Battery | Working | ECEnabler + SMCBatteryManager |
| Speakers | Working | Own driver: `kext/C434TAudio` (Intel cAVS DSP + MAX98927 amps) and the CoreAudio plug-in in `plugin/`. See [Audio](#audio). |
| Microphones | Working | DMIC array through the same driver; separate **Built-in Microphone** device with input volume |
| Headphone jack | Not yet | DA7219 headset codec, see [docs/DA7219.md](docs/DA7219.md) |
| Keyboard backlight | Working | alt + brightness keys, through the Chrome EC (`C434TKeyboard`, `SSDT-PS2-KEYS`); off after a minute idle |
| Sleep | Working | Lid close/open; audio stops before sleep and resumes after wake |

## Audio

The C434T's speakers hang off Intel's Skylake/Kaby Lake audio DSP (cAVS 1.5) and two
MAX98927 amplifiers on I2C, a combination macOS has no driver for. This repo has one:

- `kext/C434TAudio` boots the DSP firmware (Intel's `dsp_basefw.bin`, embedded), builds
  the speaker pipeline from Intel's MAX98927 topology (host DMA -> peakvol -> SSP0 with
  the board's NHLT I2S blob), configures both amps over I2C5 through VoodooI2C, and
  shares the DMA ring with user space.
- The same kext runs the two digital microphones (DMIC pipeline into a capture ring),
  stops cleanly before sleep and restarts the DSP after wake.
- `plugin/` is a CoreAudio HAL plug-in (`/Library/Audio/Plug-Ins/HAL/C434TAudio.driver`)
  that publishes **Built-in Speakers** (volume, mute) and **Built-in Microphone** (input
  volume) like a Mac, both on the kext's one stream and clock, and moves the system mix
  and mic input through those rings.

Install the plug-in on the Mac (needs Xcode Command Line Tools): `sh plugin/build.sh --install`.

## Keyboard

`kext/C434TKeyboard` gives the Chromebook keyboard a Mac layout: the top row does what
is printed on it, the modifiers sit where a Mac's are (Command next to the space bar),
and **search** works like on ChromeOS, as a key you hold for the extras.

| Key | Alone | With search held |
|---|---|---|
| back, forward, reload | Back, Forward, Reload (⌘[ ⌘] ⌘R) | F1, F2, F3 |
| full screen, overview | Full screen (⌃⌘F), Mission Control (⌃↑) | F4, F5 |
| brightness down/up, mute, volume down/up | Brightness down/up, mute, volume down/up | F6 to F10 |
| ctrl | ⌃ Control | |
| alt | ⌘ Command | Caps Lock (toggle) |
| right alt, right ctrl | ⌘ Command, ⌥ Option | |
| shift | ⇧ Shift | |
| left, right, up, down | arrows | Home, End, Page Up, Page Down |
| backspace | delete | forward delete |
| L | L | lock screen (⌃⌘Q) |
| any other key | itself | ⌥ Option + that key (e.g. search+e for accents) |
| search, tapped alone | Spotlight (⌘Space) | |
| alt + brightness down/up | keyboard backlight down/up | |

Mac shortcuts use ⌘ where ChromeOS uses ctrl, so on this keyboard they are on alt:
alt+c / alt+v copy and paste, alt+tab switches apps, alt+w closes a window, alt+q
quits. alt+backspace deletes to the start of the line; right ctrl+backspace (⌥⌫)
deletes a word, and right ctrl+arrows (⌥←/→) move by word.

Turning on System Settings → Keyboard → Keyboard Shortcuts → Function Keys → "Use F1,
F2, etc. keys as standard function keys" swaps the two top-row columns. Modifiers can
still be rearranged in System Settings → Keyboard → Keyboard Shortcuts → Modifier Keys.

## Repository layout

```
ESP/                    contents of the EFI partition (placeholder SMBIOS!)
  EFI/BOOT/BOOTx64.efi    OpenCore bootstrap (firmware fallback path)
  EFI/OC/                 OpenCore, kexts, SSDTs, config.plist
  EFI/OC/config-vesa.plist  unaccelerated fallback config
  startup.nsh             "Restore safe graphics" script (see below)
acpi/                   SSDT sources (.dsl) and compiled tables (.aml)
tools/build_efi.py      builds ESP/ from deps/ + acpi/ (the source of truth)
tools/fetch_deps.sh     downloads the pinned OpenCore and kext releases
tools/recovery-channel/ drive macOS Recovery over Wi-Fi (no SSH there)
kext/C434TAudio/        audio driver kext (DSP, speaker path, amps), buildable on Linux
kext/C434TKeyboard/     Chromebook keyboard layout on top of VoodooPS2
kext/C434TeMMCIRQ/      diagnostic kext used to find the eMMC interrupt problem
plugin/                 CoreAudio HAL plug-in (speakers and microphone)
docs/FINDINGS.md        root causes and how each was fixed
```

## Install

Start with write protection disabled and MrChromebox's **UEFI Full ROM** flashed
(see the note at the top). Everything below can be done without a USB stick if the Chromebook
already runs Linux; otherwise put the same files on a FAT32 USB drive.

1. **SMBIOS.** The committed `config.plist` has placeholders. Generate a
   `MacBookAir8,1` serial and MLB with OpenCore's `macserial`, a random UUID, and
   use the Wi-Fi MAC (no colons) as ROM:

   ```sh
   sh tools/fetch_deps.sh
   deps/OpenCore-1.0.8-DEBUG/Utilities/macserial/macserial.linux -m MacBookAir8,1 -n 1
   cat > smbios.json <<EOF
   {"serial": "<serial>", "mlb": "<mlb>", "uuid": "$(uuidgen | tr a-z A-Z)", "rom": "<wifi mac>"}
   EOF
   echo '{"ssid": "<your network>", "password": "<its password>"}' > wifi.json
   python3 tools/build_efi.py --smbios smbios.json --wifi wifi.json   # -> out/ESP/
   ```

   The Wi-Fi driver is itlwm, which joins the networks from `wifi.json` by itself
   at boot (Recovery has no network picker). `smbios.json`, `wifi.json` and `out/`
   stay out of git; do not publish them.

2. **Recovery image.** Download Sonoma recovery from Apple:

   ```sh
   python3 deps/OpenCore-1.0.8-DEBUG/Utilities/macrecovery/macrecovery.py \
     -b Mac-827FAC58A8FDFA22 -m 00000000000000000 -o com.apple.recovery.boot download
   ```

3. **EFI partition.** Copy `out/ESP/*` and the `com.apple.recovery.boot` folder
   to the root of the eMMC's EFI partition (from Linux: `/boot` or `/boot/efi`),
   and add a boot entry, e.g.
   `efibootmgr -c -d /dev/mmcblk0 -p 1 -L OpenCore -l '\EFI\OC\OpenCore.efi'`.
   OpenCore needs about 35 MB, the recovery image about 800 MB.

4. **Install.** Boot OpenCore, pick the recovery entry (Space shows hidden
   entries). In Disk Utility, *View > Show All Devices*, erase only the Linux
   partition (**not** the whole disk, or the EFI partition with OpenCore goes
   too) as APFS, then *Reinstall macOS Sonoma*. It downloads ~13 GB over Wi-Fi
   and reboots a few times; pick *macOS Installer*, then *Macintosh HD*.

5. **After install.** Install [HeliPort](https://github.com/OpenIntelWireless/HeliPort)
   to see and change Wi-Fi networks (macOS itself shows the card as Ethernet), and
   the audio plug-in (`sh plugin/build.sh --install`).

## If a graphics change ever hangs the boot

Pick **Restore safe graphics** in the OpenCore menu. It runs `startup.nsh`, which
copies `EFI/OC/config-vesa.plist` (`-igfxvesa`, unaccelerated) over
`config.plist` and reboots. The previous config is kept as
`config-last-failed.plist`.

## Rebuilding

`tools/build_efi.py` generates the whole OpenCore folder and `config.plist`;
edit it rather than the plist. `python3 tools/build_efi.py` refreshes the
committed `ESP/` (placeholders only); `--smbios` writes your real build to
`out/ESP/`. Validate with
`deps/OpenCore-1.0.8-DEBUG/Utilities/ocvalidate/ocvalidate.linux <config.plist>`.
SSDTs compile with ACPICA `iasl`.

## Credits

- [acidanthera](https://github.com/acidanthera): OpenCore, Lilu, VirtualSMC,
  WhateverGreen, EmeraldSDHC, VoodooPS2, BrcmPatchRAM, MacKernelSDK
- [OpenIntelWireless](https://github.com/OpenIntelWireless): itlwm, HeliPort,
  IntelBluetoothFirmware
- [VoodooI2C](https://github.com/VoodooI2C/VoodooI2C), [1Revenger1/ECEnabler](https://github.com/1Revenger1/ECEnabler)
- [MrChromebox](https://mrchromebox.tech/) firmware
- [olm3ca/PixelbookOSX](https://github.com/olm3ca/PixelbookOSX) (KBL-Y + coreboot
  graphics notes) and [YukiRinLL/C302CA-Sonoma-Hackintosh](https://github.com/YukiRinLL/C302CA-Sonoma-Hackintosh)
  (predecessor board research)

## License

The code in this repo (`kext/`, `plugin/`, `acpi/`, `tools/`) is licensed under the
[GNU GPL v2](LICENSE). Third-party binaries in `ESP/` are unmodified upstream releases
under their own licenses. The Intel DSP firmware embedded in `C434TAudio.kext` is
redistributed under [`LICENCE.IntcSST2`](kext/C434TAudio/LICENCE.IntcSST2).

Running macOS on non-Apple hardware is against Apple's macOS license terms; this is a
personal hardware project.
