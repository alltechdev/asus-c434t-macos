// ASUS C434T: HPET's _CRS has no IRQs, so macOS assigns the HPET timers from
// the hardware route capabilities and takes GSIs 20, 21, 11 and 12 (seen in
// ioreg). GSI 21 is the eMMC's INTB route, and SetDeviceInterrupts(21) then
// fails with kIOReturnNoInterrupt, leaving EmeraldSDHC without an interrupt.
// Under Darwin, give HPET the legacy IRQs 0 and 8 and drop IRQ 0 from the 8254
// timer (RTC declares no IRQ here). Same approach as SSDTTime's FixHPET.
// OpenCore renames HPET._CRS and TIMR._CRS to XCRS (ACPI/Patch with Base).
DefinitionBlock ("", "SSDT", 2, "C434T", "Hpet", 0x00000000)
{
    External (_SB_.PCI0.LPCB.HPET, DeviceObj)
    External (_SB_.PCI0.LPCB.HPET.XCRS, BuffObj)
    External (_SB_.PCI0.LPCB.TIMR, DeviceObj)
    External (_SB_.PCI0.LPCB.TIMR.XCRS, BuffObj)

    Scope (\_SB.PCI0.LPCB.HPET)
    {
        Method (_CRS, 0, Serialized)
        {
            If (_OSI ("Darwin"))
            {
                Return (ResourceTemplate ()
                {
                    IRQNoFlags () {0, 8}
                    Memory32Fixed (ReadWrite, 0xFED00000, 0x00000400)
                })
            }

            Return (XCRS)
        }
    }

    Scope (\_SB.PCI0.LPCB.TIMR)
    {
        Method (_CRS, 0, Serialized)
        {
            If (_OSI ("Darwin"))
            {
                Return (ResourceTemplate ()
                {
                    IO (Decode16, 0x0040, 0x0040, 0x01, 0x04)
                    IO (Decode16, 0x0050, 0x0050, 0x10, 0x04)
                })
            }

            Return (XCRS)
        }
    }
}
