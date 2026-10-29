# Tk on Android

This directory holds an Android port of Tk. It runs on the X server emulation
of `../xemu`, shared with the Wayland port: the window tree, drawing with
Cairo, fonts with fontconfig, properties and the Xlib functions Tk uses. It is
built with

    ./configure --enable-android

which defines `TK_USE_ANDROID` and `TK_USE_XEMU`, and needs Cairo,
pixman, fontconfig and FreeType built for Android. `build/` builds all of it
with the NDK (see below).

## Design

An activity has one window, an `ANativeWindow`, which is the screen of the
emulated display. What differs from Wayland is where the images Tk draws are
shown, and where input comes from:

- `tkAndroidSurface.c`: the children of the root window (the wrappers of the
  toplevels, menus and other override-redirect windows) have a backing
  image, as on Wayland. The parts of the screen which change are recorded as
  damage, and composited at idle time into the window of the activity, from
  the bottom of the stacking order to the top. Root coordinates are real, as
  with X11.
- `tkAndroidWm.c`: the window manager. `tkUnixWm.c` is used unchanged.
  Toplevels are maximized (they fill the part of the window which is not
  covered by the system bars and the soft keyboard), except dialogs
  (transient windows, and windows which cannot be resized), which are
  centered. Override-redirect windows stay where Tk puts them. The back
  button dismisses the menu which is posted (with Escape), or else closes the
  top-most toplevel with `WM_DELETE_WINDOW`.
- `tkAndroidEvent.c`: the display, and the events of the activity. The
  callbacks of the activity run in the main thread of the application, and
  Tcl in a thread of its own: the callbacks post events, which are queued and
  read by Tk through a pipe. Touches go to the gesture recognizer of
  `../generic/tkTouch.c`, shared with Wayland: a tap is a click, dragging
  scrolls (and flings), a long press is a click of the third button, or a
  drag with the first if the finger moves, and two fingers scroll and pinch
  (see `tk touch` in `../doc/tk.n`). The soft keyboard follows the focus,
  as decided by `../generic/tkKeyboard.c` (see `tk keyboard` in
  `../doc/tk.n`), with the type of the text as input type.
- `tkAndroidKey.c`: keysyms of the keys of Android, and the names of keysyms
  (from `../generic/ks_names.h`; there is no xkbcommon).
- `tkAndroidSelect.c`: the CLIPBOARD is shared with other applications
  through the `ClipboardManager` of Android, with JNI.
- `tkAndroidMain.c`: the entry point of the library the activity loads
  (`libmain.so`), which runs Tcl and Tk, and the callbacks of the activity.
  The scripts (the libraries of Tcl and Tk, and the application) are in a zip
  linked into the library, mounted at `//zipfs:/app`. The application is
  `app/main.tcl`, or else `app/widget`.
- `java/tk/android/TkActivity.java`: the activity, a `NativeActivity` with
  what cannot be done in native code: an invisible view which the soft
  keyboard edits, and whose `InputConnection` gives the text to Tk; it also
  tells Tk when the keyboard is shown or hidden.

Logical pixels are density-independent pixels (1/160 inch); images are drawn
at the density of the screen. Fonts are those of the system, through
fontconfig, with aliases of the generic families to the fonts of Android.
`[tk windowingsystem]` is `android`; the library treats it like `x11` and
`wayland`.

## Touchscreens

Besides the gestures of `tkTouch.c`, `../library/touch.tcl` adapts the
widgets to fingers on Android: the widgets which take clicks get more
padding, and the rows of treeviews and the entries of menus are taller (ttk
styles of the theme in use, applied again when it changes, and defaults of
the option database for the classic widgets), while text keeps its size. A
long press in an entry, a text, a spinbox or a combobox selects the word
there, and shows the selection with two handles, which can be dragged to
move its ends, and a toolbar to cut, copy, paste and select all; where there
is no word, the toolbar offers to paste at the insertion cursor.
Applications on other touchscreens can apply the same with
`::tk::touch::Adapt`.

## Commands for Android

As `::tk::mac` on Aqua (`tkAndroidCmds.c`):

- `::tk::android::scale ?scale?`: pixels per logical pixel; 0 goes back to the
  density of the screen.

## Building

`build/Makefile` cross-builds the dependencies, Tcl (from `../../tcl`) and Tk
with the NDK, and makes an APK. It runs in the Docker image of
`build/Dockerfile` (based on the NDK image of zippy), through `build/build.sh`,
with the SDK in `../../android-sdk`:

    docker build -t tk-android-build android/build
    android/build/build.sh deps tcl tk apk

The APK is `../../build/android-x86_64/tk.apk` (`ANDROID_ABI=arm64-v8a` for
phones). `APP_DIR` is the directory of the application (default: the demos,
whose "Your own scripts" entry edits and runs scripts).
The standard output and error of Tcl go to `adb logcat -s Tk`.

## Tests

The tests of Tk run on an emulator with

    make test-android TESTFLAGS="-file touch.test"

in a Unix build directory, or `android/build/test.sh` with the options of
tcltest. It builds `../../build/android-x86_64/tktest.apk` (`make test-apk`
of `build/Makefile`): the package `tk.wish.test`, whose library has the test
commands of tktest and runs `build/runtests.tcl`, the tests in a single
process. The APK runs on the emulator adb knows, or on an emulator started
without a window for the tests (the AVD in `TK_ANDROID_AVD`, or the only
one); a phone is only used if `ANDROID_SERIAL` names it. The output of the
tests is read with `run-as` when they end.

## Not done yet

- The insets of the system bars: the activity opts out of drawing under them,
  which Android 16 no longer allows.
- Input methods which compose text show it as typed text, not as marked text.
