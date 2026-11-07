#!/bin/sh
# A battery of tests on an Android device of your own (USB or wireless debugging): the engine's
# tests run on it, then the gallery is installed and driven by adb (taps, typing, selecting text by
# touch, Back, the app's life, start time, scrolling's frame rate, memory over a while), with a
# screenshot of each step and a PASS / FAIL / NOTE line for what can be checked from here.
#
#   ANDROID_SERIAL=192.168.3.42:35159 tools/androidDeviceTest.sh
#
# It changes none of the device's settings (theme, text size, rotation and screen readers are left
# to you) and sends no random input. Build the APK first (examples/android/buildApk.sh); the engine's
# tests are built here for the device's ABI when ANDROID_NDK_HOME is set (CMake and Ninja). Taps are
# placed in CSS pixels of the gallery's layout on a phone about 384 to 412 CSS pixels wide (most
# phones held upright); on other screens the screenshots show what they hit.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
command -v cygpath >/dev/null 2>&1 && root=$(cygpath -m "$root") # (Git for Windows: a path adb.exe reads)
out="$root/build-android/device-test"
apk="$root/build-android/cascade-gallery.apk"
pkg=org.cascade.gallery
activity=$pkg/org.cascade.CascadeActivity
export MSYS_NO_PATHCONV=1 # (Git for Windows: device paths stay as they are)
adb=${ADB:-adb}
mkdir -p "$out"
rm -f "$out"/*.png
passes=0 failures=0

pass() { passes=$((passes + 1)); echo "PASS $*"; }
fail() { failures=$((failures + 1)); echo "FAIL $*"; }
note() { echo "NOTE $*"; }
sh_() { $adb shell "$@"; }
shot() { $adb exec-out screencap -p > "$out/$1.png"; }
pid() { sh_ pidof $pkg | tr -d '\r'; }
crashes() { $adb logcat -d -b crash,main 2>/dev/null | grep -cE "FATAL EXCEPTION.*|Fatal signal|ANR in $pkg|>>> $pkg <<<"; }

# CSS pixels of the gallery to the device's
density=$(sh_ wm density | tr -d '\r' | sed -n 's/.*density: *\([0-9]*\).*/\1/p' | tail -n 1)
px() { echo "$1 $density" | awk '{printf "%d", $1 * $2 / 160}'; }
tap() { sh_ input tap "$(px "$1")" "$(px "$2")"; }
down() { sh_ input motionevent DOWN "$(px "$1")" "$(px "$2")"; }
move() { sh_ input motionevent MOVE "$(px "$1")" "$(px "$2")"; }
up() { sh_ input motionevent UP "$(px "$1")" "$(px "$2")"; }
start() { sh_ am start -W -n $activity 2>/dev/null | tr -d '\r' | sed -n 's/^TotalTime: *//p'; }

echo "== the device"
model=$(sh_ getprop ro.product.model | tr -d '\r')
release=$(sh_ getprop ro.build.version.release | tr -d '\r')
abi=$(sh_ getprop ro.product.cpu.abi | tr -d '\r')
size=$(sh_ wm size | tr -d '\r' | sed -n 's/.*size: *//p' | tail -n 1)
note "$model, Android $release, $abi, $size, density $density"
[ -n "$density" ] || { fail "no device (adb devices; ANDROID_SERIAL)"; exit 1; }

echo "== the engine's tests, on the device"
dir=/data/local/tmp/cascade
# built here for the device's ABI (build-android/tests-<abi>, again only what changed) when the NDK is
# there; otherwise the CI's build-android/tests, as it is
tests="$root/build-android/tests"
if [ -n "${ANDROID_NDK_HOME:-}" ] && command -v cmake >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1; then
  tests="$root/build-android/tests-$abi"
  ndk="$ANDROID_NDK_HOME"
  command -v cygpath >/dev/null 2>&1 && ndk=$(cygpath -m "$ndk")
  if [ ! -f "$tests/CMakeCache.txt" ]; then
    cmake -S "$root" -B "$tests" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$abi" \
      -DANDROID_PLATFORM=android-26 -DCASCADE_BUILD_TESTS=ON -DCASCADE_BUILD_EXAMPLES=OFF -DCASCADE_WINDOW=OFF >/dev/null 2>&1
  fi
  cmake --build "$tests" >/dev/null 2>&1 || fail "building the engine's tests for $abi (cmake --build $tests)"
elif [ -f "$tests/cascade-tests" ] && [ -n "$(find "$root/src" "$root/tests" -newer "$tests/cascade-tests" -name '*.[ch]' | head -n 1)" ]; then
  note "build-android/tests is older than the sources (set ANDROID_NDK_HOME to build them here)"
