#!/usr/bin/env python3
"""
Inject System.loadLibrary("ludordhook") into a decoded Ludo King APK.

Usage:
    python3 inject.py <decoded_apk_dir>
"""

import sys, os, re, shutil

def fatal(msg):
    print(f"[INJECT] ERROR: {msg}", flush=True)
    sys.exit(1)

def log(msg):
    print(f"[INJECT] {msg}", flush=True)

# ── 1. Locate AndroidManifest.xml ─────────────────────────────────────────────
apk_dir = sys.argv[1] if len(sys.argv) > 1 else "."
manifest_path = os.path.join(apk_dir, "AndroidManifest.xml")
if not os.path.exists(manifest_path):
    fatal(f"AndroidManifest.xml not found in {apk_dir}")

with open(manifest_path, "r", encoding="utf-8") as f:
    manifest = f.read()

log(f"Manifest loaded ({len(manifest)} bytes)")

# ── 2. Find existing Application class name ────────────────────────────────────
app_class_match = re.search(r'android:name="([^"]*Application[^"]*)"', manifest)
pkg_match       = re.search(r'package="([^"]+)"', manifest)

if not pkg_match:
    fatal("Cannot find package name in manifest")

package_name = pkg_match.group(1)
log(f"Package: {package_name}")

LOADER_CLASS_JAVA  = "com.ludord.hook.LudoRDLoader"
LOADER_CLASS_SMALI = "com/ludord/hook/LudoRDLoader"
LOADER_CLASS_L     = "Lcom/ludord/hook/LudoRDLoader;"

smali_base = os.path.join(apk_dir, "smali")
# Some APKs use smali_classes2, smali_classes3, etc.
smali_dirs = sorted([d for d in os.listdir(apk_dir) if d.startswith("smali")])
if not smali_dirs:
    fatal("No smali directory found")
log(f"Smali dirs: {smali_dirs}")

# Pick the first smali dir for injection
inject_smali_dir = os.path.join(apk_dir, smali_dirs[0])

# ── 3. Determine what to extend ───────────────────────────────────────────────
if app_class_match:
    original_app = app_class_match.group(1)
    # Resolve relative name
    if original_app.startswith("."):
        original_app = package_name + original_app
    log(f"Existing Application class: {original_app}")
    super_class_l = "L" + original_app.replace(".", "/") + ";"
else:
    log("No Application class found — extending android.app.Application")
    super_class_l = "Landroid/app/Application;"

# ── 4. Write our loader smali ─────────────────────────────────────────────────
loader_smali_content = f""".class public Lcom/ludord/hook/LudoRDLoader;
.super {super_class_l}

# LudoRD no-root hook loader
# Calls System.loadLibrary("ludordhook") before any game code runs.

.method public constructor <init>()V
    .registers 1
    invoke-direct {{p0}}, {super_class_l}-><init>()V
    return-void
.end method

.method public attachBaseContext(Landroid/content/Context;)V
    .registers 3
    invoke-super {{p0, p1}}, {super_class_l}->attachBaseContext(Landroid/content/Context;)V
    const-string v0, "ludordhook"
    invoke-static {{v0}}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
    return-void
.end method

.method public onCreate()V
    .registers 1
    invoke-super {{p0}}, {super_class_l}->onCreate()V
    return-void
.end method
"""

loader_dir = os.path.join(inject_smali_dir, "com", "ludord", "hook")
os.makedirs(loader_dir, exist_ok=True)
loader_path = os.path.join(loader_dir, "LudoRDLoader.smali")
with open(loader_path, "w", encoding="utf-8") as f:
    f.write(loader_smali_content)
log(f"Loader smali written: {loader_path}")

# ── 5. Update AndroidManifest.xml ─────────────────────────────────────────────
if app_class_match:
    # Replace existing android:name in <application> with our loader
    manifest = manifest.replace(
        app_class_match.group(0),
        f'android:name="{LOADER_CLASS_JAVA}"'
    )
    log(f"Replaced application class → {LOADER_CLASS_JAVA}")
else:
    # Inject android:name into <application ...>
    manifest = manifest.replace(
        "<application ",
        f'<application android:name="{LOADER_CLASS_JAVA}" ',
        1
    )
    log(f"Injected android:name={LOADER_CLASS_JAVA} into <application>")

with open(manifest_path, "w", encoding="utf-8") as f:
    f.write(manifest)
log("Manifest updated")

# ── 6. Copy native library ────────────────────────────────────────────────────
# The workflow places the built .so next to this script as ludordhook.so
so_src = os.path.join(os.path.dirname(__file__), "ludordhook.so")
if not os.path.exists(so_src):
    fatal(f"ludordhook.so not found at {so_src} — did the build step run?")

lib_dir = os.path.join(apk_dir, "lib", "arm64-v8a")
os.makedirs(lib_dir, exist_ok=True)
so_dst = os.path.join(lib_dir, "libludordhook.so")
shutil.copy2(so_src, so_dst)
log(f"Copied .so → {so_dst}")

log("✅ Injection complete")
