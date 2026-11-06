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
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>
#include <linux/input-event-codes.h>

TkXeDisplay *tkXeDisplay = NULL;

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
static int		Dispatch(int timeout);
static void		RepeatProc(void *clientData);
static Tcl_ThreadCreateType ReaderProc(void *clientData);
static void		StartReader(TkXeDisplay *wlPtr);
static void		StopReader(TkXeDisplay *wlPtr);

/*
 *----------------------------------------------------------------------
 *
 * Outputs --
 *
 *	The size of the screen is that of the first output, in logical
 *	pixels: its size in the layout from xdg-output, which knows
 *	fractional scales (the scale of wl_output is an integer), or else the
 *	size of its mode divided by its scale.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * UpdateScreenSize --
 *
 *	Set the size of the screen of the X display to that of the first
 *	output, in logical pixels.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The size of the screen changes.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateScreenSize(void)
{
    TkWlOutput *outPtr = tkXeDisplay->outputs;
    int scale;

    if (outPtr == NULL || tkXeDisplay->display == NULL) {
	return;
    }
    if (outPtr->logicalWidth > 0 && outPtr->logicalHeight > 0) {
	TkXeSetScreenSize(tkXeDisplay->display, outPtr->logicalWidth,
		outPtr->logicalHeight, outPtr->mmWidth, outPtr->mmHeight);
	return;
    }
    if (outPtr->width <= 0) {
	return;
    }
    scale = (outPtr->scale > 0) ? outPtr->scale : 1;
    TkXeSetScreenSize(tkXeDisplay->display, outPtr->width / scale,
	    outPtr->height / scale, outPtr->mmWidth, outPtr->mmHeight);
}

/*
 *----------------------------------------------------------------------
 *
 * OutputGeometry, OutputMode, OutputDone, OutputScale,
 * OutputName, OutputDescription --
 *
 *	The events of a wl_output, whose physical size, mode and scale are
 *	recorded. When the compositor has sent all the properties of an
 *	output, the screen and the scales of the surfaces are updated.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The properties of the output change. OutputDone updates the size of
 *	the screen and the scale of all surfaces.
 *
 *----------------------------------------------------------------------
 */

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
    TkXeSurface *surfPtr;

    UpdateScreenSize();
    for (surfPtr = tkXeDisplay->surfaces; surfPtr; surfPtr = surfPtr->nextPtr) {
	TkWlUpdateScale(surfPtr);
    }
}

static void
OutputScale(
    void *data,
    TCL_UNUSED(struct wl_output *),
    int32_t factor)
{
    ((TkWlOutput *)data)->scale = factor;
}

#ifdef WL_OUTPUT_NAME_SINCE_VERSION
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
#endif

static const struct wl_output_listener outputListener = {
    OutputGeometry,
    OutputMode,
    OutputDone,
    OutputScale,
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
    OutputName,
    OutputDescription
#endif
};

/*
 *----------------------------------------------------------------------
 *
 * XdgOutputLogicalPosition, XdgOutputLogicalSize, XdgOutputDone,
 * XdgOutputName, XdgOutputDescription --
 *
 *	The events of the zxdg_output_v1 of an output, whose size in the
 *	layout is recorded. From version 3, the done event of the wl_output
 *	follows them.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The logical size of the output changes; XdgOutputDone updates the
 *	size of the screen.
 *
 *----------------------------------------------------------------------
 */

static void
XdgOutputLogicalPosition(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zxdg_output_v1 *),
    TCL_UNUSED(int32_t),	/* x */
    TCL_UNUSED(int32_t))	/* y */
{
}

static void
XdgOutputLogicalSize(
    void *data,
    TCL_UNUSED(struct zxdg_output_v1 *),
    int32_t width,
    int32_t height)
{
    TkWlOutput *outPtr = (TkWlOutput *)data;

    outPtr->logicalWidth = width;
    outPtr->logicalHeight = height;
}

static void
XdgOutputDone(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zxdg_output_v1 *))
{
    UpdateScreenSize();
}

static void
XdgOutputName(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zxdg_output_v1 *),
    TCL_UNUSED(const char *))
{
}

static void
XdgOutputDescription(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zxdg_output_v1 *),
    TCL_UNUSED(const char *))
{
}

static const struct zxdg_output_v1_listener xdgOutputListener = {
    XdgOutputLogicalPosition,
    XdgOutputLogicalSize,
    XdgOutputDone,
    XdgOutputName,
    XdgOutputDescription
};

/*
 *----------------------------------------------------------------------
 *
 * WatchXdgOutput --
 *
 *	Get the zxdg_output_v1 of an output, once both the output and the
 *	manager of xdg-output are bound (in either order).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The compositor sends the logical size of the output.
 *
 *----------------------------------------------------------------------
 */

static void
WatchXdgOutput(
    TkWlOutput *outPtr)
{
    if (tkXeDisplay->xdgOutputManager == NULL || outPtr->xdgOutput) {
	return;
    }
    outPtr->xdgOutput = zxdg_output_manager_v1_get_xdg_output(
	    tkXeDisplay->xdgOutputManager, outPtr->output);
    zxdg_output_v1_add_listener(outPtr->xdgOutput, &xdgOutputListener,
	    outPtr);
}

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
 *----------------------------------------------------------------------
 *
 * SurfaceToRoot --
 *
 *	Root coordinates of a point of a surface. Wayland clients do not know
 *	where their windows are, so the position of the window is the one Tk
 *	believes it has.
 *
 * Results:
 *	None; the coordinates are stored in *rootXPtr and *rootYPtr.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
SurfaceToRoot(
    TkXeSurface *surfPtr,
    int x, int y,
    int *rootXPtr, int *rootYPtr)
{
    TkXeWindow *winPtr = surfPtr->winPtr;

    *rootXPtr = winPtr->x + x;
    *rootYPtr = winPtr->y + y;
}

/*
 *----------------------------------------------------------------------
 *
 * WindowAtPointer --
 *
 *	The Tk window at a point of a surface: the innermost window there, or
 *	its nearest ancestor which is a Tk window.
 *
 * Results:
 *	The window, or NULL if there is none.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Tk_Window
WindowAtPointer(
    TkXeSurface *surfPtr,
    int x, int y)
{
    TkXeWindow *winPtr;
    Tk_Window tkwin;

    if (surfPtr == NULL) {
	return NULL;
    }
    winPtr = TkXeWindowAtPoint(surfPtr->winPtr,
	    x - surfPtr->winPtr->borderWidth, y - surfPtr->winPtr->borderWidth);
    for (; winPtr; winPtr = winPtr->parent) {
	tkwin = Tk_IdToWindow(winPtr->display, winPtr->id);
	if (tkwin) {
	    return tkwin;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * CurrentState --
 *
 *	The state of the modifiers and the buttons, as in the state of X
 *	events.
 *
 * Results:
 *	The state.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static unsigned int
CurrentState(void)
{
    return tkXeDisplay->modState | tkXeDisplay->buttonState;
}

/*
 *----------------------------------------------------------------------
 *
 * UpdatePointer --
 *
 *	Give the position of the pointer and a state to Tk_UpdatePointer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Tk_UpdatePointer generates the crossing, motion and button events of
 *	the change, and handles the grabs.
 *
 *----------------------------------------------------------------------
 */

