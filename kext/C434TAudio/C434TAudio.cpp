// C434TAudio: Intel cAVS 1.5 (Skylake/Kaby Lake "AVS") audio DSP bring-up for
// the ASUS C434T (HDA controller 8086:9d71 in DSP mode).
//
// Step 1 of a macOS audio driver for the DA7219 + MAX98927 machine: boot the
// DSP base firmware. The sequence follows Linux sound/soc/intel/avs
// (core.c avs_hda_probe_work, loader.c avs_cldma_load_basefw, cldma.c, dsp.c):
//   HDA controller reset -> enable processing pipe (PPCTL.GPROCEN) -> disable
//   clock/power gating and L1 -> power + unreset DSP core 0 -> set up the
//   code-loader DMA (CLDMA, in the DSP BAR) -> unstall -> wait for ROM init ->
//   stream the firmware through CLDMA -> wait for BASEFW_ENTERED -> wait for the
//   FW_READY IPC notification.
// Interrupts are not used yet; everything is polled.
//
// Nothing touches the hardware at boot beyond mapping the BARs. The bring-up
// runs on demand:  sudo sysctl -w debug.c434t_audio_boot=1
// and its trace is readable with:  sysctl debug.c434t_audio_log

#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/OSByteOrder.h>
#include <sys/sysctl.h>

#include "fw_skl.h"
#include "nhlt_ssp.h"

// --- HDA controller registers (BAR0) ---
#define AZX_REG_GCAP        0x00
#define AZX_REG_GCTL        0x08
#define AZX_GCTL_RESET      (1u << 0)
#define AZX_REG_STATESTS    0x0e
#define AZX_REG_LLCH        0x14
#define AZX_REG_VS_EM2      0x1030
#define AZX_VS_EM2_L1SEN    (1u << 13)
#define AZX_VS_EM2_DUM      (1u << 23)
#define AZX_PP_CAP_ID       0x3
#define AZX_REG_PP_PPCTL    0x04
#define AZX_PPCTL_GPROCEN   (1u << 30)

// --- PCI config ---
#define AZX_PCIREG_PGCTL         0x44
#define INTEL_HDA_DEVC           0x78          // Linux hda_intel: SCH/PCH snoop control
#define INTEL_HDA_DEVC_NOSNOOP   (1u << 11)
#define AZX_PCIREG_CGCTL         0x48
#define AZX_PGCTL_LSRMD_MASK     (1u << 4)
#define AZX_CGCTL_MISCBDCGE_MASK (1u << 6)

// --- DSP registers (BAR4) ---
#define ADSP_REG_ADSPCS     0x04
#define ADSP_REG_ADSPIC     0x08
#define ADSP_REG_ADSPIS     0x0c
#define ADSPIC_IPC          (1u << 0)
#define ADSPIC_CLDMA        (1u << 1)
#define ADSPCS_CRST(cm)     (cm)
#define ADSPCS_CSTALL(cm)   ((cm) << 8)
#define ADSPCS_SPA(cm)      ((cm) << 16)
#define ADSPCS_CPA(cm)      ((cm) << 24)
#define MAIN_CORE           1u

#define SKL_REG_HIPCT       0x40
#define SKL_REG_HIPCTE      0x44
#define SKL_REG_HIPCI       0x48
#define SKL_REG_HIPCIE      0x4c
#define SKL_REG_HIPCCTL     0x50
#define SKL_HIPCT_BUSY      (1u << 31)
#define HIPCCTL_BUSY        (1u << 0)
#define HIPCCTL_DONE        (1u << 1)

#define SKL_SRAM_BASE       0x8000
#define FW_REG_STATUS       (SKL_SRAM_BASE + 0x0)
#define FW_REG_ERROR        (SKL_SRAM_BASE + 0x4)
#define ROM_INIT_DONE       0x1
#define ROM_STS_MASK        0xff
#define SKL_BASEFW_ENTERED  0xf
#define NOTIFY_FW_READY     8

// --- IPC (ipc.c / messages.h) ---
#define SKL_HIPCI_BUSY      (1u << 31)
#define SKL_HIPCIE_DONE     (1u << 30)
#define MAILBOX_SIZE        4096
#define UPLINK_MAILBOX      (SKL_SRAM_BASE + 0x1000)  // window 0 + 4K
#define DOWNLINK_MAILBOX    (SKL_SRAM_BASE + 0x2000)  // window 1
#define MSG_TARGET_MOD      (1u << 30)
#define MSG_DIRECTION_REPLY (1u << 29)
#define MOD_LARGE_CONFIG_GET 3
#define BASEFW_FIRMWARE_CONFIG 7
#define BASEFW_HARDWARE_CONFIG 8
#define BASEFW_MODULES_INFO    9
#define MOD_INIT_INSTANCE      0
#define MOD_BIND               5
#define GLB_CREATE_PIPELINE    17
#define GLB_DELETE_PIPELINE    18
#define GLB_SET_PIPELINE_STATE 19
#define PPL_STATE_RESET        2
#define PPL_STATE_PAUSED       3
#define PPL_STATE_RUNNING      4
#define DMA_HDA_HOST_OUTPUT    0
#define DMA_I2S_LINK_OUTPUT    12

// --- HDA host output stream used by the DSP host gateway (decoupled mode) ---
#define HOST_STREAM_INDEX   7      // first output stream: GCAP reports 7 input streams
#define HOST_STREAM_TAG     1      // dma_id = tag - 1 = copier vindex
#define HOST_SD             (0x80 + HOST_STREAM_INDEX * 0x20)
#define SD_FMT              0x12
#define SD_CTL_RUN          0x02
#define HOST_PERIOD_BYTES   4096
#define HOST_PERIODS        4
#define HOST_BUFFER_BYTES   (HOST_PERIOD_BYTES * HOST_PERIODS)

// --- Microphones: DMIC (PDM) -> DSP -> HDA host input stream 0 ---
// avs-topology-xml dmic/dmic-tplg.xml, 48 kHz 16-bit stereo path:
//   ppl 2 (BE): copier#3 [DMIC link input, NHLT PDM blob]
//   ppl 3 (FE): copier#4 -> peakvol#1 -> copier#5 [HDA host input DMA]
#define DMA_HDA_HOST_INPUT  1
#define DMA_DMIC_LINK_INPUT 11
#define CAP_STREAM_INDEX    0      // input streams come first
#define CAP_STREAM_TAG      1
#define CAP_SD              (0x80 + CAP_STREAM_INDEX * 0x20)

// --- Code loader DMA stream (BAR4 + 0x80) ---
#define CL_SD               0x80
#define SD_CTL              0x00
#define SD_STS              0x03
#define SD_LPIB             0x04
#define SD_CBL              0x08
#define SD_LVI              0x0c
#define SD_BDLPL            0x18
#define SD_BDLPU            0x1c
#define CL_SPBFCTL          0x24
#define CL_SD_SPIB          0x28
#define SD_CTL_STREAM_RESET 0x01
#define SD_CTL_DMA_START    0x02
#define SD_INT_COMPLETE     0x04
#define SD_INT_MASK         0x1c
#define SD_CTL_STRM_MASK    (0xfu << 20)
#define CL_STREAM_TAG       1u
#define CL_BUFFER_SIZE      (128 * 1024)

#define LOG_SIZE 8192


// --- MAX98927 speaker amplifiers on I2C5, reached through VoodooI2C ---------
// Only the non-virtual, exported I2C helpers of VoodooI2C's device nub are
// used, so a minimal declaration with the same name is enough to link.
class VoodooI2CDeviceNub : public IOService {
public:
    IOReturn readI2C(UInt8 *values, UInt16 length);
    IOReturn writeI2C(UInt8 *values, UInt16 length);
    IOReturn writeReadI2C(UInt8 *write_buffer, UInt16 write_length, UInt8 *read_buffer,
                          UInt16 read_length);
};

// One MAX98927 (ACPI MAXR @0x39 right, MAXL @0x3A left; Linux codecs/max98927.c,
// board avs/boards/max98927.c: DSP_B, codec clock consumer, 8 x 16-bit TDM slots).
class C434TAmp : public IOService {
    OSDeclareDefaultStructors(C434TAmp)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    bool configure();
    bool enable(bool on);
    void dumpStatus();
    const char *label() const { return right ? "right" : "left"; }

private:
    VoodooI2CDeviceNub *nub = nullptr;
    bool right = false;
    bool writeReg(uint16_t reg, uint8_t val);
    bool readReg(uint16_t reg, uint8_t *val);
    bool updateReg(uint16_t reg, uint8_t mask, uint8_t val);
};

static C434TAmp *gAmps[2]; // [0] left, [1] right

class C434TAudioDSP : public IOService {
    OSDeclareDefaultStructors(C434TAudioDSP)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    int bootFirmware();

private:
    IOPCIDevice *pci = nullptr;
    IOMemoryMap *hdaMap = nullptr;
    IOMemoryMap *dspMap = nullptr;
    volatile uint8_t *hda = nullptr;
    volatile uint8_t *dsp = nullptr;
    IOBufferMemoryDescriptor *clData = nullptr;
    IOBufferMemoryDescriptor *clBdl = nullptr;

    // code loader runtime
    const uint8_t *clPos = nullptr;
    uint32_t clRemaining = 0;

    uint32_t hdaRead32(uint32_t r) { return OSReadLittleInt32((void *)hda, r); }
    void hdaWrite32(uint32_t r, uint32_t v) { OSWriteLittleInt32((void *)hda, r, v); }
    uint16_t hdaRead16(uint32_t r) { return OSReadLittleInt16((void *)hda, r); }
    void hdaWrite16(uint32_t r, uint16_t v) { OSWriteLittleInt16((void *)hda, r, v); }
    uint32_t dspRead32(uint32_t r) { return OSReadLittleInt32((void *)dsp, r); }
    void dspWrite32(uint32_t r, uint32_t v) { OSWriteLittleInt32((void *)dsp, r, v); }
    uint8_t dspRead8(uint32_t r) { return dsp[r]; }
    void dspWrite8(uint32_t r, uint8_t v) { dsp[r] = v; }
    void dspUpdate32(uint32_t r, uint32_t mask, uint32_t val) {
        dspWrite32(r, (dspRead32(r) & ~mask) | val);
    }
    void dspUpdate8(uint32_t r, uint8_t mask, uint8_t val) {
        dspWrite8(r, (uint8_t)((dspRead8(r) & ~mask) | val));
    }
    bool pollDsp32(uint32_t r, uint32_t mask, uint32_t want, uint32_t intervalUs,
                   uint32_t timeoutUs, uint32_t *last = nullptr);
    bool pollDsp8(uint32_t r, uint8_t mask, uint8_t want, uint32_t intervalUs,
                  uint32_t timeoutUs);

