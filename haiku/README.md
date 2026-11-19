# Tk on Haiku

This directory holds a Haiku port of Tk. It runs on the X server emulation of
`../xemu`, shared with the Wayland and Android ports: the window tree, drawing
with Cairo, fonts with fontconfig, properties and the Xlib functions Tk uses.
It is built with

    ./configure --enable-haiku

which defines `TK_USE_HAIKU` and `TK_USE_XEMU`, and needs the development
files of cairo, pixman, fontconfig and freetype (and harfbuzz, for
bidirectional text), all in HaikuPorts:

    pkgman install cairo_devel pixman_devel fontconfig_devel \
        freetype_devel harfbuzz_devel

## Design

Every child of the root window (the wrapper of a toplevel, or an
override-redirect window: a menu, a tooltip, a dropdown) has a `BWindow`,
whose view shows the backing image of the window. What differs from the
other ports is where the images are shown, where input comes from, and the
threads:

- `tkHaikuApp.cpp`: the only file in C++, the glue of the Be API. Tk calls it
  through the C functions of `tkHaiku.h`.
- Threads: Tcl and Tk run in the thread which opened the display. The
  `BApplication` runs its looper in a thread of its own (started when the
  display is opened, unless the process already has an application), and
  every `BWindow` in another. Those threads never wait for Tk: what happens
  to the windows (input, moves and resizes by the user, activation, the
  clipboard) is posted as events, queued and read by Tk through a pipe
  (`tkHaikuEvent.c`), so that Tk may be busy for long (in a blocking read,
  say) while the windows keep drawing and the input is kept for it. Tk
  locks a window to change it, and may wait for the threads of the Be API,
  which never wait for Tk: there is no deadlock.
- `tkHaikuSurface.c`: the damaged parts of a backing image are copied at
  idle time to a `BBitmap` the view draws (the xRGB of Cairo is `B_RGB32`),
  with the window locked. A window is shown once its image was first copied.
- `tkHaikuWm.c`: the window manager. `tkUnixWm.c` is used unchanged. Haiku
  places windows where applications ask, so root coordinates are those of
  the screen, as with X11. The app_server draws the decorations around the
  window without reparenting it; they are reported in `_NET_FRAME_EXTENTS`,
  so that `wm geometry` gives the position of the decorations, as with X
  window managers which do not reparent, and the work area (the screen less
  a Deskbar across its top or bottom) in `_NET_WORKAREA`. Toplevels for which
  Tk gives no position are cascaded. When the user moves or resizes a
  window, the X window follows (the frame of the `BWindow` is read again, so
  that the late notifications of moves Tk made itself change nothing), which
  Tk learns with a `ConfigureNotify` event. Override-redirect windows are
  borderless windows above all others, which never take the focus. The close
  button sends `WM_DELETE_WINDOW`, the zoom button maximizes, and minimized
  windows go to the Deskbar (and are unmapped, as X window managers do).
  Quitting the application from the Deskbar asks its toplevels to close.
- `tkHaikuEvent.c`: pointer events go through `Tk_UpdatePointer`
  (`../generic/tkPointer.c`, shared with Aqua, Windows, Wayland and Android).
  While a button is pressed or Tk captures the pointer (a grab, a posted
  menu), a `BWindow` gets the pointer events of the whole screen, so that a
  click outside the application dismisses a menu, as with X. The keyboard
  goes to the toplevel the app_server activated.
- `tkHaikuKey.c`: keys. An X keycode is the key code of the Be API (the
  position of the key) plus 8; the keys which type characters have the
  keysym of the character in the keymap of the user, with Shift, the locks
  and Option. Alt (Command) is Mod1, Num Lock Mod2 and the Windows key
  (Option) Mod4; which keys these are is read from the keymap (the user may
  swap Control and Command), and when the user chooses another keymap, Tk
  reads it again (`MappingNotify`). The shortcuts of `BWindow` (Alt+Q,
  Alt+W, ...) are not used: the bindings of Tk decide.
