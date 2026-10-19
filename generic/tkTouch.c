/*
 * tkTouch.c --
 *
 *	Touchscreens. The ports which have touch input (Wayland) give the
 *	touches to TkTouchEvent, which recognizes gestures and turns them
 *	into the events widgets know:
 *
 *	- A tap is a click of the first button.
 *	- Dragging scrolls: it generates TouchpadScroll events, which the
 *	  scrollable widgets handle, and goes on (flings) for a while when the
 *	  finger is lifted while moving. The pointer, which a tap left where
 *	  it was, leaves the windows then.
 *	- A long press is a click of the third button (a context menu), or,
 *	  if the finger then moves, a drag with the first button (selecting
 *	  text, dragging things).
 *	- Two fingers scroll, and pinching generates <<Pinch>> virtual events,
 *	  whose data (%d) is the factor by which the distance between the
 *	  fingers changed since the last one. A second finger also ends the
 *	  drag of a finger which is a mouse, so that windows in the mouse
 *	  mode can be pinched.
 *
 *	Some widgets need the button pressed as soon as the screen is touched
 *	(scrollbars, scales, sashes), and an application may want dragging
 *	in a canvas to draw rather than scroll: the touch mode of a window
 *	("tk touch mode") says whether touching it scrolls or acts as a
 *	mouse. In the raw mode, there are no gestures: each finger which
 *	touches the window sends it <<TouchBegin>>, <<TouchMove>>, and
 *	<<TouchEnd>> or <<TouchCancel>> events, whose data (%d) is the id
 *	of the finger, as long as it is on the screen, wherever it moves.
 *	By default, the window touched scrolls if it, or one of its
 *	ancestors, is a widget which scrolls; otherwise the finger is a mouse
 *	at once. A tap on a widget which scrolls is only delivered when the
 *	finger is lifted, since until then it could be the start of a drag.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkInt.h"

/*
 * The parameters of the gestures. Distances are in pixels of the root
 * window, times in milliseconds.
 */

#define TOUCH_SLOP		8	/* Moving less is not a drag. */
#define LONG_PRESS_TIME		500	/* Holding longer is a long press. */
#define FLING_INTERVAL		16	/* Time between fling events. */
#define FLING_MIN_SPEED		0.3	/* Slower is not a fling, in pixels
					 * per millisecond. */
#define FLING_MAX_SPEED		8.0
#define FLING_STOP_SPEED	0.02	/* A fling stops when it is slower. */
#define FLING_FRICTION		0.95	/* Speed kept every FLING_INTERVAL. */
#define VELOCITY_WINDOW		100	/* The speed of a drag is measured on
					 * the last this many ms. */
#define NUM_SAMPLES		8

/*
 * Touch modes of windows.
 */

#define MODE_AUTO	0	/* Decided by the class, and the ancestors. */
#define MODE_SCROLL	1	/* Dragging scrolls. */
#define MODE_MOUSE	2	/* The finger is a mouse. */
#define MODE_RAW	3	/* The fingers send touch events. */

static const char *const modeNames[] = {
    "auto", "scroll", "mouse", "raw", NULL
};

/*
 * The number of fingers on windows in the raw mode which are followed.
 */

#define MAX_RAW		16

/*
 * The classes of the widgets which scroll, and of those which need the
 * button at once.
 */

static const char *const scrollClasses[] = {
    "Canvas", "Entry", "Listbox", "Text", "TEntry", "Treeview", NULL
};
static const char *const mouseClasses[] = {
    "Menu", "Panedwindow", "Scale", "Scrollbar", "Sizegrip", "Spinbox",
    "TCombobox", "TPanedwindow", "TScale", "TScrollbar", "TSizegrip",
    "TSpinbox", NULL
};

/*
 * States of the gesture recognizer.
 */

typedef enum {
    STATE_IDLE,			/* No finger. */
    STATE_PENDING,		/* A finger is down on a widget which scrolls;
				 * it is not known yet what it does. */
    STATE_LONG_PRESS,		/* The finger was held without moving. */
    STATE_SCROLL,		/* The finger drags: scrolling. */
    STATE_MOUSE,		/* The finger is a mouse, with the first
				 * button pressed. */
    STATE_TWO_FINGERS,		/* Two fingers scroll and pinch. */
    STATE_DONE			/* The gesture is over; waiting for the
				 * fingers to be lifted. */
} GestureState;

typedef struct {
    int id;			/* Id of the finger, or -1. */
    double x, y;		/* Position, in root coordinates. */
} Finger;

typedef struct {
    double x, y;
    unsigned long time;
} Sample;