    void gating(bool enable);
    bool resetController();
    bool enableProcessingPipe();
    bool corePower(bool on);
    bool coreReset(bool reset);
    bool coreStall(bool stall);
    void coreDisable();
    bool allocCodeLoader();
    bool cldmaReset();
    void cldmaSetup();
    void cldmaFill();
    bool cldmaStart();
    void cldmaStop();
    bool transferFirmware(const uint8_t *data, uint32_t size);
    int ipcSend(uint64_t header, const void *data, uint32_t size, uint64_t *reply,
                void *replyData, uint32_t *replySize);
    int getLargeConfig(uint16_t moduleId, uint8_t instanceId, uint8_t paramId, void *buf,
                       uint32_t *size);
    void queryFirmware();
    int ipcGlobal(uint32_t primary, uint32_t ext);
    int ipcModule(uint32_t primary, uint32_t ext, const void *data, uint32_t size);
    bool findModules();
    int buildSpeakerPath();
    int buildHeadphonePath();
    void teardownSpeakerPath();
    bool setupHostStream(bool tone);
    int buildMicPath();
    void teardownMicPath();
    bool setupCaptureStream();
public:
    int testTone();
    // Playback stream used by the CoreAudio plug-in through C434TAudioUserClient.
    // Callers hold gLock.
    int streamStart(bool tone = false);
    void streamStop();
    bool isStreaming() const { return streaming; }
    int setVolume(uint32_t q31);
    void zeroTimestamp(uint64_t *sampleTime, uint64_t *hostTime, uint64_t *seed);
    IOBufferMemoryDescriptor *ringMemory() const { return hostBuf; }
    int describePosition(char *buf, size_t len);
    uint32_t lpib() { return hdaRead32(HOST_SD + SD_LPIB); }
    static constexpr uint32_t kRingFrames = HOST_BUFFER_BYTES / 4;
private:
    uint16_t copierId = 0xffff, peakvolId = 0xffff;
    bool fwRunning = false;
    bool streaming = false;
    uint32_t volumeQ31 = 0x7fffffff;
    IOBufferMemoryDescriptor *hostBuf = nullptr;
    IOBufferMemoryDescriptor *hostBdl = nullptr;
    IOBufferMemoryDescriptor *capBuf = nullptr;
    IOBufferMemoryDescriptor *capBdl = nullptr;
    bool micRunning = false;
    uint32_t capOffset = 0;
public:
    IOBufferMemoryDescriptor *captureMemory() const { return micRunning ? capBuf : nullptr; }
    // Capture ring index for playback-timeline frame f is (f + capOffset) % kRingFrames.
    uint32_t captureOffset() const { return micRunning ? capOffset : 0xffffffffu; }
private:
    // Position tracking: a timer counts ring wraps so the plug-in's zero
    // timestamps stay continuous even if it polls less than once per ring.
    IOWorkLoop *workLoop = nullptr;
    IOTimerEventSource *posTimer = nullptr;
    IOSimpleLock *posLock = nullptr;
    uint32_t lastFrame = 0;
    uint32_t eraseFrame = 0;
    bool erase = false;   // off for the test tone, which loops a prefilled ring
    uint64_t wraps = 0;
    uint64_t zeroHost = 0;
    uint64_t zeroSeed = 1;
    void updatePosition(uint32_t *frame, uint64_t *now);
    static void posTimerFired(OSObject *owner, IOTimerEventSource *sender);

    // System sleep: the HDA/DSP and the amps lose power. Stop the stream before
    // anything powers down (otherwise the DMA loops the last ring through the
    // amps while sleep proceeds) and rebuild everything once the system is up.
    IONotifier *sleepNotifier = nullptr;
    IOTimerEventSource *resumeTimer = nullptr;
    bool suspended = false;
    bool resumeAfterWake = false;
    bool continueTimeline = false;
    static IOReturn sleepWakeHandler(void *target, void *ref, UInt32 msg, IOService *provider,
                                     void *arg, vm_size_t argSize);
    static void resumeTimerFired(OSObject *owner, IOTimerEventSource *sender);
public:
    bool isSuspended() const { return suspended; }
    void cancelResume() { resumeAfterWake = false; }
};

OSDefineMetaClassAndStructors(C434TAudioDSP, IOService)

// ---------------------------------------------------------------------------
// Trace buffer, exposed through sysctl debug.c434t_audio_log

static char gLog[LOG_SIZE];
static size_t gLogLen;
static C434TAudioDSP *gDriver;
static IOLock *gLock;

static void logLine(const char *fmt, ...) __printflike(1, 2);
static void logLine(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    IOLog("C434TAudio: %s\n", line);
    if (gLogLen + 1 < sizeof(gLog)) {
        int n = snprintf(gLog + gLogLen, sizeof(gLog) - gLogLen, "%s\n", line);
        if (n > 0)
            gLogLen = min(gLogLen + (size_t)n, sizeof(gLog) - 1);
    }
}

static int sysctlBoot SYSCTL_HANDLER_ARGS {
    int value = 0;
    int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr)
        return err;
    if (value != 1)
        return EINVAL;
    IOLockLock(gLock);
    int ret = gDriver ? gDriver->bootFirmware() : ENODEV;
    IOLockUnlock(gLock);
    return ret;
}

static int sysctlTest SYSCTL_HANDLER_ARGS {
    int value = 0;
    int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr)
        return err;
    if (value != 1)
        return EINVAL;
    IOLockLock(gLock);
    int ret = gDriver ? gDriver->testTone() : ENODEV;
    IOLockUnlock(gLock);
    return ret;
}

static int sysctlPos SYSCTL_HANDLER_ARGS {
    char buf[256] = "no device";
    if (gDriver)
        gDriver->describePosition(buf, sizeof(buf));
    return SYSCTL_OUT(req, buf, strlen(buf) + 1);
}

static int sysctlRing SYSCTL_HANDLER_ARGS {
    if (!gDriver || !gDriver->ringMemory())
        return ENXIO;
    static uint8_t snap[4 + HOST_BUFFER_BYTES];
    uint32_t lpib = gDriver->lpib();
    memcpy(snap, &lpib, 4);
    memcpy(snap + 4, gDriver->ringMemory()->getBytesNoCopy(), HOST_BUFFER_BYTES);
    return SYSCTL_OUT(req, snap, sizeof(snap));
}

static int sysctlLog SYSCTL_HANDLER_ARGS {
    return SYSCTL_OUT(req, gLog, gLogLen + 1);
}

SYSCTL_DECL(_debug);
SYSCTL_PROC(_debug, OID_AUTO, c434t_audio_boot, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlBoot, "I", "write 1 to boot the audio DSP firmware");
SYSCTL_PROC(_debug, OID_AUTO, c434t_audio_test, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlTest, "I", "write 1 to play a test tone through the speaker path");
SYSCTL_PROC(_debug, OID_AUTO, c434t_audio_pos, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
            nullptr, 0, sysctlPos, "A", "C434TAudio stream position");
SYSCTL_PROC(_debug, OID_AUTO, c434t_audio_ring, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
            nullptr, 0, sysctlRing, "S", "C434TAudio LPIB + DMA ring snapshot");
SYSCTL_PROC(_debug, OID_AUTO, c434t_audio_log, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
            nullptr, 0, sysctlLog, "A", "C434TAudio trace");

// ---------------------------------------------------------------------------

bool C434TAudioDSP::pollDsp32(uint32_t r, uint32_t mask, uint32_t want, uint32_t intervalUs,
                              uint32_t timeoutUs, uint32_t *last) {
    uint32_t v = 0;
    for (uint32_t waited = 0;; waited += intervalUs) {
        v = dspRead32(r);
        if ((v & mask) == want)
            break;
        if (waited >= timeoutUs) {
            if (last)
                *last = v;
            return false;
        }
        IODelay(intervalUs);
    }
    if (last)
        *last = v;
    return true;
}

bool C434TAudioDSP::pollDsp8(uint32_t r, uint8_t mask, uint8_t want, uint32_t intervalUs,
                             uint32_t timeoutUs) {
    for (uint32_t waited = 0;; waited += intervalUs) {
        if ((dspRead8(r) & mask) == want)
            return true;
        if (waited >= timeoutUs)
            return false;
        IODelay(intervalUs);
    }
}

bool C434TAudioDSP::start(IOService *provider) {
    if (!IOService::start(provider))
        return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return false;

    pci->setMemoryEnable(true);
    pci->setBusMasterEnable(true);
    hdaMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    dspMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress4);
    if (!hdaMap || !dspMap) {
        logLine("failed to map BARs (hda %p dsp %p)", hdaMap, dspMap);
        OSSafeReleaseNULL(hdaMap);
        OSSafeReleaseNULL(dspMap);
        return false;
    }
    hda = (volatile uint8_t *)hdaMap->getVirtualAddress();
    dsp = (volatile uint8_t *)dspMap->getVirtualAddress();

    logLine("attached: BAR0 %#llx (%#llx) BAR4 %#llx (%#llx)",
            (unsigned long long)hdaMap->getPhysicalAddress(), (unsigned long long)hdaMap->getLength(),
            (unsigned long long)dspMap->getPhysicalAddress(), (unsigned long long)dspMap->getLength());
    logLine("GCAP %#06x GCTL %#010x ADSPCS %#010x PGCTL %#010x CGCTL %#010x",
            hdaRead16(AZX_REG_GCAP), hdaRead32(AZX_REG_GCTL), dspRead32(ADSP_REG_ADSPCS),
            pci->configRead32(AZX_PCIREG_PGCTL), pci->configRead32(AZX_PCIREG_CGCTL));
    logLine("firmware image %u bytes; boot with: sudo sysctl -w debug.c434t_audio_boot=1",
            (unsigned)sizeof(gC434TFirmwareSkl));

    posLock = IOSimpleLockAlloc();
    workLoop = IOWorkLoop::workLoop();
    posTimer = IOTimerEventSource::timerEventSource(this, posTimerFired);
    resumeTimer = IOTimerEventSource::timerEventSource(this, resumeTimerFired);
    if (!posLock || !workLoop || !posTimer || !resumeTimer ||
        workLoop->addEventSource(posTimer) != kIOReturnSuccess ||
        workLoop->addEventSource(resumeTimer) != kIOReturnSuccess)
        return false;
    sleepNotifier = registerPrioritySleepWakeInterest(sleepWakeHandler, this);

    gDriver = this;
    registerService();
    return true;
}

void C434TAudioDSP::stop(IOService *provider) {
    if (gDriver == this)
        gDriver = nullptr;
    if (sleepNotifier) {
        sleepNotifier->remove();
        sleepNotifier = nullptr;
    }
    if (resumeTimer) {
        resumeTimer->cancelTimeout();
        workLoop->removeEventSource(resumeTimer);
    }
    OSSafeReleaseNULL(resumeTimer);
    IOLockLock(gLock);
    streamStop();
    IOLockUnlock(gLock);
    if (posTimer) {
        posTimer->cancelTimeout();
        workLoop->removeEventSource(posTimer);
    }
    OSSafeReleaseNULL(posTimer);
    OSSafeReleaseNULL(workLoop);
    if (posLock)
        IOSimpleLockFree(posLock);
    if (clData) {
        clData->complete();
        clBdl->complete();
    }
    OSSafeReleaseNULL(clData);
    OSSafeReleaseNULL(clBdl);
    OSSafeReleaseNULL(hdaMap);
    OSSafeReleaseNULL(dspMap);
    IOService::stop(provider);
}

// Linux avs_hda_{power,clock}_gating_enable + avs_hda_l1sen_enable.
void C434TAudioDSP::gating(bool enable) {
    uint32_t pg = pci->configRead32(AZX_PCIREG_PGCTL);
    pg = enable ? (pg & ~AZX_PGCTL_LSRMD_MASK) : (pg | AZX_PGCTL_LSRMD_MASK);
    pci->configWrite32(AZX_PCIREG_PGCTL, pg);
    uint32_t cg = pci->configRead32(AZX_PCIREG_CGCTL);
    cg = enable ? (cg | AZX_CGCTL_MISCBDCGE_MASK) : (cg & ~AZX_CGCTL_MISCBDCGE_MASK);
    pci->configWrite32(AZX_PCIREG_CGCTL, cg);
    uint32_t em2 = hdaRead32(AZX_REG_VS_EM2);
    em2 = enable ? (em2 | AZX_VS_EM2_L1SEN) : (em2 & ~AZX_VS_EM2_L1SEN);
    hdaWrite32(AZX_REG_VS_EM2, em2);
}