- `tkHaikuSelect.c`: the CLIPBOARD is shared with other applications through
  the system clipboard (text); when another application changes it, the owner
  in Tk gets a `SelectionClear` event.
- `tkHaikuCursor.c`: the cursors of the X cursor font are shown as the
  system cursors of Haiku closest to them; bitmap cursors as images.

## Scripting

The widgets of Tk can be scripted as the other applications of Haiku are,
with `hey` or any program sending scripting messages: the application and
its windows have the property `Widget` (suite `suite/x-vnd.tcl-tk-widgets`),
and a widget, named by its path name or its label (or accessible name), or
by its index in stacking order, has these properties (suite
`suite/x-vnd.tcl-tk-widget`):

| Property  | Verbs    | Of                                             |
|-----------|----------|------------------------------------------------|
| `Path`    | get      | any widget                                     |
| `Class`   | get      | any widget                                     |
| `Label`   | get      | buttons, labels                                |
| `Text`    | get, set | entries, spinboxes, comboboxes, text widgets   |
| `Value`   | get, set | check, radio buttons (1/0), scales, spinboxes  |
| `Enabled` | get, set | any widget with a state                        |
| `Focus`   | get, set | any widget                                     |
| `Frame`   | get      | any widget (on the screen)                     |

and `do` invokes a button, check or radio button. For instance:

    hey MyApp count Widget
    hey MyApp get Path of Widget 3
    hey MyApp set Text of Widget .user to "me"
    hey MyApp set Value of Widget Remember to true
    hey MyApp do Widget OK of Window Login

The window thread which gets a request posts it to Tk, which does it
(`tkHaikuScript.c`, with `::tk::haiku::Script` of `../library/haiku.tcl`)
and replies, so that a request waits for Tk while the windows do not. A
script of the application itself must not send requests to it while it
waits for them (with a blocking `exec hey`): the reply needs its event
loop.

The view of a window is named after its X window, `tk 0x` and the id in
hex, and the messages of the mouse which scripting sends it are input, as
from the app_server: `hey MyApp _MDN View 0 of Window 0 with
be:view_where="BPoint(10,10)" and buttons="int32(1)"` presses the button at
(10, 10) of the first window.

Logical pixels have 96 dpi, times the scale of the fonts the user chose
(Haiku scales its interface with the size of the plain font, 12 by default),
in steps of a quarter; `TK_HAIKU_SCALE` sets it, for testing. The generic
families `sans-serif` and `monospace`, which the named fonts of Tk use, are
the plain and fixed fonts chosen in the preferences of Haiku. The families of
the core fonts of X which scripts name (Helvetica, Courier, Times), which
other systems map to fonts of the same metrics that Haiku does not have, are
mapped to fonts of the same kind (the configuration of fontconfig on Haiku
maps them all, and `monospace` too, to Noto Sans).

`[tk windowingsystem]` is `haiku`; the library treats it like `x11`.

## Tests

The tests run in the session of the desktop, with

    make test

in the build directory. Behaviour the port cannot have (such as windows
which the app_server constrains) skips tests through constraints of
`../tests/constraints.tcl`. `../tests/haiku.test` tests the port itself: the
window manager, input, input while Tk is busy, the clipboard (with the
`clipboard` command of Haiku) and scripting (with `hey`, of the widgets and
of input). Its input comes
from the commands of `tkHaikuTest.cpp` in `tktest`, which make it the way
the user does: the pointer moves through the app_server, and the buttons
and keys are delivered by the threads of the windows, as messages of the
form the app_server sends (`testmovemouse` and `testpressbutton`, as on
Aqua, also enable tests of `grab.test`, `menu.test` and `unixEmbed.test`).

## Not done yet

- Input methods (`B_INPUT_METHOD_EVENT`).
- Drag and drop between applications.
- `send` and embedding between processes (they work within a process).
- Opacity of windows (`wm attributes -alpha`).
- Icons of windows (`wm iconphoto`).
