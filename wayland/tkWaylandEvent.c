/*
 * tkWaylandEvent.c --
 *
 *	The connection to the Wayland compositor, and the translation of
 *	Wayland input events into X events: pointer, keyboard and focus.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkWaylandInt.h"
#include <sys/mman.h>
#include <poll.h>
#include <locale.h>
#include <linux/input-event-codes.h>

TkWlDisplay *tkWlDisplay = NULL;

typedef struct {
    bool initialized;
} ThreadSpecificData;
static Tcl_ThreadDataKey dataKey;

/*
 * The X window which has the input focus, set with XSetInputFocus.
 */

static Window focusWindow = None;

static void		DisplayFileProc(void *clientData, int flags);
static void		DisplaySetupProc(void *clientData, int flags);
static void		DisplayCheckProc(void *clientData, int flags);
static void		DisplayExitHandler(void *clientData);
static void		RepeatProc(void *clientData);

/*
 *----------------------------------------------------------------------
 *
 * Outputs --
 *
 *	The size of the screen is that of the first output, in logical
 *	pixels.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateScreenSize(void)
{
    TkWlOutput *outPtr = tkWlDisplay->outputs;
    int scale;

    if (outPtr == NULL || tkWlDisplay->display == NULL
	    || outPtr->width <= 0) {
	return;
    }
    scale = (outPtr->scale > 0) ? outPtr->scale : 1;
    TkWlSetScreenSize(tkWlDisplay->display, outPtr->width / scale,
	    outPtr->height / scale, outPtr->mmWidth, outPtr->mmHeight);
}

static void
OutputGeometry(
    void *data,
    TCL_UNUSED(struct wl_output *),
    TCL_UNUSED(int32_t),	/* x */
    TCL_UNUSED(int32_t),	/* y */
    int32_t mmWidth,
    int32_t mmHeight,
    TCL_UNUSED(int32_t),	/* subpixel */
    TCL_UNUSED(const char *),	/* make */
    TCL_UNUSED(const char *),	/* model */
    int32_t transform)
{
    TkWlOutput *outPtr = (TkWlOutput *)data;

    if (transform & 1) {
	outPtr->mmWidth = mmHeight;
	outPtr->mmHeight = mmWidth;
    } else {
	outPtr->mmWidth = mmWidth;
	outPtr->mmHeight = mmHeight;
    }
}

static void
OutputMode(
    void *data,
    TCL_UNUSED(struct wl_output *),
    uint32_t flags,
    int32_t width,
    int32_t height,
    TCL_UNUSED(int32_t))	/* refresh */
{
    TkWlOutput *outPtr = (TkWlOutput *)data;

    if (flags & WL_OUTPUT_MODE_CURRENT) {
	outPtr->width = width;
	outPtr->height = height;
    }
}

static void
OutputDone(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_output *))
{
    UpdateScreenSize();
}

static void
OutputScale(
    void *data,
    TCL_UNUSED(struct wl_output *),
    int32_t factor)
{
    ((TkWlOutput *)data)->scale = factor;
}

static void
OutputName(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_output *),
    TCL_UNUSED(const char *))
{
}

static void
OutputDescription(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_output *),
    TCL_UNUSED(const char *))
{
}

static const struct wl_output_listener outputListener = {
    OutputGeometry,
    OutputMode,
    OutputDone,
    OutputScale,
    OutputName,
    OutputDescription
};

/*
 *----------------------------------------------------------------------
 *
 * Pointer events --
 *
 *	The pointer events are given to Tk_UpdatePointer (tkPointer.c), which
 *	generates the Enter, Leave, Motion and Button events, and handles the
 *	implicit and explicit grabs, as on Aqua and Windows.
 *
 *----------------------------------------------------------------------
 */

/*
 * Root coordinates of a point of a surface. Wayland clients do not know
 * where their windows are, so the position of the window is the one Tk
 * believes it has.
 */

static void
SurfaceToRoot(
    TkWlSurface *surfPtr,
    int x, int y,
    int *rootXPtr, int *rootYPtr)
{
    TkWlWindow *winPtr = surfPtr->winPtr;

    *rootXPtr = winPtr->x + x;
    *rootYPtr = winPtr->y + y;
}

/*
 * The Tk window at the position of the pointer.
 */

static Tk_Window
WindowAtPointer(
    TkWlSurface *surfPtr,
    int x, int y)
{
    TkWlWindow *winPtr;
    Tk_Window tkwin;

    if (surfPtr == NULL) {
	return NULL;
    }
    winPtr = TkWlWindowAtPoint(surfPtr->winPtr,
	    x - surfPtr->winPtr->borderWidth, y - surfPtr->winPtr->borderWidth);
    for (; winPtr; winPtr = winPtr->parent) {
	tkwin = Tk_IdToWindow(winPtr->display, winPtr->id);
	if (tkwin) {
	    return tkwin;
	}
    }
    return NULL;
}

static unsigned int
CurrentState(void)
{
    return tkWlDisplay->modState | tkWlDisplay->buttonState;
}

static void
UpdatePointer(
    unsigned int state)
{
    TkWlSurface *surfPtr = tkWlDisplay->pointerSurface;
    int x, y, rootX, rootY;

    if (surfPtr == NULL) {
	Tk_UpdatePointer(NULL, 0, 0, state);
	return;
    }
    x = (int)floor(tkWlDisplay->pointerX);
    y = (int)floor(tkWlDisplay->pointerY);
    SurfaceToRoot(surfPtr, x, y, &rootX, &rootY);
    Tk_UpdatePointer(WindowAtPointer(surfPtr, x, y), rootX, rootY, state);
}

