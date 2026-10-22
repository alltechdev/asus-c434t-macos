// ASUS C434T (rammus/shyvana), m3-8100Y 2c/4t, MrChromebox 2408.1
// coreboot declares CPUs as Device(\_SB.CPxx) with ACPI0007 and no Processor
// block, which AppleACPICPU does not enumerate. Under Darwin hide CP00-CP03 and
// declare Processor objects CPU0-CPU3 instead; CPU0 carries plugin-type=1 (XCPM).
// Adapted from YukiRinLL/C302CA-Sonoma-Hackintosh SSDT-PLUG-4200.
DefinitionBlock ("", "SSDT", 2, "C434T", "CpuPlug", 0x00000000)
{
    External (_SB_.CP00, DeviceObj)
    External (_SB_.CP01, DeviceObj)
    External (_SB_.CP02, DeviceObj)
    External (_SB_.CP03, DeviceObj)

    Scope (\_SB)
    {
        Scope (CP00)
        {
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (Zero) }
                Return (0x0F)
            }
        }

        Scope (CP01)
        {
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (Zero) }
                Return (0x0F)
            }
        }

        Scope (CP02)
        {
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (Zero) }
                Return (0x0F)
            }
        }

        Scope (CP03)
        {
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (Zero) }
                Return (0x0F)
            }
        }

        Processor (CPU0, 0x00, 0x00000000, 0x00)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, Zero)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }

            Method (_DSM, 4, NotSerialized)
            {
                If (!Arg2) { Return (Buffer (One) { 0x03 }) }
                Return (Package (0x02) { "plugin-type", One })
            }
        }

        Processor (CPU1, 0x01, 0x00000000, 0x00)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, One)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }

        Processor (CPU2, 0x02, 0x00000000, 0x00)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x02)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }

        Processor (CPU3, 0x03, 0x00000000, 0x00)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x03)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
    }
}