static void
UpdatePointer(
    unsigned int state)
{
    TkXeSurface *surfPtr = tkXeDisplay->pointerSurface;
    int x, y, rootX, rootY;

    if (surfPtr == NULL) {
	Tk_UpdatePointer(NULL, 0, 0, state);
	return;
    }
    x = (int)floor(tkXeDisplay->pointerX);
    y = (int)floor(tkXeDisplay->pointerY);
    SurfaceToRoot(surfPtr, x, y, &rootX, &rootY);
    Tk_UpdatePointer(WindowAtPointer(surfPtr, x, y), rootX, rootY, state);
}

/*
 *----------------------------------------------------------------------
 *
 * PointerEnter, PointerLeave, PointerMotion,
 * PointerButton --
 *
 *	The events of the wl_pointer for its position and its buttons.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Updates the position, the surface and the buttons of the pointer, and
 *	generates the X events of the changes. PointerEnter sets the cursor of
 *	the surface; PointerLeave releases the buttons still pressed. The
 *	serial and time of the events are recorded for the requests which need
 *	them.
 *
 *----------------------------------------------------------------------
 */

static void
PointerEnter(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    uint32_t serial,
    struct wl_surface *surface,
    wl_fixed_t x,
    wl_fixed_t y)
{
    TkXeSurface *surfPtr = surface ? TkWlSurfaceFromWl(surface) : NULL;

    tkXeDisplay->pointerSerial = serial;
    tkXeDisplay->pointerSurface = surfPtr;
    tkXeDisplay->pointerX = wl_fixed_to_double(x);
    tkXeDisplay->pointerY = wl_fixed_to_double(y);
    TkXeUpdateCursor();
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
    /*
     * The pointer leaves with buttons pressed when the compositor takes it,
     * to move or resize the window or for its window menu: the buttons are
     * released then, since the client gets no release.
     */

    if (tkXeDisplay->buttonState && tkXeDisplay->pointerSurface) {
	tkXeDisplay->buttonState = 0;
	UpdatePointer(CurrentState());
    }
    tkXeDisplay->buttonState = 0;
    tkXeDisplay->pointerSurface = NULL;
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
    tkXeDisplay->lastTime = time;
    tkXeDisplay->pointerX = wl_fixed_to_double(x);
    tkXeDisplay->pointerY = wl_fixed_to_double(y);
    if (tkXeDisplay->pointerSurface) {
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

    tkXeDisplay->lastSerial = serial;
    tkXeDisplay->lastTime = time;
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
	tkXeDisplay->buttonState |= mask;
    } else {
	tkXeDisplay->buttonState &= ~mask;
    }
    UpdatePointer(CurrentState());
}

/*
 * Scrolling: wheels generate MouseWheel events, of 120 units per notch,
 * and touchpads TouchpadScroll events, as on Aqua. The amounts of a frame
 * of pointer events are accumulated here.
 */

static uint32_t axisSource = WL_POINTER_AXIS_SOURCE_WHEEL;
static int axisValue120[2];
static double axisValue[2];

/*
 *----------------------------------------------------------------------
 *
 * PointerAxis, PointerAxisSource, PointerAxisStop,
 * PointerAxisDiscrete, PointerAxisValue120,
 * PointerAxisRelativeDirection --
 *
 *	The scroll events of the wl_pointer, which are accumulated until the
 *	end of the frame (see PointerFrame).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The amounts and the source of the scrolling of the frame change.
 *
 *----------------------------------------------------------------------
 */

static void
PointerAxis(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    uint32_t time,
    uint32_t axis,
    wl_fixed_t value)
{
    tkXeDisplay->lastTime = time;
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

#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
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

#endif

#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
static void
PointerAxisRelativeDirection(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_pointer *),
    TCL_UNUSED(uint32_t),
    TCL_UNUSED(uint32_t))
{
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * QueueScrollEvent --
 *
 *	Queue a MouseWheel or TouchpadScroll event, with its amount in the
 *	keycode, for the window with the pointer capture, or else the window
 *	under the pointer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is queued.
 *
 *----------------------------------------------------------------------
 */

static void
QueueScrollEvent(
    int type,
    unsigned int state,
    unsigned int value)
{
    TkXeSurface *surfPtr = tkXeDisplay->pointerSurface;
    Tk_Window tkwin, capture = TkpGetCapture();
    int x = (int)floor(tkXeDisplay->pointerX);
    int y = (int)floor(tkXeDisplay->pointerY);
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
    event.xbutton.time = tkXeDisplay->lastTime;
    event.xkey.keycode = value;
    Tk_QueueWindowEvent(&event, TCL_QUEUE_TAIL);
}

/*
 *----------------------------------------------------------------------
 *
 * PointerFrame --
 *
 *	The end of a frame of events of the wl_pointer: the scrolling
 *	accumulated during the frame is turned into MouseWheel events (wheels)
 *	or TouchpadScroll events (touchpads).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Scroll events are queued, and the accumulated scrolling is reset.
 *
 *----------------------------------------------------------------------
 */

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

/*
 * The last events are in newer versions of the protocol only.
 */

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
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    PointerAxisValue120,
#endif
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
    PointerAxisRelativeDirection
#endif
};

/*
 *----------------------------------------------------------------------
 *
 * Touch events --
 *
 *	The fingers on a touchscreen are given to the gesture recognizer of
 *	tkTouch.c, which turns them into clicks, scrolling and pinches.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * TouchSlot --
 *
 *	Find the slot of a touch point, or a free one.
 *
 * Results:
 *	The index of the slot, or -1.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
TouchSlot(
    int32_t id)			/* Id of the touch point, or -1 for a free
				 * slot. */
{
    int i;

    for (i = 0; i < TKWL_MAX_TOUCHES; i++) {
	if (tkXeDisplay->touches[i].id == id) {
	    return i;
	}
    }
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * Touch --
 *
 *	Give a touch at a point of a surface to the gesture recognizer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	See TkTouchEvent.
 *
 *----------------------------------------------------------------------
 */

static void
Touch(
    int type,			/* TK_TOUCH_BEGIN etc. */
    TkXeSurface *surfPtr,	/* The surface touched, or NULL. */
    int32_t id,			/* Id of the touch point. */
    double x, double y,		/* Position in the surface. */
    uint32_t time)		/* Time of the event. */
{
    Tk_Window tkwin = NULL;
    double rootX = x, rootY = y;

    if (surfPtr) {
	tkwin = WindowAtPointer(surfPtr, (int)floor(x), (int)floor(y));
	rootX += surfPtr->winPtr->x;
	rootY += surfPtr->winPtr->y;
    }
    TkTouchEvent(type, tkwin, id, rootX, rootY, time, tkXeDisplay->modState);
}

/*
 *----------------------------------------------------------------------
 *
 * TouchDown, TouchUp, TouchMotion --
 *
 *	A finger touches the screen, is lifted, or moves.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The touch is given to the gesture recognizer.
 *
 *----------------------------------------------------------------------
 */

static void
TouchDown(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_touch *),
    uint32_t serial,
    uint32_t time,
    struct wl_surface *surface,
    int32_t id,
    wl_fixed_t x,
    wl_fixed_t y)
{
    TkXeSurface *surfPtr = surface ? TkWlSurfaceFromWl(surface) : NULL;
    int slot = TouchSlot(-1);

    tkXeDisplay->lastSerial = serial;
    tkXeDisplay->lastTime = time;
    if (slot < 0) {
	return;
    }
    tkXeDisplay->touches[slot].id = id;
    tkXeDisplay->touches[slot].surfPtr = surfPtr;
    Touch(TK_TOUCH_BEGIN, surfPtr, id, wl_fixed_to_double(x),
	    wl_fixed_to_double(y), time);
}