static void
PointerEnter(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    uint32_t serial,
    struct wl_surface *surface,
    wl_fixed_t x,
    wl_fixed_t y)
{
    TkWlSurface *surfPtr = surface ? TkWlSurfaceFromWl(surface) : NULL;

    tkWlDisplay->pointerSerial = serial;
    tkWlDisplay->pointerSurface = surfPtr;
    tkWlDisplay->pointerX = wl_fixed_to_double(x);
    tkWlDisplay->pointerY = wl_fixed_to_double(y);
    TkWlUpdateCursor();
    if (surfPtr) {
	UpdatePointer(CurrentState());
    }
}

static void
PointerLeave(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    TCL_UNUSED(uint32_t),
    TCL_UNUSED(struct wl_surface *))
{
    tkWlDisplay->pointerSurface = NULL;
    UpdatePointer(CurrentState());
}

static void
PointerMotion(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    uint32_t time,
    wl_fixed_t x,
    wl_fixed_t y)
{
    tkWlDisplay->lastTime = time;
    tkWlDisplay->pointerX = wl_fixed_to_double(x);
    tkWlDisplay->pointerY = wl_fixed_to_double(y);
    if (tkWlDisplay->pointerSurface) {
	UpdatePointer(CurrentState());
    }
}

static void
PointerButton(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    uint32_t serial,
    uint32_t time,
    uint32_t button,
    uint32_t state)
{
    unsigned int mask;

    tkWlDisplay->lastSerial = serial;
    tkWlDisplay->lastTime = time;
    switch (button) {
    case BTN_LEFT:
	mask = Button1Mask;
	break;
    case BTN_MIDDLE:
	mask = Button2Mask;
	break;
    case BTN_RIGHT:
	mask = Button3Mask;
	break;
    case BTN_SIDE:
	mask = Tk_GetButtonMask(Button8);
	break;
    case BTN_EXTRA:
	mask = Tk_GetButtonMask(Button9);
	break;
    default:
	return;
    }
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
	tkWlDisplay->buttonState |= mask;
    } else {
	tkWlDisplay->buttonState &= ~mask;
    }
    UpdatePointer(CurrentState());
}

/*
 * Scrolling: wheels generate MouseWheel events, of 120 units per notch,
 * and touchpads TouchpadScroll events, as on Aqua.
 */

static uint32_t axisSource = WL_POINTER_AXIS_SOURCE_WHEEL;
static int axisValue120[2];
static double axisValue[2];

static void
PointerAxis(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    uint32_t time,
    uint32_t axis,
    wl_fixed_t value)
{
    tkWlDisplay->lastTime = time;
    if (axis < 2) {
	axisValue[axis] += wl_fixed_to_double(value);
    }
}

static void
PointerAxisSource(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    uint32_t source)
{
    axisSource = source;
}

static void
PointerAxisStop(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    TCL_UNUSED(uint32_t),
    TCL_UNUSED(uint32_t))
{
}

static void
PointerAxisDiscrete(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    uint32_t axis,
    int32_t discrete)
{
    if (axis < 2) {
	axisValue120[axis] += 120 * discrete;
    }
}

static void
PointerAxisValue120(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    uint32_t axis,
    int32_t value120)
{
    if (axis < 2) {
	axisValue120[axis] += value120;
    }
}

static void
PointerAxisRelativeDirection(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    TCL_UNUSED(uint32_t),
    TCL_UNUSED(uint32_t))
{
}

static void
QueueScrollEvent(
    int type,
    unsigned int state,
    unsigned int value)
{
    TkWlSurface *surfPtr = tkWlDisplay->pointerSurface;
    Tk_Window tkwin, capture = TkpGetCapture();
    int x = (int)floor(tkWlDisplay->pointerX);
    int y = (int)floor(tkWlDisplay->pointerY);
    int rootX, rootY, winX, winY;
    XEvent event;

    tkwin = capture ? capture : WindowAtPointer(surfPtr, x, y);
    if (tkwin == NULL || surfPtr == NULL) {
	return;
    }
    SurfaceToRoot(surfPtr, x, y, &rootX, &rootY);
    Tk_GetRootCoords(tkwin, &winX, &winY);
    memset(&event, 0, sizeof(XEvent));
    event.type = type;
    event.xany.display = Tk_Display(tkwin);
    event.xany.window = Tk_WindowId(tkwin);
    event.xany.serial = LastKnownRequestProcessed(Tk_Display(tkwin));
    event.xbutton.x = rootX - winX;
    event.xbutton.y = rootY - winY;
    event.xbutton.x_root = rootX;
    event.xbutton.y_root = rootY;
    event.xbutton.state = state;
    event.xbutton.time = tkWlDisplay->lastTime;
    event.xkey.keycode = value;
    Tk_QueueWindowEvent(&event, TCL_QUEUE_TAIL);
}

static void
PointerFrame(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *))
{
    unsigned int state = CurrentState();

    if (axisSource == WL_POINTER_AXIS_SOURCE_WHEEL
	    || axisSource == WL_POINTER_AXIS_SOURCE_WHEEL_TILT) {
	int v = axisValue120[WL_POINTER_AXIS_VERTICAL_SCROLL];
	int h = axisValue120[WL_POINTER_AXIS_HORIZONTAL_SCROLL];

	/*
	 * Wayland scrolls down for positive values, Tk up.
	 */

	if (v == 0 && h == 0) {
	    v = (int)(axisValue[WL_POINTER_AXIS_VERTICAL_SCROLL] * 12);
	    h = (int)(axisValue[WL_POINTER_AXIS_HORIZONTAL_SCROLL] * 12);
	}
	if (v != 0) {
	    QueueScrollEvent(MouseWheelEvent, state, (unsigned)-v);
	}
	if (h != 0) {
	    QueueScrollEvent(MouseWheelEvent, state | ShiftMask,
		    (unsigned)-h);
	}
    } else {
	int dx = -(int)lround(axisValue[WL_POINTER_AXIS_HORIZONTAL_SCROLL]);
	int dy = -(int)lround(axisValue[WL_POINTER_AXIS_VERTICAL_SCROLL]);

	if (dx != 0 || dy != 0) {
	    static unsigned long scrollCounter = 0;
	    unsigned delta = ((unsigned)dx << 16) | ((unsigned)dy & 0xFFFF);

	    QueueScrollEvent(TouchpadScroll, state, delta);
	    (void)scrollCounter;
	}
    }
    axisValue120[0] = axisValue120[1] = 0;
    axisValue[0] = axisValue[1] = 0.0;
    axisSource = WL_POINTER_AXIS_SOURCE_WHEEL;
}

