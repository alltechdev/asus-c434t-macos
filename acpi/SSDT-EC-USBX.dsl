// ASUS C434T: the real Chrome EC is \_SB.PCI0.LPCB.EC0 (PNP0C09) and stays as-is
// for the battery (BAT0). macOS additionally expects a device named EC; add a
// dummy one under Darwin, plus USBX USB power properties.
DefinitionBlock ("", "SSDT", 2, "C434T", "EcUsbx", 0x00001000)
{
    External (_SB_.PCI0.LPCB, DeviceObj)

    Scope (\_SB.PCI0.LPCB)
    {
        Device (EC)
        {
            Name (_HID, "ACID0001")
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
    }

    Scope (\_SB)
    {
        Device (USBX)
        {
            Name (_ADR, Zero)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }

            Method (_DSM, 4, NotSerialized)
            {
                If (!Arg2) { Return (Buffer (One) { 0x03 }) }
                Return (Package (0x08)
                {
                    "kUSBSleepPowerSupply", 0x13EC,
                    "kUSBSleepPortCurrentLimit", 0x0834,
                    "kUSBWakePowerSupply", 0x13EC,
                    "kUSBWakePortCurrentLimit", 0x0834
                })
            }
        }
    }
}