/*
 * A finger on a window in the raw mode.
 */

typedef struct {
    int id;			/* Id of the finger, or -1 for a free slot. */
    Display *display;		/* The window it touched, which gets its */
    Window window;		/* events. */
    double x, y;		/* Its last position, in root coordinates. */
} RawFinger;

typedef struct {
    bool initialized;
    Tcl_HashTable modeTable;	/* Touch modes of windows set with "tk touch
				 * mode", keyed by window. */
    GestureState state;
    Finger fingers[2];		/* The first finger, and the second for two
				 * finger gestures. */
    Display *display;		/* Of the windows below. */
    Window touched;		/* The window under the first finger when it
				 * touched the screen. */
    Window under;		/* The window under the first finger when it
				 * last moved. */
    Window scrollTarget;	/* The window which scrolls, or None. */
    double startX, startY;	/* Where the first finger touched. */
    double lastX, lastY;	/* Where scrolling is, in root coordinates:
				 * the first finger, or the middle of two. */
    double restX, restY;	/* Fractions of pixels not yet scrolled. */
    double pinchDistance;	/* Distance between two fingers at the last
				 * <<Pinch>> event. */
    unsigned int modState;	/* Modifiers of the last event. */
    Sample samples[NUM_SAMPLES];/* Recent positions, for the speed. */
    int numSamples;
    Tcl_TimerToken longPressTimer;
    Tcl_TimerToken flingTimer;
    RawFinger raw[MAX_RAW];	/* The fingers in the raw mode. */
    double flingVX, flingVY;	/* Speed of the fling, in pixels per ms. */
    unsigned long flingTime;	/* Time of the last fling event. */
} ThreadSpecificData;
static Tcl_ThreadDataKey dataKey;

static ThreadSpecificData *GetTSD(void);
static void		FlingProc(void *clientData);
static int		GetMode(Tk_Window tkwin);
static void		LongPressProc(void *clientData);
static void		ModeEventProc(void *clientData, XEvent *eventPtr);
static void		MouseButton(ThreadSpecificData *tsdPtr,
			    Tk_Window tkwin, double x, double y,
			    unsigned int buttons);
static void		QueuePinch(ThreadSpecificData *tsdPtr, double factor);
static void		QueueVirtual(Tk_Window tkwin, const char *name,
			    double x, double y, unsigned int state,
			    Tcl_Obj *dataPtr);
static bool		RawTouch(ThreadSpecificData *tsdPtr, int type,
			    Tk_Window tkwin, int id, double x, double y);
static void		Scroll(ThreadSpecificData *tsdPtr, double dx,
			    double dy);
static Tk_Window	ScrollTarget(Tk_Window tkwin, bool *mousePtr);
static void		StopFling(ThreadSpecificData *tsdPtr);
static void		StopLongPress(ThreadSpecificData *tsdPtr);

/*
 *----------------------------------------------------------------------
 *
 * GetTSD --
 *
 *	The state of the touches of the thread.
 *
 * Results:
 *	The state, initialized the first time.
 *
 * Side effects:
 *	May initialize the table of touch modes.
 *
 *----------------------------------------------------------------------
 */

static ThreadSpecificData *
GetTSD(void)
{
    ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));
    int i;

    if (!tsdPtr->initialized) {
	tsdPtr->initialized = true;
	Tcl_InitHashTable(&tsdPtr->modeTable, TCL_ONE_WORD_KEYS);
	tsdPtr->fingers[0].id = tsdPtr->fingers[1].id = -1;
	for (i = 0; i < MAX_RAW; i++) {
	    tsdPtr->raw[i].id = -1;
	}
    }
    return tsdPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * GetMode --
 *
 *	The touch mode of a window: the one set with "tk touch mode", or else
 *	that of its class.
 *
 * Results:
 *	MODE_AUTO, MODE_SCROLL, MODE_MOUSE or MODE_RAW.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
GetMode(
    Tk_Window tkwin)		/* The window. */
{
    ThreadSpecificData *tsdPtr = GetTSD();
    Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&tsdPtr->modeTable, tkwin);
    const char *className = Tk_Class(tkwin);
    int i;

    if (hPtr) {
	return PTR2INT(Tcl_GetHashValue(hPtr));
    }
    if (className == NULL) {
	return MODE_AUTO;
    }
    for (i = 0; scrollClasses[i]; i++) {
	if (!strcmp(className, scrollClasses[i])) {
	    return MODE_SCROLL;
	}
    }
    for (i = 0; mouseClasses[i]; i++) {
	if (!strcmp(className, mouseClasses[i])) {
	    return MODE_MOUSE;
	}
    }
    return MODE_AUTO;
}

