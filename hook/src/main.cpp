#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

#include "zygisk.hpp"

using namespace zygisk;

#define LOG_TAG  "LudoRDHook"
#define LOGI(...)  __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Config ────────────────────────────────────────────────────────────────────
#define CONFIG_PATH "/sdcard/LudoRD/config.conf"

static volatile int g_forceDice     = 0;
static volatile int g_redAlwaysWins = 0;

static void readConfig() {
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) { g_forceDice = 0; g_redAlwaysWins = 0; return; }
    char line[64];
    while (fgets(line, (int)sizeof(line), f)) {
        if (strncmp(line, "force_dice=", 11) == 0)      g_forceDice     = atoi(line + 11);
        if (strncmp(line, "red_always_wins=", 16) == 0) g_redAlwaysWins = atoi(line + 16);
    }
    fclose(f);
}

// ── IL2CPP minimal types ──────────────────────────────────────────────────────
struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;

struct MethodInfo {
    void*       methodPointer;
    void*       virtualMethodPointer;
    void*       invoker_method;
    const char* name;
    Il2CppClass* klass;
};

// ── IL2CPP API ────────────────────────────────────────────────────────────────
static Il2CppDomain*    (*f_domain_get)()                                            = nullptr;
static Il2CppAssembly** (*f_domain_get_assemblies)(Il2CppDomain*, size_t*)           = nullptr;
static Il2CppImage*     (*f_assembly_get_image)(Il2CppAssembly*)                     = nullptr;
static Il2CppClass*     (*f_class_from_name)(Il2CppImage*, const char*, const char*) = nullptr;
static const MethodInfo*(*f_method_from_name)(Il2CppClass*, const char*, int)        = nullptr;

// ── ARM64 inline hook ─────────────────────────────────────────────────────────
// We overwrite the first 16 bytes of the target function with:
//   LDR X16, #8       ; load 8-byte address from pc+8
//   BR  X16           ; branch to it
//   .8byte <hook_fn>  ; the absolute address

#define PATCH_SIZE 16

struct PatchSite {
    uint8_t  saved[PATCH_SIZE];   // original bytes (for the trampoline)
    void*    target;               // pointer to the patched function
    bool     installed;
};

// We hook two functions:
//   slot 0 = UnityEngine.Random.RandomRangeInt(int,int)
//   slot 1 = UnityEngine.Random.Range(float,float)  (Ludo King also uses this)
#define NUM_HOOKS 2
static PatchSite g_sites[NUM_HOOKS];

// Trampoline buffers: saved_bytes + absolute-jump back to original+PATCH_SIZE
// Layout per trampoline (32 bytes):
//   [0..15]  : original saved bytes
//   [16..31] : LDR X16, #8 / BR X16 / .8byte (target + PATCH_SIZE)
static uint8_t g_trampolines[NUM_HOOKS][32] __attribute__((aligned(8)));

// Original function pointers (point into the trampoline so IL2CPP caching is bypassed)
static int (*g_origRangeInt)(int32_t, int32_t, const MethodInfo*) = nullptr;
static float (*g_origRangeFloat)(float, float, const MethodInfo*) = nullptr;

// ── Write an absolute branch patch ───────────────────────────────────────────
static bool writePatch(void* target, void* hookFn) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(target);
    uintptr_t page = addr & ~(static_cast<uintptr_t>(getpagesize() - 1));
    size_t    len  = getpagesize() * 2;

    if (mprotect(reinterpret_cast<void*>(page), len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect RWX failed for %p: %s", target, strerror(errno));
        // Try without EXEC (some kernels split W^X)
        if (mprotect(reinterpret_cast<void*>(page), len, PROT_READ | PROT_WRITE) != 0) {
            LOGE("mprotect RW also failed: %s", strerror(errno));
            return false;
        }
    }

    uint8_t* p = reinterpret_cast<uint8_t*>(target);
    // LDR X16, #8  (0x58000050)
    p[0] = 0x50; p[1] = 0x00; p[2] = 0x00; p[3] = 0x58;
    // BR X16       (0xD61F0200)
    p[4] = 0x00; p[5] = 0x02; p[6] = 0x1F; p[7] = 0xD6;
    // 8-byte absolute address of the hook function
    uint64_t hAddr = reinterpret_cast<uint64_t>(hookFn);
    memcpy(p + 8, &hAddr, 8);

    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target) + PATCH_SIZE);
    return true;
}

// ── Check if our patch is still in place ─────────────────────────────────────
static bool isPatchIntact(int slot) {
    if (!g_sites[slot].target || !g_sites[slot].installed) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(g_sites[slot].target);
    // Check the LDR X16 opcode bytes
    return (p[0] == 0x50 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x58 &&
            p[4] == 0x00 && p[5] == 0x02 && p[6] == 0x1F && p[7] == 0xD6);
}