static void
TouchUp(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_touch *),
    uint32_t serial,
    uint32_t time,
    int32_t id)
{
    int slot = TouchSlot(id);

    tkXeDisplay->lastSerial = serial;
    tkXeDisplay->lastTime = time;
    if (slot < 0) {
	return;
    }
    tkXeDisplay->touches[slot].id = -1;
    tkXeDisplay->touches[slot].surfPtr = NULL;

    /*
     * The finger is lifted where it last moved, which tkTouch.c knows.
     */

    TkTouchEvent(TK_TOUCH_END, NULL, id, 0.0, 0.0, time,
	    tkXeDisplay->modState);
}

static void
TouchMotion(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_touch *),
    uint32_t time,
    int32_t id,
    wl_fixed_t x,
    wl_fixed_t y)
{
    int slot = TouchSlot(id);

    tkXeDisplay->lastTime = time;
    if (slot < 0) {
	return;
    }
    Touch(TK_TOUCH_MOVE, tkXeDisplay->touches[slot].surfPtr, id,
	    wl_fixed_to_double(x), wl_fixed_to_double(y), time);
}

/*
 *----------------------------------------------------------------------
 *
 * TouchFrame, TouchCancel, TouchShape, TouchOrientation --
 *
 *	The end of a frame of touch events, which are handled one by one;
 *	the compositor took the touches for a gesture of its own; the shape
 *	and orientation of a touch, which are not used.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	TouchCancel cancels the touches.
 *
 *----------------------------------------------------------------------
 */

static void
TouchFrame(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_touch *))
{
}

static void
TouchCancel(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_touch *))
{
    int i;

    for (i = 0; i < TKWL_MAX_TOUCHES; i++) {
	if (tkXeDisplay->touches[i].id >= 0) {
	    TkTouchEvent(TK_TOUCH_CANCEL, NULL, tkXeDisplay->touches[i].id,
		    0.0, 0.0, tkXeDisplay->lastTime, tkXeDisplay->modState);
	    tkXeDisplay->touches[i].id = -1;
	    tkXeDisplay->touches[i].surfPtr = NULL;
	}
    }
}

#ifdef WL_TOUCH_SHAPE_SINCE_VERSION
static void
TouchShape(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_touch *),
    TCL_UNUSED(int32_t),
    TCL_UNUSED(wl_fixed_t),
    TCL_UNUSED(wl_fixed_t))
{
}

static void
TouchOrientation(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_touch *),
    TCL_UNUSED(int32_t),
    TCL_UNUSED(wl_fixed_t))
{
}
#endif

static const struct wl_touch_listener touchListener = {
    TouchDown,
    TouchUp,
    TouchMotion,
    TouchFrame,
    TouchCancel,
#ifdef WL_TOUCH_SHAPE_SINCE_VERSION
    TouchShape,
    TouchOrientation
#endif
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

/*
 *----------------------------------------------------------------------
 *
 * InitModIndices --
 *
 *	Find the indices in a keymap of the modifiers which correspond to the
 *	X modifiers Shift, Lock, Control and Mod1 to Mod5.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The indices of the modifiers are set.
 *
 *----------------------------------------------------------------------
 */

static void
InitModIndices(
    struct xkb_keymap *keymap)
{
    int i;

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
}

/*
 *----------------------------------------------------------------------
 *
 * KeyboardKeymap --
 *
 *	The keymap event of the wl_keyboard: the keymap of the keyboard, which
 *	the compositor gives as a file descriptor.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Replaces the keymap and the keyboard state, and marks the key bindings
 *	of all displays as stale. The file descriptor is closed.
 *
 *----------------------------------------------------------------------
 */

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

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
	close(fd);
	return;
    }
    map = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
	close(fd);
	return;
    }
    keymap = xkb_keymap_new_from_string(tkXeDisplay->xkbContext, map,
	    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (keymap == NULL) {
	return;
    }
    if (tkXeDisplay->xkbState) {
	xkb_state_unref(tkXeDisplay->xkbState);
    }
    if (tkXeDisplay->keymap) {
	xkb_keymap_unref(tkXeDisplay->keymap);
    }
    tkXeDisplay->keymap = keymap;
    tkXeDisplay->xkbState = xkb_state_new(keymap);
    InitModIndices(keymap);
    if (tkXeDisplay->display) {
	TkDisplay *dispPtr = TkGetDisplayList();

	for (; dispPtr; dispPtr = dispPtr->nextPtr) {
	    dispPtr->bindInfoStale = 1;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ModState --
 *
 *	The state of the X modifiers, from the state of the keyboard.
 *
 * Results:
 *	The modifier bits of the state of X events.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static unsigned int
ModState(void)
{
    unsigned int state = 0;
    int i;

    if (tkXeDisplay->xkbState == NULL) {
	return 0;
    }
    for (i = 0; i < 8; i++) {
	if (modIndices[i] != XKB_MOD_INVALID
		&& xkb_state_mod_index_is_active(tkXeDisplay->xkbState,
		    modIndices[i], XKB_STATE_MODS_EFFECTIVE) > 0) {
	    state |= 1u << i;
	}
    }
    return state;
}

/*
 *----------------------------------------------------------------------
 *
 * QueueFocusEvent --
 *
 *	Queue a FocusIn or FocusOut event for a window.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is queued.
 *
 *----------------------------------------------------------------------
 */

static void
QueueFocusEvent(
    int type,
    Window w)
{
    XEvent event;

    memset(&event, 0, sizeof(XEvent));
    event.xfocus.type = type;
    event.xfocus.display = tkXeDisplay->display;
    event.xfocus.window = w;
    event.xfocus.mode = NotifyNormal;
    event.xfocus.detail = NotifyNonlinear;
    TkXeQueueEvent(&event);
}

/*
 *----------------------------------------------------------------------
 *
 * KeyboardEnter, KeyboardLeave --
 *
 *	The events of the wl_keyboard when the keyboard enters a surface or
 *	leaves it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Updates the surface which has the keyboard and the window which has
 *	the focus, and queues the focus events. Entering an iconified window
 *	deiconifies it; leaving stops the repeat of keys.
 *
 *----------------------------------------------------------------------
 */

static void
KeyboardEnter(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_keyboard *),
    uint32_t serial,
    struct wl_surface *surface,
    TCL_UNUSED(struct wl_array *))
{
    TkXeSurface *surfPtr = surface ? TkWlSurfaceFromWl(surface) : NULL;

    tkXeDisplay->lastSerial = serial;
    tkXeDisplay->keyboardSurface = surfPtr;
    if (surfPtr == NULL) {
	return;
    }
    if (surfPtr->iconic && surfPtr->minimizeSync == NULL) {
	/*
	 * The user deiconified the window (once the compositor has seen it
	 * minimized: the keyboard can enter it before).
	 */

	XMapWindow(surfPtr->winPtr->display, surfPtr->winPtr->id);
    }
    if (TkXeGetWindow(focusWindow) && focusWindow != surfPtr->winPtr->id) {
	QueueFocusEvent(FocusOut, focusWindow);
    }
    focusWindow = surfPtr->winPtr->id;
    QueueFocusEvent(FocusIn, focusWindow);
}

static void
KeyboardLeave(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_keyboard *),
    uint32_t serial,
    TCL_UNUSED(struct wl_surface *))
{
    tkXeDisplay->lastSerial = serial;
    tkXeDisplay->keyboardSurface = NULL;
    if (tkXeDisplay->repeatTimer) {
	Tcl_DeleteTimerHandler(tkXeDisplay->repeatTimer);
	tkXeDisplay->repeatTimer = NULL;
    }

    /*
     * The focus may be in another toplevel than the one the compositor gave
     * the keyboard to (see XSetInputFocus).
     */

    if (TkXeGetWindow(focusWindow)) {
	QueueFocusEvent(FocusOut, focusWindow);
    }
    focusWindow = None;
}

/*
 *----------------------------------------------------------------------
 *
 * QueueKeyEvent --
 *
 *	Generate a key event for a key of the keyboard (an evdev code), for
 *	the window which has the focus. The text of a KeyPress, with compose
 *	sequences, is stored in the event, as TkpGetString would compute it
 *	with an input method.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is queued. The state of the compose sequence changes.
 *
 *----------------------------------------------------------------------
 */

static void
QueueKeyEvent(
    int type,
    uint32_t key)
{
    TkXeSurface *surfPtr = tkXeDisplay->keyboardSurface;
    xkb_keycode_t keycode = key + 8;
    xkb_keysym_t keysym;
    union {
	XEvent x;
	TkKeyEvent k;
    } event;
    char buf[64];
    int len = 0, x, y, rootX, rootY;

    if (surfPtr == NULL || tkXeDisplay->xkbState == NULL) {
	return;
    }
    keysym = xkb_state_key_get_one_sym(tkXeDisplay->xkbState, keycode);
    if (type == KeyPress) {
	struct xkb_compose_state *compose = tkXeDisplay->composeState;
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
	    len = xkb_state_key_get_utf8(tkXeDisplay->xkbState, keycode, buf,
		    sizeof(buf));
	    break;
	}
	if (len < 0 || len >= (int)sizeof(buf)) {
	    len = 0;
	}
    }

    x = (int)floor(tkXeDisplay->pointerX);
    y = (int)floor(tkXeDisplay->pointerY);
    if (tkXeDisplay->pointerSurface != surfPtr) {
	x = y = 0;
    }
    SurfaceToRoot(surfPtr, x, y, &rootX, &rootY);

    memset(&event, 0, sizeof(event));
    event.x.xkey.type = type;
    event.x.xkey.display = surfPtr->winPtr->display;
    event.x.xkey.window = TkXeGetWindow(focusWindow)
	    ? focusWindow : surfPtr->winPtr->id;
    event.x.xkey.root = RootWindow(event.x.xkey.display, 0);
    event.x.xkey.time = tkXeDisplay->lastTime;
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
    TkXeQueueEvent(&event.x);
}

