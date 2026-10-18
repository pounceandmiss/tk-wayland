# Tk on Wayland

This directory holds a Wayland port of Tk which runs the X11 code of Tk on an
X server emulated inside the process. It is built with

    ./configure --enable-wayland

and needs the development files of wayland-client, wayland-cursor,
wayland-protocols, xkbcommon, cairo, pixman, fontconfig, freetype and
libdecor. No X11 library or header is used: like the Aqua and Windows ports,
the port compiles against the X11 headers in `../xlib` and implements the part
of Xlib which Tk uses (`tkIntXlibDecls.h`).

## Design

Tk's generic code and most of the X11 code (`../unix`) draw and manage
windows through Xlib. The port provides an Xlib which behaves like an X
server and an ICCCM/EWMH window manager, on top of Wayland:

- `tkWaylandWindow.c`: the window tree. Every X window is emulated; its
  visible region is computed as the X server computes it (clipped by its
  ancestors, minus its children and the siblings above it), and Expose
  events are generated, after painting the background, for the parts of
  windows which become visible. The children of the root window (the
  wrappers of Tk's toplevels, and override-redirect windows) have a
  `wl_surface` and a backing image. The damaged parts of the backing image
  are copied to `wl_shm` buffers and committed at idle time, throttled with
  frame callbacks.
- `tkWaylandDraw.c`: GCs, drawing, pixmaps and images, with Cairo, following
  the rules of X11 for which pixels are drawn (lines through the centers of
  pixels, end points included, no antialiasing). Photo images are composited
  with their alpha channel.
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
- `tkWaylandFont.c`: fonts, with fontconfig and Cairo, structured like the
  Xft implementation of the X11 port.
- `tkWaylandXlib.c`: the display, atoms, properties, regions (pixman),
  colors (24-bit TrueColor) and keysyms.
- `tkWaylandCursor.c`: cursors from the cursor theme.

Since window ids and coordinates behave as with X, the rest of Tk needs
almost no changes: `tkUnixWm.c`, `tkUnixMenu.c`, `tkUnixEmbed.c` (within the
process), `tkUnixSend.c` (within the process), the widgets and the library
run as on X11. `[tk windowingsystem]` is `wayland`; the library treats it like
`x11`, except where it queries the X server.

Wayland clients do not know where their windows are on the screen. Root
coordinates are those Tk believes the windows have, which keeps them
consistent within the application. Logical pixels have 96 dpi, and higher
resolutions are handled with the buffer scale.

## Not done yet

- The clipboard and the selection with other applications
  (`wl_data_device`, primary selection); drag and drop.
- Input methods (`text-input-v3`).
- Fractional scaling (`wp_fractional_scale_v1`, `wp_viewporter`).
- Cursors made of bitmaps; cursor-shape-v1.
- The system tray (StatusNotifierItem), `xdg-activation`, window icons
  (`xdg-toplevel-icon`).
- `send` between processes.