// ── Build trampoline for a slot ───────────────────────────────────────────────
static void buildTrampoline(int slot, void* target, void* hookFn) {
    uint8_t* tramp = g_trampolines[slot];

    // First 16 bytes: copy saved original instructions
    memcpy(tramp, g_sites[slot].saved, PATCH_SIZE);

    // Next 16 bytes: absolute jump back to target + PATCH_SIZE
    uintptr_t returnAddr = reinterpret_cast<uintptr_t>(target) + PATCH_SIZE;
    tramp[16] = 0x50; tramp[17] = 0x00; tramp[18] = 0x00; tramp[19] = 0x58; // LDR X16, #8
    tramp[20] = 0x00; tramp[21] = 0x02; tramp[22] = 0x1F; tramp[23] = 0xD6; // BR X16
    memcpy(tramp + 24, &returnAddr, 8);

    // Make trampoline executable
    uintptr_t tp   = reinterpret_cast<uintptr_t>(tramp);
    uintptr_t tpag = tp & ~(static_cast<uintptr_t>(getpagesize() - 1));
    mprotect(reinterpret_cast<void*>(tpag), getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char*>(tramp),
                            reinterpret_cast<char*>(tramp) + 32);
}

// ── Install (or re-install) a hook at a slot ─────────────────────────────────
static bool installHook(int slot, void* target, void* hookFn) {
    if (!g_sites[slot].installed) {
        // First install: save original bytes and build trampoline
        memcpy(g_sites[slot].saved, target, PATCH_SIZE);
        g_sites[slot].target    = target;
        buildTrampoline(slot, target, hookFn);
        g_sites[slot].installed = true;
        LOGI("[slot %d] First install: target=%p hook=%p trampoline=%p",
             slot, target, hookFn, g_trampolines[slot]);
    }

    bool ok = writePatch(target, hookFn);
    if (ok) LOGI("[slot %d] Patch written OK at %p", slot, target);
    else    LOGE("[slot %d] Patch FAILED at %p", slot, target);
    return ok;
}

// ── Hook implementations ──────────────────────────────────────────────────────
static int hook_RandomRangeInt(int32_t min, int32_t max, const MethodInfo* method) {
    readConfig();
    // Dice: Unity calls RandomRangeInt(1,7) → returns 1..6
    if (min == 1 && max == 7 && g_forceDice >= 1 && g_forceDice <= 6) {
        LOGI("🎲 Dice intercepted → forcing %d", g_forceDice);
        return g_forceDice;
    }
    return g_origRangeInt(min, max, method);
}

static float hook_RandomRangeFloat(float min, float max, const MethodInfo* method) {
    readConfig();
    // Dice via float: some versions call Range(1f, 7f)
    if (min >= 0.9f && min <= 1.1f && max >= 6.9f && max <= 7.1f &&
        g_forceDice >= 1 && g_forceDice <= 6) {
        LOGI("🎲 Dice (float) intercepted → forcing %d", g_forceDice);
        return static_cast<float>(g_forceDice);
    }
    return g_origRangeFloat(min, max, method);
}

// ── Watchdog: re-applies patches every 500 ms ────────────────────────────────
static void* watchdogThread(void*) {
    LOGI("Watchdog started");
    while (true) {
        usleep(500000); // 500 ms
        for (int i = 0; i < NUM_HOOKS; i++) {
            if (g_sites[i].installed && !isPatchIntact(i)) {
                LOGI("[slot %d] Patch was overwritten — re-applying", i);
                void* hookFn = (i == 0)
                    ? reinterpret_cast<void*>(hook_RandomRangeInt)
                    : reinterpret_cast<void*>(hook_RandomRangeFloat);
                writePatch(g_sites[i].target, hookFn);
            }
        }
    }
    return nullptr;
}

