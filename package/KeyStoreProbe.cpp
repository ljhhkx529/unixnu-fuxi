// SPDX-License-Identifier: BSD-2-Clause-Patent
// Mutually exclusive offline probes: unchanged target keystore loading, or
// ephemeral notification-only service. Neither supplies keys or unlock state.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_keyevents_static_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" IOService *native_keyevents_allocate(void);
extern "C" {
volatile uint32_t native_keyevents_stage, native_keyevents_refused_opens;
volatile int native_keyevents_status = -1;
volatile uint64_t native_keyevents_service;
static void aks_publish(void);
void native_keyevents_open_refused(void) {
    native_keyevents_refused_opens = native_keyevents_refused_opens + 1;
    aks_publish();
}
// sheng-aks=1|2|3: the owned AppleKeyStore answers the state queries (no passcode, nothing locked). The diagnostic page's
// extension rows (slots 0-1) carry registration status and opens/calls, then selector:count per selector seen ('e' = the
// last answer was an error). Modes 2 and 3 differ only in how aks_get_system fails.
extern char boot_progress_ext[8][48];
volatile uint32_t native_aks_opens, native_aks_calls;
static volatile uint32_t aks_count[96];
static volatile int32_t aks_rc[96];
bool native_aks_state_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        unsigned value = 0;
        cached = PE_parse_boot_argn("sheng-aks", &value, sizeof(value)) && value >= 1 && value <= 3;
    }
    return cached == 1;
}
// sheng-aks=2: answer aks_get_system like an unreachable keystore (kIOReturnError), so keybagd takes its no-system-keybag
// allow-list path instead of assuming an xART system bag was loaded.
// sheng-aks=3: answer it like a keystore that has no system keybag (kIOReturnNotFound), so keybagd looks for the bag on disk.
uint32_t native_aks_system_mode(void) {
    static int cached = -1;
    if (cached < 0) {
        unsigned value = 0;
        cached = (PE_parse_boot_argn("sheng-aks", &value, sizeof(value)) && (value == 2 || value == 3)) ? (int)value : 0;
    }
    return (uint32_t)cached;
}
// sheng-aks-sls=<n> overrides the reported lock state (default 0 = unlocked; 3 = MobileKeyBag's "no passcode").
uint32_t native_aks_lock_state(void) {
    static int cached = -1;
    if (cached < 0) {
        unsigned value = 0;
        if (!PE_parse_boot_argn("sheng-aks-sls", &value, sizeof(value)) || value > 7) value = 0;
        cached = (int)value;
    }
    return (uint32_t)cached;
}
// sheng-aks-ss=<n> overrides the DER "ss" state flags (default 4).
uint32_t native_aks_state_flags(void) {
    static int cached = -1;
    if (cached < 0) {
        unsigned value = 0;
        if (!PE_parse_boot_argn("sheng-aks-ss", &value, sizeof(value)) || value > 255) value = 4;
        cached = (int)value;
    }
    return (uint32_t)cached;
}
static void aks_publish(void) {
    // Slot 0: counters; slot 1: selector:count tokens ('e' = the last answer was an error). Slots 2-3 belong to the Data store.
    char row[48];
    unsigned used = 0;
    row[0] = 0;
    for (unsigned sel = 0; sel < 96; ++sel) {
        if (!aks_count[sel]) continue;
        char token[16];
        int n = snprintf(token, sizeof token, "%u:%u%s ", sel, aks_count[sel], aks_rc[sel] ? "e" : "");
        if (n <= 0 || used + (unsigned)n > 46) break;
        memcpy(row + used, token, (size_t)n + 1);
        used += (unsigned)n;
    }
    char head[48];
    snprintf(head, sizeof head, "AKS st=%u rc=%#x op=%u cl=%u rf=%u", native_keyevents_stage,
             (unsigned)native_keyevents_status, native_aks_opens, native_aks_calls, native_keyevents_refused_opens);
    for (unsigned i = 0; i < 47; ++i) boot_progress_ext[0][i] = head[i] ? head[i] : ' ';
    boot_progress_ext[0][47] = 0;
    unsigned i = 0;
    for (; i < 47 && row[i]; ++i) boot_progress_ext[1][i] = row[i];
    for (; i < 47; ++i) boot_progress_ext[1][i] = ' ';
    boot_progress_ext[1][47] = 0;
}
void native_aks_opened(void) {
    native_aks_opens = native_aks_opens + 1;
    aks_publish();
}
void native_aks_call(uint32_t selector, int32_t result) {
    native_aks_calls = native_aks_calls + 1;
    if (selector < 96) { aks_count[selector] = aks_count[selector] + 1; aks_rc[selector] = result; }
    aks_publish();
}
static void keyevents_status(int rc) {
    native_keyevents_status = rc;
    aks_publish();
}
static void native_keyevents_probe(void) {
    unsigned enabled = 0, original = 0;
    if (!PE_parse_boot_argn("offline-key-events", &enabled, sizeof(enabled)) || enabled != 1 ||
        native_keyevents_stage) return;
    native_keyevents_stage = 1;
    PE_parse_boot_argn("offline-keystore", &original, sizeof(original));
    keyevents_status(kIOReturnNotPermitted);
    // An explicit plane requires a path without a second plane prefix.
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) { keyevents_status(kIOReturnNoDevice); return; }
    OSData *data = chosen ? OSDynamicCast(OSData, chosen->getProperty("ephemeral-storage")) : nullptr;
    uint32_t ephemeral = 0;
    if (data && data->getLength() == sizeof(ephemeral))
        memcpy(&ephemeral, data->getBytesNoCopy(), sizeof(ephemeral));
    OSSafeReleaseNULL(chosen);
    // Boards whose Data disk is persistent (fuxi has to run first-boot migration) leave /chosen/ephemeral-storage out, so the
    // explicit sheng-aks opt-in is the gate there; with neither signal the stand-in stays out of the way.
    if ((ephemeral != 1 && !native_aks_state_enabled()) || original) return;
    const OSSymbol *name = OSSymbol::withCString("AppleKeyStore");
    bool absent = name && !OSMetaClass::getMetaClassWithName(name);
    OSSafeReleaseNULL(name);
    if (!absent) return;
    IOService *provider = IOService::getResourceService();
    if (!provider) return;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return;
    native_keyevents_static_init();
    keyevents_status(OSMetaClass::postModLoad(handle));
    if (native_keyevents_status) return;
    native_keyevents_stage = 2;
    IOService *service = native_keyevents_allocate();
    bool initialized = service && service->init();
    bool attached = initialized && service->attach(provider);
    bool started = attached && service->start(provider);
    if (!started) {
        if (attached) service->detach(provider);
        OSSafeReleaseNULL(service);
        keyevents_status(kIOReturnError);
        return;
    }
    native_keyevents_service = (uintptr_t)service;
    keyevents_status(kIOReturnSuccess);
    native_keyevents_stage = 3;
}
volatile uint32_t native_keystore_stage;
volatile int native_keystore_load_status = -1;
volatile uint64_t native_keystore_service;
volatile uint32_t native_keystore_class_size;
__attribute__((noinline)) void native_keystore_checkpoint(void) {
    __asm__ volatile("" ::: "memory");
}
void native_keystore_probe(void) {
    native_keyevents_probe();
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("offline-keystore", &enabled, sizeof(enabled)) || enabled != 1 ||
        native_keystore_stage) return;
    native_keystore_stage = 1;
    native_keystore_load_status = OSKext::loadKextWithIdentifier(
        "com.apple.driver.AppleSEPKeyStore", true, false,
        kOSKextExcludeNone, kOSKextExcludeNone, nullptr);
    native_keystore_stage = 2;
    if (!native_keystore_load_status) {
        OSDictionary *matching = IOService::serviceMatching("AppleKeyStore");
        IOService *service = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
        native_keystore_service = (uintptr_t)service;
        if (service) native_keystore_class_size = service->getMetaClass()->getClassSize();
        OSSafeReleaseNULL(service);
        OSSafeReleaseNULL(matching);
    }
    native_keystore_stage = 3;
    native_keystore_checkpoint();
}
}
