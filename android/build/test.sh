#!/bin/sh
#
# test.sh --
#
#	Run the tests of Tk on an Android emulator ("make test-android" in a
#	Unix build directory):
#
#	    android/build/test.sh ?tcltest option value ...?
#
#	The APK of the tests (tk.wish.test) is built with build.sh, with the
#	options as TESTFLAGS, installed and started; its output is printed
#	when it exits. It runs on the device of ANDROID_SERIAL, or else on the
#	first emulator adb knows, or else on an emulator started (without a
#	window) for the tests with the AVD in TK_ANDROID_AVD, or else the only
#	AVD there is, and stopped after. A device which is not an emulator is
#	only used if ANDROID_SERIAL names it. ANDROID_ABI is x86_64 by
#	default; TK_ANDROID_TIMEOUT is the time the tests may take, in seconds
#	(3600 by default).
#
# Copyright © 2026 The Tk developers.
#
# See the file "license.terms" for information on usage and redistribution of
# this file, and for a DISCLAIMER OF ALL WARRANTIES.

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
ANDROID_HOME=${ANDROID_HOME:-$top/android-sdk}
ANDROID_ABI=${ANDROID_ABI:-x86_64}
export ANDROID_ABI
adb=$ANDROID_HOME/platform-tools/adb
emulator=$ANDROID_HOME/emulator/emulator
apk=$top/build/android-$ANDROID_ABI/tktest.apk
package=tk.wish.test
timeout=${TK_ANDROID_TIMEOUT:-3600}

# Build the APK.

echo "building $apk"
"$here/build.sh" test-apk "TESTFLAGS=$*" > "$top/build/android-test.log" 2>&1 || {
    tail -20 "$top/build/android-test.log"
    echo "$0: the build failed (see $top/build/android-test.log)" >&2
    exit 1
}

# Find a device, or start an emulator.

started=
if [ -z "$ANDROID_SERIAL" ]; then
    ANDROID_SERIAL=$("$adb" devices | awk '$1 ~ /^emulator-/ && $2 == "device" {print $1; exit}')
fi
if [ -z "$ANDROID_SERIAL" ]; then
    avd=$TK_ANDROID_AVD
    if [ -z "$avd" ]; then
	avd=$("$emulator" -list-avds 2>/dev/null)
	if [ $(echo "$avd" | grep -c .) -ne 1 ]; then
	    echo "$0: no emulator is running; set TK_ANDROID_AVD to one of:" $avd >&2
	    exit 1
	fi
    fi
    port=5600
    ANDROID_SERIAL=emulator-$port
    echo "starting the emulator $avd ($ANDROID_SERIAL)"
    "$emulator" -avd "$avd" -port $port -no-window -no-audio -no-boot-anim \
	-no-snapshot-save > "$top/build/android-emulator.log" 2>&1 &
    started=$!
    trap '"$adb" -s $ANDROID_SERIAL emu kill > /dev/null 2>&1; wait $started' EXIT
    trap 'exit 1' INT TERM
    "$adb" -s $ANDROID_SERIAL wait-for-device
    n=0
    until [ "$("$adb" -s $ANDROID_SERIAL shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do
	n=$((n + 1))
	if [ $n -gt 180 ] || ! kill -0 $started 2>/dev/null; then
	    tail -20 "$top/build/android-emulator.log"
	    echo "$0: the emulator did not start" >&2
	    exit 1
	fi
	sleep 1
    done
fi
export ANDROID_SERIAL

# Run the tests: wait until the application has written tktest.done or the
# summary of tcltest, or dies, or the time is up.

"$adb" install -r "$apk" > /dev/null || exit 1
"$adb" shell am force-stop $package
"$adb" shell run-as $package rm -f files/tktest.done
"$adb" logcat -c
"$adb" shell am start -W -n $package/tk.android.TkActivity > /dev/null
t=0
status=1
while [ $t -lt $timeout ]; do
    sleep 5
    t=$((t + 5))
    if "$adb" shell run-as $package ls files/tktest.done > /dev/null 2>&1 \
	    || "$adb" shell run-as $package cat files/tktest.log 2>/dev/null \
	    | grep -q '^Sourced [0-9]* Test Files'; then
	status=0
	break
    fi
    if [ -z "$("$adb" shell pidof $package)" ]; then
	if "$adb" shell run-as $package ls files/tktest.done > /dev/null 2>&1; then
	    status=0
	else
	    echo "$0: the application died" >&2
	fi
	break
    fi
done
if [ $t -ge $timeout ]; then
    echo "$0: the tests did not end in $timeout seconds" >&2
fi
"$adb" shell run-as $package cat files/tktest.log files/tktest.err
if [ $status -ne 0 ]; then
    "$adb" logcat -d -s Tk DEBUG libc AndroidRuntime | tail -40
fi
"$adb" shell am force-stop $package
exit $status
