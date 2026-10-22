// C434TKeyboard: Chromebook keyboard layout for the ASUS C434T on macOS.
//
// VoodooPS2 turns the 8042 scancodes into ADB key codes. Before dispatching a
// key, its controller sends kPS2M_notifyKeyPressed (with the ADB code and an
// eatKey flag) to every service that publishes RM,deliverNotifications; the
// keyboard also accepts ADB keystrokes back through kPS2K_notifyKeystroke.
// This service sits in that path, on the controller's command gate, so key
// combinations come out in order.
//
// Layout (VoodooPS2 defaults in, Mac keys out):
//   top row        back, forward, reload, full screen, Mission Control,
//                  brightness -/+, mute, volume -/+ (as printed)
//   search + top   F1-F10 (both swap with "Use F1, F2, etc. as standard
//                  function keys", which VoodooPS2 reports as HIDFKeyMode)
//   ctrl, alt      Control, Command;  right alt, right ctrl: Command, Option
//   search alone   Spotlight (tap)
//   search + ...   arrows: Home/End/Page Up/Page Down, backspace: forward
//                  delete, alt: Caps Lock, L: lock screen, any other key:
//                  Option + key (ChromeOS-style search layer)
//   alt + bright.  keyboard backlight down/up (ChromeOS); it turns off after a
//                  minute without typing and on again with the next key

#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/acpi/IOACPITypes.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <libkern/c++/OSNumber.h>
#include <sys/sysctl.h>

// VoodooPS2Controller/ApplePS2Device.h
#define kPS2M_notifyKeyPressed iokit_vendor_specific_msg(102)
#define kPS2K_notifyKeystroke  iokit_vendor_specific_msg(202)
struct PS2KeyInfo {
    uint64_t time;
    UInt16 adbKeyCode;
    bool goingDown;
    bool eatKey;
};

// ADB key codes
enum : uint16_t {
    kA_L = 0x25, kA_R = 0x0f, kA_F = 0x03, kA_Q = 0x0c, kA_LBracket = 0x21, kA_RBracket = 0x1e,
    kA_Space = 0x31, kA_Backspace = 0x33, kA_CapsLock = 0x39,
    kA_Command = 0x37, kA_RCommand = 0x36, kA_Shift = 0x38, kA_Option = 0x3a,
    kA_Control = 0x3b, kA_RShift = 0x3c, kA_ROption = 0x3d, kA_RControl = 0x3e,
    kA_Home = 0x73, kA_PageUp = 0x74, kA_FwdDelete = 0x75, kA_End = 0x77, kA_PageDown = 0x79,
    kA_Left = 0x7b, kA_Right = 0x7c, kA_Down = 0x7d, kA_Up = 0x7e,
    kA_BrightnessDown = 0x6b, kA_BrightnessUp = 0x71, // F14/F15
    kA_Mute = 0x4a, kA_VolumeDown = 0x49, kA_VolumeUp = 0x48,
};
// VoodooPS2 codes for this keyboard: the top row sends F1-F10, Search is the
// left GUI key (Command by default), the Alt keys come in as Option.
static const uint8_t kTopRow[10] = {0x7a, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6d};
static constexpr uint16_t kIn_Search = 0x37, kIn_Alt = 0x3a, kIn_RAlt = 0x3d, kIn_RCtrl = 0x3e;

// What a physical key turned into on its way down, so its release matches.
static constexpr uint16_t kHeldNone = 0xffff, kHeldPass = 0x1000, kHeldCombo = 0x2000,
                          kHeldCaps = 0x3000;

// Keyboard backlight levels (percent) for alt + brightness down/up.
static const uint8_t kBacklightSteps[] = {0, 10, 20, 30, 45, 60, 80, 100};
static constexpr uint32_t kBacklightIdleMs = 60 * 1000;

struct Combo {
    uint8_t mods[2];
    uint8_t nmods;
    uint8_t key;
};

class C434TKeyboard : public IOService {
    OSDeclareDefaultStructors(C434TKeyboard)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn message(UInt32 type, IOService *provider, void *argument) override;
    int inject(uint32_t packet);
    int backlight(int set);

private:
    IOService *keyboard = nullptr;
    uint16_t held[256];
    bool out[256];             // output key state, to leave held modifiers alone
    bool searchDown = false, searchUsed = false, optionInjected = false;
    uint64_t searchTime = 0;