static void
PointerWarp(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    wl_fixed_t x,
    wl_fixed_t y)
{
    tkWlDisplay->pointerX = wl_fixed_to_double(x);
    tkWlDisplay->pointerY = wl_fixed_to_double(y);
}

static const struct wl_pointer_listener pointerListener = {
    PointerEnter,
    PointerLeave,
    PointerMotion,
    PointerButton,
    PointerAxis,
    PointerFrame,
    PointerAxisSource,
    PointerAxisStop,
    PointerAxisDiscrete,
    PointerAxisValue120,
    PointerAxisRelativeDirection,
    PointerWarp
};

/*
 *----------------------------------------------------------------------
 *
 * Keyboard events --
 *
 *	Keycodes are evdev codes plus 8, as with XKB. The keysym and the
 *	characters of a key are computed with xkbcommon (including compose
 *	sequences) when the event is generated, and stored in the TkKeyEvent.
 *
 *----------------------------------------------------------------------
 */

static const char *const modNames[8] = {
    XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CAPS, XKB_MOD_NAME_CTRL, "Mod1",
    "Mod2", "Mod3", "Mod4", "Mod5"
};
static xkb_mod_index_t modIndices[8];

static void
KeyboardKeymap(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_keyboard *),
    uint32_t format,
    int32_t fd,
    uint32_t size)
{
    struct xkb_keymap *keymap;
    char *map;
    int i;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
	close(fd);
	return;
    }
    map = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
	close(fd);
	return;
    }
    keymap = xkb_keymap_new_from_string(tkWlDisplay->xkbContext, map,
	    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (keymap == NULL) {
	return;
    }
    if (tkWlDisplay->xkbState) {
	xkb_state_unref(tkWlDisplay->xkbState);
    }
    if (tkWlDisplay->keymap) {
	xkb_keymap_unref(tkWlDisplay->keymap);
    }
    tkWlDisplay->keymap = keymap;
    tkWlDisplay->xkbState = xkb_state_new(keymap);
    for (i = 0; i < 8; i++) {
	modIndices[i] = xkb_keymap_mod_get_index(keymap, modNames[i]);
    }

    /*
     * The modifiers in Mod1-Mod5 depend on the keymap; use the usual
     * assignment of XKB when the keymap has virtual modifiers only.
     */

    if (modIndices[3] == XKB_MOD_INVALID) {
	modIndices[3] = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
    }
    if (modIndices[4] == XKB_MOD_INVALID) {
	modIndices[4] = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_NUM);
    }
    if (modIndices[6] == XKB_MOD_INVALID) {
	modIndices[6] = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_LOGO);
    }
    if (tkWlDisplay->display) {
	TkDisplay *dispPtr = TkGetDisplayList();

	for (; dispPtr; dispPtr = dispPtr->nextPtr) {
	    dispPtr->bindInfoStale = 1;
	}
    }
}

static unsigned int
ModState(void)
{
    unsigned int state = 0;
    int i;

    if (tkWlDisplay->xkbState == NULL) {
	return 0;
    }
    for (i = 0; i < 8; i++) {
	if (modIndices[i] != XKB_MOD_INVALID
		&& xkb_state_mod_index_is_active(tkWlDisplay->xkbState,
		    modIndices[i], XKB_STATE_MODS_EFFECTIVE) > 0) {
	    state |= 1u << i;
	}
    }
    return state;
}

static void
KeyboardEnter(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_keyboard *),
    uint32_t serial,
    struct wl_surface *surface,
    TCL_UNUSED(struct wl_array *))
{
    TkWlSurface *surfPtr = surface ? TkWlSurfaceFromWl(surface) : NULL;
    XEvent event;

    tkWlDisplay->lastSerial = serial;
    tkWlDisplay->keyboardSurface = surfPtr;
    if (surfPtr == NULL) {
	return;
    }
    if (surfPtr->iconic) {
	/*
	 * The user deiconified the window.
	 */

	XMapWindow(surfPtr->winPtr->display, surfPtr->winPtr->id);
    }
    focusWindow = surfPtr->winPtr->id;
    memset(&event, 0, sizeof(XEvent));
    event.xfocus.type = FocusIn;
    event.xfocus.display = surfPtr->winPtr->display;
    event.xfocus.window = surfPtr->winPtr->id;
    event.xfocus.mode = NotifyNormal;
    event.xfocus.detail = NotifyNonlinear;
    TkWlQueueEvent(&event);
}

static void
KeyboardLeave(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_keyboard *),
    uint32_t serial,
    TCL_UNUSED(struct wl_surface *))
{
    TkWlSurface *surfPtr = tkWlDisplay->keyboardSurface;
    XEvent event;

    tkWlDisplay->lastSerial = serial;
    tkWlDisplay->keyboardSurface = NULL;
    if (tkWlDisplay->repeatTimer) {
	Tcl_DeleteTimerHandler(tkWlDisplay->repeatTimer);
	tkWlDisplay->repeatTimer = NULL;
    }
    focusWindow = None;
    if (surfPtr == NULL) {
	return;
    }
    memset(&event, 0, sizeof(XEvent));
    event.xfocus.type = FocusOut;
    event.xfocus.display = surfPtr->winPtr->display;
    event.xfocus.window = surfPtr->winPtr->id;
    event.xfocus.mode = NotifyNormal;
    event.xfocus.detail = NotifyNonlinear;
    TkWlQueueEvent(&event);
}