/*
 *----------------------------------------------------------------------
 *
 * KeyboardKey --
 *
 *	The key event of the wl_keyboard.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Queues a KeyPress or KeyRelease event, and starts or stops the repeat
 *	of the key.
 *
 *----------------------------------------------------------------------
 */

static void
KeyboardKey(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_keyboard *),
    uint32_t serial,
    uint32_t time,
    uint32_t key,
    uint32_t state)
{
    tkXeDisplay->lastSerial = serial;
    tkXeDisplay->lastTime = time;
    if (tkXeDisplay->repeatTimer) {
	Tcl_DeleteTimerHandler(tkXeDisplay->repeatTimer);
	tkXeDisplay->repeatTimer = NULL;
    }
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
	QueueKeyEvent(KeyPress, key);
	if (tkXeDisplay->keymap && tkXeDisplay->repeatRate > 0
		&& xkb_keymap_key_repeats(tkXeDisplay->keymap, key + 8)) {
	    tkXeDisplay->repeatKey = key;
	    tkXeDisplay->repeatTimer = Tcl_CreateTimerHandler(
		    tkXeDisplay->repeatDelay, RepeatProc, NULL);
	}
    } else {
	QueueKeyEvent(KeyRelease, key);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * RepeatProc --
 *
 *	Timer procedure repeating a key. Key repeat is done by the client in
 *	Wayland. As on X11 with detectable auto repeat, only KeyPress events
 *	are repeated.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Queues a KeyPress event and schedules the next repeat.
 *
 *----------------------------------------------------------------------
 */

static void
RepeatProc(
    TCL_UNUSED(void *))
{
    tkXeDisplay->repeatTimer = Tcl_CreateTimerHandler(
	    1000 / tkXeDisplay->repeatRate, RepeatProc, NULL);
    QueueKeyEvent(KeyPress, tkXeDisplay->repeatKey);
}

/*
 *----------------------------------------------------------------------
 *
 * KeyboardModifiers, KeyboardRepeatInfo --
 *
 *	The events of the wl_keyboard giving the state of the modifiers, and
 *	the rate and the delay of the repeat of keys.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The state of the keyboard, or the parameters of the repeat, change.
 *
 *----------------------------------------------------------------------
 */

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
    tkXeDisplay->lastSerial = serial;
    if (tkXeDisplay->xkbState) {
	xkb_state_update_mask(tkXeDisplay->xkbState, depressed, latched,
		locked, 0, 0, group);
	tkXeDisplay->modState = ModState();
    }
}