    // Keyboard backlight, through KBLQ/KBLS on the PS2K ACPI device (SSDT-PS2-KEYS).
    IOACPIPlatformDevice *ps2k = nullptr;
    IOWorkLoop *workLoop = nullptr;
    IOTimerEventSource *idleTimer = nullptr;
    IONotifier *sleepNotifier = nullptr;
    IOLock *blLock = nullptr;
    int blLevel = 0;           // what the user chose
    bool blOff = false;        // switched off for idle or sleep
    int blState = 0;           // 0 not set up yet, 1 ready, -1 unavailable
    bool backlightReady();
    void setBacklight(int percent);
    void backlightActivity();
    void backlightStep(bool up);
    static void idleTimerFired(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn sleepWakeHandler(void *target, void *ref, UInt32 msg, IOService *provider,
                                     void *arg, vm_size_t argSize);

    IOService *findKeyboard();
    bool fkeyMode();
    void emit(uint16_t code, bool down, uint64_t time);
    void combo(const Combo &c, uint64_t time);
    void handleKey(PS2KeyInfo *info);
};

OSDefineMetaClassAndStructors(C434TKeyboard, IOService)

static C434TKeyboard *gKbd;
static int gTrace;
static char gLog[4096];
static size_t gLogLen;

static void trace(const char *fmt, ...) __printflike(1, 2);
static void trace(const char *fmt, ...) {
    if (!gTrace)
        return;
    if (gLogLen > sizeof(gLog) - 128)
        gLogLen = 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(gLog + gLogLen, sizeof(gLog) - gLogLen, fmt, ap);
    va_end(ap);
    if (n > 0)
        gLogLen = min(gLogLen + (size_t)n, sizeof(gLog) - 1);
}

bool C434TKeyboard::start(IOService *provider) {
    if (!IOService::start(provider))
        return false;
    for (auto &h : held)
        h = kHeldNone;
    bzero(out, sizeof(out));

    // We match on VoodooPS2's keyboard, so it exists from the start.
    keyboard = provider;
    keyboard->retain();

    // Nothing here touches ACPI or the EC: this runs during boot, before the EC
    // driver is up. The backlight is set up on the first key press instead.
    blLock = IOLockAlloc();
    workLoop = IOWorkLoop::workLoop();
    idleTimer = IOTimerEventSource::timerEventSource(this, idleTimerFired);
    if (!blLock || !workLoop || !idleTimer || workLoop->addEventSource(idleTimer) != kIOReturnSuccess)
        OSSafeReleaseNULL(idleTimer);
    if (getPMRootDomain())
        sleepNotifier = registerPrioritySleepWakeInterest(sleepWakeHandler, this);
    gKbd = this;
    // RM,deliverNotifications (from the personality) makes the PS/2 controller
    // add us to its key notification list once we are published.
    registerService();
    IOLog("C434TKeyboard: started\n");
    return true;
}

void C434TKeyboard::stop(IOService *provider) {
    if (gKbd == this)
        gKbd = nullptr;
    if (sleepNotifier) {
        sleepNotifier->remove();
        sleepNotifier = nullptr;
    }
    if (idleTimer) {
        idleTimer->cancelTimeout();
        workLoop->removeEventSource(idleTimer);
    }
    OSSafeReleaseNULL(idleTimer);
    OSSafeReleaseNULL(workLoop);
    OSSafeReleaseNULL(ps2k);
    if (blLock)
        IOLockFree(blLock);
    OSSafeReleaseNULL(keyboard);
    IOService::stop(provider);
}

IOService *C434TKeyboard::findKeyboard() {
    return keyboard;
}

// First use of the keyboard backlight (a key press, so the system is up): find
// KBLQ/KBLS on PS2K and pick up the EC's current level.
bool C434TKeyboard::backlightReady() {
    if (blState)
        return blState > 0;
    if (!idleTimer)
        return blState = -1, false;
    IOLockLock(blLock);
    if (!blState) {
        IORegistryEntry *e = IORegistryEntry::fromPath("IOService:/AppleACPIPlatformExpert/PS2K");
        // By name: no reference to IOACPIFamily's metaclass symbol.
        ps2k = e && e->metaCast("IOACPIPlatformDevice") ? (IOACPIPlatformDevice *)e : nullptr;
        if (!ps2k)
            OSSafeReleaseNULL(e);
        UInt32 v = 0;
        if (ps2k && ps2k->validateObject("KBLS") == kIOReturnSuccess &&
            ps2k->evaluateInteger("KBLQ", &v) == kIOReturnSuccess && v <= 100) {
            blLevel = (int)v;
            blState = 1;
            IOLog("C434TKeyboard: keyboard backlight at %d%%\n", blLevel);
        } else {
            OSSafeReleaseNULL(ps2k);
            blState = -1;
            IOLog("C434TKeyboard: no keyboard backlight methods (SSDT-PS2-KEYS)\n");
        }
    }
    IOLockUnlock(blLock);
    return blState > 0;
}

// HIDFKeyMode 1 = "Use F1, F2, etc. keys as standard function keys". VoodooPS2
// publishes it when RMCF has Function Keys Standard/Special (SSDT-PS2-KEYS).
bool C434TKeyboard::fkeyMode() {
    OSNumber *n = keyboard ? OSDynamicCast(OSNumber, keyboard->getProperty("HIDFKeyMode")) : nullptr;
    return n && n->unsigned32BitValue();
}

// Called with blLock held.
void C434TKeyboard::setBacklight(int percent) {
    OSObject *arg = OSNumber::withNumber(percent, 32);
    if (arg) {
        ps2k->evaluateObject("KBLS", nullptr, &arg, 1);
        arg->release();
    }
}

// A key went down: light the keyboard again if it was off for idle.
void C434TKeyboard::backlightActivity() {
    if (!backlightReady())
        return;
    IOLockLock(blLock);
    if (blOff) {
        blOff = false;
        setBacklight(blLevel);
    }
    IOLockUnlock(blLock);
    if (idleTimer)
        idleTimer->setTimeoutMS(kBacklightIdleMs);
}

void C434TKeyboard::backlightStep(bool up) {
    if (!backlightReady())
        return;
    IOLockLock(blLock);
    int n = sizeof(kBacklightSteps), i = 0;
    while (i < n - 1 && kBacklightSteps[i] < blLevel)
        i++;
    // blLevel may sit between two steps (EC default): move to the next one.
    if (up)
        i = kBacklightSteps[i] > blLevel ? i : min(i + 1, n - 1);
    else
        i = max(i - 1, 0);
    blLevel = kBacklightSteps[i];
    blOff = false;
    setBacklight(blLevel);
    IOLockUnlock(blLock);
    trace("keyboard backlight %d%%\n", blLevel);
}

void C434TKeyboard::idleTimerFired(OSObject *owner, IOTimerEventSource *) {
    C434TKeyboard *self = (C434TKeyboard *)owner;
    if (self->blState <= 0)
        return;
    IOLockLock(self->blLock);
    if (!self->blOff && self->blLevel) {
        self->blOff = true;
        self->setBacklight(0);
    }
    IOLockUnlock(self->blLock);
}

IOReturn C434TKeyboard::sleepWakeHandler(void *target, void *, UInt32 msg, IOService *, void *,
                                         vm_size_t) {
    C434TKeyboard *self = (C434TKeyboard *)target;
    if (self->blState <= 0)
        return kIOReturnSuccess;
    if (msg == kIOMessageSystemWillSleep) {
        IOLockLock(self->blLock);
        self->blOff = true;
        self->setBacklight(0);
        IOLockUnlock(self->blLock);
    } else if (msg == kIOMessageSystemHasPoweredOn) {
        // Wake with the keyboard lit, as after a key press.
        self->backlightActivity();
    }
    return kIOReturnSuccess;
}

void C434TKeyboard::emit(uint16_t code, bool down, uint64_t time) {
    out[code & 0xff] = down;
    PS2KeyInfo ki = {time, code, down, false};
    keyboard->message(kPS2K_notifyKeystroke, this, &ki);
    trace("  out %02x %s\n", code, down ? "down" : "up");
}

// Press the modifiers that are not already down, tap the key, release them.
void C434TKeyboard::combo(const Combo &c, uint64_t time) {
    bool pressed[2] = {false, false};
    for (int i = 0; i < c.nmods; i++)
        if (!out[c.mods[i]]) {
            emit(c.mods[i], true, time);
            pressed[i] = true;
        }
    emit(c.key, true, time);
    emit(c.key, false, time);
    for (int i = c.nmods - 1; i >= 0; i--)
        if (pressed[i])
            emit(c.mods[i], false, time);
}

static const Combo kBack = {{kA_Command}, 1, kA_LBracket};
static const Combo kForward = {{kA_Command}, 1, kA_RBracket};
static const Combo kReload = {{kA_Command}, 1, kA_R};
static const Combo kFullScreen = {{kA_Control, kA_Command}, 2, kA_F};
static const Combo kMissionControl = {{kA_Control}, 1, kA_Up};
static const Combo kLockScreen = {{kA_Control, kA_Command}, 2, kA_Q};
static const Combo kSpotlight = {{kA_Command}, 1, kA_Space};

void C434TKeyboard::handleKey(PS2KeyInfo *info) {
    uint16_t code = info->adbKeyCode;
    bool down = info->goingDown;
    uint64_t t = info->time;
    if (code > 0xff || info->eatKey || !findKeyboard())
        return;
    if (down)
        backlightActivity();

    if (code == kIn_Search) {
        info->eatKey = true;
        if (down) {
            if (!searchDown) {
                searchDown = true;
                searchUsed = optionInjected = false;
                searchTime = t;
            }
            return;
        }
        searchDown = false;
        uint64_t tapMax;
        nanoseconds_to_absolutetime(500ull * 1000000, &tapMax);
        if (optionInjected) {
            emit(kA_Option, false, t);
            optionInjected = false;
        } else if (!searchUsed && t - searchTime < tapMax) {
            trace("search tap: Spotlight\n");
            combo(kSpotlight, t);
        }
        return;
    }

    if (!down) {
        uint16_t h = held[code];
        held[code] = kHeldNone;
        if (h == kHeldNone || h == kHeldPass) {
            out[code] = false;
        } else if (h == kHeldCombo) {
            info->eatKey = true;
        } else if (h == kHeldCaps) {
            info->eatKey = true;
            emit(kA_CapsLock, true, t);
            emit(kA_CapsLock, false, t);
        } else {
            info->eatKey = true;
            emit(h, false, t);
        }
        return;
    }

    // Typematic repeat: repeat single keys, not combinations.
    uint16_t h = held[code];
    if (h != kHeldNone) {
        if (h == kHeldPass)
            return;
        info->eatKey = true;
        if (h < kHeldPass)
            emit(h, true, t);
        return;
    }

    int top = -1;
    for (int i = 0; i < 10; i++)
        if (kTopRow[i] == code)
            top = i;
    uint16_t key = kHeldPass;
    const Combo *action = nullptr;

    if (top >= 0) {
        // Media actions unless F-keys were asked for (search or the setting).
        if (searchDown == fkeyMode()) {
            static const Combo *const combos[5] = {&kBack, &kForward, &kReload, &kFullScreen,
                                                   &kMissionControl};
            static const uint16_t keys[5] = {kA_BrightnessDown, kA_BrightnessUp, kA_Mute,
                                             kA_VolumeDown, kA_VolumeUp};
            if ((top == 5 || top == 6) && (out[kA_Command] || out[kA_RCommand])) {
                // alt + brightness: keyboard backlight (ChromeOS).
                info->eatKey = true;
                held[code] = kHeldCombo;
                backlightStep(top == 6);
                return;
            }
            if (top < 5)
                action = combos[top];
            else
                key = keys[top - 5];
        } else {
            key = code;
        }
        if (searchDown)
            searchUsed = true;
    } else if (searchDown) {
        searchUsed = true;
        switch (code) {
        case kA_Left: key = kA_Home; break;
        case kA_Right: key = kA_End; break;
        case kA_Up: key = kA_PageUp; break;
        case kA_Down: key = kA_PageDown; break;
        case kA_Backspace: key = kA_FwdDelete; break;
        case kA_L: action = &kLockScreen; break;
        case kIn_Alt:
            info->eatKey = true;
            held[code] = kHeldCaps;
            trace("search+alt: Caps Lock\n");
            emit(kA_CapsLock, true, t);
            emit(kA_CapsLock, false, t);
            return;
        default:
            // Search works as Option for everything else; modifiers combine
            // with it without needing Option themselves.
            if (code != kA_Shift && code != kA_RShift && code != kA_Control && !optionInjected) {
                emit(kA_Option, true, t);
                optionInjected = true;
            }
            break;
        }
    }

    if (key == kHeldPass && !action) {
        switch (code) {
        case kIn_Alt: key = kA_Command; break;
        case kIn_RAlt: key = kA_RCommand; break;
        case kIn_RCtrl: key = kA_ROption; break;
        }
    }

    if (action) {
        info->eatKey = true;
        held[code] = kHeldCombo;
        trace("key %02x: combo\n", code);
        combo(*action, t);
    } else if (key == kHeldPass || key == code) {
        held[code] = kHeldPass;
        out[code] = true;
    } else {
        info->eatKey = true;
        held[code] = key;
        trace("key %02x -> %02x\n", code, key);
        emit(key, true, t);
    }
}

IOReturn C434TKeyboard::message(UInt32 type, IOService *provider, void *argument) {
    if (type == kPS2M_notifyKeyPressed && argument)
        handleKey((PS2KeyInfo *)argument);
    return kIOReturnSuccess;
}

// Feed a raw PS/2 packet through VoodooPS2 as if typed (its ACPI notify path):
// 0x01XX = scancode XX, 0x02XX = e0 XX; set bit 7 of XX for the release.
int C434TKeyboard::inject(uint32_t packet) {
    if (!findKeyboard())
        return ENODEV;
    keyboard->message(kIOACPIMessageDeviceNotification, this, &packet);
    return 0;
}

// Read (set < 0) or set the keyboard backlight; returns the EC's level.
int C434TKeyboard::backlight(int set) {
    if (!backlightReady())
        return -1;
    IOLockLock(blLock);
    if (set >= 0 && set <= 100) {
        blLevel = set;
        blOff = false;
        setBacklight(set);
    }
    UInt32 v = 0;
    ps2k->evaluateInteger("KBLQ", &v);
    IOLockUnlock(blLock);
    return (int)v;
}

// ---------------------------------------------------------------------------
// sysctl debug.c434t_kbd_trace (1: record mapped keys and output in
// debug.c434t_kbd_log, typed keys are not recorded) and debug.c434t_kbd_inject.

static int sysctlTrace SYSCTL_HANDLER_ARGS {
    int err = sysctl_handle_int(oidp, &gTrace, 0, req);
    if (!err && req->newptr) {
        gLogLen = 0;
        gLog[0] = 0;
    }
    return err;
}

static int sysctlInject SYSCTL_HANDLER_ARGS {
    int value = 0;
    int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr)
        return err;
    return gKbd ? gKbd->inject((uint32_t)value) : ENODEV;
}

