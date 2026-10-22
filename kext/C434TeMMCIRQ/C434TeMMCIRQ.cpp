// C434TeMMCIRQ: give the ASUS C434T eMMC controller (8086:9d2b, 00:1e.4) its
// legacy interrupt under macOS.
//
// coreboot routes the eMMC as INTB -> GSI 21 (pin register 0x02, _PRT entry
// {0x1EFFFF, 1, 0, 0x15}), yet IOPCIBridge::resolveLegacyInterrupts() leaves
// IOInterruptSpecifiers empty for it, so EmeraldSDHC cannot create its
// interrupt source. This driver probes the eMMC nub ahead of EmeraldSDHC, logs
// what the PCI family sees, and if no interrupt was resolved, repeats the
// "ResolvePCIInterrupt" lookup and finally hands the vector to the platform via
// "SetDeviceInterrupts" -- the same two calls resolveLegacyInterrupts() makes.
// probe() always declines, so EmeraldSDHC attaches afterwards as usual.

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>

// Each line goes to the kernel log and is appended to a "C434TeMMCIRQ" string
// property on the nub, which survives the dmesg ring buffer wrapping and can be
// read back with: ioreg -rln 'EMMC@1E,4'
static char gTrace[512];
static size_t gTraceLen;

static void trace(IOService *nub, const char *fmt, ...) __printflike(2, 3);
static void trace(IOService *nub, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    IOLog("C434TeMMCIRQ: %s\n", line);
    if (gTraceLen < sizeof(gTrace)) {
        int n = snprintf(gTrace + gTraceLen, sizeof(gTrace) - gTraceLen, "%s; ", line);
        if (n > 0)
            gTraceLen += (size_t)n;
    }
    nub->setProperty("C434TeMMCIRQ", gTrace);
}

#define LOG(fmt, ...) trace(provider, fmt, ##__VA_ARGS__)

// coreboot soc/intel/skylake irq.h: eMMC_IRQ
static constexpr UInt32 kFallbackGSI = 21;

class C434TeMMCIRQ : public IOService {
    OSDeclareDefaultStructors(C434TeMMCIRQ)

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
};

OSDefineMetaClassAndStructors(C434TeMMCIRQ, IOService)

static unsigned int specifierCount(IOService *nub) {
    auto specs = OSDynamicCast(OSArray, nub->getProperty(gIOInterruptSpecifiersKey));
    return specs ? specs->getCount() : 0;
}

IOService *C434TeMMCIRQ::probe(IOService *provider, SInt32 *score) {
    auto nub = OSDynamicCast(IOPCIDevice, provider);
    if (!nub)
        return nullptr;

    UInt8 line = nub->configRead8(kIOPCIConfigInterruptLine);
    UInt8 pin = nub->configRead8(kIOPCIConfigInterruptPin);
    UInt8 device = nub->getDeviceNumber();
    // Reading the property triggers the PCI family's lazy interrupt resolution.
    unsigned int before = specifierCount(nub);
    LOG("%s dev 0x%x pin %u line 0x%x, %u interrupt specifier(s) before",
        nub->getName(), device, pin, line, before);
    if (before > 0)
        return nullptr;

    IOService *bridge = nub->getProvider();
    IOService *platformNub = bridge ? bridge->getProvider() : nullptr;
    if (!platformNub) {
        LOG("no bridge provider, giving up");
        return nullptr;
    }

    UInt32 irq = 0;
    IOReturn ret = kIOReturnUnsupported;
    if (pin >= 1 && pin <= 4) {
        ret = platformNub->callPlatformFunction("ResolvePCIInterrupt", false, platformNub,
                                                (void *)(uintptr_t)device,
                                                (void *)(uintptr_t)(pin - 1), &irq);
        LOG("ResolvePCIInterrupt(dev 0x%x, pin %u) on %s -> 0x%x, irq %u",
            device, pin - 1, platformNub->getName(), ret, irq);
    }
    if (ret != kIOReturnSuccess || irq == 0) {
        irq = kFallbackGSI;
        LOG("using fallback GSI %u", irq);
    }

    ret = platformNub->callPlatformFunction("SetDeviceInterrupts", false, nub, &irq,
                                            (void *)1, (void *)false);
    LOG("SetDeviceInterrupts(%u) -> 0x%x, %u interrupt specifier(s) after",
        irq, ret, specifierCount(nub));
    return nullptr;
}
