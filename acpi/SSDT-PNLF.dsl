// Backlight device for WhateverGreen/AppleBacklight. _UID 0x10 (16) = the
// Skylake/Kaby Lake profile, which assumes a PWM max of 0x56C. coreboot programs
// a different max into BLC_PWM_PCH_CTL2 (iGPU MMIO 0xC8254, max << 16 | duty), so
// part of the brightness slider was clipped at full. _INI reprograms the max to
// 0x56C and rescales the current duty (RehabMan's SSDT-PNLF method).
DefinitionBlock ("", "SSDT", 2, "C434T", "Pnlf", 0x00000000)
{
    External (_SB_.PCI0.GFX0, DeviceObj)

    Scope (\_SB.PCI0.GFX0)
    {
        OperationRegion (PNCF, PCI_Config, 0x10, 0x08)
        Field (PNCF, DWordAcc, NoLock, Preserve)
        {
            PNBL,   32,
            PNBH,   32
        }

        Device (PNLF)
        {
            Name (_HID, EisaId ("APP0002"))
            Name (_CID, "backlight")
            Name (_UID, 0x10)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0B) }
                Return (Zero)
            }

            Method (_INI, 0, Serialized)
            {
                If (!_OSI ("Darwin")) { Return }
                Local0 = ((PNBH << 32) | PNBL) & 0xFFFFFFFFFFFFFFF0
                If (Local0 == Zero) { Return }
                OperationRegion (PNMM, SystemMemory, Local0 + 0xC8250, 0x08)
                Field (PNMM, DWordAcc, NoLock, Preserve)
                {
                    LEVW,   32,
                    LEVX,   32
                }
                Local1 = LEVX >> 16
                Local2 = 0x056C
                If (Local1 != Local2)
                {
                    If (Local1 == Zero)
                    {
                        Local3 = Local2
                    }
                    Else
                    {
                        Local3 = ((LEVX & 0xFFFF) * Local2) / Local1
                    }
                    LEVX = (Local2 << 16) | Local3
                }
            }
        }
    }
}