static int sysctlBacklight SYSCTL_HANDLER_ARGS {
    if (!gKbd)
        return ENODEV;
    int value = gKbd->backlight(-1);
    int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr)
        return err;
    gKbd->backlight(value);
    return 0;
}

static int sysctlLog SYSCTL_HANDLER_ARGS {
    return SYSCTL_OUT(req, gLog, gLogLen + 1);
}

SYSCTL_DECL(_debug);
SYSCTL_PROC(_debug, OID_AUTO, c434t_kbd_trace, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlTrace, "I", "C434TKeyboard: trace mapped keys");
SYSCTL_PROC(_debug, OID_AUTO, c434t_kbd_inject, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlInject, "I", "C434TKeyboard: inject a PS/2 packet");
SYSCTL_PROC(_debug, OID_AUTO, c434t_kbd_backlight, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            nullptr, 0, sysctlBacklight, "I", "C434TKeyboard: keyboard backlight percent");
SYSCTL_PROC(_debug, OID_AUTO, c434t_kbd_log, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
            nullptr, 0, sysctlLog, "A", "C434TKeyboard: trace");

extern "C" kern_return_t c434t_kbd_start(kmod_info_t *, void *) {
    sysctl_register_oid(&sysctl__debug_c434t_kbd_trace);
    sysctl_register_oid(&sysctl__debug_c434t_kbd_inject);
    sysctl_register_oid(&sysctl__debug_c434t_kbd_log);
    sysctl_register_oid(&sysctl__debug_c434t_kbd_backlight);
    return KERN_SUCCESS;
}

extern "C" kern_return_t c434t_kbd_stop(kmod_info_t *, void *) {
    sysctl_unregister_oid(&sysctl__debug_c434t_kbd_trace);
    sysctl_unregister_oid(&sysctl__debug_c434t_kbd_inject);
    sysctl_unregister_oid(&sysctl__debug_c434t_kbd_log);
    sysctl_unregister_oid(&sysctl__debug_c434t_kbd_backlight);
    return KERN_SUCCESS;
}
