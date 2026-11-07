#!/bin/sh
# Builds the gallery as an Android APK with the NDK and the SDK's command-line tools: no Gradle. The
# library is built by CMake with the NDK's toolchain; cascade's activity (src/platform/android/java)
# is compiled by javac against the platform's android.jar and turned into classes.dex by d8; aapt2
# makes the APK from the manifest and the interface folder (as assets); the library and the dex go
# in beside them (stored, so the system maps the library straight from the APK), then zipalign (16
# KB pages) and apksigner (signed with the debug key, made the first time).
#
#   ANDROID_HOME=~/Android/Sdk ANDROID_NDK_HOME=~/Android/Sdk/ndk/27.2.12479018 examples/android/buildApk.sh
#   adb install -r build-android/cascade-gallery.apk
#
# ABI (arm64-v8a; x86_64 for the emulator; several, separated by spaces, go into one APK), API (the
# lowest Android version, 26) and PLATFORM (the android.jar to build against, the newest installed)
# can be set too. javac, keytool and the SDK's tools need a JDK.
set -e

: "${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK}"
ABI=${ABI:-arm64-v8a}
API=${API:-26}

root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/build-android"
tools=$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -n 1)
PLATFORM=${PLATFORM:-$(ls -d "$ANDROID_HOME"/platforms/android-* | sed 's/.*android-//' | sort -n | tail -n 1)}
jar="$ANDROID_HOME/platforms/android-$PLATFORM/android.jar"
apksigner="$tools/apksigner"
d8="$tools/d8"
[ -f "$apksigner.bat" ] && apksigner="$apksigner.bat" # (Windows)
[ -f "$d8.bat" ] && d8="$d8.bat"

stage="$out/stage"
rm -rf "$stage" && mkdir -p "$stage/assets"

# the library, for each ABI
for abi in $ABI; do
  cmake -S "$root" -B "$out/$abi" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$API" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$out/$abi" --target cascade-gallery
  mkdir -p "$stage/lib/$abi"
  cp "$out/$abi/libcascade-gallery.so" "$stage/lib/$abi/"
done

# cascade's activity: Java 8 bytecode against android.jar, then dex
rm -rf "$out/classes" && mkdir -p "$out/classes"
javac -source 8 -target 8 -nowarn -Xlint:-options -bootclasspath "$jar" -classpath "$jar" -d "$out/classes" \
  $(find "$root/src/platform/android/java" -name '*.java')
"$d8" --release --min-api "$API" --lib "$jar" --output "$stage" $(find "$out/classes" -name '*.class')

# the APK: manifest and assets, then the dex and the libraries (stored, not compressed)
cp -R "$root/examples/gallery/ui" "$stage/assets/ui"
# (the assets go in with the dex, not by aapt2's -A: on Windows it names them with backslashes,
# which Android does not find)
"$tools/aapt2" link -o "$out/unsigned.apk" --manifest "$root/examples/android/AndroidManifest.xml" -I "$jar"
if command -v zip >/dev/null 2>&1; then
  (cd "$stage" && zip -q -r "$out/unsigned.apk" classes.dex assets && zip -q -0 -r "$out/unsigned.apk" lib)
else
  # (no zip, as in Git for Windows: the JDK's jar adds them, without a manifest of its own)
  jar -u -M -f "$out/unsigned.apk" -C "$stage" classes.dex -C "$stage" assets
  jar -u -0 -M -f "$out/unsigned.apk" -C "$stage" lib
fi
"$tools/zipalign" -f -P 16 4 "$out/unsigned.apk" "$out/aligned.apk" 2>/dev/null || "$tools/zipalign" -f -p 4 "$out/unsigned.apk" "$out/aligned.apk"

# signed with the debug key
keystore="$HOME/.android/debug.keystore"
if [ ! -f "$keystore" ]; then
  mkdir -p "$HOME/.android"
  keytool -genkeypair -keystore "$keystore" -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
fi
"$apksigner" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android \
  --out "$out/cascade-gallery.apk" "$out/aligned.apk"
echo "built $out/cascade-gallery.apk"
