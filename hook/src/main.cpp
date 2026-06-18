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

#include "zygisk.hpp"

using namespace zygisk;

#define LOG_TAG  "LudoRDHook"
#define LOGI(...)  __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Config ────────────────────────────────────────────────────────────────────
// Written by the Ludo RD Client app; read by this hook inside com.ludo.king
#define CONFIG_PATH "/sdcard/LudoRD/config.conf"

static volatile int g_forceDice     = 0;   // 0 = random, 1-6 = forced value
static volatile int g_redAlwaysWins = 0;   // not yet implemented (needs deeper hook)

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

// Unity 6 IL2CPP MethodInfo — offset 0 is always methodPointer
struct MethodInfo {
    void*       methodPointer;        // offset  0  ← we replace this
    void*       virtualMethodPointer; // offset  8
    void*       invoker_method;       // offset 16
    const char* name;                 // offset 24
    Il2CppClass* klass;              // offset 32
    // ... rest omitted
};

// ── IL2CPP API (all exported from libil2cpp.so) ───────────────────────────────
static Il2CppDomain*    (*f_domain_get)()                                          = nullptr;
static Il2CppAssembly** (*f_domain_get_assemblies)(Il2CppDomain*, size_t*)         = nullptr;
static Il2CppImage*     (*f_assembly_get_image)(Il2CppAssembly*)                   = nullptr;
static Il2CppClass*     (*f_class_from_name)(Il2CppImage*, const char*, const char*) = nullptr;
static const MethodInfo*(*f_method_from_name)(Il2CppClass*, const char*, int)      = nullptr;

// ── Hook state ────────────────────────────────────────────────────────────────
static void* g_origRangeInt = nullptr;

// IL2CPP static method calling convention (arm64):
//   int RandomRangeInt(int32_t min, int32_t max, const MethodInfo* method)
static int hook_RandomRangeInt(int32_t min, int32_t max, const MethodInfo* method) {
    readConfig();

    // Dice roll: Unity calls RandomRangeInt(1, 7) to get a 1-6 result
    if (min == 1 && max == 7 && g_forceDice >= 1 && g_forceDice <= 6) {
        LOGI("Dice intercepted → forcing %d (original would be random 1-6)", g_forceDice);
        return g_forceDice;
    }

    // Pass through everything else
    using Fn = int(*)(int32_t, int32_t, const MethodInfo*);
    return reinterpret_cast<Fn>(g_origRangeInt)(min, max, method);
}

// ── Hook installer (runs on a background thread) ──────────────────────────────
static void* hookThread(void*) {
    LOGI("Hook thread started — waiting for libil2cpp.so...");

    void* il2cpp_handle = nullptr;
    // Poll until libil2cpp.so is loaded into the process
    for (int i = 0; i < 600; i++) {
        il2cpp_handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (il2cpp_handle) break;
        usleep(100000); // 100 ms
    }

    if (!il2cpp_handle) { LOGE("libil2cpp.so never appeared"); return nullptr; }
    LOGI("libil2cpp.so handle: %p", il2cpp_handle);

    // Resolve the IL2CPP API functions
#define RESOLVE(var, name) \
    var = reinterpret_cast<decltype(var)>(dlsym(il2cpp_handle, name)); \
    if (!var) { LOGE("dlsym failed: " name); return nullptr; }

    RESOLVE(f_domain_get,            "il2cpp_domain_get")
    RESOLVE(f_domain_get_assemblies, "il2cpp_domain_get_assemblies")
    RESOLVE(f_assembly_get_image,    "il2cpp_assembly_get_image")
    RESOLVE(f_class_from_name,       "il2cpp_class_from_name")
    RESOLVE(f_method_from_name,      "il2cpp_class_get_method_from_name")
#undef RESOLVE

    // Wait for the IL2CPP runtime to fully initialize
    Il2CppDomain* domain = nullptr;
    for (int i = 0; i < 300; i++) {
        domain = f_domain_get();
        if (domain) break;
        usleep(100000);
    }
    if (!domain) { LOGE("IL2CPP domain never initialized"); return nullptr; }
    LOGI("IL2CPP domain ready: %p", domain);

    // Walk all assemblies to find UnityEngine.Random
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
    LOGI("UnityEngine.Random class: %p", randomClass);

    // Get RandomRangeInt(int, int) — 2 parameters
    const MethodInfo* mi = f_method_from_name(randomClass, "RandomRangeInt", 2);
    if (!mi) { LOGE("RandomRangeInt method not found"); return nullptr; }
    LOGI("RandomRangeInt MethodInfo: %p  methodPointer: %p", mi, mi->methodPointer);

    // Save original and replace the methodPointer
    g_origRangeInt = mi->methodPointer;

    // mprotect the page holding MethodInfo to make it writable
    uintptr_t addr  = reinterpret_cast<uintptr_t>(mi);
    uintptr_t page  = addr & ~(static_cast<uintptr_t>(getpagesize() - 1));
    size_t    len   = getpagesize() * 2;
    if (mprotect(reinterpret_cast<void*>(page), len, PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect failed — trying without write protection bypass");
    }

    // Patch the method pointer (cast away const — we own this memory at runtime)
    MethodInfo* mi_rw = const_cast<MethodInfo*>(mi);
    mi_rw->methodPointer = reinterpret_cast<void*>(hook_RandomRangeInt);

    // Flush I-cache so the CPU sees the new pointer
    __builtin___clear_cache(
        reinterpret_cast<char*>(mi_rw),
        reinterpret_cast<char*>(mi_rw) + sizeof(void*));

    LOGI("✅ Hook installed: RandomRangeInt → hook_RandomRangeInt  (orig: %p)", g_origRangeInt);
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
            if (isTarget) LOGI("Target process detected: %s", proc);
            env->ReleaseStringUTFChars(args->nice_name, proc);
        }
        if (!isTarget) {
            // Don't waste memory in other processes
            api->setOption(Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!isTarget) return;
        LOGI("postAppSpecialize — launching hook thread");
        pthread_t tid;
        pthread_create(&tid, nullptr, hookThread, nullptr);
        pthread_detach(tid);
    }

    void preServerSpecialize(ServerSpecializeArgs* args) override {
        api->setOption(Option::DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api*    api      = nullptr;
    JNIEnv* env      = nullptr;
    bool    isTarget = false;
};

REGISTER_ZYGISK_MODULE(LudoRDModule)
