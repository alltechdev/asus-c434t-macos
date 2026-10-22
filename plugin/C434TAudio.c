// C434TAudio.driver: CoreAudio HAL plug-in (AudioServerPlugIn) for the ASUS C434T
// built-in speakers and microphones. The hardware side lives in C434TAudio.kext:
// the Intel cAVS DSP pipelines, the MAX98927 amplifiers and the DMIC array. This
// plug-in publishes two devices like a Mac does, Built-in Speakers and Built-in
// Microphone, maps the kext's playback and capture DMA rings through
// C434TAudioUserClient and moves the audio in and out of them. Both devices run
// off the same kext stream and clock.
//
// Structure follows Apple's NullAudio AudioServerPlugIn sample.

#include <CoreAudio/AudioServerPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Device = 2,
    kObjectID_Stream = 3,
    kObjectID_Volume = 4,
    kObjectID_Mute = 5,
    kObjectID_StreamIn = 6,
    kObjectID_Mic = 7,
    kObjectID_InVolume = 8,
};

#define kDeviceUID          "C434TAudio:Speakers"
#define kMicUID             "C434TAudio:Microphone"
#define kDeviceModelUID     "C434TAudio:Model"
#define kClockDomain        0x63343334 /* 'c434': speakers and mic share the DMA clock */
#define kSampleRate         48000.0
#define kChannels           2
#define kLatencyFrames      0
#define kSafetyOffsetFrames 384
#define kVolumeMinDB        -64.0f
#define kVolumeMaxDB        0.0f
// Microphone input volume: software gain, scalar 0..1 linear in dB.
#define kInGainMinDB        -30.0f
#define kInGainMaxDB        18.0f

// C434TAudioUserClient selectors (kext/C434TAudio/C434TAudio.cpp).
enum { kSelStart = 0, kSelStop = 1, kSelTimestamp = 2, kSelVolume = 3 };

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef gHost = NULL;
static UInt32 gRefCount = 0;
static UInt32 gIORunning = 0;          // both devices; the hardware runs while > 0
static UInt32 gSpeakerRunning = 0, gMicRunning = 0;
static Float32 gVolumeScalar = 1.0f;
static bool gMute = false;
static Float32 gInGainScalar = -kInGainMinDB / (kInGainMaxDB - kInGainMinDB); // 0 dB
static Float32 gInGain = 1.0f;

static io_connect_t gConnection = IO_OBJECT_NULL;
static int16_t *gRing = NULL;
static UInt32 gRingFrames = 4096;
static mach_vm_address_t gRingAddress = 0;
static mach_vm_size_t gRingSize = 0;
// Capture ring (microphones); index for playback-timeline frame f is (f + gCapOffset) % gRingFrames.
static int16_t *gCapRing = NULL;
static UInt32 gCapOffset = 0;
static mach_vm_address_t gCapAddress = 0;
static mach_vm_size_t gCapSize = 0;

// Diagnostics (log show --predicate 'subsystem == "com.c434t.audio"').
static os_log_t gLog;
static double gNsPerTick;
static Float64 gZeroSample;
static UInt64 gZeroHost;
static double gMinMargin = 1e9, gMaxMargin = -1e9;
static UInt64 gCycles, gLastReport;
static double gMinPeriodMs = 1e9, gMaxPeriodMs = 0;

// ---------------------------------------------------------------------------
// Kext connection

