# Compositors and libdecor

What the compositors and libdecor do which the port has to live with: what
they do, the versions it was seen with, and whether the port works around it
or leaves it alone. The tests run on each with `make test-sway`,
`make test-gnome`, `make test-kde` and `make test-weston` (see
`../unix/waylandTest`).

## All compositors

- **The keyboard focus is the compositor's.** A client cannot take the
  keyboard; Tk moves the focus between its own windows while it has the
  keyboard, and otherwise asks for the activation of the toplevel (with
  xdg-activation), which the compositor may refuse. When a new window gets the
  keyboard differs between compositors (sway gives it to a window as it is
  shown; the headless sessions of the tests of Weston and GNOME have no
  keyboard at all). The tests of `focus.test` and the focus tests of
  `unixEmbed.test` assume X11 without a window manager, where focus changes
  are immediate: they are not expected to pass everywhere, and timing tuned
  for one compositor broke them on another.
- **Toplevels are placed by the compositor.** `wm geometry +x+y` is not
  applied, and Tk does not know where its toplevels are (constraint
  `placedToplevels` in the tests).

## Sway

- Sends a floating window the size it first gave it in the configures of
  other changes (activation, a maximization which it ignores). A size which
  does not change is not taken as a resize, and the window keeps the size the
  application gave it. A stale size can still win after a gridded window is
  resized (`listbox.test` run alone, open).
- Sway 1.11 ignores the maximization of a floating window (`wayland-4.14`).

## Mutter (GNOME)

- A popup which grabs the input must be attached to the top-most popup which
  grabs it, once that one is shown (a cascade attached to the toplevel ends
  the connection).
- A popup the compositor dismissed can no longer be the parent of a popup.
- Restoring a window unmaximized twice gave it the size of the screen: the
  vertical and horizontal maximizations, which Tk changes together, are
  requested once.
- Draws no decorations: libdecor's GTK plugin does (see below).

## KWin (KDE)

- Draws the decorations itself (server-side), so the minimum size of the
  decorations of libdecor does not apply.
- `kwin_wayland` has file capabilities (`CAP_SYS_RESOURCE` on Debian), so it
  does not start in a container which drops them.

## Weston

- Gives a size of 0x0 to a fullscreen window which is minimized. The
  configure is not acknowledged; the window keeps its state until it gets a
  size.
- Weston 15 crashed when a toplevel was destroyed before it was configured:
  toplevels are created at idle time, so that windows mapped and unmapped at
  once never get one.
- Toplevels cannot be minimized before they are shown: a toplevel iconified
  before its first buffer is unmapped for Tk at once, and minimized after its
  first buffer.
- The move which `unixWm.test` makes to measure the offsets of the window
  manager is sometimes applied late, and then all of `unixWm-2.*` and
  `unixWm-3.*` fail together.

## libdecor

- The GTK plugin (used on Weston and GNOME) raises the minimum size of a
  window to the one of its header bar, 168 pixels wide in the tests: windows
  are not narrower, and tests which ask for narrower windows fail there (they
  pass on sway and KWin). Setting the size limits again drops that minimum
  until the decorations are drawn again, so the port keeps it; without that, a
  menubar which wraps at the narrower width made the size go to and fro
  without end.
- The GTK plugin prints `Gdk-CRITICAL ... gdk_seat_get_keyboard` on stderr
  when the seat has no keyboard (the headless Weston and GNOME of the tests),
  which makes the tests that check stderr fail.
- libdecor 0.2.2 and older crashed with a NULL parent, and fails for good when
  the minimum size is larger than the maximum size, which X11 window managers
  accept.
- libdecor before 0.2.3 draws the decorations of a frame without a size,
  which the cairo plugin crashes on (mmap of 0 bytes): their visibility is set
  once the frame has a size.
- The cairo plugin of libdecor before 0.2 raises the minimum size above the
  maximum size; the maximum size is raised as well.
- libdecor 0.1.0 (Ubuntu 22.04) lacks `libdecor_frame_get_min_content_size`,
  so the port does not build with it.

## Debian 12 (bookworm, oldstable): not tested

Debian 12 is oldstable (Debian LTS only), and the port is not tested on it
any more. It cannot build the port with its own
wayland-protocols (1.31, without xdg-toplevel-icon); with wayland-protocols
1.45 from source, these were seen and left alone:

- KWin 5.27 with libdecor 0.1.1: Tk sometimes crashes (SIGSEGV in
  `wl_display_read_events`, called by the dispatch of libdecor's cairo
  plugin) at the start of the tests; not with `WAYLAND_DEBUG` set.
- Sway 1.7: the connection is lost after `wayland-4.14` when the whole of
  `wayland.test` runs; it maximizes a floating window, and does not make a
  window fullscreen at the size of the output (`wayland-4.8`,
  `wayland-4.12`).
- Weston 10: when a fullscreen window whose size changed is maximized and
  leaves fullscreen, it sends a maximized configure of 0x0, and ends the
  connection if the committed geometry changes ("xdg_surface geometry does
  not match the configured maximized state (0 x 0)"; the random test
  `wayland-13.2` hits it now and then). Not acknowledging the configure
  leaves an ordinary window fullscreen, and answering with the last size
  given triggers the error.