// snd_hdac_bus_init_chip(full_reset): enter and leave controller reset.
bool C434TAudioDSP::resetController() {
    hdaWrite16(AZX_REG_STATESTS, 0x7fff);
    hdaWrite32(AZX_REG_GCTL, hdaRead32(AZX_REG_GCTL) & ~AZX_GCTL_RESET);
    for (int i = 0; i < 100 && (hdaRead32(AZX_REG_GCTL) & AZX_GCTL_RESET); i++)
        IODelay(1000);
    if (hdaRead32(AZX_REG_GCTL) & AZX_GCTL_RESET)
        return false;
    IODelay(500);
    hdaWrite32(AZX_REG_GCTL, hdaRead32(AZX_REG_GCTL) | AZX_GCTL_RESET);
    for (int i = 0; i < 100 && !(hdaRead32(AZX_REG_GCTL) & AZX_GCTL_RESET); i++)
        IODelay(1000);
    if (!(hdaRead32(AZX_REG_GCTL) & AZX_GCTL_RESET))
        return false;
    IOSleep(1);
    // Address incorrect capture position reporting (Linux sets DUM after reset).
    hdaWrite32(AZX_REG_VS_EM2, hdaRead32(AZX_REG_VS_EM2) | AZX_VS_EM2_DUM);
    return true;
}

// snd_hdac_bus_parse_capabilities + snd_hdac_ext_bus_ppcap_enable.
bool C434TAudioDSP::enableProcessingPipe() {
    uint32_t off = hdaRead32(AZX_REG_LLCH) & 0xffff;
    for (int n = 0; off && off < 0x4000 && n < 16; n++) {
        uint32_t hdr = hdaRead32(off);
        uint32_t id = (hdr >> 16) & 0xfff;
        if (id == AZX_PP_CAP_ID) {
            uint32_t ppctl = off + AZX_REG_PP_PPCTL;
            hdaWrite32(ppctl, hdaRead32(ppctl) | AZX_PPCTL_GPROCEN);
            logLine("PP capability at %#x, PPCTL now %#010x", off, hdaRead32(ppctl));
            return true;
        }
        off = hdr & 0xffff;
    }
    return false;
}

bool C434TAudioDSP::corePower(bool on) {
    dspUpdate32(ADSP_REG_ADSPCS, ADSPCS_SPA(MAIN_CORE), on ? ADSPCS_SPA(MAIN_CORE) : 0);
    IODelay(1500);
    return pollDsp32(ADSP_REG_ADSPCS, ADSPCS_CPA(MAIN_CORE), on ? ADSPCS_CPA(MAIN_CORE) : 0,
                     500, 10000);
}

bool C434TAudioDSP::coreReset(bool reset) {
    dspUpdate32(ADSP_REG_ADSPCS, ADSPCS_CRST(MAIN_CORE), reset ? ADSPCS_CRST(MAIN_CORE) : 0);
    return pollDsp32(ADSP_REG_ADSPCS, ADSPCS_CRST(MAIN_CORE),
                     reset ? ADSPCS_CRST(MAIN_CORE) : 0, 500, 10000);
}

bool C434TAudioDSP::coreStall(bool stall) {
    dspUpdate32(ADSP_REG_ADSPCS, ADSPCS_CSTALL(MAIN_CORE), stall ? ADSPCS_CSTALL(MAIN_CORE) : 0);
    bool ok = pollDsp32(ADSP_REG_ADSPCS, ADSPCS_CSTALL(MAIN_CORE),
                        stall ? ADSPCS_CSTALL(MAIN_CORE) : 0, 500, 10000);
    IODelay(1500);
    return ok;
}

void C434TAudioDSP::coreDisable() {
    coreStall(true);
    coreReset(true);
    corePower(false);
}

bool C434TAudioDSP::allocCodeLoader() {
    if (clData)
        return true;
    // Physically contiguous, below 4 GB, page aligned (BDL needs 128-byte alignment).
    clData = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, CL_BUFFER_SIZE,
        0x00000000FFFFF000ULL);
    clBdl = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 4096,
        0x00000000FFFFF000ULL);
    if (!clData || !clBdl) {
        OSSafeReleaseNULL(clData);
        OSSafeReleaseNULL(clBdl);
        return false;
    }
    clData->prepare();
    clBdl->prepare();
    bzero(clData->getBytesNoCopy(), CL_BUFFER_SIZE);
    bzero(clBdl->getBytesNoCopy(), 4096);
    return true;
}

void C434TAudioDSP::cldmaStop() {
    dspUpdate32(ADSP_REG_ADSPIC, ADSPIC_CLDMA, 0);
    dspUpdate8(CL_SD + SD_CTL, SD_INT_MASK | SD_CTL_DMA_START, 0);
    pollDsp8(CL_SD + SD_CTL, SD_CTL_DMA_START, 0, 3, 300);
}

bool C434TAudioDSP::cldmaReset() {
    cldmaStop();
    dspUpdate8(CL_SD + SD_CTL, SD_CTL_STREAM_RESET, SD_CTL_STREAM_RESET);
    if (!pollDsp8(CL_SD + SD_CTL, SD_CTL_STREAM_RESET, SD_CTL_STREAM_RESET, 3, 300))
        return false;
    dspUpdate8(CL_SD + SD_CTL, SD_CTL_STREAM_RESET, 0);
    return pollDsp8(CL_SD + SD_CTL, SD_CTL_STREAM_RESET, 0, 3, 300);
}

// hda_cldma_setup: two BDL entries of half the buffer each, IOC on the last.
void C434TAudioDSP::cldmaSetup() {
    uint64_t data = clData->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    uint64_t bdlPhys = clBdl->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    uint32_t *bdl = (uint32_t *)clBdl->getBytesNoCopy();
    const uint32_t chunk = CL_BUFFER_SIZE / 2;
    for (uint32_t i = 0; i < 2; i++) {
        uint64_t addr = data + i * chunk;
        bdl[i * 4 + 0] = OSSwapHostToLittleInt32((uint32_t)addr);
        bdl[i * 4 + 1] = OSSwapHostToLittleInt32((uint32_t)(addr >> 32));
        bdl[i * 4 + 2] = OSSwapHostToLittleInt32(chunk);
        bdl[i * 4 + 3] = OSSwapHostToLittleInt32(i == 1 ? 1 : 0);
    }
    dspWrite32(CL_SD + SD_BDLPL, (uint32_t)bdlPhys & ~0x7fu);
    dspWrite32(CL_SD + SD_BDLPU, (uint32_t)(bdlPhys >> 32));
    dspWrite32(CL_SD + SD_CBL, CL_BUFFER_SIZE);
    dspWrite8(CL_SD + SD_LVI, 1);
    dspUpdate32(CL_SD + SD_CTL, SD_CTL_STRM_MASK, CL_STREAM_TAG << 20);
    dspWrite32(CL_SD + CL_SPBFCTL, 1);
    logLine("CLDMA: data %#llx BDL %#llx", (unsigned long long)data, (unsigned long long)bdlPhys);
}

// hda_cldma_fill: copy the next chunk at the current SPIB position, then advance SPIB.
void C434TAudioDSP::cldmaFill() {
    uint8_t *buf = (uint8_t *)clData->getBytesNoCopy();
    uint32_t size = clRemaining > CL_BUFFER_SIZE ? CL_BUFFER_SIZE : clRemaining;
    uint32_t offset = dspRead32(CL_SD + CL_SD_SPIB);
    if (offset + size > CL_BUFFER_SIZE) {
        uint32_t ss = CL_BUFFER_SIZE - offset;
        memcpy(buf + offset, clPos, ss);
        offset = 0;
        size -= ss;
        clPos += ss;
        clRemaining -= ss;
    }
    memcpy(buf + offset, clPos, size);
    clPos += size;
    clRemaining -= size;
    dspWrite32(CL_SD + CL_SD_SPIB, offset + size);
}

bool C434TAudioDSP::cldmaStart() {
    dspUpdate32(ADSP_REG_ADSPIC, ADSPIC_CLDMA, ADSPIC_CLDMA);
    dspUpdate8(CL_SD + SD_CTL, SD_INT_MASK | SD_CTL_DMA_START, SD_INT_MASK | SD_CTL_DMA_START);
    return pollDsp8(CL_SD + SD_CTL, SD_CTL_DMA_START, SD_CTL_DMA_START, 3, 300);
}

// hda_cldma_transfer + cldma_memcpy_work, polling SD_STS instead of the IRQ.
bool C434TAudioDSP::transferFirmware(const uint8_t *data, uint32_t size) {
    clPos = data;
    clRemaining = size;
    cldmaFill();
    if (!cldmaStart()) {
        logLine("CLDMA did not start, SD_CTL %#04x", dspRead8(CL_SD + SD_CTL));
        return false;
    }
    while (true) {
        uint32_t waited = 0;
        uint8_t sts = 0;
        while (!((sts = dspRead8(CL_SD + SD_STS)) & SD_INT_MASK) && waited < 300000) {
            IODelay(50);
            waited += 50;
        }
        if (!(sts & SD_INT_COMPLETE)) {
            logLine("CLDMA transfer stalled: SD_STS %#04x LPIB %#x remaining %u", sts,
                    dspRead32(CL_SD + SD_LPIB), clRemaining);
            return false;
        }
        dspWrite8(CL_SD + SD_STS, sts); // write-1-to-clear
        cldmaFill();
        // The final chunk usually ends before the buffer boundary, where the only
        // IOC is: the DMA stops at SPIB without a completion interrupt (Linux
        // logs a harmless "cldma IOC timeout" there). The caller polls the ROM
        // status for BASEFW_ENTERED while the DMA drains, then stops it.
        if (!clRemaining)
            return true;
    }
}

// avs_dsp_send_tx + avs_dsp_receive_rx, polled. Unsolicited notifications that
// arrive while waiting are acknowledged and logged.
int C434TAudioDSP::ipcSend(uint64_t header, const void *data, uint32_t size, uint64_t *reply,
                           void *replyData, uint32_t *replySize) {
    if (!pollDsp32(SKL_REG_HIPCI, SKL_HIPCI_BUSY, 0, 50, 100000)) {
        logLine("IPC: previous request still busy, HIPCI %#010x", dspRead32(SKL_REG_HIPCI));
        return EBUSY;
    }
    const uint32_t *words = (const uint32_t *)data;
    for (uint32_t i = 0; i < size / 4; i++)
        dspWrite32(DOWNLINK_MAILBOX + i * 4, words[i]);
    dspWrite32(SKL_REG_HIPCIE, (uint32_t)(header >> 32));
    dspWrite32(SKL_REG_HIPCI, (uint32_t)header | SKL_HIPCI_BUSY);

    for (uint32_t waited = 0; waited < 500000; waited += 50) {
        uint32_t hipct = dspRead32(SKL_REG_HIPCT);
        if (!(hipct & SKL_HIPCT_BUSY)) {
            IODelay(50);
            continue;
        }
        uint32_t ext = dspRead32(SKL_REG_HIPCTE);
        if (!(hipct & MSG_DIRECTION_REPLY)) {
            logLine("IPC: notification %#010x %#010x while waiting", hipct, ext);
            dspWrite32(SKL_REG_HIPCT, SKL_HIPCT_BUSY);
            continue;
        }
        uint32_t status = hipct & 0xffffff;
        if (!status && replyData && replySize) {
            uint32_t n = *replySize;
            bool largeGet = (hipct & MSG_TARGET_MOD) && ((hipct >> 24) & 0x1f) == MOD_LARGE_CONFIG_GET;
            if (largeGet)
                n = min(n, min((uint32_t)MAILBOX_SIZE, ext & 0xfffff));
            uint32_t *out = (uint32_t *)replyData;
            for (uint32_t i = 0; i < (n + 3) / 4; i++)
                out[i] = dspRead32(UPLINK_MAILBOX + i * 4);
            *replySize = n;
        }
        if (reply)
            *reply = ((uint64_t)ext << 32) | hipct;
        dspWrite32(SKL_REG_HIPCT, SKL_HIPCT_BUSY);                      // reply consumed
        dspWrite32(SKL_REG_HIPCIE, dspRead32(SKL_REG_HIPCIE) | SKL_HIPCIE_DONE); // ack done
        return status ? EPROTO : 0;
    }
    logLine("IPC: reply timeout, HIPCI %#010x HIPCIE %#010x HIPCT %#010x status %#010x error %#010x",
            dspRead32(SKL_REG_HIPCI), dspRead32(SKL_REG_HIPCIE), dspRead32(SKL_REG_HIPCT),
            dspRead32(FW_REG_STATUS), dspRead32(FW_REG_ERROR));
    return ETIMEDOUT;
}