static kern_return_t OpenHardware(void) {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                       IOServiceMatching("C434TAudioDSP"));
    if (!service)
        return kIOReturnNoDevice;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gConnection);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS)
        return kr;
    uint64_t out[2] = {0, 0xffffffff};
    uint32_t outCount = 2;
    kr = IOConnectCallScalarMethod(gConnection, kSelStart, NULL, 0, out, &outCount);
    if (kr == KERN_SUCCESS) {
        gRingFrames = (UInt32)out[0];
        kr = IOConnectMapMemory64(gConnection, 0, mach_task_self(), &gRingAddress, &gRingSize,
                                  kIOMapAnywhere);
        gRing = (int16_t *)(uintptr_t)gRingAddress;
    }
    gCapRing = NULL;
    if (kr == KERN_SUCCESS && out[1] != 0xffffffff &&
        IOConnectMapMemory64(gConnection, 1, mach_task_self(), &gCapAddress, &gCapSize,
                             kIOMapAnywhere) == KERN_SUCCESS) {
        gCapOffset = (UInt32)out[1];
        gCapRing = (int16_t *)(uintptr_t)gCapAddress;
    }
    if (kr != KERN_SUCCESS) {
        IOServiceClose(gConnection);
        gConnection = IO_OBJECT_NULL;
        gRing = NULL;
    }
    return kr;
}

static void CloseHardware(void) {
    if (gConnection == IO_OBJECT_NULL)
        return;
    IOConnectCallScalarMethod(gConnection, kSelStop, NULL, 0, NULL, NULL);
    if (gRingAddress)
        IOConnectUnmapMemory64(gConnection, 0, mach_task_self(), gRingAddress);
    if (gCapRing)
        IOConnectUnmapMemory64(gConnection, 1, mach_task_self(), gCapAddress);
    gCapRing = NULL;
    gCapAddress = 0;
    IOServiceClose(gConnection);
    gConnection = IO_OBJECT_NULL;
    gRing = NULL;
    gRingAddress = 0;
}

// Volume curve: scalar 0..1 maps linearly onto -64..0 dB; the DSP's peakvol
// takes a Q1.31 linear gain.
static Float32 ScalarToDB(Float32 scalar) {
    return kVolumeMinDB + scalar * (kVolumeMaxDB - kVolumeMinDB);
}

static Float32 DBToScalar(Float32 db) {
    if (db < kVolumeMinDB) db = kVolumeMinDB;
    if (db > kVolumeMaxDB) db = kVolumeMaxDB;
    return (db - kVolumeMinDB) / (kVolumeMaxDB - kVolumeMinDB);
}

static void PushVolume(void) {
    if (gConnection == IO_OBJECT_NULL)
        return;
    double gain = gVolumeScalar <= 0.0f ? 0.0 : pow(10.0, ScalarToDB(gVolumeScalar) / 20.0);
    uint64_t q31 = (uint64_t)(gain * 2147483647.0);
    IOConnectCallScalarMethod(gConnection, kSelVolume, &q31, 1, NULL, NULL);
}

// ---------------------------------------------------------------------------
// Plug-in interface

static HRESULT C434T_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG C434T_AddRef(void *inDriver);
static ULONG C434T_Release(void *inDriver);
static OSStatus C434T_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus C434T_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus C434T_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus C434T_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus C434T_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus C434T_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus C434T_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean C434T_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
static OSStatus C434T_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
static OSStatus C434T_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static OSStatus C434T_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, UInt32 *, void *);
static OSStatus C434T_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
static OSStatus C434T_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus C434T_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus C434T_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
static OSStatus C434T_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
static OSStatus C434T_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
static OSStatus C434T_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus C434T_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    C434T_QueryInterface, C434T_AddRef, C434T_Release,
    C434T_Initialize, C434T_CreateDevice, C434T_DestroyDevice,
    C434T_AddDeviceClient, C434T_RemoveDeviceClient,
    C434T_PerformDeviceConfigurationChange, C434T_AbortDeviceConfigurationChange,
    C434T_HasProperty, C434T_IsPropertySettable, C434T_GetPropertyDataSize,
    C434T_GetPropertyData, C434T_SetPropertyData,
    C434T_StartIO, C434T_StopIO, C434T_GetZeroTimeStamp,
    C434T_WillDoIOOperation, C434T_BeginIOOperation, C434T_DoIOOperation,
    C434T_EndIOOperation,
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

void *C434T_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    (void)inAllocator;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID))
        return gDriverRef;
    return NULL;
}

