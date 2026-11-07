#!/bin/sh
# Runs on the CI's iOS simulator (build.yml): cascade-tests and cascade-capi-tests inside the
# simulator, then the gallery's app, kept running long enough to draw, with a screenshot.
set -e
mkdir -p shots
# the first iPhone the installed Xcode has
udid=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
devices = json.load(sys.stdin)["devices"]
for runtime in sorted(devices, reverse=True):
    if "iOS" not in runtime: continue
    for d in devices[runtime]:
        if d["name"].startswith("iPhone"):
            print(d["udid"]); sys.exit(0)
sys.exit(1)')
xcrun simctl boot "$udid"
xcrun simctl bootstatus "$udid" -b

work=$(mktemp -d)
here=$(pwd)
(cd "$work" && xcrun simctl spawn "$udid" "$here/build-ios/cascade-tests.app/cascade-tests" &&
  xcrun simctl spawn "$udid" "$here/build-ios/cascade-capi-tests.app/cascade-capi-tests")

xcrun simctl install "$udid" build-ios/cascade-gallery.app
xcrun simctl launch "$udid" org.cascade.gallery
sleep 10
xcrun simctl io "$udid" screenshot shots/ios-gallery.png
# still running (no crash), and it drew something
xcrun simctl spawn "$udid" launchctl list | grep -q org.cascade.gallery
test "$(stat -f %z shots/ios-gallery.png)" -gt 20000
xcrun simctl shutdown "$udid"