// avs_ipc_get_large_config with no request payload, single block.
int C434TAudioDSP::getLargeConfig(uint16_t moduleId, uint8_t instanceId, uint8_t paramId,
                                  void *buf, uint32_t *size) {
    uint32_t primary = moduleId | ((uint32_t)instanceId << 16) |
                       ((uint32_t)MOD_LARGE_CONFIG_GET << 24) | MSG_TARGET_MOD;
    uint32_t ext = ((uint32_t)paramId << 20) | (1u << 29); // init_block
    uint64_t reply = 0;
    int ret = ipcSend(((uint64_t)ext << 32) | primary, nullptr, 0, &reply, buf, size);
    if (ret)
        logLine("large config get mod %u param %u failed: %d, reply %#llx", moduleId, paramId,
                ret, (unsigned long long)reply);
    return ret;
}

// avs_ipc_get_fw_config / avs_ipc_get_hw_config: walk the TLV replies.
void C434TAudioDSP::queryFirmware() {
    static uint32_t buf[MAILBOX_SIZE / 4];
    uint32_t size = sizeof(buf);
    if (!getLargeConfig(0, 0, BASEFW_FIRMWARE_CONFIG, buf, &size)) {
        logLine("FW config: %u bytes", size);
        for (uint32_t off = 0; off + 8 <= size;) {
            uint32_t type = buf[off / 4], len = buf[off / 4 + 1];
            const uint32_t *v = &buf[off / 4 + 2];
            switch (type) {
            case 0:
                logLine("  fw version %u.%u.%u.%u", v[0] & 0xffff, v[0] >> 16, v[1] & 0xffff, v[1] >> 16);
                break;
            case 6: logLine("  DL mailbox %u bytes", v[0]); break;
            case 7: logLine("  UL mailbox %u bytes", v[0]); break;
            case 9: logLine("  max pipelines %u", v[0]); break;
            case 12: logLine("  modules %u", v[0]); break;
            case 13: logLine("  max module instances %u", v[0]); break;
            case 17: logLine("  max libraries %u", v[0]); break;
            default: break;
            }
            off += 8 + ((len + 3) & ~3u);
        }
    }
    size = sizeof(buf);
    if (!getLargeConfig(0, 0, BASEFW_HARDWARE_CONFIG, buf, &size)) {
        logLine("HW config: %u bytes", size);
        for (uint32_t off = 0; off + 8 <= size;) {
            uint32_t type = buf[off / 4], len = buf[off / 4 + 1];
            const uint32_t *v = &buf[off / 4 + 2];
            switch (type) {
            case 0: logLine("  cAVS version %#x", v[0]); break;
            case 1: logLine("  DSP cores %u", v[0]); break;
            case 2: logLine("  memory page %u bytes", v[0]); break;
            case 3: logLine("  physical memory %u pages", v[0]); break;
            case 4: {
                logLine("  I2S version %#x, %u controllers", v[0], v[1]);
                for (uint32_t i = 0; i < v[1] && i < 8; i++)
                    logLine("    SSP%u base %#x", i, v[2 + i]);
                break;
            }
            case 6: logLine("  gateways %u", v[0]); break;
            default: break;
            }
            off += 8 + ((len + 3) & ~3u);
        }
    }
}

// ---------------------------------------------------------------------------
// Speaker path from avs-topology-xml i2s/max98927-tplg.xml (SSP0, 48 kHz):
//   ppl 0 (FE): copier#0 [HDA host DMA] -> peakvol#0 -> copier#1
//   ppl 1 (BE): copier#2 [I2S link SSP0, NHLT blob]
//   bind copier#1 -> copier#2
// Linux: path.c avs_copier_create / avs_peakvol_create, messages.c.

static const int16_t gSine1k[48] = {0, 1069, 2120, 3135, 4096, 4987, 5793, 6499, 7094, 7568, 7913, 8122, 8192, 8122, 7913, 7568, 7094, 6499, 5793, 4987, 4096, 3135, 2120, 1069, 0, -1069, -2120, -3135, -4096, -4987, -5793, -6499, -7094, -7568, -7913, -8122, -8192, -8122, -7913, -7568, -7094, -6499, -5793, -4987, -4096, -3135, -2120, -1069};

static const uint8_t kCopierUuid[16] = {0x83, 0x0c, 0xa0, 0x9b, 0x12, 0xca, 0x83, 0x4a,
                                        0x94, 0x3c, 0x1f, 0xa2, 0xe8, 0x2f, 0x9d, 0xda};
static const uint8_t kPeakvolUuid[16] = {0x23, 0x13, 0x17, 0x8a, 0xa3, 0x94, 0x1d, 0x4e,
                                         0xaf, 0xe9, 0xfe, 0x5d, 0xba, 0xa4, 0xc3, 0x93};

// avs_audio_format: 48 kHz stereo, 16 valid bits in a 16- or 32-bit container.
static void putFormat(uint32_t *w, uint32_t container, uint32_t valid = 16) {
    w[0] = 48000;
    w[1] = container;
    w[2] = 0xFFFFFF10;      // channel map L, R
    w[3] = 1;               // channel config: stereo
    w[4] = 0;               // interleaved
    w[5] = 2 | (valid << 8); // channels | valid bits << 8 | sample type 0
}

// avs_modcfg_base
static uint32_t *putBase(uint32_t *w, uint32_t cpc, uint32_t ibs, uint32_t obs, uint32_t container,
                         uint32_t valid = 16) {
    w[0] = cpc;
    w[1] = ibs;
    w[2] = obs;
    w[3] = 0;               // is_pages
    putFormat(w + 4, container, valid);
    return w + 10;
}

int C434TAudioDSP::ipcGlobal(uint32_t primary, uint32_t ext) {
    return ipcSend(((uint64_t)ext << 32) | primary, nullptr, 0, nullptr, nullptr, nullptr);
}

int C434TAudioDSP::ipcModule(uint32_t primary, uint32_t ext, const void *data, uint32_t size) {
    return ipcSend(((uint64_t)ext << 32) | primary | MSG_TARGET_MOD, data, size, nullptr,
                   nullptr, nullptr);
}

bool C434TAudioDSP::findModules() {
    static uint8_t buf[MAILBOX_SIZE];
    uint32_t size = sizeof(buf);
    if (getLargeConfig(0, 0, BASEFW_MODULES_INFO, buf, &size))
        return false;
    uint32_t count = *(uint32_t *)buf;
    for (uint32_t i = 0; i < count && 4 + (i + 1) * 116 <= size; i++) {
        const uint8_t *e = buf + 4 + i * 116;
        uint16_t id = *(const uint16_t *)e;
        char name[9] = {};
        memcpy(name, e + 4, 8);
        logLine("  module %u: %s", id, name);
        if (!memcmp(e + 12, kCopierUuid, 16))
            copierId = id;
        if (!memcmp(e + 12, kPeakvolUuid, 16))
            peakvolId = id;
    }
    logLine("copier id %u, peakvol id %u", copierId, peakvolId);
    return copierId != 0xffff && peakvolId != 0xffff;
}