static HRESULT C434T_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    if (inDriver != gDriverRef || !outInterface)
        return kAudioHardwareIllegalOperationError;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gStateMutex);
        gRefCount++;
        pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(requested);
    return result;
}

static ULONG C434T_AddRef(void *inDriver) {
    if (inDriver != gDriverRef)
        return 0;
    pthread_mutex_lock(&gStateMutex);
    ULONG n = ++gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return n;
}

static ULONG C434T_Release(void *inDriver) {
    if (inDriver != gDriverRef)
        return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount)
        gRefCount--;
    ULONG n = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return n;
}

static OSStatus C434T_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef)
        return kAudioHardwareBadObjectError;
    gHost = inHost;
    gLog = os_log_create("com.c434t.audio", "plugin");
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    gNsPerTick = (double)tb.numer / tb.denom;
    return kAudioHardwareNoError;
}

static OSStatus C434T_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef a, const AudioServerPlugInClientInfo *c, AudioObjectID *o) {
    (void)d; (void)a; (void)c; (void)o;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus C434T_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) {
    (void)d; (void)o;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus C434T_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {
    (void)d; (void)o; (void)c;
    return kAudioHardwareNoError;
}

static OSStatus C434T_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {
    (void)d; (void)o; (void)c;
    return kAudioHardwareNoError;
}

static OSStatus C434T_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {
    (void)d; (void)o; (void)a; (void)i;
    return kAudioHardwareNoError;
}

static OSStatus C434T_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {
    (void)d; (void)o; (void)a; (void)i;
    return kAudioHardwareNoError;
}

// ---------------------------------------------------------------------------
// Properties

static AudioStreamBasicDescription VirtualFormat(void) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kSampleRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * kChannels;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

static AudioStreamBasicDescription PhysicalFormat(void) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kSampleRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 2 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 2 * kChannels;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 16;
    return f;
}

static bool IsDevice(AudioObjectID obj) {
    return obj == kObjectID_Device || obj == kObjectID_Mic;
}

static Float32 InScalarToDB(Float32 scalar) {
    return kInGainMinDB + scalar * (kInGainMaxDB - kInGainMinDB);
}

static Float32 InDBToScalar(Float32 db) {
    if (db < kInGainMinDB) db = kInGainMinDB;
    if (db > kInGainMaxDB) db = kInGainMaxDB;
    return (db - kInGainMinDB) / (kInGainMaxDB - kInGainMinDB);
}

// Streams and controls a device owns in the given scope. Returns the count.
static UInt32 DeviceObjects(AudioObjectID dev, AudioObjectPropertySelector sel,
                            AudioObjectPropertyScope scope, AudioObjectID ids[3]) {
    bool mic = dev == kObjectID_Mic;
    bool global = scope != kAudioObjectPropertyScopeInput && scope != kAudioObjectPropertyScopeOutput;
    bool ours = global || scope == (mic ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput);
    UInt32 n = 0;
    if (!ours)
        return 0;
    if (sel != kAudioObjectPropertyControlList)
        ids[n++] = mic ? kObjectID_StreamIn : kObjectID_Stream;
    if (sel != kAudioDevicePropertyStreams) {
        if (mic) {
            ids[n++] = kObjectID_InVolume;
        } else {
            ids[n++] = kObjectID_Volume;
            ids[n++] = kObjectID_Mute;
        }
    }
    return n;
}

static Boolean C434T_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid,
                                 const AudioObjectPropertyAddress *addr) {
    UInt32 size = 0;
    (void)pid;
    if (inDriver != gDriverRef || !addr)
        return false;
    return C434T_GetPropertyDataSize(inDriver, obj, 0, addr, 0, NULL, &size) == kAudioHardwareNoError;
}