static void
KeyboardRepeatInfo(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_keyboard *),
    int32_t rate,
    int32_t delay)
{
    tkXeDisplay->repeatRate = rate;
    tkXeDisplay->repeatDelay = delay;
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

/*
 *----------------------------------------------------------------------
 *
 * SeatCapabilities, SeatName --
 *
 *	The events of the wl_seat: its capabilities, the pointer and the
 *	keyboard it has, and its name, which is not used.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	SeatCapabilities gets or releases the pointer and the keyboard of the
 *	seat.
 *
 *----------------------------------------------------------------------
 */

static void
SeatCapabilities(
    TCL_UNUSED(void *),
    struct wl_seat *seat,
    uint32_t caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !tkXeDisplay->pointer) {
	tkXeDisplay->pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(tkXeDisplay->pointer, &pointerListener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && tkXeDisplay->pointer) {
	wl_pointer_release(tkXeDisplay->pointer);
	tkXeDisplay->pointer = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !tkXeDisplay->keyboard) {
	tkXeDisplay->keyboard = wl_seat_get_keyboard(seat);
	wl_keyboard_add_listener(tkXeDisplay->keyboard, &keyboardListener,
		NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD)
	    && tkXeDisplay->keyboard) {
	wl_keyboard_release(tkXeDisplay->keyboard);
	tkXeDisplay->keyboard = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !tkXeDisplay->touch) {
	tkXeDisplay->touch = wl_seat_get_touch(seat);
	wl_touch_add_listener(tkXeDisplay->touch, &touchListener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && tkXeDisplay->touch) {
	TouchCancel(NULL, tkXeDisplay->touch);
	wl_touch_release(tkXeDisplay->touch);
	tkXeDisplay->touch = NULL;
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

/*
 *----------------------------------------------------------------------
 *
 * WmBasePing --
 *
 *	The ping event of the xdg_wm_base, by which the compositor checks that
 *	the application responds.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A pong is sent.
 *
 *----------------------------------------------------------------------
 */

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

/*
 * The versions of the interfaces whose events this file handles depend on
 * the version of the header.
 */

#if defined(WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
#   define SEAT_VERSION 9
#elif defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
#   define SEAT_VERSION 8
#else
#   define SEAT_VERSION 7
#endif
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
#   define OUTPUT_VERSION 4
#else
#   define OUTPUT_VERSION 3
#endif
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
#   define COMPOSITOR_VERSION 6
#else
#   define COMPOSITOR_VERSION 5
#endif

/*
 *----------------------------------------------------------------------
 *
 * RegistryGlobal --
 *
 *	The global event of the wl_registry: bind the interfaces Tk uses, in
 *	the versions the headers support.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The globals are bound; outputs are added to the list of outputs.
 *	Compositors whose globals start with "weston_" are recorded as Weston.
 *
 *----------------------------------------------------------------------
 */

static void
RegistryGlobal(
    TCL_UNUSED(void *),
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
#define MIN(a, b) ((a) < (b) ? (a) : (b))
    if (!strncmp(interface, "weston_", 7)) {
	tkXeDisplay->weston = true;
    }
    if (TkWlSelectionBind(registry, name, interface, version)
	    || TkWlTextInputBind(registry, name, interface)
	    || TkWlCmdsBind(registry, name, interface)) {
	return;
    }
    if (!strcmp(interface, wl_compositor_interface.name)) {
	tkXeDisplay->compositor = (struct wl_compositor *)wl_registry_bind(
		registry, name, &wl_compositor_interface,
		MIN(version, COMPOSITOR_VERSION));
    } else if (!strcmp(interface, wl_shm_interface.name)) {
	tkXeDisplay->shm = (struct wl_shm *)wl_registry_bind(registry, name,
		&wl_shm_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name)
	    && tkXeDisplay->seat == NULL) {
	tkXeDisplay->seat = (struct wl_seat *)wl_registry_bind(registry, name,
		&wl_seat_interface, MIN(version, SEAT_VERSION));
	wl_seat_add_listener(tkXeDisplay->seat, &seatListener, NULL);
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
	tkXeDisplay->viewporter = (struct wp_viewporter *)wl_registry_bind(
		registry, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(interface,
	    wp_fractional_scale_manager_v1_interface.name)) {
	tkXeDisplay->fractionalScaleManager =
		(struct wp_fractional_scale_manager_v1 *)wl_registry_bind(
		registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (!strcmp(interface,
	    xdg_toplevel_icon_manager_v1_interface.name)) {
	tkXeDisplay->iconManager = (struct xdg_toplevel_icon_manager_v1 *)
		wl_registry_bind(registry, name,
		&xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (!strcmp(interface, xdg_activation_v1_interface.name)) {
	tkXeDisplay->activation = (struct xdg_activation_v1 *)
		wl_registry_bind(registry, name, &xdg_activation_v1_interface,
		1);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
	tkXeDisplay->wmBase = (struct xdg_wm_base *)wl_registry_bind(
		registry, name, &xdg_wm_base_interface, 1);
	xdg_wm_base_add_listener(tkXeDisplay->wmBase, &wmBaseListener, NULL);
    } else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
	TkWlOutput *outPtr;

	tkXeDisplay->xdgOutputManager = (struct zxdg_output_manager_v1 *)
		wl_registry_bind(registry, name,
		&zxdg_output_manager_v1_interface, MIN(version, 3));
	for (outPtr = tkXeDisplay->outputs; outPtr; outPtr = outPtr->nextPtr) {
	    WatchXdgOutput(outPtr);
	}
    } else if (!strcmp(interface, wl_output_interface.name)) {
	TkWlOutput *outPtr = (TkWlOutput *)Tcl_Alloc(sizeof(TkWlOutput));
	TkWlOutput **lastPtrPtr = &tkXeDisplay->outputs;

	memset(outPtr, 0, sizeof(TkWlOutput));
	outPtr->name = name;
	outPtr->scale = 1;
	outPtr->output = (struct wl_output *)wl_registry_bind(registry, name,
		&wl_output_interface, MIN(version, OUTPUT_VERSION));
	wl_output_add_listener(outPtr->output, &outputListener, outPtr);
	WatchXdgOutput(outPtr);
	while (*lastPtrPtr) {
	    lastPtrPtr = &(*lastPtrPtr)->nextPtr;
	}
	*lastPtrPtr = outPtr;
    }
#undef MIN
}

/*
 *----------------------------------------------------------------------
 *
 * RegistryGlobalRemove --
 *
 *	The global_remove event of the wl_registry. Only outputs are removed
 *	at run time.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A removed output is released, and the size of the screen updated.
 *
 *----------------------------------------------------------------------
 */

static void
RegistryGlobalRemove(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_registry *),
    uint32_t name)
{
    TkWlOutput **outPtrPtr;

    for (outPtrPtr = &tkXeDisplay->outputs; *outPtrPtr;
	    outPtrPtr = &(*outPtrPtr)->nextPtr) {
	TkWlOutput *outPtr = *outPtrPtr;

	if (outPtr->name == name) {
	    *outPtrPtr = outPtr->nextPtr;
	    if (outPtr->xdgOutput) {
		zxdg_output_v1_destroy(outPtr->xdgOutput);
	    }
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

/*
 *----------------------------------------------------------------------
 *
 * DecorError --
 *
 *	The error callback of libdecor.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The message is printed on stderr.
 *
 *----------------------------------------------------------------------
 */

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
 * Results:
 *	The new display, or NULL if the connection fails, or if the process
 *	already has one which belongs to a display.
 *
 * Side effects:
 *	Connects to the compositor, binds its globals, and creates the
 *	emulated X display, libdecor, the selections and the text input.
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
    int i;

    if (tkXeDisplay) {
	/*
	 * Only one connection per process; any name refers to it.
	 */

	if (tkXeDisplay->display == NULL) {
	    return NULL;
	}
	for (dispPtr = TkGetDisplayList(); dispPtr;
		dispPtr = dispPtr->nextPtr) {
	    if (dispPtr->display == tkXeDisplay->display) {
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

    tkXeDisplay = (TkXeDisplay *)Tcl_Alloc(sizeof(TkXeDisplay));
    memset(tkXeDisplay, 0, sizeof(TkXeDisplay));
    tkXeDisplay->wlDisplay = wlDisplay;
    for (i = 0; i < TKWL_MAX_TOUCHES; i++) {
	tkXeDisplay->touches[i].id = -1;
    }
    tkXeDisplay->repeatRate = 25;
    tkXeDisplay->repeatDelay = 600;

    /*
     * TK_WAYLAND_SCALE forces the scale of the surfaces, for testing.
     */

    if (getenv("TK_WAYLAND_SCALE")) {
	tkXeDisplay->forcedScale = atof(getenv("TK_WAYLAND_SCALE"));
    }
    tkXeDisplay->xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
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
    tkXeDisplay->composeTable = xkb_compose_table_new_from_locale(
	    tkXeDisplay->xkbContext, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (tkXeDisplay->composeTable) {
	tkXeDisplay->composeState = xkb_compose_state_new(
		tkXeDisplay->composeTable, XKB_COMPOSE_STATE_NO_FLAGS);
    }

    tkXeDisplay->registry = wl_display_get_registry(wlDisplay);
    wl_registry_add_listener(tkXeDisplay->registry, &registryListener, NULL);
    wl_display_roundtrip(wlDisplay);
    if (!tkXeDisplay->compositor || !tkXeDisplay->shm) {
	wl_display_disconnect(wlDisplay);
	Tcl_Free(tkXeDisplay);
	tkXeDisplay = NULL;
	return NULL;
    }

    /*
     * Get the outputs, the keymap and the capabilities of the seat.
     */

    wl_display_roundtrip(wlDisplay);
    wl_display_roundtrip(wlDisplay);

    tkXeDisplay->decor = libdecor_new(wlDisplay, &decorInterface);
    TkWlSelectionInit();
    TkWlTextInputInit();

    /*
     * Without a keyboard (as on a headless compositor), use the default
     * keymap, so that keysyms can still be mapped to keycodes, for [event
     * generate].
     */

    if (tkXeDisplay->keymap == NULL) {
	tkXeDisplay->keymap = xkb_keymap_new_from_names(
		tkXeDisplay->xkbContext, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (tkXeDisplay->keymap) {
	    tkXeDisplay->xkbState = xkb_state_new(tkXeDisplay->keymap);
	    InitModIndices(tkXeDisplay->keymap);
	}
    }

    outPtr = tkXeDisplay->outputs;
    tkXeDisplay->display = TkXeOpenXDisplay(
	    displayNameStr ? displayNameStr : "wayland-0",
	    wl_display_get_fd(wlDisplay), 1920, 1080, 0, 0);
    TkXeSetScreenSize(tkXeDisplay->display, 1920, 1080, 0, 0);
    if (outPtr) {
	UpdateScreenSize();
    }

    dispPtr = (TkDisplay *)Tcl_Alloc(sizeof(TkDisplay));
    memset(dispPtr, 0, sizeof(TkDisplay));
    dispPtr->display = tkXeDisplay->display;
    StartReader(tkXeDisplay);
    Tcl_CreateFileHandler(TkWlEventFd(), TCL_READABLE, DisplayFileProc,
	    dispPtr);
    TkpInitKeymapInfo(dispPtr);
    return dispPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkpCloseDisplay --
 *
 *	Close a display. The connection to the compositor stays open, since
 *	there is only one per process.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Cleans up the send and window manager data of the display, stops
 *	reading the connection (and the reader thread), and flushes it.
 *
 *----------------------------------------------------------------------
 */

void
TkpCloseDisplay(
    TkDisplay *dispPtr)
{
    TkSendCleanup(dispPtr);
    TkWmCleanup(dispPtr);
    if (dispPtr->display != NULL && tkXeDisplay
	    && dispPtr->display == tkXeDisplay->display) {
	Tcl_DeleteFileHandler(TkWlEventFd());
	StopReader(tkXeDisplay);
	wl_display_flush(tkXeDisplay->wlDisplay);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkClipCleanup --
 *
 *	Clean up the clipboard of a display; the selections of the Wayland
 *	port have nothing per display.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

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
 *	A thread reads the events of the connection as soon as the compositor
 *	sends them, and queues them in libwayland for the main thread, which
 *	dispatches them. Compositors disconnect a client whose events they
 *	cannot send: Weston when 4 KiB of them are waiting, which motions of
 *	the pointer fill in a few seconds. So without the thread, Tk would be
 *	killed when busy for a few seconds, as when a script waits on a
 *	blocking socket, or runs exec. The thread only reads, so the
 *	compositor can still tell that Tk does not respond (to its pings).
 *
 *	The reader writes to a pipe after reading events; Tk waits on the pipe
 *	instead of the connection. libwayland lets threads read a connection
 *	together (wl_display_prepare_read), so the code reading it in the main
 *	thread (wl_display_roundtrip, libdecor) still works. Requests are
 *	flushed before Tcl waits for events.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * StartReader, StopReader --
 *
 *	Start and stop the thread reading the connection. If it cannot be
 *	started, Tk reads the connection itself.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The thread and its pipes are created, or stopped and closed.
 *
 *----------------------------------------------------------------------
 */

static void
StartReader(
    TkXeDisplay *wlPtr)
{
    int i;

    wlPtr->wakeFds[0] = wlPtr->wakeFds[1] = -1;
    wlPtr->stopFds[0] = wlPtr->stopFds[1] = -1;
    if (pipe(wlPtr->wakeFds) < 0) {
	wlPtr->wakeFds[0] = wlPtr->wakeFds[1] = -1;
	return;
    }
    if (pipe(wlPtr->stopFds) < 0) {
	goto error;
    }
    for (i = 0; i < 2; i++) {
	fcntl(wlPtr->wakeFds[i], F_SETFD, FD_CLOEXEC);
	fcntl(wlPtr->wakeFds[i], F_SETFL, O_NONBLOCK);
	fcntl(wlPtr->stopFds[i], F_SETFD, FD_CLOEXEC);
    }
    wlPtr->readerQueue = wl_display_create_queue(wlPtr->wlDisplay);
    if (wlPtr->readerQueue && Tcl_CreateThread(&wlPtr->reader, ReaderProc,
	    wlPtr, TCL_THREAD_STACK_DEFAULT, TCL_THREAD_JOINABLE) == TCL_OK) {
	return;
    }

  error:
    if (wlPtr->readerQueue) {
	wl_event_queue_destroy(wlPtr->readerQueue);
	wlPtr->readerQueue = NULL;
    }
    for (i = 0; i < 2; i++) {
	if (wlPtr->wakeFds[i] >= 0) {
	    close(wlPtr->wakeFds[i]);
	}
	if (wlPtr->stopFds[i] >= 0) {
	    close(wlPtr->stopFds[i]);
	}
	wlPtr->wakeFds[i] = wlPtr->stopFds[i] = -1;
    }
}

static void
StopReader(
    TkXeDisplay *wlPtr)
{
    int i, result;

    if (wlPtr->readerQueue == NULL) {
	return;
    }
    while (write(wlPtr->stopFds[1], "", 1) < 0 && errno == EINTR) {
	/* Retry. */
    }
    Tcl_JoinThread(wlPtr->reader, &result);
    wl_event_queue_destroy(wlPtr->readerQueue);
    wlPtr->readerQueue = NULL;
    for (i = 0; i < 2; i++) {
	close(wlPtr->wakeFds[i]);
	close(wlPtr->stopFds[i]);
	wlPtr->wakeFds[i] = wlPtr->stopFds[i] = -1;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ReaderProc --
 *
 *	The thread reading the connection: it waits for it to be readable,
 *	reads the events, and writes to the wake pipe, until it is stopped or
 *	the connection fails. It prepares its reads on its own queue, which
 *	stays empty, so that the events waiting for the main thread do not
 *	stop it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The events are queued for the main thread, and the wake pipe is
 *	written.
 *
 *----------------------------------------------------------------------
 */

static Tcl_ThreadCreateType
ReaderProc(
    void *clientData)
{
    TkXeDisplay *wlPtr = (TkXeDisplay *)clientData;
    struct pollfd pfd[2];
    bool failed = false;

    pfd[0].fd = wl_display_get_fd(wlPtr->wlDisplay);
    pfd[0].events = POLLIN;
    pfd[1].fd = wlPtr->stopFds[0];
    pfd[1].events = POLLIN;
    while (!failed) {
	if (wl_display_prepare_read_queue(wlPtr->wlDisplay,
		wlPtr->readerQueue) != 0) {
	    wl_display_dispatch_queue_pending(wlPtr->wlDisplay,
		    wlPtr->readerQueue);
	    continue;
	}
	if (poll(pfd, 2, -1) < 0) {
	    wl_display_cancel_read(wlPtr->wlDisplay);
	    if (errno == EINTR) {
		continue;
	    }
	    failed = true;
	} else if (pfd[1].revents) {
	    wl_display_cancel_read(wlPtr->wlDisplay);
	    break;
	} else if (pfd[0].revents) {
	    failed = wl_display_read_events(wlPtr->wlDisplay) < 0;
	} else {
	    wl_display_cancel_read(wlPtr->wlDisplay);
	    continue;
	}

	/*
	 * Wake the main thread, which finds the error of the connection if
	 * it failed. The pipe may be full, which wakes it as well.
	 */

	while (write(wlPtr->wakeFds[1], "", 1) < 0 && errno == EINTR) {
	    /* Retry. */
	}
    }
    TCL_THREAD_CREATE_RETURN;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlEventFd --
 *
 *	The file descriptor to wait on for the events of the compositor: the
 *	wake pipe, or the connection if there is no reader thread.
 *
 * Results:
 *	The file descriptor.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
TkWlEventFd(void)
{
    if (tkXeDisplay->readerQueue) {
	return tkXeDisplay->wakeFds[0];
    }
    return wl_display_get_fd(tkXeDisplay->wlDisplay);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlDispatchEvents --
 *
 *	Dispatch the events read by the reader thread, and those waiting to be
 *	read, when the file descriptor of TkWlEventFd is readable, and before
 *	Tcl waits for events.
 *
 * Results:
 *	The number of events dispatched, or -1 if the connection failed, with
 *	errno set.
 *
 * Side effects:
 *	The wake pipe is emptied, the connection read, and the listeners of
 *	the events are called.
 *
 *----------------------------------------------------------------------
 */

int
TkWlDispatchEvents(void)
{
    struct wl_display *wlDisplay = tkXeDisplay->wlDisplay;
    struct pollfd pfd;
    char buf[64];
    int n = 0, error;

    if (tkXeDisplay->readerQueue == NULL) {
	return Dispatch(0);
    }
    while (read(tkXeDisplay->wakeFds[0], buf, sizeof(buf)) > 0) {
	/* Empty the pipe. */
    }

    /*
     * Read the events which arrived since the reader last read, so that
     * [update] processes all the events sent before it was called, as when
     * Tk read the connection itself.
     */

    if (tkXeDisplay->decor) {
	n = libdecor_dispatch(tkXeDisplay->decor, 0);
    } else {
	while (wl_display_prepare_read(wlDisplay) != 0) {
	    n += wl_display_dispatch_pending(wlDisplay);
	}
	pfd.fd = wl_display_get_fd(wlDisplay);
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 0) > 0) {
	    wl_display_read_events(wlDisplay);
	} else {
	    wl_display_cancel_read(wlDisplay);
	}
	n += wl_display_dispatch_pending(wlDisplay);
    }
    error = wl_display_get_error(wlDisplay);
    if (error) {
	errno = error;
	return -1;
    }
    return n;
}

/*
 *----------------------------------------------------------------------
 *
 * TkCreateXEventSource --
 *
 *	Create the event source of Tcl which reads the events of the
 *	compositor, once per thread.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The event source and an exit handler removing it are created.
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

/*
 *----------------------------------------------------------------------
 *
 * DisplayExitHandler --
 *
 *	Remove the event source at exit.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The event source is deleted.
 *
 *----------------------------------------------------------------------
 */

static void
DisplayExitHandler(
    TCL_UNUSED(void *))
{
    ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    Tcl_DeleteEventSource(DisplaySetupProc, DisplayCheckProc, NULL);
    tsdPtr->initialized = false;
}

/*
 *----------------------------------------------------------------------
 *
 * DisplaySetupProc, DisplayCheckProc --
 *
 *	The procedures of the event source. Before Tcl waits for events, the
 *	events already read (and with the reader thread, those waiting) are
 *	dispatched, and the requests are flushed; the other events are read by
 *	DisplayFileProc when they arrive.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	DisplaySetupProc dispatches the pending events, which queues X events,
 *	updates the text input and flushes the requests. If events were
 *	dispatched, Tcl does not block.
 *
 *----------------------------------------------------------------------
 */

static void
DisplaySetupProc(
    TCL_UNUSED(void *),
    int flags)
{
    if (!(flags & TCL_WINDOW_EVENTS) || tkXeDisplay == NULL) {
	return;
    }
    if ((tkXeDisplay->readerQueue ? TkWlDispatchEvents()
	    : wl_display_dispatch_pending(tkXeDisplay->wlDisplay)) > 0) {
	Tcl_Time blockTime = {0, 0};

	Tcl_SetMaxBlockTime(&blockTime);
    }
    TkWlTextInputUpdate();
    wl_display_flush(tkXeDisplay->wlDisplay);
}

static void
DisplayCheckProc(
    TCL_UNUSED(void *),
    TCL_UNUSED(int))
{
}

/*
 *----------------------------------------------------------------------
 *
 * Dispatch --
 *
 *	Read and dispatch the events of the connection, through libdecor,
 *	since its plugins may have their own event sources.
 *
 * Results:
 *	The number of events dispatched, or -1 on error.
 *
 * Side effects:
 *	The listeners of the events are called.
 *
 *----------------------------------------------------------------------
 */

static int
Dispatch(
    int timeout)
{
    if (tkXeDisplay->decor) {
	return libdecor_dispatch(tkXeDisplay->decor, timeout);
    }
    return wl_display_dispatch(tkXeDisplay->wlDisplay);
}

/*
 *----------------------------------------------------------------------
 *
 * DisplayFileProc --
 *
 *	File handler of the events of the connection (see TkWlEventFd),
 *	called when they can be dispatched.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Dispatches the events. Panics if the compositor closed the connection.
 *
 *----------------------------------------------------------------------
 */

static void
DisplayFileProc(
    TCL_UNUSED(void *),
    TCL_UNUSED(int))
{
    if (TkWlDispatchEvents() < 0) {
	/*
	 * The compositor went away. There is nothing more Tk can do.
	 */

	Tcl_DeleteFileHandler(TkWlEventFd());
	Tcl_Panic("lost the connection to the Wayland compositor: %s",
		strerror(errno));
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkUnixDoOneXEvent --
 *
 *	Process a window event, waiting for one until a time at most (or
 *	forever if timePtr is NULL); used by the code of Tk which waits for
 *	particular events.
 *
 * Results:
 *	True if an event was processed or the time is not yet over, false if
 *	the time is over or there is no connection.
 *
 * Side effects:
 *	Events are read, dispatched and processed.
 *
 *----------------------------------------------------------------------
 */

bool
TkUnixDoOneXEvent(
    Tcl_Time *timePtr)
{
    struct pollfd pfd;
    int timeout = -1;

    if (Tcl_ServiceEvent(TCL_WINDOW_EVENTS)) {
	return true;
    }
    if (tkXeDisplay == NULL) {
	return false;
    }
    wl_display_dispatch_pending(tkXeDisplay->wlDisplay);
    if (Tcl_ServiceEvent(TCL_WINDOW_EVENTS)) {
	return true;
    }
    wl_display_flush(tkXeDisplay->wlDisplay);
    if (timePtr) {
	Tcl_Time now;

	Tcl_GetTime(&now);
	timeout = (int)((timePtr->sec - now.sec) * 1000
		+ (timePtr->usec - now.usec) / 1000);
	if (timeout < 0) {
	    timeout = 0;
	}
    }
    pfd.fd = TkWlEventFd();
    pfd.events = POLLIN;
    if (poll(&pfd, 1, timeout) > 0) {
	TkWlDispatchEvents();
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

/*
 *----------------------------------------------------------------------
 *
 * TkXeCurrentTime --
 *
 *	The time of the last input event, which serves as the current server
 *	time.
 *
 * Results:
 *	The time, or CurrentTime if there is no connection.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Time
TkXeCurrentTime(void)
{
    return tkXeDisplay ? tkXeDisplay->lastTime : CurrentTime;
}

/*
 *----------------------------------------------------------------------
 *
 * Keysyms --
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * TkpInitKeymapInfo --
 *
 *	Initialize the information about the modifiers of a display: the
 *	keycodes of the modifier keys, and which X modifiers are Meta, Alt and
 *	Mode_switch, as XKB assigns them.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The modifier information of the display is (re)computed, and is no
 *	longer stale.
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
 *----------------------------------------------------------------------
 *
 * KeycodeToKeysym --
 *
 *	The keysym of a key with a modifier state, for events which were not
 *	generated by the keyboard: Shift and Lock select the second level,
 *	Mod5 the third.
 *
 * Results:
 *	The keysym, or NoSymbol.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static KeySym
KeycodeToKeysym(
    unsigned int keycode,
    unsigned int state)
{
    struct xkb_keymap *keymap = tkXeDisplay ? tkXeDisplay->keymap : NULL;
    xkb_layout_index_t layout = 0;
    xkb_level_index_t level = 0;
    const xkb_keysym_t *syms;
    int n;

    if (keymap == NULL || keycode == 0) {
	return NoSymbol;
    }
    if (tkXeDisplay->xkbState) {
	layout = xkb_state_key_get_layout(tkXeDisplay->xkbState, keycode);
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

/*
 *----------------------------------------------------------------------
 *
 * TkpGetKeySym --
 *
 *	The keysym of a key event: the one stored in the event, or else the
 *	one of its keycode and state.
 *
 * Results:
 *	The keysym, or NoSymbol.
 *
 * Side effects:
 *	The modifier information of the display is updated if it is stale.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * TkpGetString --
 *
 *	The text of a key event: the one stored in the event, or else the text
 *	of its keysym, made a control character with Control.
 *
 * Results:
 *	The text, in the DString.
 *
 * Side effects:
 *	The text is computed and stored in the event if it had none.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * TkpSetKeycodeAndState --
 *
 *	Set the keycode, and the modifiers needed, of a key event generated
 *	with [event generate] for a keysym.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The keycode, state and keysym of the event are set.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * XSetInputFocus --
 *
 *	Give the focus to a window. A Wayland client cannot take the keyboard,
 *	so the focus is moved between the windows of the application when it
 *	has the keyboard, as an X server would, and otherwise the compositor
 *	is asked to activate the toplevel.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Focus events are queued; the toplevel may be activated.
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
    TkXeWindow *winPtr = TkXeGetWindow(focus);
    TkXeWindow *topPtr = winPtr ? TkXeTopWindow(winPtr) : NULL;

    TkXeNextRequest(display);
    if (topPtr == NULL || tkXeDisplay->keyboardSurface == NULL) {
	/*
	 * Only the compositor can give the keyboard to the application.
	 */

	if (topPtr) {
	    TkXeWmActivate(topPtr->surface);
	}
	return Success;
    }

    /*
     * The application has the keyboard: like an X server, move the focus
     * between its windows, which the keys go to. The compositor is asked to
     * activate the toplevel, so that it is shown in front.
     */

    if (topPtr != tkXeDisplay->keyboardSurface->winPtr) {
	TkXeWmActivate(topPtr->surface);
    }
    if (focus != focusWindow) {
	if (TkXeGetWindow(focusWindow)) {
	    QueueFocusEvent(FocusOut, focusWindow);
	}
	focusWindow = focus;
	QueueFocusEvent(FocusIn, focusWindow);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XGetInputFocus --
 *
 *	The window which has the focus.
 *
 * Results:
 *	Success; the window, or None, is stored in *focusPtr.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
XGetInputFocus(
    Display *display,
    Window *focusPtr,
    int *revertPtr)
{
    TkXeNextRequest(display);
    *focusPtr = TkXeGetWindow(focusWindow) ? focusWindow : None;
    *revertPtr = RevertToParent;
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XGrabKeyboard, XUngrabKeyboard --
 *
 *	Grabbing the keyboard is not possible in Wayland; the grabs of Tk work
 *	on the windows of the application.
 *
 * Results:
 *	GrabSuccess, or Success.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
XGrabKeyboard(
    Display *display,
    TCL_UNUSED(Window),
    TCL_UNUSED(Bool),
    TCL_UNUSED(int),
    TCL_UNUSED(int),
    TCL_UNUSED(Time))
{
    TkXeNextRequest(display);
    return GrabSuccess;
}

int
XUngrabKeyboard(
    Display *display,
    TCL_UNUSED(Time))
{
    TkXeNextRequest(display);
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

/*
 *----------------------------------------------------------------------
 *
 * TkpSetCapture, TkpGetCapture --
 *
 *	Set and get the window which captures the pointer events (for the
 *	grabs of tkPointer.c).
 *
 * Results:
 *	TkpGetCapture returns the window, or NULL.
 *
 * Side effects:
 *	TkpSetCapture changes the window with the capture.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * XQueryPointer --
 *
 *	The position of the pointer, in root coordinates and in a window, and
 *	the state of the modifiers and buttons.
 *
 * Results:
 *	True if the pointer is on a surface of the application, else False
 *	with the position of the root.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

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
    TkXeSurface *surfPtr = tkXeDisplay->pointerSurface;
    int rootX = 0, rootY = 0, x, y;

    TkXeNextRequest(display);
    if (surfPtr) {
	SurfaceToRoot(surfPtr, (int)floor(tkXeDisplay->pointerX),
		(int)floor(tkXeDisplay->pointerY), &rootX, &rootY);
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
 *----------------------------------------------------------------------
 *
 * XWarpPointer, TkpWarpPointer --
 *
 *	Move the pointer, which Wayland clients cannot do.
 *
 * Results:
 *	Success, or nothing.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
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
    TkXeNextRequest(display);
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
 *	Send an event to a window. Events can only be sent to the windows of
 *	this process; client messages to the root window are requests to the
 *	window manager, which tkWaylandWm.c handles.
 *
 * Results:
 *	1 if the event was sent, 0 if the window does not exist.
 *
 * Side effects:
 *	The event is queued, or handled by the window manager.
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

    TkXeNextRequest(display);
    if (w == PointerWindow || w == InputFocus) {
	w = focusWindow;
    }
    event.xany.display = display;
    if (w == RootWindow(display, 0) && TkXeWmClientMessage(&event)) {
	return 1;
    }
    if (TkXeGetWindow(w) == NULL) {
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