/*
 * Generate a key event. The text of a KeyPress is stored in the event, as
 * TkpGetString would compute it with an input method.
 */

static void
QueueKeyEvent(
    int type,
    uint32_t key)
{
    TkWlSurface *surfPtr = tkWlDisplay->keyboardSurface;
    xkb_keycode_t keycode = key + 8;
    xkb_keysym_t keysym;
    union {
	XEvent x;
	TkKeyEvent k;
    } event;
    char buf[64];
    int len = 0, x, y, rootX, rootY;

    if (surfPtr == NULL || tkWlDisplay->xkbState == NULL) {
	return;
    }
    keysym = xkb_state_key_get_one_sym(tkWlDisplay->xkbState, keycode);
    if (type == KeyPress) {
	struct xkb_compose_state *compose = tkWlDisplay->composeState;
	enum xkb_compose_status status = XKB_COMPOSE_NOTHING;

	if (compose && keysym != XKB_KEY_NoSymbol
		&& xkb_compose_state_feed(compose, keysym)
		    == XKB_COMPOSE_FEED_ACCEPTED) {
	    status = xkb_compose_state_get_status(compose);
	}
	switch (status) {
	case XKB_COMPOSE_COMPOSING:
	    break;
	case XKB_COMPOSE_COMPOSED:
	    len = xkb_compose_state_get_utf8(compose, buf, sizeof(buf));
	    keysym = xkb_compose_state_get_one_sym(compose);
	    xkb_compose_state_reset(compose);
	    break;
	case XKB_COMPOSE_CANCELLED:
	    xkb_compose_state_reset(compose);
	    break;
	default:
	    len = xkb_state_key_get_utf8(tkWlDisplay->xkbState, keycode, buf,
		    sizeof(buf));
	    break;
	}
	if (len < 0 || len >= (int)sizeof(buf)) {
	    len = 0;
	}
    }

    x = (int)floor(tkWlDisplay->pointerX);
    y = (int)floor(tkWlDisplay->pointerY);
    if (tkWlDisplay->pointerSurface != surfPtr) {
	x = y = 0;
    }
    SurfaceToRoot(surfPtr, x, y, &rootX, &rootY);

    memset(&event, 0, sizeof(event));
    event.x.xkey.type = type;
    event.x.xkey.display = surfPtr->winPtr->display;
    event.x.xkey.window = (focusWindow != None && TkWlGetWindow(focusWindow)
	    && TkWlTopWindow(TkWlGetWindow(focusWindow)) == surfPtr->winPtr)
	    ? focusWindow : surfPtr->winPtr->id;
    event.x.xkey.root = RootWindow(event.x.xkey.display, 0);
    event.x.xkey.time = tkWlDisplay->lastTime;
    event.x.xkey.x = x;
    event.x.xkey.y = y;
    event.x.xkey.x_root = rootX;
    event.x.xkey.y_root = rootY;
    event.x.xkey.state = CurrentState();
    event.x.xkey.keycode = (keycode <= 255) ? keycode : 0;
    event.x.xkey.same_screen = True;
    event.k.keysym = keysym;
    if (type == KeyPress) {
	event.k.charValuePtr = (char *)Tcl_Alloc(len + 1);
	memcpy(event.k.charValuePtr, buf, len);
	event.k.charValuePtr[len] = '\0';
	event.k.charValueLen = len;
    }
    TkWlQueueEvent(&event.x);
}

static void
KeyboardKey(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_keyboard *),
    uint32_t serial,
    uint32_t time,
    uint32_t key,
    uint32_t state)
{
    tkWlDisplay->lastSerial = serial;
    tkWlDisplay->lastTime = time;
    if (tkWlDisplay->repeatTimer) {
	Tcl_DeleteTimerHandler(tkWlDisplay->repeatTimer);
	tkWlDisplay->repeatTimer = NULL;
    }
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
	QueueKeyEvent(KeyPress, key);
	if (tkWlDisplay->keymap && tkWlDisplay->repeatRate > 0
		&& xkb_keymap_key_repeats(tkWlDisplay->keymap, key + 8)) {
	    tkWlDisplay->repeatKey = key;
	    tkWlDisplay->repeatTimer = Tcl_CreateTimerHandler(
		    tkWlDisplay->repeatDelay, RepeatProc, NULL);
	}
    } else {
	QueueKeyEvent(KeyRelease, key);
    }
}

/*
 * Key repeat is done by the client in Wayland. As on X11 with detectable
 * auto repeat, only KeyPress events are repeated.
 */

static void
RepeatProc(
    TCL_UNUSED(void *))
{
    tkWlDisplay->repeatTimer = Tcl_CreateTimerHandler(
	    1000 / tkWlDisplay->repeatRate, RepeatProc, NULL);
    QueueKeyEvent(KeyPress, tkWlDisplay->repeatKey);
}

static void
KeyboardModifiers(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_keyboard *),
    uint32_t serial,
    uint32_t depressed,
    uint32_t latched,
    uint32_t locked,
    uint32_t group)
{
    tkWlDisplay->lastSerial = serial;
    if (tkWlDisplay->xkbState) {
	xkb_state_update_mask(tkWlDisplay->xkbState, depressed, latched,
		locked, 0, 0, group);
	tkWlDisplay->modState = ModState();
    }
}

static void
KeyboardRepeatInfo(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_keyboard *),
    int32_t rate,
    int32_t delay)
{
    tkWlDisplay->repeatRate = rate;
    tkWlDisplay->repeatDelay = delay;
}