static OSStatus C434T_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid,
                                         const AudioObjectPropertyAddress *addr, Boolean *outSettable) {
    (void)pid;
    if (inDriver != gDriverRef || !addr || !outSettable)
        return kAudioHardwareIllegalOperationError;
    if (!C434T_HasProperty(inDriver, obj, 0, addr))
        return kAudioHardwareUnknownPropertyError;
    *outSettable = false;
    if (((obj == kObjectID_Volume || obj == kObjectID_InVolume) &&
         (addr->mSelector == kAudioLevelControlPropertyScalarValue ||
          addr->mSelector == kAudioLevelControlPropertyDecibelValue)) ||
        (obj == kObjectID_Mute && addr->mSelector == kAudioBooleanControlPropertyValue))
        *outSettable = true;
    return kAudioHardwareNoError;
}

#define SIZE_OR_FAIL(type) do { *outSize = sizeof(type); return kAudioHardwareNoError; } while (0)

static OSStatus C434T_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid,
                                          const AudioObjectPropertyAddress *addr, UInt32 qsize,
                                          const void *qdata, UInt32 *outSize) {
    (void)pid; (void)qsize; (void)qdata;
    if (inDriver != gDriverRef || !addr || !outSize)
        return kAudioHardwareIllegalOperationError;
    switch (obj) {
    case kObjectID_PlugIn:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner: SIZE_OR_FAIL(AudioObjectID);
        case kAudioObjectPropertyManufacturer: SIZE_OR_FAIL(CFStringRef);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            *outSize = 2 * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        case kAudioPlugInPropertyTranslateUIDToDevice: SIZE_OR_FAIL(AudioObjectID);
        case kAudioPlugInPropertyResourceBundle: SIZE_OR_FAIL(CFStringRef);
        }
        break;
    case kObjectID_Device:
    case kObjectID_Mic:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner: SIZE_OR_FAIL(AudioClassID);
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID: SIZE_OR_FAIL(CFStringRef);
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod: SIZE_OR_FAIL(UInt32);
        case kAudioDevicePropertyRelatedDevices: SIZE_OR_FAIL(AudioObjectID);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID ids[3];
            *outSize = DeviceObjects(obj, addr->mSelector, addr->mScope, ids) * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyNominalSampleRate: SIZE_OR_FAIL(Float64);
        case kAudioDevicePropertyAvailableNominalSampleRates: SIZE_OR_FAIL(AudioValueRange);
        case kAudioDevicePropertyPreferredChannelsForStereo:
            *outSize = 2 * sizeof(UInt32);
            return kAudioHardwareNoError;
        case kAudioDevicePropertyPreferredChannelLayout:
            *outSize = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription);
            return kAudioHardwareNoError;
        }
        break;
    case kObjectID_Stream:
    case kObjectID_StreamIn:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner: SIZE_OR_FAIL(AudioClassID);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return kAudioHardwareNoError;
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency: SIZE_OR_FAIL(UInt32);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: SIZE_OR_FAIL(AudioStreamBasicDescription);
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: SIZE_OR_FAIL(AudioStreamRangedDescription);
        }
        break;
    case kObjectID_Volume:
    case kObjectID_InVolume:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner: SIZE_OR_FAIL(AudioClassID);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return kAudioHardwareNoError;
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement: SIZE_OR_FAIL(UInt32);
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar: SIZE_OR_FAIL(Float32);
        case kAudioLevelControlPropertyDecibelRange: SIZE_OR_FAIL(AudioValueRange);
        }
        break;
    case kObjectID_Mute:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner: SIZE_OR_FAIL(AudioClassID);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return kAudioHardwareNoError;
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
        case kAudioBooleanControlPropertyValue: SIZE_OR_FAIL(UInt32);
        }
        break;
    default:
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnknownPropertyError;
}

#define PUT(type, value) do { \
    if (inSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; \
    *(type *)outData = (value); *outSize = sizeof(type); return kAudioHardwareNoError; } while (0)

