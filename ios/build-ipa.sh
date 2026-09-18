#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
DEPLOYMENT_TARGET=${IOS_DEPLOYMENT_TARGET:-14.0}
BUNDLE_IDENTIFIER=${IOS_BUNDLE_IDENTIFIER:-com.nanodata.armsx}
BUILD_ROOT="${REPO_ROOT}/build/ios-ipa"
OBJECT_ROOT="${BUILD_ROOT}/objects"
STAGE_ROOT="${BUILD_ROOT}/stage"
APP_ROOT="${STAGE_ROOT}/Payload/ARMSX.app"
FRAMEWORKS_ROOT="${APP_ROOT}/Frameworks"
IPA_OUTPUT=${IPA_OUTPUT:-"${REPO_ROOT}/dist/ARMSX-iOS14-JIT.ipa"}
ENTITLEMENTS="${REPO_ROOT}/ios/HostApp/Sources/ARMSX.entitlements"
SDL_FRAMEWORK="${REPO_ROOT}/ios/Frameworks/SDL2.xcframework/ios-arm64/SDL2.framework"

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Required tool not found: $1" >&2
        exit 1
    fi
}

mach_o_minos() {
    otool -l "$1" | awk '
        $1 == "cmd" && $2 == "LC_BUILD_VERSION" { build_version = 1; next }
        build_version && $1 == "minos" { print $2; exit }
        $1 == "cmd" && $2 == "LC_VERSION_MIN_IPHONEOS" { legacy_version = 1; next }
        legacy_version && $1 == "version" { print $2; exit }
    '
}

verify_minos() {
    binary=$1
    minos=$(mach_o_minos "$binary")
    if [ -z "$minos" ]; then
        echo "Could not read the iOS deployment target from ${binary}" >&2
        exit 1
    fi
    if ! awk -v actual="$minos" -v requested="$DEPLOYMENT_TARGET" \
        'BEGIN { exit !((actual + 0) <= (requested + 0)) }'; then
        echo "${binary} requires iOS ${minos}, above requested iOS ${DEPLOYMENT_TARGET}" >&2
        exit 1
    fi
    printf '  minos %-5s %s\n' "$minos" "$binary"
}

for tool in codesign ditto grep ldid otool plutil shasum sips xcrun zip; do
    require_tool "$tool"
done

if [ ! -d "$SDL_FRAMEWORK" ]; then
    echo "Missing device SDL2 framework: ${SDL_FRAMEWORK}" >&2
    exit 1
fi

if [ "${SKIP_NATIVE_BUILD:-0}" != "1" ]; then
    (
        cd "$REPO_ROOT"
        IOS_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" ./build.sh ios
    )
fi

if [ ! -f "${REPO_ROOT}/ios/Frameworks/libarmsx.dylib" ]; then
    echo "Missing ios/Frameworks/libarmsx.dylib; run ./build.sh ios first." >&2
    exit 1
fi

SDKROOT=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos --find clang)
CXX=$(xcrun --sdk iphoneos --find clang++)
STRIP=$(xcrun --sdk iphoneos --find strip)
SDL_PARENT=$(dirname "$SDL_FRAMEWORK")
SOURCE_ROOT="${REPO_ROOT}/ios/HostApp/Sources"

rm -rf "$OBJECT_ROOT" "$STAGE_ROOT"
mkdir -p "$OBJECT_ROOT/ModuleCache" "$FRAMEWORKS_ROOT" "$(dirname "$IPA_OUTPUT")"

"$CC" -arch arm64 -isysroot "$SDKROOT" \
    -miphoneos-version-min="$DEPLOYMENT_TARGET" -fobjc-arc -O2 -fmodules \
    -fmodules-cache-path="$OBJECT_ROOT/ModuleCache" -I"$SOURCE_ROOT" -F"$SDL_PARENT" \
    -c "$SOURCE_ROOT/main.m" -o "$OBJECT_ROOT/main.o"

for source in AppDelegate.mm EmulatorRunner.mm; do
    "$CXX" -arch arm64 -isysroot "$SDKROOT" \
        -miphoneos-version-min="$DEPLOYMENT_TARGET" -fobjc-arc -O2 -fmodules \
        -fmodules-cache-path="$OBJECT_ROOT/ModuleCache" -I"$SOURCE_ROOT" -F"$SDL_PARENT" \
        -std=gnu++14 -c "$SOURCE_ROOT/$source" -o "$OBJECT_ROOT/${source%.mm}.o"
done

"$CXX" -arch arm64 -isysroot "$SDKROOT" \
    -miphoneos-version-min="$DEPLOYMENT_TARGET" \
    "$OBJECT_ROOT/main.o" "$OBJECT_ROOT/AppDelegate.o" "$OBJECT_ROOT/EmulatorRunner.o" \
    "${REPO_ROOT}/ios/Frameworks/libarmsx.dylib" \
    -F"$SDL_PARENT" -framework SDL2 -framework UIKit -framework Foundation -framework CoreMotion \
    -framework UniformTypeIdentifiers -Wl,-rpath,@executable_path/Frameworks \
    -o "$APP_ROOT/ARMSX"

