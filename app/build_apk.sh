#!/bin/bash
# Build FusionControl.apk without Gradle: aapt2 + javac + d8 + zipalign + apksigner
#
# Env overrides:
#   ANDROID_SDK   Android SDK root (default: $HOME/android-sdk)
#   BUILD_TOOLS   build-tools dir (default: $ANDROID_SDK/build-tools/34.0.0)
#   JAVA_HOME     JDK 17+ to prepend to PATH (optional if java/javac/keytool
#                 are already on PATH)
#   KEYSTORE      keystore path (default: ./debug.keystore, auto-generated)
#
# Usage:  ANDROID_SDK=/path/to/sdk JAVA_HOME=/path/to/jdk ./build_apk.sh
set -e
APP="$(cd "$(dirname "$0")" && pwd)"
SDK="${ANDROID_SDK:-$HOME/android-sdk}"
BT="${BUILD_TOOLS:-$SDK/build-tools/34.0.0}"
AJ="$SDK/platforms/android-34/android.jar"
OUT="$APP/out"
KS="${KEYSTORE:-$APP/debug.keystore}"
[ -n "$JAVA_HOME" ] && export PATH="$JAVA_HOME/bin:$PATH"

rm -rf "$OUT"; mkdir -p "$OUT"

echo "== aapt2 compile/link"
"$BT/aapt2" compile --dir "$APP/res" -o "$OUT/res.zip"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$AJ" \
    --manifest "$APP/AndroidManifest.xml" \
    --min-sdk-version 24 --target-sdk-version 34 \
    "$OUT/res.zip"

echo "== javac"
mkdir -p "$OUT/classes"
find "$APP/java" -name '*.java' > "$OUT/sources.txt"
javac -classpath "$AJ" -source 8 -target 8 -d "$OUT/classes" @"$OUT/sources.txt" 2>&1 | grep -v "^warning: \[bootstrap\]" || true

echo "== d8"
"$BT/d8" --release --min-api 24 --output "$OUT" $(find "$OUT/classes" -name '*.class')

echo "== package dex"
(cd "$OUT" && zip -q -j base.apk classes.dex)

echo "== align + sign"
if [ ! -f "$KS" ]; then
  keytool -genkeypair -keystore "$KS" -alias fusion \
    -storepass fusion123 -keypass fusion123 -keyalg RSA -keysize 2048 \
    -validity 10000 -dname "CN=Fusion Control" >/dev/null 2>&1
fi
"$BT/zipalign" -f 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:fusion123 \
    --out "$APP/FusionControl.apk" "$OUT/aligned.apk"
echo "built $APP/FusionControl.apk"