fi
if [ -x "$tests/cascade-tests" ] || [ -f "$tests/cascade-tests" ]; then
  sh_ "rm -rf $dir && mkdir -p $dir" && $adb push "$tests/cascade-tests" "$tests/cascade-capi-tests" $dir/ >/dev/null
  for t in cascade-tests cascade-capi-tests; do
    r=$(sh_ "cd $dir && chmod 755 $t && ./$t 2>&1 | tail -n 1" | tr -d '\r')
    case "$r" in *" 0 failures") pass "$t: $r" ;; *) fail "$t: $r" ;; esac
  done
else
  note "no engine tests built (ANDROID_NDK_HOME, or build-android/tests): they were not run"
fi

echo "== installing and starting"
$adb install -r "$apk" >/dev/null 2>&1 && pass "installed" || { fail "install"; exit 1; }
$adb logcat -c
times=""
for i in 1 2 3; do
  sh_ am force-stop $pkg
  times="$times $(start)"
  sleep 1
done
note "cold start, ms:$times (am start -W, TotalTime)"
sleep 2
[ -n "$(pid)" ] && pass "running" || fail "not running after it started"
presenter=$($adb logcat -d -s cascade:V | sed -n 's/.*presenting with //p' | tail -n 1 | tr -d '\r')
case "$presenter" in
  Vulkan*) pass "presents with $presenter" ;;
  "") fail "the log does not say which presenter" ;;
  *) note "presents with $presenter (the software path: no Vulkan here)"; $adb logcat -d -s cascade:V | grep -i vulkan ;;
esac
$adb logcat -d -s cascade:V | grep -q "cannot read the page" && fail "the page was not found in the APK" || pass "the page loaded"
shot 01-start
[ "$(wc -c < "$out/01-start.png")" -gt 60000 ] && pass "it drew the page" || fail "the screen looks blank (01-start.png)"

echo "== taps and pages"
for i in 1 2 3; do tap 88 192; sleep 0.3; done
shot 02-clicked-3-times
tap 192 91; sleep 0.8; shot 03-layout-tab
tap 310 91; sleep 0.8; shot 04-canvas-tab
tap 73 91; sleep 0.8; shot 05-controls-tab
note "02-05: the button says 3 clicks; the three pages each drew"
# the Opacity slider dragged along (a little up and down too, as a thumb does): it follows, the page stays
sh_ input swipe "$(px 260)" "$(px 366)" "$(px 140)" "$(px 372)" 500
sleep 0.6
shot 05b-slider-dragged
note "05b: the Opacity slider moved left by the drag (about 40%), the page did not scroll"

echo "== scrolling: frames while flinging"
# (SurfaceFlinger's frame statistics, gathered only while flinging, then turned off again)
sh_ dumpsys SurfaceFlinger --timestats -enable -clear >/dev/null 2>&1
for i in 1 2 3 4; do
  sh_ input swipe "$(px 200)" "$(px 650)" "$(px 200)" "$(px 200)" 120
  sh_ input swipe "$(px 200)" "$(px 200)" "$(px 200)" "$(px 650)" 120
done
sh_ dumpsys SurfaceFlinger --timestats -dump | tr -d '\r' > "$out/timestats.txt"
sh_ dumpsys SurfaceFlinger --timestats -disable >/dev/null 2>&1
r=$(awk -v act="$activity" '
  /^displayRefreshRate/ { hz = $3 }
  /^layerName/ { mine = index($0, "SurfaceView[" act "]") > 0 && index($0, "(BLAST)") > 0 }
  mine && /^totalFrames/ { total = $3 } mine && /^droppedFrames/ { dropped = $3 } mine && /^jankyFrames/ { janky = $3 }
  mine && /^present2present histogram/ { getline; n = split($0, b, " "); for (i = 1; i <= n; ++i) { split(b[i], kv, "ms="); if (kv[1] + 0 <= 1000 / hz + 1) fast += kv[2] } mine = 0 }
  END {
    if (!total) { print "NOTE no frame statistics for the page (timestats)"; exit }
    printf "NOTE %d frames, %d at the screen'"'"'s full rate (%d Hz), %d dropped, %d janky\n", total, fast, hz, dropped, janky
    print (dropped + janky > total / 20) ? "FAIL" : "PASS"
  }' "$out/timestats.txt")
echo "$r" | grep "^NOTE"
case "$r" in *PASS*) pass "scrolling keeps up with the screen" ;; *FAIL*) fail "more than one frame in twenty dropped or janky while scrolling" ;; esac
shot 06-scrolled

