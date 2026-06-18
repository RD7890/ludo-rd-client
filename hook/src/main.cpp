#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

#include "zygisk.hpp"

using namespace zygisk;

#define LOG_TAG "LudoRDHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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

// ── Fallback XorShift RNG (replaces original for non-dice calls) ───────────────
// We can't use a trampoline — ARM64 PC-relative instructions (ADRP/BL)
// break when re-executed from a different address.  Instead, replicate the
// int-range logic ourselves so callers still get valid random values.
static uint64_t g_xorSeed = 0;

static void initSeed() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_xorSeed = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 32) ^ 0xDEADBEEF12345678ULL;
    if (g_xorSeed == 0) g_xorSeed = 1;
}

static uint64_t xorshift64() {
    uint64_t x = g_xorSeed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_xorSeed = x;
    return x;
}

static int randRange(int min, int max) {
    if (max <= min) return min;
    return min + (int)(xorshift64() % (uint64_t)(max - min));
}

// ── IL2CPP minimal types ──────────────────────────────────────────────────────
struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;

struct MethodInfo {
    void*        methodPointer;
    void*        virtualMethodPointer;
    void*        invoker_method;
    const char*  name;
    Il2CppClass* klass;
};

// ── IL2CPP API ────────────────────────────────────────────────────────────────
static Il2CppDomain*    (*f_domain_get)()                                            = nullptr;
static Il2CppAssembly** (*f_domain_get_assemblies)(Il2CppDomain*, size_t*)           = nullptr;
static Il2CppImage*     (*f_assembly_get_image)(Il2CppAssembly*)                     = nullptr;
static Il2CppClass*     (*f_class_from_name)(Il2CppImage*, const char*, const char*) = nullptr;
static const MethodInfo*(*f_method_from_name)(Il2CppClass*, const char*, int)        = nullptr;

// ── Patch state ───────────────────────────────────────────────────────────────
#define PATCH_SIZE 16

static void*   g_targetInt       = nullptr;  // RandomRangeInt function ptr
static void*   g_targetFloat     = nullptr;  // Range(float,float) function ptr
static uint8_t g_patchBytes[PATCH_SIZE];     // the patch we write

// Build the 16-byte ARM64 absolute-branch patch into g_patchBytes targeting hookFn
static void buildPatch(void* hookFn) {
    // LDR X16, #8   — load 8-byte address from PC+8
    g_patchBytes[0] = 0x50; g_patchBytes[1] = 0x00;
    g_patchBytes[2] = 0x00; g_patchBytes[3] = 0x58;
    // BR X16
    g_patchBytes[4] = 0x00; g_patchBytes[5] = 0x02;
    g_patchBytes[6] = 0x1F; g_patchBytes[7] = 0xD6;
    // Absolute 64-bit address of our hook
    uint64_t addr = (uint64_t)(uintptr_t)hookFn;
    memcpy(g_patchBytes + 8, &addr, 8);
}

// ── Write to a code page via /proc/self/mem (bypasses W^X) ───────────────────
static bool memWrite(void* target, const void* src, size_t len) {
    int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) {
        LOGE("open /proc/self/mem failed: %s", strerror(errno));
        return false;
    }
    off_t off = (off_t)(uintptr_t)target;
    if (lseek(fd, off, SEEK_SET) != off) {
        LOGE("lseek to %p failed: %s", target, strerror(errno));
        close(fd);
        return false;
    }
    ssize_t written = write(fd, src, len);
    close(fd);
    if (written != (ssize_t)len) {
        LOGE("write to %p failed (wrote %zd/%zu): %s", target, written, len, strerror(errno));
        return false;
    }
    __builtin___clear_cache((char*)target, (char*)target + len);
    return true;
}

// Check if our patch is still intact at a target address
static bool isPatchIntact(void* target) {
    if (!target) return false;
    const uint8_t* p = (const uint8_t*)target;
    // Check LDR X16,#8 opcode
    return p[0] == 0x50 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x58 &&
           p[4] == 0x00 && p[5] == 0x02 && p[6] == 0x1F && p[7] == 0xD6;
}

static bool applyPatch(void* target) {
    bool ok = memWrite(target, g_patchBytes, PATCH_SIZE);
    if (ok) LOGI("Patch applied at %p", target);
    else    LOGE("Patch FAILED at %p", target);
    return ok;
}

// ── Hook functions ────────────────────────────────────────────────────────────
// These are the functions the patch branches to.
// We do NOT call back to the original (trampoline approach is broken for ARM64
// PC-relative code). For non-dice calls we return our own XorShift random value,
// which is indistinguishable from Unity's for AI/game-logic purposes.

static int hook_RandomRangeInt(int32_t min, int32_t max, const MethodInfo* /*method*/) {
    readConfig();
    if (min == 1 && max == 7 && g_forceDice >= 1 && g_forceDice <= 6) {
        LOGI("DICE FORCED %d (req min=%d max=%d)", g_forceDice, min, max);
        return g_forceDice;
    }
    // Pass-through: use our own RNG
    return randRange(min, max);
}

static float hook_RandomRangeFloat(float min, float max, const MethodInfo* /*method*/) {
    readConfig();
    // Dice via float path: Range(1f, 7f)
    if (min >= 0.9f && min <= 1.1f && max >= 6.9f && max <= 7.1f &&
        g_forceDice >= 1 && g_forceDice <= 6) {
        LOGI("DICE(f) FORCED %d", g_forceDice);
        return (float)g_forceDice;
    }
    // Pass-through
    if (max <= min) return min;
    uint64_t r = xorshift64();
    return min + (float)(r & 0xFFFFFF) / (float)0x1000000 * (max - min);
}