static OSStatus C434T_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid,
                                      const AudioObjectPropertyAddress *addr, UInt32 qsize, const void *qdata,
                                      UInt32 inSize, UInt32 *outSize, void *outData) {
    (void)pid;
    if (inDriver != gDriverRef || !addr || !outSize || !outData)
        return kAudioHardwareIllegalOperationError;
    bool mic = obj == kObjectID_Mic;
    switch (obj) {
    case kObjectID_PlugIn:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("C434TAudio"));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            AudioObjectID ids[2] = {kObjectID_Device, kObjectID_Mic};
            UInt32 n = inSize / sizeof(AudioObjectID);
            n = n < 2 ? n : 2;
            memcpy(outData, ids, n * sizeof(AudioObjectID));
            *outSize = n * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            if (qsize != sizeof(CFStringRef) || !qdata)
                return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *(const CFStringRef *)qdata;
            AudioObjectID dev = kAudioObjectUnknown;
            if (CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo)
                dev = kObjectID_Device;
            else if (CFStringCompare(uid, CFSTR(kMicUID), 0) == kCFCompareEqualTo)
                dev = kObjectID_Mic;
            PUT(AudioObjectID, dev);
        }
        case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
        }
        break;

    case kObjectID_Device:
    case kObjectID_Mic:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjectID_PlugIn);
        case kAudioObjectPropertyName:
            PUT(CFStringRef, mic ? CFSTR("Built-in Microphone") : CFSTR("Built-in Speakers"));
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("ASUS / Intel"));
        case kAudioDevicePropertyDeviceUID: PUT(CFStringRef, mic ? CFSTR(kMicUID) : CFSTR(kDeviceUID));
        case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR(kDeviceModelUID));
        case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyRelatedDevices: PUT(AudioObjectID, obj);
        case kAudioDevicePropertyClockDomain: PUT(UInt32, kClockDomain);
        case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: PUT(UInt32, (mic ? gMicRunning : gSpeakerRunning) > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, !mic);
        case kAudioDevicePropertyLatency: PUT(UInt32, kLatencyFrames);
        case kAudioDevicePropertySafetyOffset: PUT(UInt32, kSafetyOffsetFrames);
        case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, gRingFrames);
        case kAudioDevicePropertyNominalSampleRate: PUT(Float64, kSampleRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange r = {kSampleRate, kSampleRate};
            PUT(AudioValueRange, r);
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID ids[3];
            UInt32 n = DeviceObjects(obj, addr->mSelector, addr->mScope, ids);
            n = n < inSize / sizeof(AudioObjectID) ? n : inSize / sizeof(AudioObjectID);
            memcpy(outData, ids, n * sizeof(AudioObjectID));
            *outSize = n * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inSize < 2 * sizeof(UInt32))
                return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1;
            ((UInt32 *)outData)[1] = 2;
            *outSize = 2 * sizeof(UInt32);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 size = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription);
            if (inSize < size)
                return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *layout = (AudioChannelLayout *)outData;
            memset(layout, 0, size);
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            layout->mNumberChannelDescriptions = kChannels;
            layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outSize = size;
            return kAudioHardwareNoError;
        }
        }
        break;

    case kObjectID_Stream:
    case kObjectID_StreamIn:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner:
            PUT(AudioObjectID, obj == kObjectID_StreamIn ? kObjectID_Mic : kObjectID_Device);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return kAudioHardwareNoError;
        case kAudioStreamPropertyIsActive: PUT(UInt32, 1);
        case kAudioStreamPropertyDirection: PUT(UInt32, obj == kObjectID_StreamIn ? 1 : 0);
        case kAudioStreamPropertyTerminalType:
            PUT(UInt32, obj == kObjectID_StreamIn ? kAudioStreamTerminalTypeMicrophone
                                                  : kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
        case kAudioStreamPropertyLatency: PUT(UInt32, kLatencyFrames);
        case kAudioStreamPropertyVirtualFormat: PUT(AudioStreamBasicDescription, VirtualFormat());
        case kAudioStreamPropertyPhysicalFormat: PUT(AudioStreamBasicDescription, PhysicalFormat());
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r;
            r.mFormat = addr->mSelector == kAudioStreamPropertyAvailableVirtualFormats ? VirtualFormat() : PhysicalFormat();
            r.mSampleRateRange.mMinimum = kSampleRate;
            r.mSampleRateRange.mMaximum = kSampleRate;
            if (inSize < sizeof(r)) { *outSize = 0; return kAudioHardwareNoError; }
            PUT(AudioStreamRangedDescription, r);
        }
        }
        break;

    case kObjectID_Volume:
    case kObjectID_InVolume: {
        bool in = obj == kObjectID_InVolume;
        Float32 scalar = in ? gInGainScalar : gVolumeScalar;
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, in ? kObjectID_Mic : kObjectID_Device);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return kAudioHardwareNoError;
        case kAudioControlPropertyScope:
            PUT(UInt32, in ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: PUT(UInt32, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: PUT(Float32, scalar);
        case kAudioLevelControlPropertyDecibelValue:
            PUT(Float32, in ? InScalarToDB(scalar) : ScalarToDB(scalar));
        case kAudioLevelControlPropertyDecibelRange: {
            AudioValueRange r = {in ? kInGainMinDB : kVolumeMinDB, in ? kInGainMaxDB : kVolumeMaxDB};
            PUT(AudioValueRange, r);
        }
        case kAudioLevelControlPropertyConvertScalarToDecibels: {
            if (inSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            Float32 v = *(Float32 *)outData;
            PUT(Float32, in ? InScalarToDB(v) : ScalarToDB(v));
        }
        case kAudioLevelControlPropertyConvertDecibelsToScalar: {
            if (inSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            Float32 v = *(Float32 *)outData;
            PUT(Float32, in ? InDBToScalar(v) : DBToScalar(v));
        }
        }
        break;
    }

    case kObjectID_Mute:
        switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjectID_Device);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return kAudioHardwareNoError;
        case kAudioControlPropertyScope: PUT(UInt32, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: PUT(UInt32, kAudioObjectPropertyElementMain);
        case kAudioBooleanControlPropertyValue: PUT(UInt32, gMute ? 1 : 0);
        }
        break;

    default:
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus C434T_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid,
                                      const AudioObjectPropertyAddress *addr, UInt32 qsize, const void *qdata,
                                      UInt32 inSize, const void *inData) {
    (void)pid; (void)qsize; (void)qdata;
    if (inDriver != gDriverRef || !addr || !inData)
        return kAudioHardwareIllegalOperationError;
    AudioObjectPropertyAddress changed[2];
    UInt32 nChanged = 0;
    if ((obj == kObjectID_Volume || obj == kObjectID_InVolume) &&
        (addr->mSelector == kAudioLevelControlPropertyScalarValue ||
         addr->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        if (inSize != sizeof(Float32))
            return kAudioHardwareBadPropertySizeError;
        bool in = obj == kObjectID_InVolume;
        Float32 v = *(const Float32 *)inData;
        Float32 scalar = addr->mSelector == kAudioLevelControlPropertyScalarValue ? v
                       : in ? InDBToScalar(v) : DBToScalar(v);
        scalar = scalar < 0.0f ? 0.0f : (scalar > 1.0f ? 1.0f : scalar);
        pthread_mutex_lock(&gStateMutex);
        Float32 *cur = in ? &gInGainScalar : &gVolumeScalar;
        if (scalar != *cur) {
            *cur = scalar;
            if (in)
                gInGain = (Float32)pow(10.0, InScalarToDB(scalar) / 20.0);
            else
                PushVolume();
            changed[nChanged++] = (AudioObjectPropertyAddress){kAudioLevelControlPropertyScalarValue,
                kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
            changed[nChanged++] = (AudioObjectPropertyAddress){kAudioLevelControlPropertyDecibelValue,
                kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        }
        pthread_mutex_unlock(&gStateMutex);
    } else if (obj == kObjectID_Mute && addr->mSelector == kAudioBooleanControlPropertyValue) {
        if (inSize != sizeof(UInt32))
            return kAudioHardwareBadPropertySizeError;
        bool mute = *(const UInt32 *)inData != 0;
        if (mute != gMute) {
            gMute = mute;
            changed[nChanged++] = (AudioObjectPropertyAddress){kAudioBooleanControlPropertyValue,
                kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        }
    } else {
        return kAudioHardwareUnknownPropertyError;
    }
    if (nChanged && gHost)
        gHost->PropertiesChanged(gHost, obj, nChanged, changed);
    return kAudioHardwareNoError;
}

// ---------------------------------------------------------------------------
// IO
//
// The kext runs playback and capture together, so the hardware starts with the
// first of the two devices and stops with the last.

static OSStatus C434T_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (inDriver != gDriverRef || !IsDevice(dev))
        return kAudioHardwareBadObjectError;
    OSStatus status = kAudioHardwareNoError;
    pthread_mutex_lock(&gStateMutex);
    if (gIORunning == 0) {
        if (OpenHardware() != KERN_SUCCESS)
            status = kAudioHardwareNotRunningError;
        else
            PushVolume();
    }
    if (status == kAudioHardwareNoError) {
        gIORunning++;
        if (dev == kObjectID_Mic)
            gMicRunning++;
        else
            gSpeakerRunning++;
    }
    pthread_mutex_unlock(&gStateMutex);
    return status;
}

static OSStatus C434T_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (inDriver != gDriverRef || !IsDevice(dev))
        return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    UInt32 *mine = dev == kObjectID_Mic ? &gMicRunning : &gSpeakerRunning;
    if (*mine) {
        (*mine)--;
        if (gIORunning && --gIORunning == 0)
            CloseHardware();
    }
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus C434T_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID dev, UInt32 client,
                                       Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
    (void)client;
    if (inDriver != gDriverRef || !IsDevice(dev))
        return kAudioHardwareBadObjectError;
    uint64_t out[4] = {0, 0, 1, 0xffffffff};
    uint32_t n = 4;
    if (gConnection == IO_OBJECT_NULL ||
        IOConnectCallScalarMethod(gConnection, kSelTimestamp, NULL, 0, out, &n) != KERN_SUCCESS)
        return kAudioHardwareNotRunningError;
    // The kext restarts the stream after a system wake, with a new capture offset.
    if (n > 3 && out[3] != 0xffffffff)
        gCapOffset = (UInt32)out[3];
    if (dev == kObjectID_Device) {
        if ((Float64)out[0] != gZeroSample && gZeroHost) {
            double ms = (out[1] - gZeroHost) * gNsPerTick / 1e6;
            double expected = ((Float64)out[0] - gZeroSample) / kSampleRate * 1000.0;
            double err = ms - expected;
            if (err < gMinPeriodMs) gMinPeriodMs = err;
            if (err > gMaxPeriodMs) gMaxPeriodMs = err;
        }
        gZeroSample = (Float64)out[0];
        gZeroHost = out[1];
    }
    *outSampleTime = (Float64)out[0];
    *outHostTime = out[1];
    *outSeed = out[2];
    return kAudioHardwareNoError;
}

static OSStatus C434T_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID dev, UInt32 client,
                                        UInt32 op, Boolean *outWillDo, Boolean *outInPlace) {
    (void)client;
    if (inDriver != gDriverRef || !IsDevice(dev))
        return kAudioHardwareBadObjectError;
    *outWillDo = dev == kObjectID_Mic ? op == kAudioServerPlugInIOOperationReadInput
                                      : op == kAudioServerPlugInIOOperationWriteMix;
    *outInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus C434T_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 n,
                                       const AudioServerPlugInIOCycleInfo *i) {
    (void)d; (void)o; (void)c; (void)op; (void)n; (void)i;
    return kAudioHardwareNoError;
}

static OSStatus C434T_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 n,
                                     const AudioServerPlugInIOCycleInfo *i) {
    (void)d; (void)o; (void)c; (void)op; (void)n; (void)i;
    return kAudioHardwareNoError;
}