static const struct wl_keyboard_listener keyboardListener = {
    KeyboardKeymap,
    KeyboardEnter,
    KeyboardLeave,
    KeyboardKey,
    KeyboardModifiers,
    KeyboardRepeatInfo
};

/*
 *----------------------------------------------------------------------
 *
 * Seat and registry --
 *
 *----------------------------------------------------------------------
 */

static void
SeatCapabilities(
    TCL_UNUSED(void *),
    struct wl_seat *seat,
    uint32_t caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !tkWlDisplay->pointer) {
	tkWlDisplay->pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(tkWlDisplay->pointer, &pointerListener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && tkWlDisplay->pointer) {
	wl_pointer_release(tkWlDisplay->pointer);
	tkWlDisplay->pointer = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !tkWlDisplay->keyboard) {
	tkWlDisplay->keyboard = wl_seat_get_keyboard(seat);
	wl_keyboard_add_listener(tkWlDisplay->keyboard, &keyboardListener,
		NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD)
	    && tkWlDisplay->keyboard) {
	wl_keyboard_release(tkWlDisplay->keyboard);
	tkWlDisplay->keyboard = NULL;
    }
}

static void
SeatName(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_seat *),
    TCL_UNUSED(const char *))
{
}

static const struct wl_seat_listener seatListener = {
    SeatCapabilities,
    SeatName
};

static void
WmBasePing(
    TCL_UNUSED(void *),
    struct xdg_wm_base *wmBase,
    uint32_t serial)
{
    xdg_wm_base_pong(wmBase, serial);
}

static const struct xdg_wm_base_listener wmBaseListener = {
    WmBasePing
};

static void
RegistryGlobal(
    TCL_UNUSED(void *),
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
#define MIN(a, b) ((a) < (b) ? (a) : (b))
    if (!strcmp(interface, wl_compositor_interface.name)) {
	tkWlDisplay->compositor = (struct wl_compositor *)wl_registry_bind(
		registry, name, &wl_compositor_interface, MIN(version, 6));
    } else if (!strcmp(interface, wl_shm_interface.name)) {
	tkWlDisplay->shm = (struct wl_shm *)wl_registry_bind(registry, name,
		&wl_shm_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name)
	    && tkWlDisplay->seat == NULL) {
	tkWlDisplay->seat = (struct wl_seat *)wl_registry_bind(registry, name,
		&wl_seat_interface, MIN(version, 9));
	wl_seat_add_listener(tkWlDisplay->seat, &seatListener, NULL);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
	tkWlDisplay->wmBase = (struct xdg_wm_base *)wl_registry_bind(
		registry, name, &xdg_wm_base_interface, 1);
	xdg_wm_base_add_listener(tkWlDisplay->wmBase, &wmBaseListener, NULL);
    } else if (!strcmp(interface, wl_output_interface.name)) {
	TkWlOutput *outPtr = (TkWlOutput *)Tcl_Alloc(sizeof(TkWlOutput));
	TkWlOutput **lastPtrPtr = &tkWlDisplay->outputs;

	memset(outPtr, 0, sizeof(TkWlOutput));
	outPtr->name = name;
	outPtr->scale = 1;
	outPtr->output = (struct wl_output *)wl_registry_bind(registry, name,
		&wl_output_interface, MIN(version, 4));
	wl_output_add_listener(outPtr->output, &outputListener, outPtr);
	while (*lastPtrPtr) {
	    lastPtrPtr = &(*lastPtrPtr)->nextPtr;
	}
	*lastPtrPtr = outPtr;
    }
#undef MIN
}

static void
RegistryGlobalRemove(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_registry *),
    uint32_t name)
{
    TkWlOutput **outPtrPtr;

    for (outPtrPtr = &tkWlDisplay->outputs; *outPtrPtr;
	    outPtrPtr = &(*outPtrPtr)->nextPtr) {
	TkWlOutput *outPtr = *outPtrPtr;

	if (outPtr->name == name) {
	    *outPtrPtr = outPtr->nextPtr;
	    wl_output_release(outPtr->output);
	    Tcl_Free(outPtr);
	    UpdateScreenSize();
	    break;
	}
    }
}

static const struct wl_registry_listener registryListener = {
    RegistryGlobal,
    RegistryGlobalRemove
};

static void
DecorError(
    TCL_UNUSED(struct libdecor *),
    TCL_UNUSED(enum libdecor_error),
    const char *message)
{
    fprintf(stderr, "libdecor error: %s\n", message);
}