// ── Watchdog: re-apply patch every 200 ms ─────────────────────────────────────
static void* watchdogThread(void*) {
    LOGI("Watchdog running");
    while (true) {
        usleep(200000); // 200 ms
        if (g_targetInt && !isPatchIntact(g_targetInt)) {
            LOGI("Patch was wiped — re-applying (int)");
            applyPatch(g_targetInt);
        }
        if (g_targetFloat && !isPatchIntact(g_targetFloat)) {
            LOGI("Patch was wiped — re-applying (float)");
            applyPatch(g_targetFloat);
        }
    }
    return nullptr;
}

// ── Hook installer ────────────────────────────────────────────────────────────
static void* hookThread(void*) {
    LOGI("Hook thread: waiting for libil2cpp.so...");
    initSeed();

    void* il2cpp = nullptr;
    for (int i = 0; i < 600; i++) {
        il2cpp = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (il2cpp) break;
        usleep(100000);
    }
    if (!il2cpp) { LOGE("libil2cpp.so never loaded"); return nullptr; }
    LOGI("libil2cpp.so: %p", il2cpp);

#define RESOLVE(var, sym) \
    var = reinterpret_cast<decltype(var)>(dlsym(il2cpp, sym)); \
    if (!var) { LOGE("dlsym missing: " sym); return nullptr; }

    RESOLVE(f_domain_get,            "il2cpp_domain_get")
    RESOLVE(f_domain_get_assemblies, "il2cpp_domain_get_assemblies")
    RESOLVE(f_assembly_get_image,    "il2cpp_assembly_get_image")
    RESOLVE(f_class_from_name,       "il2cpp_class_from_name")
    RESOLVE(f_method_from_name,      "il2cpp_class_get_method_from_name")
#undef RESOLVE

    Il2CppDomain* domain = nullptr;
    for (int i = 0; i < 300; i++) {
        domain = f_domain_get();
        if (domain) break;
        usleep(100000);
    }
    if (!domain) { LOGE("IL2CPP domain never ready"); return nullptr; }

    size_t count = 0;
    Il2CppAssembly** assemblies = f_domain_get_assemblies(domain, &count);
    LOGI("Assembly count: %zu", count);

    Il2CppClass* randClass = nullptr;
    for (size_t i = 0; i < count && !randClass; i++) {
        Il2CppImage* img = f_assembly_get_image(assemblies[i]);
        if (img) randClass = f_class_from_name(img, "UnityEngine", "Random");
    }
    if (!randClass) { LOGE("UnityEngine.Random not found"); return nullptr; }
    LOGI("UnityEngine.Random: %p", randClass);

    // ── Slot 0: RandomRangeInt ────────────────────────────────────────────────
    const MethodInfo* mi_int = f_method_from_name(randClass, "RandomRangeInt", 2);
    if (mi_int && mi_int->methodPointer) {
        g_targetInt = mi_int->methodPointer;
        LOGI("RandomRangeInt at %p", g_targetInt);

        buildPatch(reinterpret_cast<void*>(hook_RandomRangeInt));

        // Inline patch via /proc/self/mem
        if (applyPatch(g_targetInt)) {
            LOGI("RandomRangeInt inline-hooked (/proc/self/mem)");
        }

        // Belt+suspenders: also redirect the dispatch-table pointer
        // so indirect call sites hit our hook immediately
        MethodInfo* mi_rw = const_cast<MethodInfo*>(mi_int);
        uintptr_t page = (uintptr_t)mi_rw & ~(uintptr_t)(getpagesize()-1);
        mprotect((void*)page, getpagesize()*2, PROT_READ|PROT_WRITE);
        mi_rw->methodPointer = reinterpret_cast<void*>(hook_RandomRangeInt);
        __builtin___clear_cache((char*)mi_rw, (char*)mi_rw + sizeof(void*));
        LOGI("MethodInfo->methodPointer also redirected");

    } else {
        LOGE("RandomRangeInt MethodInfo not found");
    }

    // ── Slot 1: Range(float,float) ────────────────────────────────────────────
    const MethodInfo* mi_f = f_method_from_name(randClass, "Range", 2);
    if (mi_f && mi_f->methodPointer) {
        g_targetFloat = mi_f->methodPointer;
        LOGI("Range(float) at %p", g_targetFloat);

        buildPatch(reinterpret_cast<void*>(hook_RandomRangeFloat));
        applyPatch(g_targetFloat);

        MethodInfo* mi_rw = const_cast<MethodInfo*>(mi_f);
        uintptr_t page = (uintptr_t)mi_rw & ~(uintptr_t)(getpagesize()-1);
        mprotect((void*)page, getpagesize()*2, PROT_READ|PROT_WRITE);
        mi_rw->methodPointer = reinterpret_cast<void*>(hook_RandomRangeFloat);
        __builtin___clear_cache((char*)mi_rw, (char*)mi_rw + sizeof(void*));
        LOGI("Range(float) hooked");
    } else {
        LOGI("Range(float) not found — skipping");
    }

    // ── Watchdog ──────────────────────────────────────────────────────────────
    pthread_t wdTid;
    pthread_create(&wdTid, nullptr, watchdogThread, nullptr);
    pthread_detach(wdTid);
    LOGI("All hooks installed. Watchdog running every 200ms.");

    return nullptr;
}

// ── Zygisk module ─────────────────────────────────────────────────────────────
class LudoRDModule : public ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        const char* proc = env->GetStringUTFChars(args->nice_name, nullptr);
        if (proc) {
            isTarget = strcmp(proc, "com.ludo.king") == 0;
            if (isTarget) LOGI("Target: %s", proc);
            env->ReleaseStringUTFChars(args->nice_name, proc);
        }
        if (!isTarget) api->setOption(Option::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!isTarget) return;
        LOGI("postAppSpecialize — starting hook thread");
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