cp "$SOURCE_ROOT/Info.plist" "$APP_ROOT/Info.plist"
plutil -replace CFBundleExecutable -string ARMSX "$APP_ROOT/Info.plist"
plutil -replace CFBundleIdentifier -string "$BUNDLE_IDENTIFIER" "$APP_ROOT/Info.plist"
plutil -replace CFBundleName -string ARMSX "$APP_ROOT/Info.plist"
plutil -replace MinimumOSVersion -string "$DEPLOYMENT_TARGET" "$APP_ROOT/Info.plist"
plutil -lint "$APP_ROOT/Info.plist"

xcrun ibtool --compile "$APP_ROOT/LaunchScreen.storyboardc" \
    "$SOURCE_ROOT/LaunchScreen.storyboard" \
    --minimum-deployment-target "$DEPLOYMENT_TARGET" \
    --target-device iphone \
    --target-device ipad

cp "${REPO_ROOT}/ios/HostApp/PrivacyInfo.xcprivacy" "$APP_ROOT/PrivacyInfo.xcprivacy"
cp -R "${REPO_ROOT}/icons" "$APP_ROOT/icons"
cp "${REPO_ROOT}/ios/Frameworks/libarmsx.dylib" "$FRAMEWORKS_ROOT/libarmsx.dylib"
ditto "$SDL_FRAMEWORK" "$FRAMEWORKS_ROOT/SDL2.framework"
rm -rf "$FRAMEWORKS_ROOT/SDL2.framework/Headers" \
    "$FRAMEWORKS_ROOT/SDL2.framework/CMake"
rm -f "$FRAMEWORKS_ROOT/SDL2.framework/License.txt" \
    "$FRAMEWORKS_ROOT/SDL2.framework/ReadMe.txt"

for bios_candidate in \
    "${REPO_ROOT}/ios/bios.bin" \
    "${REPO_ROOT}/ios/Contents/bios.bin" \
    "${REPO_ROOT}/bin/bios.bin"; do
    if [ -f "$bios_candidate" ]; then
        cp "$bios_candidate" "$APP_ROOT/bios.bin"
        break
    fi
done

# Legacy icon files remain supported by the TrollStore/Dopamine target range.
# The JPEG round-trip flattens transparency before producing each PNG.
sips -s format jpeg "${REPO_ROOT}/icons/AppIconLarge.png" \
    --out "$OBJECT_ROOT/AppIcon.jpg" >/dev/null
for icon_spec in \
    '120 AppIcon60x60@2x.png' \
    '180 AppIcon60x60@3x.png' \
    '152 AppIcon76x76@2x~ipad.png' \
    '167 AppIcon83.5x83.5@2x~ipad.png'; do
    icon_size=${icon_spec%% *}
    icon_name=${icon_spec#* }
    sips -z "$icon_size" "$icon_size" -s format png "$OBJECT_ROOT/AppIcon.jpg" \
        --out "$APP_ROOT/$icon_name" >/dev/null
done

"$STRIP" -S -x "$APP_ROOT/ARMSX" "$FRAMEWORKS_ROOT/libarmsx.dylib"
ldid -S "$FRAMEWORKS_ROOT/SDL2.framework/SDL2"
ldid -S "$FRAMEWORKS_ROOT/libarmsx.dylib"
ldid -I"$BUNDLE_IDENTIFIER" -S"$ENTITLEMENTS" "$APP_ROOT/ARMSX"

SIGNED_IDENTIFIER=$(codesign -dvv "$APP_ROOT/ARMSX" 2>&1 | sed -n 's/^Identifier=//p')
if [ "$SIGNED_IDENTIFIER" != "$BUNDLE_IDENTIFIER" ]; then
    echo "Signed identifier ${SIGNED_IDENTIFIER:-<missing>} does not match bundle identifier ${BUNDLE_IDENTIFIER}" >&2
    exit 1
fi

echo "Deployment targets:"
printf '  identifier %s\n' "$SIGNED_IDENTIFIER"
verify_minos "$APP_ROOT/ARMSX"
verify_minos "$FRAMEWORKS_ROOT/libarmsx.dylib"
verify_minos "$FRAMEWORKS_ROOT/SDL2.framework/SDL2"

SIGNED_ENTITLEMENTS="$OBJECT_ROOT/signed-entitlements.plist"
ldid -e "$APP_ROOT/ARMSX" > "$SIGNED_ENTITLEMENTS"
for entitlement in \
    com.apple.security.cs.allow-jit \
    com.apple.security.cs.allow-unsigned-executable-memory \
    get-task-allow; do
    if ! grep -q "<key>${entitlement}</key>" "$SIGNED_ENTITLEMENTS"; then
        echo "Signed app is missing entitlement: ${entitlement}" >&2
        exit 1
    fi
done

if grep -q '<key>dynamic-codesigning</key>' "$SIGNED_ENTITLEMENTS"; then
    echo "Signed app contains TrollStore-incompatible entitlement: dynamic-codesigning" >&2
    exit 1
fi

rm -f "$IPA_OUTPUT"
(
    cd "$STAGE_ROOT"
    zip -qry "$IPA_OUTPUT" Payload
)

echo "Created ${IPA_OUTPUT}"
shasum -a 256 "$IPA_OUTPUT"
