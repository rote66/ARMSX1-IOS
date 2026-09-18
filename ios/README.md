# iOS host scaffold

This directory contains a minimal UIKit host for the shared `libarmsx` build. It embeds SDL2 as a dynamic framework and launches the native FSUI donor shell directly through `external_main`.

## Layout
- `Frameworks/` – drop `SDL2.xcframework` (built from commit 385e995790930bc70ce43533f621caedec033895) and the iOS-built `libarmsx.dylib` here.
- `HostApp/` – source, Info.plist and an XcodeGen `project.yml` that describes a single-app target.
- `bin/bios.bin` – optional test BIOS is bundled (resource marked optional) so the iOS target can boot out-of-the-box during development.

## Building libarmsx for iOS
```
./build.sh ios
```
This forces dynamic SDL, uses the iPhone SDK toolchain, and outputs `bin/libarmsx.dylib` for device (arm64). The default deployment target is iOS 14.0. FSUI code generation requires Python with `jinja2`; set `FSUI_PYTHON_EXECUTABLE` when it is not available from the default Python.

## Building a TrollStore/Dopamine IPA with Xcode 27

Xcode 27 rejects deployment targets below iOS 15 in its build system, even though its clang and linker still accept and emit an iOS 14 Mach-O deployment target. The IPA script compiles the host directly with the SDK toolchain, then applies the JIT entitlements with `ldid`:

```sh
./ios/build-ipa.sh
```

The single output, `dist/ARMSX-iOS14-JIT.ipa`, is fakesigned for TrollStore or a Dopamine/AppSync installation. It includes `allow-jit`, unsigned executable memory, `dynamic-codesigning`, and `get-task-allow`. The signing step also sets the Mach-O CodeDirectory identifier to the same value as `CFBundleIdentifier`; this prevents jailbreak installation tools from assigning the process and Files app different data containers. The current ARMSX CPU engines are interpreters; these entitlements permit JIT memory but do not turn the cached interpreter into a dynamic recompiler.

## Generating/opening the app project
The `HostApp/project.yml` is XcodeGen-ready. After placing the frameworks:
```
cd ios/HostApp
xcodegen generate
open PSXEHost.xcodeproj
```
The target embeds both `SDL2.xcframework` and `libarmsx.dylib` into the app bundle and runs `external_main` on launch using the host `UIWindow`. There is no React Native, CocoaPods, or JS bundle step in the iOS flow anymore.