static struct libdecor_interface decorInterface = {
    DecorError,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/*
 *----------------------------------------------------------------------
 *
 * TkpOpenDisplay --
 *
 *	Connect to the compositor. There can only be one connection per
 *	process.
 *
 *----------------------------------------------------------------------
 */

TkDisplay *
TkpOpenDisplay(
    const char *displayNameStr)
{
    TkDisplay *dispPtr;
    struct wl_display *wlDisplay;
    const char *locale;
    TkWlOutput *outPtr;

    if (tkWlDisplay) {
	/*
	 * Only one connection per process; any name refers to it.
	 */

	if (tkWlDisplay->display == NULL) {
	    return NULL;
	}
	for (dispPtr = TkGetDisplayList(); dispPtr; dispPtr = dispPtr->nextPtr) {
	    if (dispPtr->display == tkWlDisplay->display) {
		return NULL;
	    }
	}
    }
    wlDisplay = wl_display_connect(
	    (displayNameStr && displayNameStr[0] && displayNameStr[0] != ':')
	    ? displayNameStr : NULL);
    if (wlDisplay == NULL) {
	return NULL;
    }

    tkWlDisplay = (TkWlDisplay *)Tcl_Alloc(sizeof(TkWlDisplay));
    memset(tkWlDisplay, 0, sizeof(TkWlDisplay));
    tkWlDisplay->wlDisplay = wlDisplay;
    tkWlDisplay->repeatRate = 25;
    tkWlDisplay->repeatDelay = 600;
    tkWlDisplay->xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    locale = setlocale(LC_CTYPE, NULL);
    if (!locale || !strcmp(locale, "C")) {
	locale = getenv("LC_ALL");
	if (!locale || !*locale) {
	    locale = getenv("LC_CTYPE");
	}
	if (!locale || !*locale) {
	    locale = getenv("LANG");
	}
	if (!locale || !*locale) {
	    locale = "C";
	}
    }
    tkWlDisplay->composeTable = xkb_compose_table_new_from_locale(
	    tkWlDisplay->xkbContext, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (tkWlDisplay->composeTable) {
	tkWlDisplay->composeState = xkb_compose_state_new(
		tkWlDisplay->composeTable, XKB_COMPOSE_STATE_NO_FLAGS);
    }

    tkWlDisplay->registry = wl_display_get_registry(wlDisplay);
    wl_registry_add_listener(tkWlDisplay->registry, &registryListener, NULL);
    wl_display_roundtrip(wlDisplay);
    if (!tkWlDisplay->compositor || !tkWlDisplay->shm) {
	wl_display_disconnect(wlDisplay);
	Tcl_Free(tkWlDisplay);
	tkWlDisplay = NULL;
	return NULL;
    }

    /*
     * Get the outputs, the keymap and the capabilities of the seat.
     */

    wl_display_roundtrip(wlDisplay);
    wl_display_roundtrip(wlDisplay);

    tkWlDisplay->decor = libdecor_new(wlDisplay, &decorInterface);

    outPtr = tkWlDisplay->outputs;
    tkWlDisplay->display = TkWlOpenXDisplay(
	    displayNameStr ? displayNameStr : "wayland-0",
	    wl_display_get_fd(wlDisplay), 1920, 1080, 0, 0);
    TkWlSetScreenSize(tkWlDisplay->display, 1920, 1080, 0, 0);
    if (outPtr) {
	UpdateScreenSize();
    }

    dispPtr = (TkDisplay *)Tcl_Alloc(sizeof(TkDisplay));
    memset(dispPtr, 0, sizeof(TkDisplay));
    dispPtr->display = tkWlDisplay->display;
    Tcl_CreateFileHandler(wl_display_get_fd(wlDisplay), TCL_READABLE,
	    DisplayFileProc, dispPtr);
    TkpInitKeymapInfo(dispPtr);
    return dispPtr;
}

void
TkpCloseDisplay(
    TkDisplay *dispPtr)
{
    TkSendCleanup(dispPtr);
    TkWmCleanup(dispPtr);
    if (dispPtr->display != NULL && tkWlDisplay
	    && dispPtr->display == tkWlDisplay->display) {
	Tcl_DeleteFileHandler(wl_display_get_fd(tkWlDisplay->wlDisplay));
	wl_display_flush(tkWlDisplay->wlDisplay);
    }
}

void
TkClipCleanup(
    TCL_UNUSED(TkDisplay *))
{
}

/*
 *----------------------------------------------------------------------
 *
 * The event source --
 *
 *	Wayland events are read when the connection is readable. Requests
 *	are flushed before Tcl waits for events.
 *
 *----------------------------------------------------------------------
 */

void
TkCreateXEventSource(void)
{
    ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    if (!tsdPtr->initialized) {
	tsdPtr->initialized = true;
	Tcl_CreateEventSource(DisplaySetupProc, DisplayCheckProc, NULL);
	TkCreateExitHandler(DisplayExitHandler, NULL);
    }
}

static void
DisplayExitHandler(
    TCL_UNUSED(void *))
{
    ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    Tcl_DeleteEventSource(DisplaySetupProc, DisplayCheckProc, NULL);
    tsdPtr->initialized = false;
}

static void
DisplaySetupProc(
    TCL_UNUSED(void *),
    int flags)
{
    if (!(flags & TCL_WINDOW_EVENTS) || tkWlDisplay == NULL) {
	return;
    }
    if (wl_display_dispatch_pending(tkWlDisplay->wlDisplay) > 0) {
	Tcl_Time blockTime = {0, 0};

	Tcl_SetMaxBlockTime(&blockTime);
    }
    wl_display_flush(tkWlDisplay->wlDisplay);
}

static void
DisplayCheckProc(
    TCL_UNUSED(void *),
    TCL_UNUSED(int))
{
}

/*
 * Read and dispatch the events of the connection, through libdecor, since
 * its plugins may have their own event sources.
 */

static int
Dispatch(
    int timeout)
{
    if (tkWlDisplay->decor) {
	return libdecor_dispatch(tkWlDisplay->decor, timeout);
    }
    return wl_display_dispatch(tkWlDisplay->wlDisplay);
}

static void
DisplayFileProc(
    TCL_UNUSED(void *),
    TCL_UNUSED(int))
{
    if (Dispatch(0) < 0) {
	/*
	 * The compositor went away. There is nothing more Tk can do.
	 */

	Tcl_DeleteFileHandler(wl_display_get_fd(tkWlDisplay->wlDisplay));
	Tcl_Panic("lost the connection to the Wayland compositor: %s",
		strerror(errno));
    }
}

bool
TkUnixDoOneXEvent(
    Tcl_Time *timePtr)
{
    struct pollfd pfd;
    int timeout = -1;

    if (Tcl_ServiceEvent(TCL_WINDOW_EVENTS)) {
	return true;
    }
    if (tkWlDisplay == NULL) {
	return false;
    }
    wl_display_dispatch_pending(tkWlDisplay->wlDisplay);
    if (Tcl_ServiceEvent(TCL_WINDOW_EVENTS)) {
	return true;
    }
    wl_display_flush(tkWlDisplay->wlDisplay);
    if (timePtr) {
	Tcl_Time now;

	Tcl_GetTime(&now);
	timeout = (int)((timePtr->sec - now.sec) * 1000
		+ (timePtr->usec - now.usec) / 1000);
	if (timeout < 0) {
	    timeout = 0;
	}
    }
    pfd.fd = wl_display_get_fd(tkWlDisplay->wlDisplay);
    pfd.events = POLLIN;
    if (poll(&pfd, 1, timeout) > 0) {
	Dispatch(0);
    }
    if (Tcl_ServiceEvent(TCL_WINDOW_EVENTS)) {
	return true;
    }
    if (timePtr) {
	Tcl_Time now;

	Tcl_GetTime(&now);
	if ((now.sec > timePtr->sec) || ((now.sec == timePtr->sec)
		&& (now.usec > timePtr->usec))) {
	    return false;
	}
    }
    return true;
}

Time
TkWlCurrentTime(void)
{
    return tkWlDisplay ? tkWlDisplay->lastTime : CurrentTime;
}

/*
 *----------------------------------------------------------------------
 *
 * Keysyms --
 *
 *----------------------------------------------------------------------
 */

void
TkpInitKeymapInfo(
    TkDisplay *dispPtr)
{
    static const KeySym modKeysyms[] = {
	XK_Shift_L, XK_Shift_R, XK_Control_L, XK_Control_R, XK_Caps_Lock,
	XK_Shift_Lock, XK_Meta_L, XK_Meta_R, XK_Alt_L, XK_Alt_R, XK_Super_L,
	XK_Super_R, XK_Hyper_L, XK_Hyper_R, XK_Mode_switch,
	XK_ISO_Level3_Shift, XK_Num_Lock
    };
    size_t i;

    dispPtr->bindInfoStale = 0;
    dispPtr->lockUsage = LU_CAPS;
    dispPtr->modeModMask = Mod5Mask;
    dispPtr->metaModMask = Mod1Mask;
    dispPtr->altModMask = Mod1Mask;
    if (dispPtr->modKeyCodes != NULL) {
	Tcl_Free(dispPtr->modKeyCodes);
    }
    dispPtr->modKeyCodes = (KeyCode *)Tcl_Alloc(
	    sizeof(modKeysyms) / sizeof(modKeysyms[0]) * sizeof(KeyCode));
    dispPtr->numModKeyCodes = 0;
    for (i = 0; i < sizeof(modKeysyms) / sizeof(modKeysyms[0]); i++) {
	KeyCode code = XKeysymToKeycode(dispPtr->display, modKeysyms[i]);
	int j;

	if (code == 0) {
	    continue;
	}
	for (j = 0; j < dispPtr->numModKeyCodes; j++) {
	    if (dispPtr->modKeyCodes[j] == code) {
		break;
	    }
	}
	if (j == dispPtr->numModKeyCodes) {
	    dispPtr->modKeyCodes[dispPtr->numModKeyCodes++] = code;
	}
    }
}

/*
 * The keysym of a key with a modifier state, for events which were not
 * generated by the keyboard.
 */

static KeySym
KeycodeToKeysym(
    unsigned int keycode,
    unsigned int state)
{
    struct xkb_keymap *keymap = tkWlDisplay ? tkWlDisplay->keymap : NULL;
    xkb_layout_index_t layout = 0;
    xkb_level_index_t level = 0;
    const xkb_keysym_t *syms;
    int n;

    if (keymap == NULL || keycode == 0) {
	return NoSymbol;
    }
    if (tkWlDisplay->xkbState) {
	layout = xkb_state_key_get_layout(tkWlDisplay->xkbState, keycode);
	if (layout == XKB_LAYOUT_INVALID) {
	    layout = 0;
	}
    }
    if (state & (ShiftMask | LockMask)) {
	level = 1;
    }
    if (state & Mod5Mask) {
	level += 2;
    }
    n = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, level,
	    &syms);
    if (n < 1 && level > 0) {
	n = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0,
		&syms);
    }
    return (n > 0) ? syms[0] : NoSymbol;
}

