// What Xcode generates as <target>_info.c for a kext. _realmain/_antimain
// register and unregister the debug.c434t_kbd_* sysctl nodes.
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t c434t_kbd_start(kmod_info_t *ki, void *data);
extern kern_return_t c434t_kbd_stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(com.c434t.keyboard, "0.1.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = c434t_kbd_start;
__private_extern__ kmod_stop_func_t *_antimain = c434t_kbd_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