int C434TAudioDSP::buildSpeakerPath() {
    static uint32_t cfg[MAILBOX_SIZE / 4];
    int ret;

    // CREATE_PIPELINE: 12 pages, priority 0, not low-power.
    for (uint32_t ppl = 0; ppl < 2; ppl++) {
        ret = ipcGlobal(12 | (0u << 11) | (ppl << 16) | (GLB_CREATE_PIPELINE << 24), 0);
        if (ret) {
            logLine("create pipeline %u failed: %d", ppl, ret);
            return ret;
        }
    }

    // copier#0: HDA host output gateway, dma_id = stream tag - 1.
    uint32_t *w = putBase(cfg, 14473, 192, 192, 16);
    putFormat(w, 16);               // out_fmt
    w[6] = 0;                       // feature mask
    w[7] = (DMA_HDA_HOST_OUTPUT << 8) | (HOST_STREAM_TAG - 1);
    w[8] = 768;                     // dma buffer size
    w[9] = 1;                       // config_length: gateway attributes only
    w[10] = 0;
    ret = ipcModule(copierId | (0u << 16) | (MOD_INIT_INSTANCE << 24),
                    21 | (0u << 16), cfg, 21 * 4);
    if (ret) {
        logLine("init host copier failed: %d", ret);
        return ret;
    }

    // peakvol#0: unity gain on all channels.
    w = putBase(cfg, 14473, 192, 192, 16);
    w[0] = 0xffffffff;              // channel_id: all channels
    w[1] = 0x7fffffff;              // target volume (S32_MAX)
    w[2] = 0;                       // curve type
    w[3] = 0;
    w[4] = 0;                       // curve duration (u64)
    w[5] = 0;
    ret = ipcModule(peakvolId | (0u << 16) | (MOD_INIT_INSTANCE << 24),
                    16 | (0u << 16), cfg, 16 * 4);
    if (ret) {
        logLine("init peakvol failed: %d", ret);
        return ret;
    }

    // copier#1: no gateway, feeds the BE pipeline.
    w = putBase(cfg, 14473, 192, 192, 16);
    putFormat(w, 16);
    w[6] = 0;
    w[7] = 0xffffffff;              // INVALID_NODE_ID
    w[8] = 0;
    w[9] = 1;
    w[10] = 0;
    ret = ipcModule(copierId | (1u << 16) | (MOD_INIT_INSTANCE << 24),
                    21 | (0u << 16), cfg, 21 * 4);
    if (ret) {
        logLine("init FE out copier failed: %d", ret);
        return ret;
    }

    // copier#2: I2S link output on SSP0 (vindex instance 0, slot 0), 32-bit
    // container out, NHLT gateway blob for 48 kHz/16-bit/2 ch.
    w = putBase(cfg, 14473, 192, 384, 16);
    putFormat(w, 32);
    w[6] = 0;
    w[7] = (DMA_I2S_LINK_OUTPUT << 8) | 0x00;
    w[8] = 768;
    w[9] = sizeof(gNhltSsp0Dir0Fmt4) / 4;
    memcpy(&w[10], gNhltSsp0Dir0Fmt4, sizeof(gNhltSsp0Dir0Fmt4));
    uint32_t words = 20 + sizeof(gNhltSsp0Dir0Fmt4) / 4;
    ret = ipcModule(copierId | (2u << 16) | (MOD_INIT_INSTANCE << 24),
                    words | (1u << 16), cfg, words * 4);
    if (ret) {
        logLine("init SSP0 copier failed: %d", ret);
        return ret;
    }

    // BIND src -> dst, queue 0 -> 0: copier#0 -> peakvol#0 -> copier#1 -> copier#2.
    struct { uint16_t mod; uint8_t inst; uint16_t dmod; uint8_t dinst; } binds[] = {
        {copierId, 0, peakvolId, 0}, {peakvolId, 0, copierId, 1}, {copierId, 1, copierId, 2}};
    for (auto &b : binds) {
        ret = ipcModule(b.mod | ((uint32_t)b.inst << 16) | (MOD_BIND << 24),
                        b.dmod | ((uint32_t)b.dinst << 16), nullptr, 0);
        if (ret) {
            logLine("bind %u.%u -> %u.%u failed: %d", b.mod, b.inst, b.dmod, b.dinst, ret);
            return ret;
        }
    }
    for (uint32_t ppl = 0; ppl < 2; ppl++)
        ipcGlobal(PPL_STATE_RESET | (ppl << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    logLine("speaker path built (pipelines 0, 1)");
    return 0;
}

// Headphone path, after avs-topology-xml i2s/da7219-tplg.xml (SSP1, 48 kHz,
// 24 valid bits in 32-bit slots) with a peakvol stage added so the macOS volume
// slider works the same as on the speakers:
//   ppl 0 (FE): copier#0 [HDA host DMA] -> peakvol#0 -> copier#1 (16 -> 32/24)
//   ppl 1 (BE): copier#2 [I2S link SSP1, NHLT blob]
int C434TAudioDSP::buildHeadphonePath() {
    static uint32_t cfg[MAILBOX_SIZE / 4];
    int ret;
    for (uint32_t ppl = 0; ppl < 2; ppl++) {
        ret = ipcGlobal(12 | (ppl << 16) | (GLB_CREATE_PIPELINE << 24), 0);
        if (ret) {
            logLine("create pipeline %u failed: %d", ppl, ret);
            return ret;
        }
    }
    uint32_t *w = putBase(cfg, 12000, 192, 192, 16);
    putFormat(w, 16);
    w[6] = 0;
    w[7] = (DMA_HDA_HOST_OUTPUT << 8) | (HOST_STREAM_TAG - 1);
    w[8] = 768;
    w[9] = 1;
    w[10] = 0;
    ret = ipcModule(copierId | (0u << 16) | (MOD_INIT_INSTANCE << 24), 21 | (0u << 16), cfg, 21 * 4);
    if (ret) {
        logLine("init host copier failed: %d", ret);
        return ret;
    }
    w = putBase(cfg, 12000, 192, 192, 16);
    w[0] = 0xffffffff;
    w[1] = 0x7fffffff;
    w[2] = w[3] = w[4] = w[5] = 0;
    ret = ipcModule(peakvolId | (0u << 16) | (MOD_INIT_INSTANCE << 24), 16 | (0u << 16), cfg, 16 * 4);
    if (ret) {
        logLine("init peakvol failed: %d", ret);
        return ret;
    }
    w = putBase(cfg, 12000, 192, 384, 16);
    putFormat(w, 32, 24);
    w[6] = 0;
    w[7] = 0xffffffff;
    w[8] = 0;
    w[9] = 1;
    w[10] = 0;
    ret = ipcModule(copierId | (1u << 16) | (MOD_INIT_INSTANCE << 24), 21 | (0u << 16), cfg, 21 * 4);
    if (ret) {
        logLine("init FE out copier failed: %d", ret);
        return ret;
    }
    // copier#2: I2S link output on SSP1 (vindex instance 1, slot 0).
    w = putBase(cfg, 12000, 384, 384, 32, 24);
    putFormat(w, 32, 24);
    w[6] = 0;
    w[7] = (DMA_I2S_LINK_OUTPUT << 8) | 0x10;
    w[8] = 768;
    w[9] = sizeof(gNhltSsp1Dir0Fmt2) / 4;
    memcpy(&w[10], gNhltSsp1Dir0Fmt2, sizeof(gNhltSsp1Dir0Fmt2));
    uint32_t words = 20 + sizeof(gNhltSsp1Dir0Fmt2) / 4;
    ret = ipcModule(copierId | (2u << 16) | (MOD_INIT_INSTANCE << 24), words | (1u << 16), cfg, words * 4);
    if (ret) {
        logLine("init SSP1 copier failed: %d", ret);
        return ret;
    }
    struct { uint16_t mod; uint8_t inst; uint16_t dmod; uint8_t dinst; } binds[] = {
        {copierId, 0, peakvolId, 0}, {peakvolId, 0, copierId, 1}, {copierId, 1, copierId, 2}};
    for (auto &b : binds) {
        ret = ipcModule(b.mod | ((uint32_t)b.inst << 16) | (MOD_BIND << 24),
                        b.dmod | ((uint32_t)b.dinst << 16), nullptr, 0);
        if (ret) {
            logLine("bind %u.%u -> %u.%u failed: %d", b.mod, b.inst, b.dmod, b.dinst, ret);
            return ret;
        }
    }
    for (uint32_t ppl = 0; ppl < 2; ppl++)
        ipcGlobal(PPL_STATE_RESET | (ppl << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    logLine("headphone path built (pipelines 0, 1)");
    return 0;
}

void C434TAudioDSP::teardownSpeakerPath() {
    for (uint32_t ppl = 0; ppl < 2; ppl++)
        ipcGlobal(PPL_STATE_PAUSED | (ppl << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    for (uint32_t ppl = 0; ppl < 2; ppl++)
        ipcGlobal(PPL_STATE_RESET | (ppl << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    for (uint32_t ppl = 0; ppl < 2; ppl++)
        ipcGlobal((ppl << 16) | (GLB_DELETE_PIPELINE << 24), 0);
}

// Host output stream HOST_STREAM_INDEX, decoupled from the HDA link so the DSP
// host gateway consumes it (snd_hdac_ext_stream_decouple + snd_hdac_stream_setup).
bool C434TAudioDSP::setupHostStream(bool tone) {
    if (!hostBuf) {
        hostBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryKernelUserShared,
            HOST_BUFFER_BYTES, 0x00000000FFFFF000ULL);
        hostBdl = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 4096,
            0x00000000FFFFF000ULL);
        if (!hostBuf || !hostBdl)
            return false;
        hostBuf->prepare();
        hostBdl->prepare();
    }
    int16_t *pcm = (int16_t *)hostBuf->getBytesNoCopy();
    for (uint32_t i = 0; i < HOST_BUFFER_BYTES / 4; i++)
        pcm[2 * i] = pcm[2 * i + 1] = tone ? gSine1k[i % 48] : 0;

    uint64_t data = hostBuf->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    uint64_t bdlPhys = hostBdl->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    uint32_t *bdl = (uint32_t *)hostBdl->getBytesNoCopy();
    for (uint32_t i = 0; i < HOST_PERIODS; i++) {
        uint64_t addr = data + i * HOST_PERIOD_BYTES;
        bdl[i * 4 + 0] = (uint32_t)addr;
        bdl[i * 4 + 1] = (uint32_t)(addr >> 32);
        bdl[i * 4 + 2] = HOST_PERIOD_BYTES;
        bdl[i * 4 + 3] = 1; // IOC
    }

    // Decouple: PPCTL.PROCEN(index).
    uint32_t off = hdaRead32(AZX_REG_LLCH) & 0xffff;
    for (int n = 0; off && off < 0x4000 && n < 16; n++) {
        uint32_t hdr = hdaRead32(off);
        if (((hdr >> 16) & 0xfff) == AZX_PP_CAP_ID) {
            hdaWrite32(off + AZX_REG_PP_PPCTL,
                       hdaRead32(off + AZX_REG_PP_PPCTL) | (1u << HOST_STREAM_INDEX));
            break;
        }
        off = hdr & 0xffff;
    }

    // Stream reset.
    hda[HOST_SD + SD_CTL] = hda[HOST_SD + SD_CTL] | SD_CTL_STREAM_RESET;
    for (int i = 0; i < 300 && !(hda[HOST_SD + SD_CTL] & SD_CTL_STREAM_RESET); i++)
        IODelay(3);
    hda[HOST_SD + SD_CTL] = hda[HOST_SD + SD_CTL] & ~SD_CTL_STREAM_RESET;
    for (int i = 0; i < 300 && (hda[HOST_SD + SD_CTL] & SD_CTL_STREAM_RESET); i++)
        IODelay(3);
    hda[HOST_SD + SD_STS] = 0x1c;

    hdaWrite32(HOST_SD + SD_CTL, (hdaRead32(HOST_SD + SD_CTL) & ~SD_CTL_STRM_MASK) |
                                     (HOST_STREAM_TAG << 20));
    hdaWrite32(HOST_SD + SD_CBL, HOST_BUFFER_BYTES);
    hdaWrite16(HOST_SD + SD_FMT, 0x0011);   // 48 kHz, 16-bit, 2 channels
    hdaWrite16(HOST_SD + SD_LVI, HOST_PERIODS - 1);
    hdaWrite32(HOST_SD + SD_BDLPL, (uint32_t)bdlPhys);
    hdaWrite32(HOST_SD + SD_BDLPU, (uint32_t)(bdlPhys >> 32));
    logLine("host stream %u: tag %u, buffer %#llx, SD_CTL %#010x", HOST_STREAM_INDEX,
            HOST_STREAM_TAG, (unsigned long long)data, hdaRead32(HOST_SD + SD_CTL));
    return true;
}

int C434TAudioDSP::buildMicPath() {
    static uint32_t cfg[MAILBOX_SIZE / 4];
    int ret;
    for (uint32_t ppl = 2; ppl < 4; ppl++) {
        ret = ipcGlobal(2 | (ppl << 16) | (GLB_CREATE_PIPELINE << 24), 0);
        if (ret) {
            logLine("create mic pipeline %u failed: %d", ppl, ret);
            return ret;
        }
    }
    // copier#3: DMIC link input, PDM blob for 48 kHz / 16-bit / 2 ch.
    uint32_t *w = putBase(cfg, 10000, 192, 192, 16);
    putFormat(w, 16);
    w[6] = 0;
    w[7] = (DMA_DMIC_LINK_INPUT << 8) | 0x00;
    w[8] = 384;
    w[9] = sizeof(gNhltPdm0Dir1Fmt0) / 4;
    memcpy(&w[10], gNhltPdm0Dir1Fmt0, sizeof(gNhltPdm0Dir1Fmt0));
    uint32_t words = 20 + sizeof(gNhltPdm0Dir1Fmt0) / 4;
    ret = ipcModule(copierId | (3u << 16) | (MOD_INIT_INSTANCE << 24), words | (2u << 16), cfg, words * 4);
    if (ret) {
        logLine("init DMIC copier failed: %d", ret);
        return ret;
    }
    // copier#4: FE input, no gateway.
    w = putBase(cfg, 10000, 192, 192, 16);
    putFormat(w, 16);
    w[6] = 0;
    w[7] = 0xffffffff;
    w[8] = 0;
    w[9] = 1;
    w[10] = 0;
    ret = ipcModule(copierId | (4u << 16) | (MOD_INIT_INSTANCE << 24), 21 | (3u << 16), cfg, 21 * 4);
    if (ret) {
        logLine("init mic FE copier failed: %d", ret);
        return ret;
    }
    // peakvol#1: unity mic gain.
    w = putBase(cfg, 10000, 192, 192, 16);
    w[0] = 0xffffffff;
    w[1] = 0x7fffffff;
    w[2] = w[3] = w[4] = w[5] = 0;
    ret = ipcModule(peakvolId | (1u << 16) | (MOD_INIT_INSTANCE << 24), 16 | (3u << 16), cfg, 16 * 4);
    if (ret) {
        logLine("init mic peakvol failed: %d", ret);
        return ret;
    }
    // copier#5: HDA host input gateway, dma_id = capture stream tag - 1.
    w = putBase(cfg, 10000, 192, 192, 16);
    putFormat(w, 16);
    w[6] = 0;
    w[7] = (DMA_HDA_HOST_INPUT << 8) | (CAP_STREAM_TAG - 1);
    w[8] = 384;
    w[9] = 1;
    w[10] = 0;
    ret = ipcModule(copierId | (5u << 16) | (MOD_INIT_INSTANCE << 24), 21 | (3u << 16), cfg, 21 * 4);
    if (ret) {
        logLine("init mic host copier failed: %d", ret);
        return ret;
    }
    struct { uint16_t mod; uint8_t inst; uint16_t dmod; uint8_t dinst; } binds[] = {
        {copierId, 3, copierId, 4}, {copierId, 4, peakvolId, 1}, {peakvolId, 1, copierId, 5}};
    for (auto &b : binds) {
        ret = ipcModule(b.mod | ((uint32_t)b.inst << 16) | (MOD_BIND << 24),
                        b.dmod | ((uint32_t)b.dinst << 16), nullptr, 0);
        if (ret) {
            logLine("mic bind %u.%u -> %u.%u failed: %d", b.mod, b.inst, b.dmod, b.dinst, ret);
            return ret;
        }
    }
    for (uint32_t ppl = 2; ppl < 4; ppl++)
        ipcGlobal(PPL_STATE_RESET | (ppl << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    logLine("mic path built (pipelines 2, 3)");
    return 0;
}

void C434TAudioDSP::teardownMicPath() {
    for (uint32_t ppl = 2; ppl < 4; ppl++)
        ipcGlobal(PPL_STATE_PAUSED | (ppl << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    for (uint32_t ppl = 2; ppl < 4; ppl++)
        ipcGlobal(PPL_STATE_RESET | (ppl << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    for (uint32_t ppl = 2; ppl < 4; ppl++)
        ipcGlobal((ppl << 16) | (GLB_DELETE_PIPELINE << 24), 0);
}

// Input stream CAP_STREAM_INDEX, decoupled, same ring layout as playback.
bool C434TAudioDSP::setupCaptureStream() {
    if (!capBuf) {
        capBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryKernelUserShared,
            HOST_BUFFER_BYTES, 0x00000000FFFFF000ULL);
        capBdl = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 4096,
            0x00000000FFFFF000ULL);
        if (!capBuf || !capBdl)
            return false;
        capBuf->prepare();
        capBdl->prepare();
    }
    bzero(capBuf->getBytesNoCopy(), HOST_BUFFER_BYTES);
    uint64_t data = capBuf->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    uint64_t bdlPhys = capBdl->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    uint32_t *bdl = (uint32_t *)capBdl->getBytesNoCopy();
    for (uint32_t i = 0; i < HOST_PERIODS; i++) {
        uint64_t addr = data + i * HOST_PERIOD_BYTES;
        bdl[i * 4 + 0] = (uint32_t)addr;
        bdl[i * 4 + 1] = (uint32_t)(addr >> 32);
        bdl[i * 4 + 2] = HOST_PERIOD_BYTES;
        bdl[i * 4 + 3] = 1;
    }
    uint32_t off = hdaRead32(AZX_REG_LLCH) & 0xffff;
    for (int n = 0; off && off < 0x4000 && n < 16; n++) {
        uint32_t hdr = hdaRead32(off);
        if (((hdr >> 16) & 0xfff) == AZX_PP_CAP_ID) {
            hdaWrite32(off + AZX_REG_PP_PPCTL,
                       hdaRead32(off + AZX_REG_PP_PPCTL) | (1u << CAP_STREAM_INDEX));
            break;
        }
        off = hdr & 0xffff;
    }
    hda[CAP_SD + SD_CTL] = hda[CAP_SD + SD_CTL] | SD_CTL_STREAM_RESET;
    for (int i = 0; i < 300 && !(hda[CAP_SD + SD_CTL] & SD_CTL_STREAM_RESET); i++)
        IODelay(3);
    hda[CAP_SD + SD_CTL] = hda[CAP_SD + SD_CTL] & ~SD_CTL_STREAM_RESET;
    for (int i = 0; i < 300 && (hda[CAP_SD + SD_CTL] & SD_CTL_STREAM_RESET); i++)
        IODelay(3);
    hda[CAP_SD + SD_STS] = 0x1c;
    hdaWrite32(CAP_SD + SD_CTL, (hdaRead32(CAP_SD + SD_CTL) & ~SD_CTL_STRM_MASK) |
                                    (CAP_STREAM_TAG << 20));
    hdaWrite32(CAP_SD + SD_CBL, HOST_BUFFER_BYTES);
    hdaWrite16(CAP_SD + SD_FMT, 0x0011);
    hdaWrite16(CAP_SD + SD_LVI, HOST_PERIODS - 1);
    hdaWrite32(CAP_SD + SD_BDLPL, (uint32_t)bdlPhys);
    hdaWrite32(CAP_SD + SD_BDLPU, (uint32_t)(bdlPhys >> 32));
    return true;
}

// Build the speaker path, start the host DMA and the amps. With tone, the
// ring is pre-filled with a 1 kHz sine; otherwise with silence for the plug-in.
int C434TAudioDSP::streamStart(bool tone) {
    if (streaming)
        return 0;
    if (suspended)
        return EBUSY;
    if (!fwRunning) {
        int ret = bootFirmware();
        if (ret)
            return ret;
    }
    if (copierId == 0xffff && !findModules())
        return ENOENT;
    int ret = buildSpeakerPath();
    if (ret) {
        teardownSpeakerPath();
        return ret;
    }
    if (!setupHostStream(tone)) {
        teardownSpeakerPath();
        return ENOMEM;
    }
    // Make the HDA DMA snoop CPU caches: the plug-in writes the ring through a
    // cached mapping and a non-snooped DMA would read stale samples (crackle).
    uint16_t devc = pci->configRead16(INTEL_HDA_DEVC);
    if (devc & INTEL_HDA_DEVC_NOSNOOP)
        pci->configWrite16(INTEL_HDA_DEVC, devc & ~INTEL_HDA_DEVC_NOSNOOP);
    logLine("DEVC %#06x -> %#06x", devc, pci->configRead16(INTEL_HDA_DEVC));
    // Pause BE then FE, run BE, start host DMA, run FE (playback start order).
    ipcGlobal(PPL_STATE_PAUSED | (1u << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    ipcGlobal(PPL_STATE_PAUSED | (0u << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    ret = ipcGlobal(PPL_STATE_RUNNING | (1u << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    if (ret)
        logLine("run BE pipeline failed: %d", ret);

    IOSimpleLockLock(posLock);
    uint64_t oldWraps = wraps, oldZero = zeroHost;
    lastFrame = 0;
    wraps = 0;
    clock_get_uptime(&zeroHost);
    zeroSeed++;
    IOSimpleLockUnlock(posLock);
    hda[HOST_SD + SD_CTL] = hda[HOST_SD + SD_CTL] | SD_CTL_RUN;
    ret = ipcGlobal(PPL_STATE_RUNNING | (0u << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
    if (ret)
        logLine("run FE pipeline failed: %d", ret);
    // SSP0 is now clocking: bring up the amplifiers (DAPM POST_PMU order).
    for (C434TAmp *amp : gAmps) {
        if (!amp) {
            logLine("an amplifier is missing (I2C5 / VoodooI2C)");
            continue;
        }
        amp->configure();
        amp->enable(true);
    }
    if (volumeQ31 != 0x7fffffff)
        setVolume(volumeQ31);
    // Microphones: optional, playback works without them.
    micRunning = false;
    if (!tone && buildMicPath() == 0 && setupCaptureStream()) {
        ipcGlobal(PPL_STATE_PAUSED | (2u << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
        ipcGlobal(PPL_STATE_PAUSED | (3u << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
        hda[CAP_SD + SD_CTL] = hda[CAP_SD + SD_CTL] | SD_CTL_RUN;
        int r3 = ipcGlobal(PPL_STATE_RUNNING | (3u << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
        int r2 = ipcGlobal(PPL_STATE_RUNNING | (2u << 16) | (GLB_SET_PIPELINE_STATE << 24), 0);
        micRunning = !r2 && !r3;
        logLine("mic %s (run FE %d BE %d)", micRunning ? "running" : "failed", r3, r2);
    } else if (!tone) {
        teardownMicPath();
    }
    // Anchor the first zero timestamp on the live DMA position, now that the
    // pipelines and amps are up: the HAL keeps whatever offset the first
    // timestamp carries, and the setup above takes tens of milliseconds.
    {
        uint64_t now, back;
        IOSimpleLockLock(posLock);
        uint32_t f = (hdaRead32(HOST_SD + SD_LPIB) / 4) % kRingFrames;
        uint32_t c = (hdaRead32(CAP_SD + SD_LPIB) / 4) % kRingFrames;
        clock_get_uptime(&now);
        capOffset = (c + kRingFrames - f) % kRingFrames;
        nanoseconds_to_absolutetime((uint64_t)f * 1000000000ull / 48000, &back);
        // After a wake, keep the plug-in's sample time running forward: start
        // from the ring count the old timeline would have reached by now.
        uint64_t base = 0;
        if (continueTimeline && oldZero && now > oldZero) {
            uint64_t ns;
            absolutetime_to_nanoseconds(now - oldZero, &ns);
            base = oldWraps + ns / ((uint64_t)kRingFrames * 1000000000ull / 48000) + 1;
        }
        zeroHost = now - back;
        lastFrame = f;
        eraseFrame = f;
        erase = !tone;
        wraps = base;
        IOSimpleLockUnlock(posLock);
    }
    streaming = true;
    posTimer->setTimeoutMS(10);
    logLine("stream started");
    return 0;
}

void C434TAudioDSP::streamStop() {
    if (!streaming)
        return;
    streaming = false;
    posTimer->cancelTimeout();
    for (C434TAmp *amp : gAmps)
        if (amp)
            amp->enable(false);
    hda[HOST_SD + SD_CTL] = hda[HOST_SD + SD_CTL] & ~SD_CTL_RUN;
    teardownSpeakerPath();
    if (micRunning) {
        hda[CAP_SD + SD_CTL] = hda[CAP_SD + SD_CTL] & ~SD_CTL_RUN;
        teardownMicPath();
        micRunning = false;
    }
    logLine("stream stopped");
}

IOReturn C434TAudioDSP::sleepWakeHandler(void *target, void *, UInt32 msg, IOService *, void *,
                                         vm_size_t) {
    C434TAudioDSP *self = (C434TAudioDSP *)target;
    switch (msg) {
    case kIOMessageSystemWillSleep:
    case kIOMessageSystemWillPowerOff:
    case kIOMessageSystemWillRestart:
        self->resumeTimer->cancelTimeout();
        IOLockLock(gLock);
        self->suspended = true;
        if (self->streaming) {
            self->resumeAfterWake = true;
            self->streamStop();
        }
        if (self->fwRunning) {
            self->coreDisable();
            self->fwRunning = false;
        }
        logLine("system %s: stream %s", msg == kIOMessageSystemWillSleep ? "sleep" : "shutdown",
                self->resumeAfterWake ? "suspended" : "idle");
        IOLockUnlock(gLock);
        break;
    case kIOMessageSystemHasPoweredOn: {
        IOLockLock(gLock);
        self->suspended = false;
        bool resume = self->resumeAfterWake;
        IOLockUnlock(gLock);
        // Give the I2C controller (amps) a moment to finish its own wake.
        if (resume)
            self->resumeTimer->setTimeoutMS(300);
        break;
    }
    }
    return kIOReturnSuccess;
}

void C434TAudioDSP::resumeTimerFired(OSObject *owner, IOTimerEventSource *) {
    C434TAudioDSP *self = (C434TAudioDSP *)owner;
    IOLockLock(gLock);
    if (self->resumeAfterWake && !self->suspended && !self->streaming) {
        self->continueTimeline = true;
        int ret = self->streamStart(false);
        self->continueTimeline = false;
        logLine("wake: stream restart %d", ret);
    }
    self->resumeAfterWake = false;
    IOLockUnlock(gLock);
}

// peakvol LARGE_CONFIG_SET AVS_PEAKVOL_VOLUME, all channels, no ramp.
int C434TAudioDSP::setVolume(uint32_t q31) {
    volumeQ31 = min(q31, 0x7fffffffu);
    if (!streaming && !fwRunning)
        return 0;
    uint32_t cfg[6] = {0xffffffff, volumeQ31, 0, 0, 0, 0};
    int ret = ipcModule(peakvolId | (0u << 16) | (4u << 24),
                        sizeof(cfg) | (0u << 20) | (1u << 28) | (1u << 29), cfg, sizeof(cfg));
    logLine("volume %#010x -> %d", volumeQ31, ret);
    return ret;
}

// Read the DMA position (LPIB) and count ring wraps. Called with posLock held.
void C434TAudioDSP::updatePosition(uint32_t *frame, uint64_t *now) {
    uint32_t f = (hdaRead32(HOST_SD + SD_LPIB) / 4) % kRingFrames;
    clock_get_uptime(now);
    if (f < lastFrame) {
        // Host time at which the ring restarted: now minus the frames played
        // since, smoothed against the nominal ring period.
        uint64_t ns = (uint64_t)f * 1000000000ull / 48000, back;
        nanoseconds_to_absolutetime(ns, &back);
        uint64_t measured = *now - back;
        uint64_t periodAbs;
        nanoseconds_to_absolutetime((uint64_t)kRingFrames * 1000000000ull / 48000, &periodAbs);
        uint64_t predicted = zeroHost + periodAbs;
        int64_t err = (int64_t)(measured - predicted);
        zeroHost = wraps ? predicted + err / 8 : measured;
        wraps++;
    }
    lastFrame = f;
    *frame = f;
}

int C434TAudioDSP::describePosition(char *buf, size_t len) {
    IOSimpleLockLock(posLock);
    uint64_t w = wraps;
    uint32_t lf = lastFrame;
    IOSimpleLockUnlock(posLock);
    return snprintf(buf, len,
                    "streaming %d wraps %llu lastFrame %u LPIB %u SD_CTL %#010x SD_STS %#04x "
                    "fw %#010x err %#010x HIPCT %#010x DEVC %#06x mic %d capLPIB %u",
                    streaming, (unsigned long long)w, lf, hdaRead32(HOST_SD + SD_LPIB),
                    hdaRead32(HOST_SD + SD_CTL), hda[HOST_SD + SD_STS], dspRead32(FW_REG_STATUS),
                    dspRead32(FW_REG_ERROR), dspRead32(SKL_REG_HIPCT),
                    pci->configRead16(INTEL_HDA_DEVC), micRunning, hdaRead32(CAP_SD + SD_LPIB));
}

void C434TAudioDSP::posTimerFired(OSObject *owner, IOTimerEventSource *sender) {
    C434TAudioDSP *self = (C434TAudioDSP *)owner;
    if (!self->streaming)
        return;
    uint32_t f;
    uint64_t now;
    IOSimpleLockLock(self->posLock);
    self->updatePosition(&f, &now);
    uint32_t from = self->eraseFrame;
    self->eraseFrame = f;
    IOSimpleLockUnlock(self->posLock);
    // Erase head: zero what the DMA has already fetched. The plug-in writes
    // ahead of the DMA; if it stops (lid closed, system on its way to sleep, a
    // hung client) the ring decays to silence instead of looping its last 85 ms.
    int16_t *pcm = (int16_t *)self->hostBuf->getBytesNoCopy();
    if (!self->erase) {
    } else if (f >= from) {
        bzero(pcm + 2 * from, (f - from) * 4);
    } else {
        bzero(pcm + 2 * from, (kRingFrames - from) * 4);
        bzero(pcm, f * 4);
    }
    // Clear sticky stream status (BCIS/FIFOE/DESE); nothing is interrupt driven.
    self->hda[HOST_SD + SD_STS] = 0x1c;
    sender->setTimeoutMS(10);
}

// AudioServerPlugIn GetZeroTimeStamp: sample time of the last ring start and
// the host time it happened at.
void C434TAudioDSP::zeroTimestamp(uint64_t *sampleTime, uint64_t *hostTime, uint64_t *seed) {
    uint32_t f;
    uint64_t now;
    IOSimpleLockLock(posLock);
    if (streaming)
        updatePosition(&f, &now);
    *sampleTime = wraps * kRingFrames;
    *hostTime = zeroHost;
    *seed = zeroSeed;
    IOSimpleLockUnlock(posLock);
}

int C434TAudioDSP::testTone() {
    gLogLen = 0;
    gLog[0] = 0;
    bool resume = streaming;
    if (resume) {
        logLine("pausing the CoreAudio stream for the test tone");
        streamStop();
    }
    int ret = streamStart(true);
    if (ret)
        return ret;
    for (int i = 0; i < 8; i++) {
        IOSleep(250);
        uint64_t st, ht, seed;
        zeroTimestamp(&st, &ht, &seed);
        logLine("t=%dms LPIB %u zero sample %llu fw status %#010x error %#010x", (i + 1) * 250,
                hdaRead32(HOST_SD + SD_LPIB), (unsigned long long)st, dspRead32(FW_REG_STATUS),
                dspRead32(FW_REG_ERROR));
    }
    streamStop();
    logLine("test tone done");
    if (resume)
        streamStart(false);
    return 0;
}

int C434TAudioDSP::bootFirmware() {
    gLogLen = 0;
    gLog[0] = 0;
    uint32_t reg = 0;
    int ret = EIO;

    fwRunning = false;
    logLine("--- boot: GCTL %#010x ADSPCS %#010x", hdaRead32(AZX_REG_GCTL), dspRead32(ADSP_REG_ADSPCS));
    if (!allocCodeLoader()) {
        logLine("code loader buffer allocation failed");
        return ENOMEM;
    }
    if (!resetController()) {
        logLine("HDA controller reset failed, GCTL %#010x", hdaRead32(AZX_REG_GCTL));
        return EIO;
    }
    logLine("controller out of reset: GCTL %#010x STATESTS %#06x", hdaRead32(AZX_REG_GCTL),
            hdaRead16(AZX_REG_STATESTS));
    if (!enableProcessingPipe()) {
        logLine("processing pipe capability not found");
        return EIO;
    }

    coreDisable();
    logLine("core 0 disabled: ADSPCS %#010x", dspRead32(ADSP_REG_ADSPCS));
    gating(false);

    if (!corePower(true)) {
        logLine("core 0 power-up failed: ADSPCS %#010x", dspRead32(ADSP_REG_ADSPCS));
        goto out;
    }
    if (!coreReset(false)) {
        logLine("core 0 reset exit failed: ADSPCS %#010x", dspRead32(ADSP_REG_ADSPCS));
        goto out;
    }
    logLine("core 0 powered: ADSPCS %#010x", dspRead32(ADSP_REG_ADSPCS));
    if (!cldmaReset()) {
        logLine("CLDMA reset failed: SD_CTL %#04x", dspRead8(CL_SD + SD_CTL));
        goto out;
    }
    cldmaSetup();
    if (!coreStall(false)) {
        logLine("core 0 unstall failed: ADSPCS %#010x", dspRead32(ADSP_REG_ADSPCS));
        goto out;
    }
    // avs_dsp_interrupt_control(true)
    dspUpdate32(ADSP_REG_ADSPIC, ADSPIC_IPC, ADSPIC_IPC);
    dspUpdate32(SKL_REG_HIPCCTL, HIPCCTL_DONE | HIPCCTL_BUSY, HIPCCTL_DONE | HIPCCTL_BUSY);

    if (!pollDsp32(FW_REG_STATUS, ROM_INIT_DONE, ROM_INIT_DONE, 5, 1000000, &reg)) {
        logLine("ROM init timeout: status %#010x error %#010x", reg, dspRead32(FW_REG_ERROR));
        coreDisable();
        goto out;
    }
    logLine("ROM init done: status %#010x", reg);

    if (!transferFirmware(gC434TFirmwareSkl, sizeof(gC434TFirmwareSkl))) {
        cldmaStop();
        coreDisable();
        goto out;
    }
    if (!pollDsp32(FW_REG_STATUS, ROM_STS_MASK, SKL_BASEFW_ENTERED, 500, 3000000, &reg)) {
        cldmaStop();
        logLine("BASEFW_ENTERED timeout: status %#010x error %#010x", reg, dspRead32(FW_REG_ERROR));
        coreDisable();
        goto out;
    }
    cldmaStop();
    logLine("base firmware entered: status %#010x", reg);

    // FW_READY arrives as an unsolicited notification in HIPCT.
    if (!pollDsp32(SKL_REG_HIPCT, SKL_HIPCT_BUSY, SKL_HIPCT_BUSY, 500, 3000000, &reg)) {
        logLine("FW_READY timeout: HIPCT %#010x status %#010x error %#010x", reg,
                dspRead32(FW_REG_STATUS), dspRead32(FW_REG_ERROR));
        goto out;
    }
    {
        uint32_t ext = dspRead32(SKL_REG_HIPCTE);
        uint32_t type = (reg >> 16) & 0xff;
        logLine("DSP notification: HIPCT %#010x HIPCTE %#010x (type %u%s)", reg, ext, type,
                type == NOTIFY_FW_READY ? " = FW_READY" : "");
        dspWrite32(SKL_REG_HIPCT, SKL_HIPCT_BUSY); // tell the DSP we took it
        ret = type == NOTIFY_FW_READY ? 0 : EPROTO;
    }
    if (!ret) {
        logLine("DSP base firmware is running");
        fwRunning = true;
        queryFirmware();
        findModules();
    }

out:
    gating(true);
    logLine("--- done (%d): ADSPCS %#010x status %#010x error %#010x", ret,
            dspRead32(ADSP_REG_ADSPCS), dspRead32(FW_REG_STATUS), dspRead32(FW_REG_ERROR));
    return ret;
}



// ---------------------------------------------------------------------------
// User client for the CoreAudio HAL plug-in (plugin/C434TAudio.driver).
//   selector 0 start      -> out: ring frames, capture offset (0xffffffff: no mic)
//   selector 1 stop
//   selector 2 timestamp  -> out: zero sample time, zero host time, seed[, capture offset]
//   selector 3 volume     <- in: Q1.31 linear gain
//   memory type 0: the playback DMA ring, 16-bit stereo 48 kHz, kRingFrames frames.
//   memory type 1: the capture DMA ring, same layout.

class C434TAudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(C434TAudioUserClient)

public:
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override;

private:
    C434TAudioDSP *dsp = nullptr;
    bool started = false;
    static IOReturn sStart(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sStop(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sTimestamp(OSObject *, void *, IOExternalMethodArguments *);
    static IOReturn sVolume(OSObject *, void *, IOExternalMethodArguments *);
};

OSDefineMetaClassAndStructors(C434TAudioUserClient, IOUserClient)

bool C434TAudioUserClient::start(IOService *provider) {
    dsp = OSDynamicCast(C434TAudioDSP, provider);
    return dsp && IOUserClient::start(provider);
}

IOReturn C434TAudioUserClient::clientClose() {
    if (started) {
        IOLockLock(gLock);
        dsp->cancelResume();
        dsp->streamStop();
        IOLockUnlock(gLock);
        started = false;
    }
    terminate();
    return kIOReturnSuccess;
}

IOReturn C434TAudioUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                   IOMemoryDescriptor **memory) {
    IOBufferMemoryDescriptor *ring = type == 0 ? dsp->ringMemory()
                                   : type == 1 ? dsp->captureMemory() : nullptr;
    if (!ring)
        return kIOReturnBadArgument;
    ring->retain();
    *memory = ring;
    *options = kIOMapWriteCombineCache;
    return kIOReturnSuccess;
}

IOReturn C434TAudioUserClient::sStart(OSObject *target, void *, IOExternalMethodArguments *args) {
    C434TAudioUserClient *self = (C434TAudioUserClient *)target;
    IOLockLock(gLock);
    self->dsp->cancelResume();
    int ret = self->dsp->streamStart(false);
    IOLockUnlock(gLock);
    if (ret)
        return kIOReturnError;
    self->started = true;
    args->scalarOutput[0] = C434TAudioDSP::kRingFrames;
    args->scalarOutput[1] = self->dsp->captureOffset();
    return kIOReturnSuccess;
}

IOReturn C434TAudioUserClient::sStop(OSObject *target, void *, IOExternalMethodArguments *) {
    C434TAudioUserClient *self = (C434TAudioUserClient *)target;
    IOLockLock(gLock);
    self->dsp->cancelResume();
    self->dsp->streamStop();
    IOLockUnlock(gLock);
    self->started = false;
    return kIOReturnSuccess;
}

IOReturn C434TAudioUserClient::sTimestamp(OSObject *target, void *, IOExternalMethodArguments *args) {
    C434TAudioUserClient *self = (C434TAudioUserClient *)target;
    if (args->scalarOutputCount < 3)
        return kIOReturnBadArgument;
    self->dsp->zeroTimestamp(&args->scalarOutput[0], &args->scalarOutput[1], &args->scalarOutput[2]);
    // The capture offset changes when the stream is restarted after a wake.
    if (args->scalarOutputCount > 3)
        args->scalarOutput[3] = self->dsp->captureOffset();
    return kIOReturnSuccess;
}

IOReturn C434TAudioUserClient::sVolume(OSObject *target, void *, IOExternalMethodArguments *args) {
    C434TAudioUserClient *self = (C434TAudioUserClient *)target;
    IOLockLock(gLock);
    int ret = self->dsp->setVolume((uint32_t)args->scalarInput[0]);
    IOLockUnlock(gLock);
    return ret ? kIOReturnError : kIOReturnSuccess;
}

IOReturn C434TAudioUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                              IOExternalMethodDispatch *dispatch, OSObject *target,
                                              void *reference) {
    static const IOExternalMethodDispatch methods[] = {
        {(IOExternalMethodAction)&C434TAudioUserClient::sStart, 0, 0, 2, 0},
        {(IOExternalMethodAction)&C434TAudioUserClient::sStop, 0, 0, 0, 0},
        {(IOExternalMethodAction)&C434TAudioUserClient::sTimestamp, 0, 0, kIOUCVariableStructureSize, 0},
        {(IOExternalMethodAction)&C434TAudioUserClient::sVolume, 1, 0, 0, 0},
    };
    if (selector >= sizeof(methods) / sizeof(methods[0]))
        return kIOReturnUnsupported;
    return IOUserClient::externalMethod(selector, args, (IOExternalMethodDispatch *)&methods[selector],
                                        this, nullptr);
}

// ---------------------------------------------------------------------------
// MAX98927

OSDefineMetaClassAndStructors(C434TAmp, IOService)

bool C434TAmp::writeReg(uint16_t reg, uint8_t val) {
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)reg, val};
    return nub->writeI2C(buf, 3) == kIOReturnSuccess;
}

bool C434TAmp::readReg(uint16_t reg, uint8_t *val) {
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return nub->writeReadI2C(addr, 2, val, 1) == kIOReturnSuccess;
}

bool C434TAmp::updateReg(uint16_t reg, uint8_t mask, uint8_t val) {
    uint8_t cur = 0;
    if (!readReg(reg, &cur))
        return false;
    return writeReg(reg, (uint8_t)((cur & ~mask) | (val & mask)));
}

bool C434TAmp::start(IOService *provider) {
    if (!IOService::start(provider) || !provider->metaCast("VoodooI2CDeviceNub"))
        return false;
    nub = (VoodooI2CDeviceNub *)provider;
    right = !strcmp(provider->getName(), "MAXR");
    uint8_t rev = 0;
    bool ok = readReg(0x01ff, &rev);
    logLine("MAX98927 %s (%s): revision %#04x%s", label(), provider->getName(), rev,
            ok ? "" : " (I2C read failed)");
    gAmps[right ? 1 : 0] = this;
    registerService();
    return true;
}

void C434TAmp::stop(IOService *provider) {
    if (gAmps[right ? 1 : 0] == this)
        gAmps[right ? 1 : 0] = nullptr;
    IOService::stop(provider);
}

bool C434TAmp::configure() {
    const uint8_t v = right ? 4 : 6, i = right ? 5 : 7;  // ACPI vmon/imon-slot-no
    const uint8_t tx = right ? 0x30 : 0xc0;               // board TDM tx mask
    bool ok = true;
    uint8_t rev = 0;
    ok &= readReg(0x01ff, &rev);
    logLine("MAX98927 %s (%s) revision %#04x", label(), nub->getName(), rev);
    // max98927_probe
    ok &= writeReg(0x0100, 0x01);                          // soft reset
    IOSleep(5);
    static const uint16_t init[][2] = {
        {0x001c, 0xff}, {0x001d, 0xff}, {0x0025, 0x80}, {0x0026, 0x01},
        {0x0036, 0x38}, {0x003c, 0x05}, {0x0037, 0x03}, {0x003f, 0xf7},
        {0x0040, 0x1c}, {0x0042, 0x3e}, {0x0043, 0x04}, {0x0044, 0x00},
        {0x0045, 0x24}, {0x007f, 0x06}, {0x0082, 0x08}, {0x0086, 0x01},
        {0x0087, 0x10},
    };
    for (auto &r : init)
        ok &= writeReg(r[0], (uint8_t)r[1]);
    ok &= writeReg(0x001e, (uint8_t)((i << 4) | v));      // I/V slot sources
    // max98927_dai_set_fmt(DSP_B | NB_NF | CBC_CFC)
    ok &= updateReg(0x0021, 0x03, 0x00);                   // PCM consumer
    ok &= updateReg(0x0020, 0x04, 0x00);                   // normal BCLK edge
    ok &= updateReg(0x0018, 0x03, 0x03);                   // RX ch0/ch1
    ok &= updateReg(0x0020, 0x38, 0x03 << 3);              // TDM mode 0 (DSP_B)
    ok &= updateReg(0x003b, 0x03, 0x00);                   // speaker source: PCM
    ok &= updateReg(0x0035, 0x01, 0x00);                   // PDM RX off
    // max98927_dai_tdm_slot(tx, rx 0x3, 8 slots, 16 bits)
    ok &= updateReg(0x0022, 0x0f, 6);                      // BSEL: 128 BCLK/LRCLK
    ok &= updateReg(0x0020, 0xc0, 0x40);                   // 16-bit channels
    ok &= writeReg(0x0018, 0x03);
    ok &= writeReg(0x0019, 0x00);
    ok &= writeReg(0x001a, tx);
    ok &= writeReg(0x001b, 0x00);
    ok &= writeReg(0x001c, (uint8_t)~tx);
    ok &= writeReg(0x001d, 0xff);
    // max98927_dai_hw_params(48 kHz, 16-bit), interleave off
    ok &= updateReg(0x0023, 0x0f, 0x08);
    ok &= writeReg(0x0024, 0x88);                          // SR and IV ADC SR = 48 kHz
    // DAI Sel Mux: left amp plays slot 0, right amp slot 1.
    ok &= updateReg(0x0025, 0xc0, (uint8_t)((right ? 1 : 0) << 6));
    logLine("MAX98927 %s configured%s", label(), ok ? "" : " (I2C errors)");
    return ok;
}

void C434TAmp::dumpStatus() {
    static const uint16_t regs[] = {0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0011,
                                    0x0018, 0x001a, 0x0020, 0x0021, 0x0022, 0x0023, 0x0024,
                                    0x0025, 0x003a, 0x00ff};
    char line[200];
    size_t n = 0;
    for (uint16_t r : regs) {
        uint8_t v = 0xee;
        readReg(r, &v);
        n += snprintf(line + n, sizeof(line) - n, " %03x=%02x", r, v);
        if (n >= sizeof(line))
            break;
    }
    logLine("MAX98927 %s:%s", label(), line);
}

// max98927_dac_event POST_PMU / POST_PMD
bool C434TAmp::enable(bool on) {
    bool ok;
    if (on)
        ok = updateReg(0x003a, 0x01, 0x01) && updateReg(0x00ff, 0x01, 0x01);
    else
        ok = updateReg(0x00ff, 0x01, 0x00) && updateReg(0x003a, 0x01, 0x00);
    uint8_t g = 0, a = 0;
    readReg(0x00ff, &g);
    readReg(0x003a, &a);
    // GLOBAL reads back 0 even while the amp is playing; AMP_EN reflects the write.
    logLine("MAX98927 %s %s: GLOBAL %#04x AMP_EN %#04x", label(), on ? "on" : "off", g, a);
    return ok;
}

// ---------------------------------------------------------------------------
// Module start/stop: register the sysctl nodes.

extern "C" kern_return_t c434t_audio_start(kmod_info_t *, void *) {
    gLock = IOLockAlloc();
    sysctl_register_oid(&sysctl__debug_c434t_audio_boot);
    sysctl_register_oid(&sysctl__debug_c434t_audio_log);
    sysctl_register_oid(&sysctl__debug_c434t_audio_test);
    sysctl_register_oid(&sysctl__debug_c434t_audio_pos);
    sysctl_register_oid(&sysctl__debug_c434t_audio_ring);
    return KERN_SUCCESS;
}

extern "C" kern_return_t c434t_audio_stop(kmod_info_t *, void *) {
    sysctl_unregister_oid(&sysctl__debug_c434t_audio_boot);
    sysctl_unregister_oid(&sysctl__debug_c434t_audio_log);
    sysctl_unregister_oid(&sysctl__debug_c434t_audio_test);
    sysctl_unregister_oid(&sysctl__debug_c434t_audio_pos);
    sysctl_unregister_oid(&sysctl__debug_c434t_audio_ring);
    if (gLock)
        IOLockFree(gLock);
    return KERN_SUCCESS;
}