/*
 *----------------------------------------------------------------------
 *
 * ScrollTarget --
 *
 *	Find the window which scrolls when a window is touched: the window,
 *	or the nearest of its ancestors in its toplevel, whose mode is
 *	scroll, unless a window with the mouse mode is found first.
 *
 * Results:
 *	The window, or NULL if the finger is a mouse; *mousePtr says whether
 *	a window asked for the mouse mode.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Tk_Window
ScrollTarget(
    Tk_Window tkwin,		/* The window touched. */
    bool *mousePtr)		/* Returns whether the mode is mouse. */
{
    *mousePtr = false;
    for (; tkwin; tkwin = Tk_Parent(tkwin)) {
	switch (GetMode(tkwin)) {
	case MODE_SCROLL:
	    return tkwin;
	case MODE_MOUSE:
	case MODE_RAW:
	    *mousePtr = true;
	    return NULL;
	}
	if (Tk_IsTopLevel(tkwin)) {
	    break;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * MouseButton --
 *
 *	Move the pointer, and press or release buttons, through
 *	Tk_UpdatePointer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Crossing, motion and button events are generated.
 *
 *----------------------------------------------------------------------
 */

static void
MouseButton(
    ThreadSpecificData *tsdPtr,	/* The state of the touches. */
    Tk_Window tkwin,		/* The window under the finger, or NULL. */
    double x, double y,		/* Root coordinates. */
    unsigned int buttons)	/* The buttons pressed. */
{
    Tk_UpdatePointer(tkwin, (int)floor(x), (int)floor(y),
	    (int)(tsdPtr->modState | buttons));
}

/*
 *----------------------------------------------------------------------
 *
 * Scroll --
 *
 *	Scroll the window being scrolled, with a TouchpadScroll event whose
 *	delta is the movement of the finger, in whole pixels.
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
Scroll(
    ThreadSpecificData *tsdPtr,	/* The state of the touches. */
    double dx, double dy)	/* The movement, in pixels. */
{
    Tk_Window tkwin = Tk_IdToWindow(tsdPtr->display, tsdPtr->scrollTarget);
    int ix, iy, winX, winY;
    XEvent event;

    if (tkwin == NULL) {
	return;
    }
    tsdPtr->restX += dx;
    tsdPtr->restY += dy;
    ix = (int)tsdPtr->restX;
    iy = (int)tsdPtr->restY;
    if (ix == 0 && iy == 0) {
	return;
    }
    tsdPtr->restX -= ix;
    tsdPtr->restY -= iy;
    Tk_GetRootCoords(tkwin, &winX, &winY);
    memset(&event, 0, sizeof(XEvent));
    event.type = TouchpadScroll;
    event.xany.display = Tk_Display(tkwin);
    event.xany.window = Tk_WindowId(tkwin);
    event.xany.serial = LastKnownRequestProcessed(Tk_Display(tkwin));
    event.xbutton.x_root = (int)floor(tsdPtr->lastX);
    event.xbutton.y_root = (int)floor(tsdPtr->lastY);
    event.xbutton.x = event.xbutton.x_root - winX;
    event.xbutton.y = event.xbutton.y_root - winY;
    event.xbutton.state = tsdPtr->modState;
    event.xbutton.time = TkpGetEventTime();
    event.xkey.keycode = ((unsigned)ix << 16) | ((unsigned)iy & 0xFFFF);
    Tk_QueueWindowEvent(&event, TCL_QUEUE_TAIL);
}

/*
 *----------------------------------------------------------------------
 *
 * QueueVirtual --
 *
 *	Queue a virtual event, with data, for a window, at a point.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is queued; the data are released with it.
 *
 *----------------------------------------------------------------------
 */

static void
QueueVirtual(
    Tk_Window tkwin,		/* The window. */
    const char *name,		/* The name of the event, without <<>>. */
    double x, double y,		/* The point, in root coordinates. */
    unsigned int state,		/* The modifiers. */
    Tcl_Obj *dataPtr)		/* The data (%d). */
{
    union {
	XEvent general;
	XVirtualEvent virt;
    } event;
    int winX, winY;

    Tcl_IncrRefCount(dataPtr);
    if (Tk_WindowId(tkwin) == None) {
	Tcl_DecrRefCount(dataPtr);
	return;
    }
    Tk_GetRootCoords(tkwin, &winX, &winY);
    memset(&event, 0, sizeof(event));
    event.general.xany.type = VirtualEvent;
    event.general.xany.serial = NextRequest(Tk_Display(tkwin));
    event.general.xany.send_event = False;
    event.general.xany.window = Tk_WindowId(tkwin);
    event.general.xany.display = Tk_Display(tkwin);
    event.virt.name = Tk_GetUid(name);
    event.virt.root = RootWindow(Tk_Display(tkwin), Tk_ScreenNumber(tkwin));
    event.virt.time = TkpGetEventTime();
    event.virt.x_root = (int)floor(x);
    event.virt.y_root = (int)floor(y);
    event.virt.x = event.virt.x_root - winX;
    event.virt.y = event.virt.y_root - winY;
    event.virt.state = state;
    event.virt.same_screen = True;
    event.virt.user_data = dataPtr;
    Tk_QueueWindowEvent(&event.general, TCL_QUEUE_TAIL);
}

/*
 *----------------------------------------------------------------------
 *
 * QueuePinch --
 *
 *	Queue a <<Pinch>> virtual event for the window touched, at the middle
 *	of the fingers.
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
QueuePinch(
    ThreadSpecificData *tsdPtr,	/* The state of the touches. */
    double factor)		/* Change of the distance of the fingers. */
{
    Tk_Window tkwin = Tk_IdToWindow(tsdPtr->display, tsdPtr->touched);

    if (tkwin) {
	QueueVirtual(tkwin, "Pinch", tsdPtr->lastX, tsdPtr->lastY,
		tsdPtr->modState, Tcl_NewDoubleObj(factor));
    }
}

/*
 *----------------------------------------------------------------------
 *
 * RawTouch --
 *
 *	Handle a touch of a finger on a window in the raw mode (or in a
 *	window whose nearest ancestor with a mode has the raw mode): the
 *	window the finger touched gets a <<TouchBegin>>, <<TouchMove>>,
 *	<<TouchEnd>> or <<TouchCancel>> event, with the id of the finger as
 *	data.
 *
 * Results:
 *	True if the touch is that of a finger in the raw mode, which the
 *	gestures ignore.
 *
 * Side effects:
 *	An event may be queued.
 *
 *----------------------------------------------------------------------
 */

static bool
RawTouch(
    ThreadSpecificData *tsdPtr,	/* The state of the touches. */
    int type,			/* TK_TOUCH_BEGIN etc. */
    Tk_Window tkwin,		/* The window under the finger, or NULL. */
    int id,			/* The finger. */
    double x, double y)		/* Its position, in root coordinates. */
{
    static const char *const names[] = {
	"TouchBegin", "TouchMove", "TouchEnd", "TouchCancel"
    };
    RawFinger *rawPtr = NULL;
    Tk_Window target;
    int i;

    for (i = 0; i < MAX_RAW; i++) {
	if (tsdPtr->raw[i].id == id) {
	    rawPtr = &tsdPtr->raw[i];
	    break;
	}
    }
    if (rawPtr == NULL) {
	if (type != TK_TOUCH_BEGIN || tkwin == NULL) {
	    return false;
	}

	/*
	 * A new finger is raw if the nearest window with a mode, from the
	 * window touched up to its toplevel, has the raw mode.
	 */

	for (target = tkwin; target; target = Tk_Parent(target)) {
	    int mode = GetMode(target);

	    if (mode != MODE_AUTO) {
		if (mode != MODE_RAW) {
		    return false;
		}
		break;
	    }
	    if (Tk_IsTopLevel(target)) {
		return false;
	    }
	}
	if (target == NULL) {
	    return false;
	}
	for (i = 0; i < MAX_RAW; i++) {
	    if (tsdPtr->raw[i].id < 0) {
		rawPtr = &tsdPtr->raw[i];
		break;
	    }
	}
	if (rawPtr == NULL) {
	    /*
	     * Too many fingers: this one is ignored.
	     */

	    return true;
	}
	rawPtr->id = id;
	rawPtr->display = Tk_Display(tkwin);
	rawPtr->window = Tk_WindowId(tkwin);
    }

    /*
     * The finger is lifted where it last was.
     */

    if (type == TK_TOUCH_BEGIN || type == TK_TOUCH_MOVE) {
	rawPtr->x = x;
	rawPtr->y = y;
    }
    target = Tk_IdToWindow(rawPtr->display, rawPtr->window);
    if (target) {
	QueueVirtual(target, names[type], rawPtr->x, rawPtr->y,
		tsdPtr->modState, Tcl_NewWideIntObj(id));
    }
    if (type == TK_TOUCH_END || type == TK_TOUCH_CANCEL) {
	rawPtr->id = -1;
    }
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * StopLongPress, StopFling --
 *
 *	Cancel the timer of the long press, or the fling.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The timer is deleted.
 *
 *----------------------------------------------------------------------
 */

static void
StopLongPress(
    ThreadSpecificData *tsdPtr)	/* The state of the touches. */
{
    if (tsdPtr->longPressTimer) {
	Tcl_DeleteTimerHandler(tsdPtr->longPressTimer);
	tsdPtr->longPressTimer = NULL;
    }
}

static void
StopFling(
    ThreadSpecificData *tsdPtr)	/* The state of the touches. */
{
    if (tsdPtr->flingTimer) {
	Tcl_DeleteTimerHandler(tsdPtr->flingTimer);
	tsdPtr->flingTimer = NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * LongPressProc --
 *
 *	The finger was held without moving: it is a long press.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The state changes.
 *
 *----------------------------------------------------------------------
 */

static void
LongPressProc(
    TCL_UNUSED(void *))
{
    ThreadSpecificData *tsdPtr = GetTSD();

    tsdPtr->longPressTimer = NULL;
    if (tsdPtr->state == STATE_PENDING) {
	tsdPtr->state = STATE_LONG_PRESS;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * FlingProc --
 *
 *	Go on scrolling after the finger was lifted, slower and slower. The
 *	distance and the slowing down are computed from the time since the
 *	last fling event, so that a fling goes as far when the timer is late.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Scrolls, and schedules itself again until the fling stops.
 *
 *----------------------------------------------------------------------
 */

static void
FlingProc(
    TCL_UNUSED(void *))
{
    ThreadSpecificData *tsdPtr = GetTSD();
    unsigned long now = TkpGetEventTime();
    double dt = (double)(now - tsdPtr->flingTime);
    double friction = pow(FLING_FRICTION, dt / FLING_INTERVAL);

    tsdPtr->flingTimer = NULL;
    tsdPtr->flingTime = now;

    /*
     * The distance is the integral of the speed, which decreases
     * exponentially.
     */

    if (friction < 1.0) {
	double distance = dt * (1.0 - friction) / -log(friction);

	Scroll(tsdPtr, tsdPtr->flingVX * distance,
		tsdPtr->flingVY * distance);
    }
    tsdPtr->flingVX *= friction;
    tsdPtr->flingVY *= friction;
    if (hypot(tsdPtr->flingVX, tsdPtr->flingVY) < FLING_STOP_SPEED
	    || Tk_IdToWindow(tsdPtr->display, tsdPtr->scrollTarget) == NULL) {
	return;
    }
    tsdPtr->flingTimer = Tcl_CreateTimerHandler(FLING_INTERVAL, FlingProc,
	    NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * AddSample, Velocity --
 *
 *	Record the positions of the finger which scrolls, and compute its
 *	speed over the last VELOCITY_WINDOW milliseconds.
 *
 * Results:
 *	Velocity returns the speed in *vxPtr and *vyPtr, in pixels per ms.
 *
 * Side effects:
 *	AddSample records a sample.
 *
 *----------------------------------------------------------------------
 */

static void
AddSample(
    ThreadSpecificData *tsdPtr,	/* The state of the touches. */
    double x, double y,		/* The position. */
    unsigned long time)		/* Its time. */
{
    if (tsdPtr->numSamples == NUM_SAMPLES) {
	memmove(tsdPtr->samples, tsdPtr->samples + 1,
		(NUM_SAMPLES - 1) * sizeof(Sample));
	tsdPtr->numSamples--;
    }
    tsdPtr->samples[tsdPtr->numSamples].x = x;
    tsdPtr->samples[tsdPtr->numSamples].y = y;
    tsdPtr->samples[tsdPtr->numSamples].time = time;
    tsdPtr->numSamples++;
}

static void
Velocity(
    ThreadSpecificData *tsdPtr,	/* The state of the touches. */
    unsigned long now,		/* Time the finger was lifted. */
    double *vxPtr, double *vyPtr)
				/* Returns the speed. */
{
    Sample *lastPtr, *firstPtr;
    double dt, speed;
    int i;

    *vxPtr = *vyPtr = 0.0;
    if (tsdPtr->numSamples < 2) {
	return;
    }
    lastPtr = &tsdPtr->samples[tsdPtr->numSamples - 1];
    if (now - lastPtr->time > VELOCITY_WINDOW / 2) {
	/*
	 * The finger stopped before it was lifted.
	 */

	return;
    }
    firstPtr = lastPtr;
    for (i = tsdPtr->numSamples - 2; i >= 0; i--) {
	if (lastPtr->time - tsdPtr->samples[i].time > VELOCITY_WINDOW) {
	    break;
	}
	firstPtr = &tsdPtr->samples[i];
    }
    dt = (double)(lastPtr->time - firstPtr->time);
    if (dt <= 0.0) {
	return;
    }
    *vxPtr = (lastPtr->x - firstPtr->x) / dt;
    *vyPtr = (lastPtr->y - firstPtr->y) / dt;
    speed = hypot(*vxPtr, *vyPtr);
    if (speed > FLING_MAX_SPEED) {
	*vxPtr *= FLING_MAX_SPEED / speed;
	*vyPtr *= FLING_MAX_SPEED / speed;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkTouchEvent --
 *
 *	Called by the ports for each touch: a finger touches the screen,
 *	moves, is lifted, or the touch is cancelled (by the system, which
 *	took it for a gesture of its own). The position and window of
 *	TK_TOUCH_END and TK_TOUCH_CANCEL are not used: the finger is lifted
 *	where it last was.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Events are generated for the gestures recognized.
 *
 *----------------------------------------------------------------------
 */

void
TkTouchEvent(
    int type,			/* TK_TOUCH_BEGIN, _MOVE, _END or _CANCEL. */
    Tk_Window tkwin,		/* The window under the finger, or NULL. */
    int id,			/* Identifies the finger. */
    double x, double y,		/* Position, in root coordinates. */
    unsigned long time,		/* Time of the event, in milliseconds. */
    unsigned int modState)	/* The modifiers (ShiftMask etc.). */
{
    ThreadSpecificData *tsdPtr = GetTSD();
    Finger *fingerPtr = NULL;
    int i;

    tsdPtr->modState = modState;
    if (RawTouch(tsdPtr, type, tkwin, id, x, y)) {
	return;
    }
    for (i = 0; i < 2; i++) {
	if (tsdPtr->fingers[i].id == id) {
	    fingerPtr = &tsdPtr->fingers[i];
	}
    }

    switch (type) {
    case TK_TOUCH_BEGIN:
	if (tsdPtr->fingers[0].id < 0) {
	    bool mouse;
	    Tk_Window target;

	    /*
	     * The first finger: stop flinging, and decide whether it scrolls
	     * or is a mouse.
	     */

	    StopFling(tsdPtr);
	    StopLongPress(tsdPtr);
	    tsdPtr->fingers[0].id = id;
	    tsdPtr->fingers[0].x = tsdPtr->startX = tsdPtr->lastX = x;
	    tsdPtr->fingers[0].y = tsdPtr->startY = tsdPtr->lastY = y;
	    tsdPtr->restX = tsdPtr->restY = 0.0;
	    tsdPtr->numSamples = 0;
	    AddSample(tsdPtr, x, y, time);
	    tsdPtr->display = tkwin ? Tk_Display(tkwin) : NULL;
	    tsdPtr->touched = tsdPtr->under = tkwin ? Tk_WindowId(tkwin) : None;
	    target = tkwin ? ScrollTarget(tkwin, &mouse) : NULL;
	    tsdPtr->scrollTarget = target ? Tk_WindowId(target) : None;
	    if (target == NULL) {
		tsdPtr->state = STATE_MOUSE;
		MouseButton(tsdPtr, tkwin, x, y, 0);
		MouseButton(tsdPtr, tkwin, x, y, Button1Mask);
	    } else {
		tsdPtr->state = STATE_PENDING;
		tsdPtr->longPressTimer = Tcl_CreateTimerHandler(
			LONG_PRESS_TIME, LongPressProc, NULL);
	    }
	} else if (tsdPtr->fingers[1].id < 0
		&& (tsdPtr->state == STATE_PENDING
		    || tsdPtr->state == STATE_LONG_PRESS
		    || tsdPtr->state == STATE_SCROLL
		    || tsdPtr->state == STATE_MOUSE)) {
	    /*
	     * A second finger: two finger scrolling and pinching. When the
	     * first finger was a mouse, its button is released first.
	     */

	    StopLongPress(tsdPtr);
	    if (tsdPtr->state == STATE_MOUSE) {
		MouseButton(tsdPtr, Tk_IdToWindow(tsdPtr->display,
			tsdPtr->under), tsdPtr->fingers[0].x,
			tsdPtr->fingers[0].y, 0);
	    }
	    MouseButton(tsdPtr, NULL, x, y, 0);
	    tsdPtr->fingers[1].id = id;
	    tsdPtr->fingers[1].x = x;
	    tsdPtr->fingers[1].y = y;
	    tsdPtr->state = STATE_TWO_FINGERS;
	    tsdPtr->lastX = (tsdPtr->fingers[0].x + x) / 2;
	    tsdPtr->lastY = (tsdPtr->fingers[0].y + y) / 2;
	    tsdPtr->pinchDistance = hypot(tsdPtr->fingers[0].x - x,
		    tsdPtr->fingers[0].y - y);
	}
	break;

    case TK_TOUCH_MOVE:
	if (fingerPtr == NULL) {
	    break;
	}
	fingerPtr->x = x;
	fingerPtr->y = y;
	if (fingerPtr == &tsdPtr->fingers[0]) {
	    tsdPtr->under = tkwin ? Tk_WindowId(tkwin) : None;
	}
	switch (tsdPtr->state) {
	case STATE_PENDING:
	case STATE_LONG_PRESS:
	    if (hypot(x - tsdPtr->startX, y - tsdPtr->startY) < TOUCH_SLOP) {
		break;
	    }
	    StopLongPress(tsdPtr);
	    if (tsdPtr->state == STATE_LONG_PRESS) {
		/*
		 * A long press, then a drag: the first button drags from where
		 * the screen was touched.
		 */

		Tk_Window touched = Tk_IdToWindow(tsdPtr->display,
			tsdPtr->touched);

		tsdPtr->state = STATE_MOUSE;
		MouseButton(tsdPtr, touched, tsdPtr->startX, tsdPtr->startY, 0);
		MouseButton(tsdPtr, touched, tsdPtr->startX, tsdPtr->startY,
			Button1Mask);
		MouseButton(tsdPtr, tkwin, x, y, Button1Mask);
		break;
	    }
	    tsdPtr->state = STATE_SCROLL;

	    /*
	     * The pointer leaves the windows while the fingers scroll: there
	     * is nothing under it, which widgets would show as such while
	     * their contents move.
	     */

	    MouseButton(tsdPtr, NULL, x, y, 0);
	    /* FALLTHRU */
	case STATE_SCROLL:
	    AddSample(tsdPtr, x, y, time);
	    Scroll(tsdPtr, x - tsdPtr->lastX, y - tsdPtr->lastY);
	    tsdPtr->lastX = x;
	    tsdPtr->lastY = y;
	    break;
	case STATE_MOUSE:
	    if (fingerPtr == &tsdPtr->fingers[0]) {
		MouseButton(tsdPtr, tkwin, x, y, Button1Mask);
	    }
	    break;
	case STATE_TWO_FINGERS: {
	    double mx = (tsdPtr->fingers[0].x + tsdPtr->fingers[1].x) / 2;
	    double my = (tsdPtr->fingers[0].y + tsdPtr->fingers[1].y) / 2;
	    double distance = hypot(
		    tsdPtr->fingers[0].x - tsdPtr->fingers[1].x,
		    tsdPtr->fingers[0].y - tsdPtr->fingers[1].y);

	    Scroll(tsdPtr, mx - tsdPtr->lastX, my - tsdPtr->lastY);
	    tsdPtr->lastX = mx;
	    tsdPtr->lastY = my;
	    if (tsdPtr->pinchDistance > 0.0 && distance > 0.0
		    && fabs(distance / tsdPtr->pinchDistance - 1.0) > 0.01) {
		QueuePinch(tsdPtr, distance / tsdPtr->pinchDistance);
		tsdPtr->pinchDistance = distance;
	    }
	    break;
	}
	default:
	    break;
	}
	break;

    case TK_TOUCH_END:
    case TK_TOUCH_CANCEL:
	if (fingerPtr == NULL) {
	    break;
	}

	/*
	 * The finger is lifted where it last moved.
	 */

	x = fingerPtr->x;
	y = fingerPtr->y;
	tkwin = Tk_IdToWindow(tsdPtr->display, tsdPtr->under);
	fingerPtr->id = -1;
	if (fingerPtr == &tsdPtr->fingers[1]) {
	    /*
	     * The second finger was lifted: the gesture is over.
	     */

	    if (tsdPtr->state == STATE_TWO_FINGERS) {
		tsdPtr->state = STATE_DONE;
	    }
	    break;
	}
	StopLongPress(tsdPtr);
	switch (tsdPtr->state) {
	case STATE_PENDING:
	case STATE_LONG_PRESS:
	    if (type == TK_TOUCH_END) {
		/*
		 * A tap, or a long press: a click where the screen was
		 * touched.
		 */

		Tk_Window touched = Tk_IdToWindow(tsdPtr->display,
			tsdPtr->touched);
		unsigned int button = (tsdPtr->state == STATE_LONG_PRESS)
			? Button3Mask : Button1Mask;

		MouseButton(tsdPtr, touched, tsdPtr->startX, tsdPtr->startY, 0);
		MouseButton(tsdPtr, touched, tsdPtr->startX, tsdPtr->startY,
			button);
		MouseButton(tsdPtr, touched, tsdPtr->startX, tsdPtr->startY, 0);
	    }
	    break;
	case STATE_SCROLL:
	    if (type == TK_TOUCH_END) {
		double vx, vy;

		Velocity(tsdPtr, time, &vx, &vy);
		if (hypot(vx, vy) >= FLING_MIN_SPEED) {
		    tsdPtr->flingVX = vx;
		    tsdPtr->flingVY = vy;
		    tsdPtr->flingTime = TkpGetEventTime();
		    tsdPtr->flingTimer = Tcl_CreateTimerHandler(FLING_INTERVAL,
			    FlingProc, NULL);
		}
	    }
	    break;
	case STATE_MOUSE:
	    MouseButton(tsdPtr, tkwin, x, y, 0);
	    break;
	default:
	    break;
	}
	tsdPtr->state = (tsdPtr->fingers[1].id < 0) ? STATE_IDLE : STATE_DONE;
	if (tsdPtr->state == STATE_IDLE) {
	    tsdPtr->fingers[0].id = -1;
	}
	break;
    }
    if (tsdPtr->fingers[0].id < 0 && tsdPtr->fingers[1].id < 0) {
	tsdPtr->state = STATE_IDLE;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ModeEventProc --
 *
 *	Forget the touch mode of a window when it is destroyed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The entry of the window is deleted.
 *
 *----------------------------------------------------------------------
 */

static void
ModeEventProc(
    void *clientData,		/* The window. */
    XEvent *eventPtr)		/* A StructureNotify event. */
{
    ThreadSpecificData *tsdPtr = GetTSD();
    Tcl_HashEntry *hPtr;

    if (eventPtr->type != DestroyNotify) {
	return;
    }
    hPtr = Tcl_FindHashEntry(&tsdPtr->modeTable, clientData);
    if (hPtr) {
	Tcl_DeleteHashEntry(hPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkTouchObjCmd --
 *
 *	Implements "tk touch mode window ?mode?": get or set the touch mode
 *	of a window, auto, scroll or mouse.
 *
 * Results:
 *	A standard Tcl result: the mode.
 *
 * Side effects:
 *	May set the mode of the window.
 *
 *----------------------------------------------------------------------
 */

int
TkTouchObjCmd(
    void *clientData,		/* The main window. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,		/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    static const char *const optionNames[] = {"mode", NULL};
    ThreadSpecificData *tsdPtr = GetTSD();
    Tk_Window tkwin;
    Tcl_HashEntry *hPtr;
    int option, mode, isNew;

    if (objc < 3 || objc > 4) {
	Tcl_WrongNumArgs(interp, 1, objv, "mode window ?mode?");
	return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], optionNames, "option", 0,
	    &option) != TCL_OK) {
	return TCL_ERROR;
    }
    tkwin = Tk_NameToWindow(interp, Tcl_GetString(objv[2]),
	    (Tk_Window)clientData);
    if (tkwin == NULL) {
	return TCL_ERROR;
    }
    hPtr = Tcl_FindHashEntry(&tsdPtr->modeTable, tkwin);
    if (objc == 4) {
	if (Tcl_GetIndexFromObj(interp, objv[3], modeNames, "mode", 0,
		&mode) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (mode == MODE_AUTO) {
	    if (hPtr) {
		Tcl_DeleteHashEntry(hPtr);
		Tk_DeleteEventHandler(tkwin, StructureNotifyMask,
			ModeEventProc, tkwin);
	    }
	} else {
	    if (hPtr == NULL) {
		hPtr = Tcl_CreateHashEntry(&tsdPtr->modeTable, tkwin, &isNew);
		Tk_CreateEventHandler(tkwin, StructureNotifyMask,
			ModeEventProc, tkwin);
	    }
	    Tcl_SetHashValue(hPtr, INT2PTR(mode));
	}
	hPtr = Tcl_FindHashEntry(&tsdPtr->modeTable, tkwin);
    }
    mode = hPtr ? PTR2INT(Tcl_GetHashValue(hPtr)) : MODE_AUTO;
    Tcl_SetObjResult(interp, Tcl_NewStringObj(modeNames[mode], -1));
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
