// ASUS C434T SerialIO I2C layout (from the live ACPI dump):
//   I2C0 -> H05C  SiS touchscreen, PNP0C50 HID, GSI 31   (VoodooI2CHID)
//   I2C1 -> D015  ELAN0000 touchpad, GSI 51              (VoodooI2CELAN, polling)
//   I2C5 -> MAXR/MAXL (MAX98927 amps, C434TAudio) + DLG7 (DA7219 codec)
//   I2C2/I2C3/I2C4 -> no children
// Empty controllers only make VoodooI2C time out and slow boot, so hide
// I2C2-I2C4 under Darwin.
DefinitionBlock ("", "SSDT", 2, "C434T", "I2cOff", 0x00000000)
{
    External (_SB_.PCI0.I2C2, DeviceObj)
    External (_SB_.PCI0.I2C3, DeviceObj)
    External (_SB_.PCI0.I2C4, DeviceObj)

    Scope (\_SB.PCI0.I2C2)
    {
        Method (_STA, 0, NotSerialized)
        {
            If (_OSI ("Darwin")) { Return (Zero) }
            Return (0x0F)
        }
    }

    Scope (\_SB.PCI0.I2C3)
    {
        Method (_STA, 0, NotSerialized)
        {
            If (_OSI ("Darwin")) { Return (Zero) }
            Return (0x0F)
        }
    }

    Scope (\_SB.PCI0.I2C4)
    {
        Method (_STA, 0, NotSerialized)
        {
            If (_OSI ("Darwin")) { Return (Zero) }
            Return (0x0F)
        }
    }

}
