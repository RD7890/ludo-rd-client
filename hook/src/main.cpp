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

#define LOG_TAG "LudoRDHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Config ────────────────────────────────────────────────────────────────────
#define CONFIG_PATH "/sdcard/LudoRD/config.conf"

static volatile int g_forceDice = 0;

static void readConfig() {
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) { g_forceDice = 0; return; }
    char line[64];
    while (fgets(line, (int)sizeof(line), f)) {
        if (strncmp(line, "force_dice=", 11) == 0)
            g_forceDice = atoi(line + 11);
    }
    fclose(f);
}

// ── XorShift64 pass-through RNG ───────────────────────────────────────────────
static uint64_t g_xorSeed = 0;

static void initSeed() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_xorSeed = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 32) ^ 0xDEADBEEF12345678ULL;
    if (!g_xorSeed) g_xorSeed = 1;
}

static uint64_t xorshift64() {
    uint64_t x = g_xorSeed;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return (g_xorSeed = x);
}

static int randRange(int min, int max) {
    if (max <= min) return min;
    return min + (int)(xorshift64() % (uint64_t)(max - min));
}

// ── IL2CPP types ──────────────────────────────────────────────────────────────
struct Il2CppDomain; struct Il2CppAssembly;
struct Il2CppImage;  struct Il2CppClass;

struct MethodInfo {
    void* methodPointer; void* virtualMethodPointer;
    void* invoker_method; const char* name; Il2CppClass* klass;
};

static Il2CppDomain*    (*f_domain_get)()                                            = nullptr;
static Il2CppAssembly** (*f_domain_get_assemblies)(Il2CppDomain*, size_t*)           = nullptr;
static Il2CppImage*     (*f_assembly_get_image)(Il2CppAssembly*)                     = nullptr;
static Il2CppClass*     (*f_class_from_name)(Il2CppImage*, const char*, const char*) = nullptr;
static const MethodInfo*(*f_method_from_name)(Il2CppClass*, const char*, int)        = nullptr;

// ── Patch state ───────────────────────────────────────────────────────────────
#define PATCH_SIZE 16

static void*   g_targetInt   = nullptr;
static void*   g_targetFloat = nullptr;
static uint8_t g_patchInt[PATCH_SIZE];
static uint8_t g_patchFloat[PATCH_SIZE];

static void buildPatch(uint8_t* buf, void* hookFn) {
    // LDR X16, #8
    buf[0]=0x50; buf[1]=0x00; buf[2]=0x00; buf[3]=0x58;
    // BR X16
    buf[4]=0x00; buf[5]=0x02; buf[6]=0x1F; buf[7]=0xD6;
    // 64-bit absolute address
    uint64_t addr = (uint64_t)(uintptr_t)hookFn;
    memcpy(buf + 8, &addr, 8);
}

// Write via /proc/self/mem — bypasses W^X enforcement
static bool memWrite(void* target, const void* src, size_t len) {
    int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) { LOGE("open /proc/self/mem: %s", strerror(errno)); return false; }
    off_t off = (off_t)(uintptr_t)target;
    if (lseek(fd, off, SEEK_SET) != off) {
        LOGE("lseek %p: %s", target, strerror(errno));
        close(fd); return false;
    }
    ssize_t w = write(fd, src, len);
    close(fd);
    if (w != (ssize_t)len) {
        LOGE("write %p failed (%zd/%zu): %s", target, w, len, strerror(errno));
        return false;
    }
    __builtin___clear_cache((char*)target, (char*)target + len);
    return true;
}

static bool isPatchIntact(void* target) {
    if (!target) return false;
    const uint8_t* p = (const uint8_t*)target;
    return p[0]==0x50 && p[1]==0x00 && p[2]==0x00 && p[3]==0x58 &&
           p[4]==0x00 && p[5]==0x02 && p[6]==0x1F && p[7]==0xD6;
}

// ── Hook functions ────────────────────────────────────────────────────────────
static int hook_RandomRangeInt(int32_t min, int32_t max, const MethodInfo*) {
    readConfig();
    if (min == 1 && max == 7 && g_forceDice >= 1 && g_forceDice <= 6) {
        LOGI("DICE FORCED %d", g_forceDice);
        return g_forceDice;
    }
    return randRange(min, max);
}