// ── Hook installer thread ─────────────────────────────────────────────────────
static void* hookThread(void*) {
    LOGI("Hook thread started — waiting for libil2cpp.so...");

    void* il2cpp_handle = nullptr;
    for (int i = 0; i < 600; i++) {
        il2cpp_handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (il2cpp_handle) break;
        usleep(100000);
    }
    if (!il2cpp_handle) { LOGE("libil2cpp.so never appeared"); return nullptr; }
    LOGI("libil2cpp.so handle: %p", il2cpp_handle);

#define RESOLVE(var, name) \
    var = reinterpret_cast<decltype(var)>(dlsym(il2cpp_handle, name)); \
    if (!var) { LOGE("dlsym failed: " name); return nullptr; }

    RESOLVE(f_domain_get,            "il2cpp_domain_get")
    RESOLVE(f_domain_get_assemblies, "il2cpp_domain_get_assemblies")
    RESOLVE(f_assembly_get_image,    "il2cpp_assembly_get_image")
    RESOLVE(f_class_from_name,       "il2cpp_class_from_name")
    RESOLVE(f_method_from_name,      "il2cpp_class_get_method_from_name")
#undef RESOLVE

    // Wait for IL2CPP domain
    Il2CppDomain* domain = nullptr;
    for (int i = 0; i < 300; i++) {
        domain = f_domain_get();
        if (domain) break;
        usleep(100000);
    }
    if (!domain) { LOGE("IL2CPP domain never initialized"); return nullptr; }
    LOGI("IL2CPP domain: %p", domain);

    // Walk assemblies to find UnityEngine.Random
    size_t count = 0;
    Il2CppAssembly** assemblies = f_domain_get_assemblies(domain, &count);
    LOGI("Assembly count: %zu", count);

    Il2CppClass* randomClass = nullptr;
    for (size_t i = 0; i < count && !randomClass; i++) {
        Il2CppImage* img = f_assembly_get_image(assemblies[i]);
        if (!img) continue;
        randomClass = f_class_from_name(img, "UnityEngine", "Random");
    }
    if (!randomClass) { LOGE("UnityEngine.Random class not found"); return nullptr; }
    LOGI("UnityEngine.Random: %p", randomClass);

    // ── Slot 0: RandomRangeInt(int, int) ──────────────────────────────────────
    const MethodInfo* mi_int = f_method_from_name(randomClass, "RandomRangeInt", 2);
    if (mi_int && mi_int->methodPointer) {
        LOGI("RandomRangeInt ptr: %p", mi_int->methodPointer);
        if (installHook(0, mi_int->methodPointer,
                        reinterpret_cast<void*>(hook_RandomRangeInt))) {
            // Point the trampoline so our original caller works
            g_origRangeInt = reinterpret_cast<
                int(*)(int32_t, int32_t, const MethodInfo*)>(g_trampolines[0]);
            LOGI("✅ RandomRangeInt hooked. Trampoline: %p", g_origRangeInt);
        }
    } else {
        LOGE("RandomRangeInt method/pointer not found");
    }

    // ── Slot 1: Range(float, float) ───────────────────────────────────────────
    const MethodInfo* mi_float = f_method_from_name(randomClass, "Range", 2);
    if (mi_float && mi_float->methodPointer) {
        LOGI("Range(float) ptr: %p", mi_float->methodPointer);
        if (installHook(1, mi_float->methodPointer,
                        reinterpret_cast<void*>(hook_RandomRangeFloat))) {
            g_origRangeFloat = reinterpret_cast<
                float(*)(float, float, const MethodInfo*)>(g_trampolines[1]);
            LOGI("✅ Range(float) hooked. Trampoline: %p", g_origRangeFloat);
        }
    } else {
        LOGI("Range(float) not found — skipping (not fatal)");
    }

    // ── Patch the MethodInfo dispatch table too (belt + suspenders) ───────────
    // Some IL2CPP call sites go through the table, others use the cached raw ptr.
    // Patching both covers all paths.
    if (mi_int) {
        MethodInfo* mi_rw = const_cast<MethodInfo*>(mi_int);
        uintptr_t addr = reinterpret_cast<uintptr_t>(mi_rw);
        uintptr_t page = addr & ~(static_cast<uintptr_t>(getpagesize() - 1));
        mprotect(reinterpret_cast<void*>(page), getpagesize() * 2, PROT_READ | PROT_WRITE);
        mi_rw->methodPointer = reinterpret_cast<void*>(hook_RandomRangeInt);
        __builtin___clear_cache(reinterpret_cast<char*>(mi_rw),
                                reinterpret_cast<char*>(mi_rw) + sizeof(void*));
        LOGI("✅ MethodInfo dispatch table also patched");
    }

    // ── Start watchdog ────────────────────────────────────────────────────────
    pthread_t wdTid;
    pthread_create(&wdTid, nullptr, watchdogThread, nullptr);
    pthread_detach(wdTid);
    LOGI("Watchdog thread launched");

    return nullptr;
}

// ── Zygisk Module ─────────────────────────────────────────────────────────────
class LudoRDModule : public ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        const char* proc = env->GetStringUTFChars(args->nice_name, nullptr);
        if (proc) {
            isTarget = (strcmp(proc, "com.ludo.king") == 0);
            if (isTarget) LOGI("Target process: %s", proc);
            env->ReleaseStringUTFChars(args->nice_name, proc);
        }
        if (!isTarget) api->setOption(Option::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!isTarget) return;
        LOGI("postAppSpecialize — launching hook thread");
        pthread_t tid;
        pthread_create(&tid, nullptr, hookThread, nullptr);
        pthread_detach(tid);
    }

    void preServerSpecialize(ServerSpecializeArgs*) override {
        api->setOption(Option::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api*    api      = nullptr;
    JNIEnv* env      = nullptr;
    bool    isTarget = false;
};

REGISTER_ZYGISK_MODULE(LudoRDModule)
