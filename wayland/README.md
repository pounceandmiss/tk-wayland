# Tk on Wayland

This directory holds a Wayland port of Tk which runs the X11 code of Tk on the
X server emulated inside the process of `../xemu`. It is built with

    ./configure --enable-wayland

and needs the development files of wayland-client, wayland-cursor,
wayland-protocols, xkbcommon, cairo, pixman, fontconfig, freetype and
libdecor. No X11 library or header is used: like the Aqua and Windows ports,
the port compiles against the X11 headers in `../xlib` and implements the part
of Xlib which Tk uses (`tkIntXlibDecls.h`).

## Design

Tk's generic code and most of the X11 code (`../unix`) draw and manage
windows through Xlib. `../xemu` provides an Xlib which behaves like an X
server: the window tree, drawing, fonts and properties (see its README). The
port shows what is drawn with Wayland, and is an ICCCM/EWMH window manager:

- `tkWaylandSurface.c`: the surfaces of the children of the root window (the
  wrappers of Tk's toplevels, and override-redirect windows), which have a
  `wl_surface`. The damaged parts of their backing images are copied to
  `wl_shm` buffers and committed at idle time, throttled with frame
  callbacks.
- `tkWaylandWm.c`: the window manager. `tkUnixWm.c` is used unchanged; the
  hints it puts in properties (`WM_NAME`, `WM_NORMAL_HINTS`, `WM_CLASS`,
  `WM_TRANSIENT_FOR`, `_NET_WM_STATE`, ...) are mapped to `xdg_toplevel`
  requests through libdecor, and the configure and close events of the
  compositor to `ConfigureNotify` and `WM_DELETE_WINDOW` events.
  Override-redirect windows (menus, combobox dropdowns, tooltips) are
  `xdg_popup`s of the surface which received the input that posted them,
  positioned where Tk put them; menus and dropdowns grab the input.
- `tkWaylandEvent.c`: the connection, outputs and seat. Pointer events go
  through `Tk_UpdatePointer` (`../generic/tkPointer.c`, shared with Aqua and
  Windows); keyboard events are translated with xkbcommon, with compose
  sequences, and key repeat is done by the client.
- `tkWaylandSelect.c`, `tkWaylandTextInput.c`: selections and input
  methods (below).
- `tkWaylandCursor.c`: cursors from the cursor theme.

`[tk windowingsystem]` is `wayland`; the library treats it like `x11`, except
where it queries the X server.

Wayland clients do not know where their windows are on the screen. Root
coordinates are those Tk believes the windows have, which keeps them
consistent within the application. Logical pixels have 96 dpi, and higher
resolutions are handled with the buffer scale.

## Also implemented

- The CLIPBOARD and PRIMARY selections with other clients
  (`wl_data_device`, primary selection protocol).
- HiDPI and fractional scaling (`wp_fractional_scale_v1`, `wp_viewporter`);
  pixmaps have the scale of the window they are made for.
  `TK_WAYLAND_SCALE=1.5` forces a scale, for testing.
- Activation (`xdg-activation-v1`) for `focus -force`, `raise`,
  deiconify, and the startup token of launchers.
- Input methods (`text-input-v3`), shown with the IME marked text virtual
  events used on Aqua. Text input is enabled while Tk wants the on-screen
  keyboard (`tk keyboard`, `../generic/tkKeyboard.c`), with the type of the
  text as content type; compositors show their on-screen keyboard then.

## Commands for Wayland

What Wayland has that X11 has not is in the namespace `::tk::wayland`, as
`::tk::mac` on Aqua (`tkWaylandCmds.c`):

- `appid window ?id?`: the app_id of a toplevel, which compositors match
  with the `.desktop` file of the application; by default its class.
- `decorations window ?server|none?`: whether the compositor (or libdecor)
  decorates the toplevel, or nothing does, for applications which draw their
  own title bar.
- `corners window ?radius?`: rounded corners for a toplevel without
  decorations (square while it is maximized, fullscreen or tiled).
- `startmove window`, `startresize window edge` (`n`, `se`, ...),
  `windowmenu window ?x y?`: interactive move and resize, and the window
  menu of the compositor, for such a title bar; called from a `ButtonPress`
  binding.
- `inhibitidle window ?boolean?`: keep the screen on while the toplevel is
  visible (idle-inhibit-unstable-v1).
- `activationtoken`: a token for a program started by the application, given
  to it in `XDG_ACTIVATION_TOKEN`.
- `scale window`: the scale of the surface of the toplevel.

`demos/headerbar.tcl` is a window with a title bar written in Tcl.

## Not done yet

- Drag and drop between applications.
- cursor-shape-v1.
- The system tray (StatusNotifierItem).
- `send` and embedding between processes (they work within a process).
- `tk inactive` (the idle time of the user).
