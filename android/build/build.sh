#!/bin/sh
# Run the Makefile of the Android build in the tk-android-build image, with
# the directory above the Tk sources (which holds the Tcl sources, the build
# directories and the SDK) mounted at the same path.
#
#	android/build/build.sh [make arguments]

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)

exec docker run --rm -i -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -e ANDROID_ABI -e APP_DIR \
    -v "$top:$top" -w "$here" tk-android-build \
    make -f "$here/Makefile" "$@"