static float hook_RandomRangeFloat(float min, float max, const MethodInfo*) {
    readConfig();
    if (min >= 0.9f && min <= 1.1f && max >= 6.9f && max <= 7.1f &&
        g_forceDice >= 1 && g_forceDice <= 6) {
        LOGI("DICE(f) FORCED %d", g_forceDice);
        return (float)g_forceDice;
    }
    if (max <= min) return min;
    return min + (float)(xorshift64() & 0xFFFFFF) / (float)0x1000000 * (max - min);
}

// ── Watchdog — re-applies patch every 200 ms ─────────────────────────────────
static void* watchdogThread(void*) {
    while (true) {
        usleep(200000);
        if (g_targetInt   && !isPatchIntact(g_targetInt))   memWrite(g_targetInt,   g_patchInt,   PATCH_SIZE);
        if (g_targetFloat && !isPatchIntact(g_targetFloat)) memWrite(g_targetFloat, g_patchFloat, PATCH_SIZE);
    }
    return nullptr;
}

// ── Main hook installer ───────────────────────────────────────────────────────
static void* hookThread(void*) {
    LOGI("Hook thread started (no-root / direct inject mode)");
    initSeed();

    void* il2cpp = nullptr;
    for (int i = 0; i < 600 && !il2cpp; i++) {
        il2cpp = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
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
    for (int i = 0; i < 300 && !domain; i++) { domain = f_domain_get(); usleep(100000); }
    if (!domain) { LOGE("IL2CPP domain never ready"); return nullptr; }

    size_t count = 0;
    Il2CppAssembly** assemblies = f_domain_get_assemblies(domain, &count);
    LOGI("Assemblies: %zu", count);

    Il2CppClass* randClass = nullptr;
    for (size_t i = 0; i < count && !randClass; i++) {
        Il2CppImage* img = f_assembly_get_image(assemblies[i]);
        if (img) randClass = f_class_from_name(img, "UnityEngine", "Random");
    }
    if (!randClass) { LOGE("UnityEngine.Random not found"); return nullptr; }

    // Slot 0 — RandomRangeInt(int,int)
    const MethodInfo* mi_int = f_method_from_name(randClass, "RandomRangeInt", 2);
    if (mi_int && mi_int->methodPointer) {
        g_targetInt = mi_int->methodPointer;
        buildPatch(g_patchInt, (void*)hook_RandomRangeInt);
        bool ok = memWrite(g_targetInt, g_patchInt, PATCH_SIZE);
        LOGI("RandomRangeInt %s at %p", ok ? "hooked" : "FAILED", g_targetInt);

        // Also patch the dispatch table pointer
        MethodInfo* rw = const_cast<MethodInfo*>(mi_int);
        uintptr_t pg = (uintptr_t)rw & ~(uintptr_t)(getpagesize()-1);
        mprotect((void*)pg, getpagesize()*2, PROT_READ|PROT_WRITE);
        rw->methodPointer = (void*)hook_RandomRangeInt;
        __builtin___clear_cache((char*)rw, (char*)rw + sizeof(void*));
    }

    // Slot 1 — Range(float,float)
    const MethodInfo* mi_f = f_method_from_name(randClass, "Range", 2);
    if (mi_f && mi_f->methodPointer) {
        g_targetFloat = mi_f->methodPointer;
        buildPatch(g_patchFloat, (void*)hook_RandomRangeFloat);
        memWrite(g_targetFloat, g_patchFloat, PATCH_SIZE);
        LOGI("Range(float) hooked at %p", g_targetFloat);

        MethodInfo* rw = const_cast<MethodInfo*>(mi_f);
        uintptr_t pg = (uintptr_t)rw & ~(uintptr_t)(getpagesize()-1);
        mprotect((void*)pg, getpagesize()*2, PROT_READ|PROT_WRITE);
        rw->methodPointer = (void*)hook_RandomRangeFloat;
        __builtin___clear_cache((char*)rw, (char*)rw + sizeof(void*));
    }

    pthread_t wdTid;
    pthread_create(&wdTid, nullptr, watchdogThread, nullptr);
    pthread_detach(wdTid);
    LOGI("All hooks installed. Watchdog running.");
    return nullptr;
}

// ── Entry point — called by the injected smali via System.loadLibrary ─────────
extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    LOGI("JNI_OnLoad — launching hook thread");
    pthread_t tid;
    pthread_create(&tid, nullptr, hookThread, nullptr);
    pthread_detach(tid);
    return JNI_VERSION_1_6;
}