echo "== memory and CPU"
mem() { sh_ dumpsys meminfo $pkg | tr -d '\r' | awk '/TOTAL PSS:/ { print $3; exit } /^ *TOTAL / { print $2; exit }'; }
# (the process's CPU time over 10 s, from /proc: clock ticks of 10 ms, so ticks / 10 s = percent of one core)
cpu() {
  p=$(pid)
  sh_ "a=\$(cut -d' ' -f14,15 /proc/$p/stat); sleep 10; b=\$(cut -d' ' -f14,15 /proc/$p/stat); echo \$a \$b" | tr -d '\r' |
    awk '{ printf "%.1f", (($3 + $4) - ($1 + $2)) / 10 }'
}
m0=$(mem)
note "memory now: ${m0:-?} KB (PSS); CPU over 10 s with the gallery's animation running: $(cpu)% of one core"

echo "== typing"
tap 300 438; sleep 1.2
sh_ input keyevent KEYCODE_MOVE_END
sh_ input text "_ok"
sleep 0.6
shot 07-typed
note "07: the Name field ends with _ok, the keyboard is up and the page sits above it"

echo "== selecting text by touch"
down 128 438; sleep 1.0; up 128 438; sleep 0.8
shot 08-long-press
note "08: a word selected, two handles under it, the system's floating menu (Cut, Copy, Paste, Select all)"
# the end handle (below the line's foot, right of the word) dragged to the field's end, the magnifier while held
down 152 452; move 175 452; move 230 452; move 330 452; sleep 0.4
shot 09-magnifier
up 330 452; sleep 0.8
shot 10-dragged
note "09: the magnifier over the dragged handle (Android 9 and later); 10: the selection to the end, the menu again"

echo "== Back"
# (the keyboard takes the first Back; then the handles go, then the field's focus, then the app)
sh_ input keyevent BACK; sleep 0.8; shot 11-back-keyboard
sh_ input keyevent BACK; sleep 0.6; shot 12-back-handles
sh_ input keyevent BACK; sleep 0.6; shot 13-back-focus
note "11: the keyboard gone; 12: the handles and menu gone, the selection kept; 13: the field without the focus"
sh_ input keyevent BACK; sleep 1.5
# (the activity ends; Android keeps the process cached, so it is the screen on top that tells)
ontop() { sh_ dumpsys activity activities | tr -d '\r' | grep -m 1 topResumedActivity | grep -c "$pkg"; }
[ "$(ontop)" = 0 ] && pass "a fourth Back ended the app" || fail "still on top after the fourth Back: see 13-back-focus.png"

echo "== the app's life"
start >/dev/null; sleep 1.5
p1=$(pid)
sh_ input keyevent HOME; sleep 2
[ -n "$(pid)" ] && [ "$(pid)" = "$p1" ] && pass "in the background it stays" || fail "it ended in the background"
sh_ am send-trim-memory "$p1" RUNNING_CRITICAL >/dev/null 2>&1; sleep 1
warm=$(start); sleep 1.5
[ "$(pid)" = "$p1" ] && pass "came back (warm start $warm ms) after a memory warning" || fail "did not come back as it was"
shot 14-back-from-home
sh_ input keyevent HOME; sleep 1.5
sh_ am kill $pkg; sleep 1
[ -z "$(pid)" ] && pass "the system may end it in the background (am kill)" || note "am kill left it running"
cold=$(start); sleep 2
[ -n "$(pid)" ] && pass "started again after that ($cold ms)" || fail "did not start again"
shot 15-after-kill
# the surface coming and going quickly: start, home, start, ...
for i in 1 2 3 4 5; do sh_ am start -n $activity >/dev/null 2>&1; sleep 0.3; sh_ input keyevent HOME; sleep 0.3; done
start >/dev/null; sleep 1.5
[ -n "$(pid)" ] && pass "five quick trips to the background and back" || fail "it ended during the quick trips"
shot 16-after-trips

echo "== a while running (memory)"
m1=$(mem)
sleep 30
m2=$(mem)
note "memory: ${m1:-?} KB, then ${m2:-?} KB after 30 s of animation"
if [ -n "$m1" ] && [ -n "$m2" ] && [ "$m2" -gt $((m1 + m1 / 5)) ]; then fail "memory grew by more than a fifth in 30 s"; else pass "memory steady over 30 s"; fi

echo "== crashes"
c=$(crashes)
[ "$c" = 0 ] && pass "no crash, ANR or fatal signal in the log" || { fail "$c crash lines in the log"; $adb logcat -d -b crash | tail -n 40; }
$adb logcat -d > "$out/logcat.txt"

echo "== left to you (the device's settings, the other apps)"
note "the theme, text size and rotation: the page follows each (prefers-color-scheme, font size, a landscape layout)"
note "TalkBack: each control is read by name; double tap activates; the slider is set with volume keys"
note "drag text or a picture from another app in split screen onto the page; copy a picture and paste it"
echo "screenshots and the log: $out"
echo "$passes passed, $failures failed"
[ "$failures" = 0 ]