KeySym
TkpGetKeySym(
    TkDisplay *dispPtr,
    XEvent *eventPtr)
{
    TkKeyEvent *kePtr = (TkKeyEvent *)eventPtr;

    if (dispPtr->bindInfoStale) {
	TkpInitKeymapInfo(dispPtr);
    }
    if (kePtr->keysym != NoSymbol) {
	return kePtr->keysym;
    }
    return KeycodeToKeysym(eventPtr->xkey.keycode, eventPtr->xkey.state);
}

const char *
TkpGetString(
    TCL_UNUSED(TkWindow *),
    XEvent *eventPtr,
    Tcl_DString *dsPtr)
{
    TkKeyEvent *kePtr = (TkKeyEvent *)eventPtr;

    if (kePtr->charValuePtr == NULL) {
	char buf[16];
	int len = 0;

	if (eventPtr->type == KeyPress) {
	    KeySym keysym = kePtr->keysym;

	    if (keysym == NoSymbol) {
		keysym = KeycodeToKeysym(eventPtr->xkey.keycode,
			eventPtr->xkey.state);
	    }
	    len = xkb_keysym_to_utf8((xkb_keysym_t)keysym, buf, sizeof(buf));
	    if (len > 0) {
		len--;		/* The length includes the terminating 0. */
	    } else {
		len = 0;
	    }
	    if (len == 1 && (eventPtr->xkey.state & ControlMask)
		    && ((buf[0] >= '@' && buf[0] <= '_')
			|| (buf[0] >= 'a' && buf[0] <= 'z'))) {
		buf[0] &= 0x1F;
	    }
	}
	kePtr->charValuePtr = (char *)Tcl_Alloc(len + 1);
	memcpy(kePtr->charValuePtr, buf, len);
	kePtr->charValuePtr[len] = '\0';
	kePtr->charValueLen = len;
    }
    Tcl_DStringSetLength(dsPtr, kePtr->charValueLen);
    memcpy(Tcl_DStringValue(dsPtr), kePtr->charValuePtr,
	    kePtr->charValueLen + 1);
    return Tcl_DStringValue(dsPtr);
}

