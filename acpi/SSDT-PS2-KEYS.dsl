// ASUS C434T keyboard: VoodooPS2 settings (RMCF on the PS/2 keyboard's ACPI
// device). The Chromebook layout itself (top-row actions, search layer,
// modifiers) is done by kext/C434TKeyboard on top of VoodooPS2's defaults.
//
// Function Keys Standard/Special are identity maps: having both makes VoodooPS2
// follow and publish HIDFKeyMode ("Use F1, F2, etc. keys as standard function
// keys"), which C434TKeyboard reads to swap the top row's two layers.
//
// KBLQ/KBLS read and set the keyboard backlight (percent) in the Chrome EC's
// RAM, like coreboot's \_SB.KBLT KBQC/KBCM, which macOS never sees because that
// device's _STA is 0. (Not named KK*: VoodooPS2 would grab those for itself.)
DefinitionBlock ("", "SSDT", 2, "C434T", "Ps2Keys", 0x00000000)
{
    External (_SB_.PCI0.PS2K, DeviceObj)
    External (_SB_.PCI0.LPCB.EC0_.KBLV, FieldUnitObj)

    Scope (\_SB.PCI0.PS2K)
    {
        Name (RMCF, Package ()
        {
            "Keyboard", Package ()
            {
                "Function Keys Standard", Package ()
                {
                    "3b=3b",
                },
                "Function Keys Special", Package ()
                {
                    "3b=3b",
                },
            },
        })

        Method (KBLQ, 0, Serialized)
        {
            Return (\_SB.PCI0.LPCB.EC0.KBLV)
        }

        Method (KBLS, 1, Serialized)
        {
            \_SB.PCI0.LPCB.EC0.KBLV = Arg0
        }
    }
}
