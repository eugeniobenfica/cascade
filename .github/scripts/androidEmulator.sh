#!/bin/sh
# Runs on the CI's Android emulator (build.yml): cascade-tests and cascade-capi-tests as programs
# from the shell, then the gallery's APK, kept running long enough to draw, with a screenshot and
# the log of what it said.
set -e
mkdir -p shots
# (the emulator's own apps can be slow to answer on CI: their not-responding dialogs stay hidden)
adb shell settings put global hide_error_dialogs 1 || true
dir=/data/local/tmp/cascade
adb shell "rm -rf $dir && mkdir -p $dir"
adb push build-android/tests/cascade-tests build-android/tests/cascade-capi-tests $dir/ >/dev/null
adb shell "cd $dir && chmod 755 cascade-tests cascade-capi-tests && ./cascade-tests && ./cascade-capi-tests"

adb install -r build-android/cascade-gallery.apk
adb logcat -c
adb shell monkey -p org.cascade.gallery -c android.intent.category.LAUNCHER 1 >/dev/null
sleep 10
adb shell am broadcast -a android.intent.action.CLOSE_SYSTEM_DIALOGS >/dev/null 2>&1 || true
sleep 1
adb exec-out screencap -p > shots/android-gallery.png
adb logcat -d > shots/android-logcat.txt || true
adb logcat -d -s cascade:V AndroidRuntime:E DEBUG:V ActivityTaskManager:I > shots/android-log.txt || true
cat shots/android-log.txt
# still running (no crash), and it drew something: the gallery's dark interface has many colors
adb shell pidof org.cascade.gallery
test "$(stat -c %s shots/android-gallery.png)" -gt 20000