void
TkpSetKeycodeAndState(
    Tk_Window tkwin,
    KeySym keySym,
    XEvent *eventPtr)
{
    TkDisplay *dispPtr = ((TkWindow *)tkwin)->dispPtr;
    KeyCode keycode = (keySym == NoSymbol) ? 0
	    : XKeysymToKeycode(dispPtr->display, keySym);
    unsigned int state;

    eventPtr->xkey.keycode = keycode;
    if (keycode != 0) {
	for (state = 0; state < 4; state++) {
	    unsigned int mods = ((state & 1) ? ShiftMask : 0)
		    | ((state & 2) ? Mod5Mask : 0);

	    if (KeycodeToKeysym(keycode, mods) == keySym) {
		eventPtr->xkey.state |= mods;
		break;
	    }
	}
    }
    ((TkKeyEvent *)eventPtr)->keysym = keySym;
}

/*
 *----------------------------------------------------------------------
 *
 * Focus --
 *
 *	A Wayland client cannot take the keyboard focus, so the focus is only
 *	moved between the windows of the surface which has it.
 *
 *----------------------------------------------------------------------
 */

int
XSetInputFocus(
    Display *display,
    Window focus,
    TCL_UNUSED(int),
    TCL_UNUSED(Time))
{
    TkWlWindow *winPtr = TkWlGetWindow(focus);

    TkWlNextRequest(display);
    if (winPtr && tkWlDisplay->keyboardSurface
	    && TkWlTopWindow(winPtr) == tkWlDisplay->keyboardSurface->winPtr) {
	focusWindow = focus;
    }
    return Success;
}

int
XGetInputFocus(
    Display *display,
    Window *focusPtr,
    int *revertPtr)
{
    TkWlNextRequest(display);
    *focusPtr = TkWlGetWindow(focusWindow) ? focusWindow : None;
    *revertPtr = RevertToParent;
    return Success;
}

int
XGrabKeyboard(
    Display *display,
    TCL_UNUSED(Window),
    TCL_UNUSED(Bool),
    TCL_UNUSED(int),
    TCL_UNUSED(int),
    TCL_UNUSED(Time))
{
    TkWlNextRequest(display);
    return GrabSuccess;
}

int
XUngrabKeyboard(
    Display *display,
    TCL_UNUSED(Time))
{
    TkWlNextRequest(display);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * Pointer position and capture --
 *
 *----------------------------------------------------------------------
 */

static TkWindow *captureWinPtr = NULL;

void
TkpSetCapture(
    TkWindow *winPtr)
{
    captureWinPtr = winPtr;
}

Tk_Window
TkpGetCapture(void)
{
    return (Tk_Window)captureWinPtr;
}

Bool
XQueryPointer(
    Display *display,
    Window w,
    Window *rootPtr,
    Window *childPtr,
    int *rootXPtr, int *rootYPtr,
    int *winXPtr, int *winYPtr,
    unsigned int *maskPtr)
{
    TkWlSurface *surfPtr = tkWlDisplay->pointerSurface;
    int rootX = 0, rootY = 0, x, y;

    TkWlNextRequest(display);
    if (surfPtr) {
	SurfaceToRoot(surfPtr, (int)floor(tkWlDisplay->pointerX),
		(int)floor(tkWlDisplay->pointerY), &rootX, &rootY);
    }
    *rootPtr = RootWindow(display, 0);
    *childPtr = None;
    *rootXPtr = rootX;
    *rootYPtr = rootY;
    XTranslateCoordinates(display, *rootPtr, w, rootX, rootY, &x, &y,
	    childPtr);
    *winXPtr = x;
    *winYPtr = y;
    *maskPtr = CurrentState();
    return surfPtr != NULL;
}

/*
 * Wayland clients cannot move the pointer.
 */

int
XWarpPointer(
    Display *display,
    TCL_UNUSED(Window),
    TCL_UNUSED(Window),
    TCL_UNUSED(int),
    TCL_UNUSED(int),
    TCL_UNUSED(unsigned int),
    TCL_UNUSED(unsigned int),
    TCL_UNUSED(int),
    TCL_UNUSED(int))
{
    TkWlNextRequest(display);
    return Success;
}

void
TkpWarpPointer(
    TCL_UNUSED(TkDisplay *))
{
}

/*
 *----------------------------------------------------------------------
 *
 * XSendEvent --
 *
 *	Events can only be sent to the windows of this process.
 *
 *----------------------------------------------------------------------
 */

Status
XSendEvent(
    Display *display,
    Window w,
    TCL_UNUSED(Bool),
    TCL_UNUSED(long),
    XEvent *eventPtr)
{
    XEvent event = *eventPtr;

    TkWlNextRequest(display);
    if (w == PointerWindow || w == InputFocus) {
	w = focusWindow;
    }
    event.xany.display = display;
    if (w == RootWindow(display, 0) && TkWlWmClientMessage(&event)) {
	return 1;
    }
    if (TkWlGetWindow(w) == NULL) {
	return 0;
    }
    event.xany.display = display;
    event.xany.window = w;
    event.xany.serial = LastKnownRequestProcessed(display);
    event.xany.send_event = True;
    Tk_QueueWindowEvent(&event, TCL_QUEUE_TAIL);
    return 1;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
