# The X server emulation

This directory holds an X server emulated inside the process, on which the
ports of Tk to systems without X run the X11 code of Tk: the Wayland port
(`../wayland`) and the Android port (`../android`). Like the Aqua and Windows
ports, they compile against the X11 headers in `../xlib` and implement the
part of Xlib which Tk uses (`tkIntXlibDecls.h`); unlike them, they emulate it
far enough that `tkUnixWm.c`, `tkUnixMenu.c`, `tkUnixEmbed.c` (within the
process), `tkUnixSend.c` (within the process), the widgets and the library
run as on X11. Both ports are configured with `TK_USE_XEMU` defined.

## Design

- `tkXemuWindow.c`: the window tree. Every X window is emulated; its visible
  region is computed as the X server computes it (clipped by its ancestors,
  minus its children and the siblings above it), and Expose events are
  generated, after painting the background, for the parts of windows which
  become visible. A window which moves keeps its contents, which are copied
  in the backing image, as the X server does, when it moves by a whole
  number of pixels of the image (`TkXeMoveStep` gives the distances); at
  other distances, at fractional scales, all of it is exposed and redrawn.
  The children of the root window (the wrappers of Tk's toplevels, and
  override-redirect windows) have a surface, with a backing image.
- `tkXemuDraw.c`: GCs, drawing, pixmaps and images, with Cairo, following the
  rules of X11 for which pixels are drawn (lines through the centers of
  pixels, end points included, no antialiasing). Photo images are composited
  with their alpha channel. Backing images and pixmaps have the scale of the
  screen, which may be fractional.
- `tkXemuHints.c`: the Xlib functions with which Tk gives hints to the window
  manager. As in Xlib, they only set properties (`WM_NAME`, `WM_NORMAL_HINTS`,
  `WM_CLASS`, `WM_TRANSIENT_FOR`, `_NET_WM_STATE`, ...), which the window
  manager of the port reads.
- `tkXemuFont.c`: fonts, with fontconfig and Cairo, structured like the Xft
  implementation of the X11 port. With HarfBuzz, `tkXemuXft.c` emulates the
  part of Xft which the bidi font code of the X11 port uses instead, with
  `X11/Xft/Xft.h`.
- `tkXemuXlib.c`: the display, atoms, properties, regions (pixman), colors
  (24-bit TrueColor) and keysyms.
- `tkXemuMisc.c`: the insertion cursor, for input methods and the on-screen
  keyboard, and event times.

Logical pixels have 96 dpi; higher resolutions are handled with the scale of
the surfaces. Pointer events go through `Tk_UpdatePointer`
(`../generic/tkPointer.c`, shared with Aqua and Windows).

## Ports

A port shows the backing images of the surfaces, gives Tk its input as X
events, and is the window manager. `tkXemuInt.h` lists what it provides: the
structures of the surfaces and of the display, with the members the
emulation uses, the functions of surfaces (`TkXeNewSurface`,
`TkXeResizeBacking`, `TkXeAddDamage`, ...), of input (`TkXeCurrentTime`,
`TkXeUpdateCursor`) and of the window manager (`TkXeWmMapSurface`,
`TkXeWmPropertyChanged`, ...), and the part of xkbcommon which the Xlib
emulation uses. Its header (`tkWaylandInt.h`, `tkAndroidInt.h`) is included
at the end of `tkXemuInt.h`.