// WriteMix: the HAL has already converted its float mix to the stream's
// physical format (no ConvertMix here), so the buffer holds interleaved 16-bit
// frames; copy them into the DMA ring at the cycle's output sample time.
// ReadInput: the same in reverse from the capture ring, with the input gain.
static OSStatus C434T_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID dev, AudioObjectID stream,
                                    UInt32 client, UInt32 op, UInt32 frames,
                                    const AudioServerPlugInIOCycleInfo *cycle, void *mainBuffer, void *secondary) {
    (void)client; (void)secondary;
    if (inDriver != gDriverRef || !IsDevice(dev))
        return kAudioHardwareBadObjectError;
    if (op == kAudioServerPlugInIOOperationReadInput && stream == kObjectID_StreamIn) {
        // Physical format: interleaved 16-bit; silence if the mics are not running.
        int16_t *dst = (int16_t *)mainBuffer;
        if (!gCapRing) {
            memset(dst, 0, frames * 2 * sizeof(int16_t));
            return kAudioHardwareNoError;
        }
        UInt32 pos = (UInt32)fmod(cycle->mInputTime.mSampleTime + gCapOffset, (Float64)gRingFrames);
        UInt32 done = 0;
        while (done < frames) {
            UInt32 n = frames - done;
            if (n > gRingFrames - pos)
                n = gRingFrames - pos;
            memcpy(dst + 2 * done, gCapRing + 2 * pos, n * 2 * sizeof(int16_t));
            done += n;
            pos = 0;
        }
        Float32 gain = gInGain;
        if (gain != 1.0f) {
            for (UInt32 i = 0; i < frames * 2; i++) {
                Float32 v = dst[i] * gain;
                dst[i] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
            }
        }
        return kAudioHardwareNoError;
    }
    if (op != kAudioServerPlugInIOOperationWriteMix || stream != kObjectID_Stream || !gRing)
        return kAudioHardwareNoError;
    UInt64 now = mach_absolute_time();
    double hwNow = gZeroSample + (now - gZeroHost) * gNsPerTick * kSampleRate / 1e9;
    double margin = cycle->mOutputTime.mSampleTime - hwNow;
    if (margin < gMinMargin) gMinMargin = margin;
    if (margin > gMaxMargin) gMaxMargin = margin;
    if (++gCycles - gLastReport >= 100) {
        os_log_debug(gLog, "cycles %llu frames %u margin min %.0f max %.0f frames, ring period error %.2f..%.2f ms, zero %.0f @ %llu, out %.0f",
               gCycles, frames, gMinMargin, gMaxMargin, gMinPeriodMs, gMaxPeriodMs, gZeroSample,
               gZeroHost, cycle->mOutputTime.mSampleTime);
        gLastReport = gCycles;
        gMinMargin = 1e9; gMaxMargin = -1e9; gMinPeriodMs = 1e9; gMaxPeriodMs = 0;
    }
    const int16_t *in = (const int16_t *)mainBuffer;
    UInt32 pos = (UInt32)fmod(cycle->mOutputTime.mSampleTime, (Float64)gRingFrames);
    UInt32 done = 0;
    while (done < frames) {
        UInt32 n = frames - done;
        if (n > gRingFrames - pos)
            n = gRingFrames - pos;
        if (gMute)
            memset(gRing + 2 * pos, 0, n * 2 * sizeof(int16_t));
        else
            memcpy(gRing + 2 * pos, in + 2 * done, n * 2 * sizeof(int16_t));
        done += n;
        pos = 0;
    }
    return kAudioHardwareNoError;
}
