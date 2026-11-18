/*
 * tkHaikuEvent.c --
 *
 *	The display of the Haiku port, and the translation of the events of
 *	the application and its windows into X events: the mouse, keys,
 *	focus, and the changes the user makes to the windows.
 *
 *	The application and its windows run in threads of their own, and post
 *	events with TkHaikuPostEvent, which queues them and writes a byte to a
 *	pipe. Tcl watches the pipe with a file handler, which handles the
 *	queued events in the thread of Tk.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkHaikuInt.h"
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <poll.h>
#include <pthread.h>

TkXeDisplay *tkXeDisplay = NULL;

/*
 * The queue of the events posted by the glue, protected by a mutex.
 */

typedef struct QueuedEvent {
    TkHaikuEvent event;
    struct QueuedEvent *nextPtr;
} QueuedEvent;

static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
static QueuedEvent *queueHead = NULL;
static QueuedEvent *queueTail = NULL;
static int queuePipe[2] = {-1, -1};

typedef struct {
    bool initialized;
} ThreadSpecificData;
static Tcl_ThreadDataKey dataKey;

/*
 * The X window which has the input focus, set with XSetInputFocus, and the
 * toplevel which has the keyboard, given by the window manager.
 */

static Window focusWindow = None;
static TkXeSurface *keyboardSurface = NULL;

/*
 * The window which has captured the pointer, and the surface whose BWindow
 * gets the pointer events of the whole screen for it.
 */

static TkWindow *captureWinPtr = NULL;
static TkXeSurface *captureSurface = NULL;

/*
 * Whether the window under the pointer is to be found again at idle time.
 */

static bool pointerIdlePending = false;

static void		ConfigureFonts(void);
static void		DisplayExitHandler(void *clientData);
static void		DisplayFileProc(void *clientData, int flags);
static void		DisplaySetupProc(void *clientData, int flags);
static void		DisplayCheckProc(void *clientData, int flags);
static void		HandleEvent(TkHaikuEvent *eventPtr);
static void		HandleKey(TkHaikuEvent *eventPtr);
static void		HandleModifiers(TkHaikuEvent *eventPtr);
static void		HandlePointer(TkHaikuEvent *eventPtr);
static void		HandleWheel(TkHaikuEvent *eventPtr);
static bool		IsMove(const TkHaikuEvent *eventPtr);
static void		PointerIdle(void *clientData);
static void		ProcessQueue(void);
static void		QueueFocusEvent(int type, Window w);
static void		QueueFocusDetail(int type, Window w, int detail);
static bool		IsAncestor(Window ancestor, Window w);
static void		QueueKeyEvent(int type, KeySym keysym,
			    unsigned int keycode, const char *chars,
			    int length);
static void		QueueWheelEvent(unsigned int state, int delta);
static void		UpdatePointer(void);
static void		WarpTo(int x, int y);
static Tk_Window	WindowAtPoint(int x, int y);
static Tk_Window	WindowInSurface(TkXeSurface *surfPtr, int x, int y);

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuPostEvent --
 *
 *	Queue an event of the application or of a window, and wake Tk up.
 *	Called by the glue, from any thread.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The event is queued, and the pipe Tk watches is written to.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuPostEvent(
    const TkHaikuEvent *eventPtr)	/* The event. */
{
    QueuedEvent *qPtr = (QueuedEvent *)malloc(sizeof(QueuedEvent));
    char byte = 0;

    qPtr->event = *eventPtr;
    qPtr->nextPtr = NULL;
    pthread_mutex_lock(&queueMutex);
    if (queueTail) {
	queueTail->nextPtr = qPtr;
    } else {
	queueHead = qPtr;
    }
    queueTail = qPtr;
    if (queuePipe[1] >= 0 && write(queuePipe[1], &byte, 1) < 0) {
	/*
	 * The pipe is full: Tk has bytes to read already.
	 */
    }
    pthread_mutex_unlock(&queueMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * ProcessQueue, IsMove --
 *
 *	Handle the events posted by the glue. A move of the pointer followed
 *	by another is dropped, as X servers compress motion.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Whatever the events do.
 *
 *----------------------------------------------------------------------
 */

static bool
IsMove(
    const TkHaikuEvent *eventPtr)	/* An event. */
{
    return eventPtr->type == TKHAIKU_POINTER
	    && eventPtr->u.pointer.action == TKHAIKU_MOVE;
}

static void
ProcessQueue(void)
{
    char buf[64];

    if (queuePipe[0] >= 0) {
	while (read(queuePipe[0], buf, sizeof(buf)) > 0) {
	    /* Empty the pipe. */
	}
    }
    for (;;) {
	QueuedEvent *qPtr;
	bool superseded;

	pthread_mutex_lock(&queueMutex);
	qPtr = queueHead;
	if (qPtr) {
	    queueHead = qPtr->nextPtr;
	    if (queueHead == NULL) {
		queueTail = NULL;
	    }
	}
	superseded = qPtr && queueHead && IsMove(&qPtr->event)
		&& IsMove(&queueHead->event)
		&& queueHead->event.u.pointer.buttons
			== qPtr->event.u.pointer.buttons;
	pthread_mutex_unlock(&queueMutex);
	if (qPtr == NULL) {
	    break;
	}
	if (!superseded) {
	    HandleEvent(&qPtr->event);
	}
	free(qPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * HandleEvent --
 *
 *	Handle an event of the application or of a window.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	X events are queued, windows change, ...
 *
 *----------------------------------------------------------------------
 */

static void
HandleEvent(
    TkHaikuEvent *eventPtr)	/* The event. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    TkXeSurface *surfPtr = NULL;

    if (dispPtr->display == NULL) {
	return;
    }
    if (eventPtr->time) {
	dispPtr->lastTime = (Time)eventPtr->time;
    }
    if (eventPtr->window) {
	surfPtr = TkXeGetSurface((Window)eventPtr->window);
    }
    switch (eventPtr->type) {
    case TKHAIKU_POINTER:
	HandlePointer(eventPtr);
	break;
    case TKHAIKU_WHEEL:
	HandleWheel(eventPtr);
	break;
    case TKHAIKU_KEY:
	HandleKey(eventPtr);
	break;
    case TKHAIKU_MODIFIERS:
	HandleModifiers(eventPtr);
	break;
    case TKHAIKU_ACTIVATED:
    case TKHAIKU_MOVED:
    case TKHAIKU_RESIZED:
    case TKHAIKU_CLOSE:
    case TKHAIKU_MINIMIZED:
    case TKHAIKU_ZOOM:
	if (surfPtr) {
	    TkHaikuWmEvent(surfPtr, eventPtr);
	}
	break;
    case TKHAIKU_SCREEN:
	TkHaikuUpdateScreen();
	TkHaikuWmScreenChanged();
	break;
    case TKHAIKU_CLIPBOARD:
	TkHaikuClipboardChanged();
	break;
    case TKHAIKU_QUIT:
	TkHaikuWmQuit();
	break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Pointer events --
 *
 *	The mouse is given to Tk_UpdatePointer (tkPointer.c), which
 *	generates the Enter, Leave, Motion and Button events, and handles
 *	grabs, as on Aqua, Windows, Wayland and Android.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * WindowAtPoint, WindowInSurface --
 *
 *	Find the Tk window at a point of the screen (in a surface).
 *
 * Results:
 *	The deepest Tk window there, or NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Tk_Window
WindowAtPoint(
    int x, int y)		/* In root coordinates. */
{
    return WindowInSurface(TkHaikuSurfaceAt(x, y, NULL), x, y);
}

static Tk_Window
WindowInSurface(
    TkXeSurface *surfPtr,	/* The surface at the point, or NULL. */
    int x, int y)		/* In root coordinates. */
{
    TkXeWindow *winPtr;

    if (surfPtr == NULL) {
	return NULL;
    }
    winPtr = surfPtr->winPtr;
    winPtr = TkXeWindowAtPoint(winPtr, x - winPtr->x - winPtr->borderWidth,
	    y - winPtr->y - winPtr->borderWidth);
    for (; winPtr; winPtr = winPtr->parent) {
	Tk_Window tkwin = Tk_IdToWindow(winPtr->display, winPtr->id);

	if (tkwin) {
	    return tkwin;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * UpdatePointer --
 *
 *	Tell tkPointer.c where the pointer is and which buttons are
 *	pressed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Crossing, motion and button events are queued.
 *
 *----------------------------------------------------------------------
 */

static void
UpdatePointer(void)
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    int x = (int)floor(dispPtr->pointerX);
    int y = (int)floor(dispPtr->pointerY);

    Tk_UpdatePointer(dispPtr->pointerInside ? WindowAtPoint(x, y) : NULL,
	    x, y, dispPtr->modState | dispPtr->buttonState);
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuPointerWindowChanged, PointerIdle --
 *
 *	Called when the window tree changed: the pointer may now be in
 *	another window (the window it was in was destroyed, unmapped or
 *	moved, or another was mapped under it), which gets crossing events at
 *	idle time, as the X server generates them. (The app_server sends
 *	events too, but with the time of the last move of the mouse, from
 *	before a warp.)
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Crossing events may be queued.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuPointerWindowChanged(void)
{
    if (!pointerIdlePending) {
	pointerIdlePending = true;
	Tcl_DoWhenIdle(PointerIdle, NULL);
    }
}

static void
PointerIdle(
    TCL_UNUSED(void *))
{
    TkXeDisplay *dispPtr = tkXeDisplay;

    pointerIdlePending = false;
    if (dispPtr->display == NULL) {
	return;
    }
    dispPtr->pointerSurface = TkHaikuSurfaceAt((int)floor(dispPtr->pointerX),
	    (int)floor(dispPtr->pointerY), NULL);
    dispPtr->pointerInside = (dispPtr->pointerSurface != NULL);
    UpdatePointer();
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuPointerLeaveSurface --
 *
 *	Called when the window of a surface is about to be unmapped or
 *	destroyed: if the pointer is in it, it goes into the window below at
 *	once, while the window it leaves still exists, so that the crossing
 *	events are as X gives them (from one toplevel to another, they are
 *	nonlinear).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Crossing events may be queued.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuPointerLeaveSurface(
    TkXeSurface *surfPtr)	/* The surface going away. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    int x = (int)floor(dispPtr->pointerX), y = (int)floor(dispPtr->pointerY);

    if (dispPtr->display == NULL || !dispPtr->pointerInside
	    || dispPtr->pointerSurface != surfPtr) {
	return;
    }
    dispPtr->pointerSurface = TkHaikuSurfaceAt(x, y, surfPtr);
    dispPtr->pointerInside = (dispPtr->pointerSurface != NULL);
    Tk_UpdatePointer(WindowInSurface(dispPtr->pointerSurface, x, y), x, y,
	    dispPtr->modState | dispPtr->buttonState);
}

/*
 *----------------------------------------------------------------------
 *
 * HandlePointer --
 *
 *	Handle a mouse event of a window. The pointer is in Tk if it is in
 *	the window the event is for, or (when the pointer is grabbed, and the
 *	window gets the events of the whole screen) in another window of Tk.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Pointer events are generated.
 *
 *----------------------------------------------------------------------
 */

static void
HandlePointer(
    TkHaikuEvent *eventPtr)	/* A TKHAIKU_POINTER event. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    uint32_t b = eventPtr->u.pointer.buttons;
    TkXeSurface *eventSurfPtr = TkXeGetSurface((Window)eventPtr->window);
    TkXeSurface *atPtr;

    if (dispPtr->warpPending) {
	/*
	 * The input server moves the pointer where it was warped later: the
	 * events until it is there are from before (their time may even be
	 * later than the warp). The pointer may not move (it was there
	 * already), so this lasts half a second at most.
	 */

	if (TkHaikuToScreen(dispPtr->warpX) == (int)eventPtr->u.pointer.x
		&& TkHaikuToScreen(dispPtr->warpY)
			== (int)eventPtr->u.pointer.y) {
	    dispPtr->warpPending = false;
	} else if (eventPtr->time < dispPtr->warpTime + 500) {
	    return;
	} else {
	    dispPtr->warpPending = false;
	}
    } else if (eventPtr->time < dispPtr->warpTime) {
	return;
    }
    dispPtr->pointerX = TkHaikuFromScreen(eventPtr->u.pointer.x);
    dispPtr->pointerY = TkHaikuFromScreen(eventPtr->u.pointer.y);
    dispPtr->modState = TkHaikuModState(eventPtr->u.pointer.modifiers);
    dispPtr->buttonState = ((b & TKHAIKU_BUTTON1) ? Button1Mask : 0)
	    | ((b & TKHAIKU_BUTTON3) ? Button2Mask : 0)
	    | ((b & TKHAIKU_BUTTON2) ? Button3Mask : 0);
    atPtr = TkHaikuSurfaceAt((int)floor(dispPtr->pointerX),
	    (int)floor(dispPtr->pointerY), NULL);
    if (eventPtr->u.pointer.action == TKHAIKU_EXIT) {
	/*
	 * The pointer left a window: it is still in Tk if it went into
	 * another window of Tk, which will tell.
	 */

	if (dispPtr->pointerSurface == eventSurfPtr) {
	    dispPtr->pointerSurface = NULL;
	    dispPtr->pointerInside = false;
	}
    } else if (eventPtr->u.pointer.inside) {
	dispPtr->pointerSurface = eventSurfPtr;
	dispPtr->pointerInside = true;
    } else {
	dispPtr->pointerInside = (atPtr != NULL && atPtr != eventSurfPtr);
	dispPtr->pointerSurface = dispPtr->pointerInside ? atPtr : NULL;
    }
    UpdatePointer();
}

/*
 *----------------------------------------------------------------------
 *
 * QueueWheelEvent --
 *
 *	Queue a MouseWheel event, of 120 units per notch, for the window
 *	under the pointer (or the window which captured it).
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
QueueWheelEvent(
    unsigned int state,		/* The modifiers. */
    int delta)			/* The amount, positive to scroll up. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    int x = (int)floor(dispPtr->pointerX), y = (int)floor(dispPtr->pointerY);
    Tk_Window tkwin = captureWinPtr ? (Tk_Window)captureWinPtr
	    : WindowAtPoint(x, y);
    int winX, winY;
    XEvent event;

    if (tkwin == NULL || delta == 0) {
	return;
    }
    Tk_GetRootCoords(tkwin, &winX, &winY);
    memset(&event, 0, sizeof(XEvent));
    event.type = MouseWheelEvent;
    event.xany.display = Tk_Display(tkwin);
    event.xany.window = Tk_WindowId(tkwin);
    event.xany.serial = LastKnownRequestProcessed(Tk_Display(tkwin));
    event.xbutton.x = x - winX;
    event.xbutton.y = y - winY;
    event.xbutton.x_root = x;
    event.xbutton.y_root = y;
    event.xbutton.state = state;
    event.xbutton.time = dispPtr->lastTime;
    event.xkey.keycode = (unsigned)delta;
    Tk_QueueWindowEvent(&event, TCL_QUEUE_TAIL);
}

/*
 *----------------------------------------------------------------------
 *
 * HandleWheel --
 *
 *	Handle a turn of the mouse wheel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	MouseWheel events are queued, with Shift for horizontal scrolling.
 *
 *----------------------------------------------------------------------
 */

static void
HandleWheel(
    TkHaikuEvent *eventPtr)	/* A TKHAIKU_WHEEL event. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    unsigned int state;

    dispPtr->modState = TkHaikuModState(eventPtr->u.wheel.modifiers);
    state = dispPtr->modState | dispPtr->buttonState;
    QueueWheelEvent(state, (int)lround(-eventPtr->u.wheel.dy * 120));
    QueueWheelEvent(state | ShiftMask,
	    (int)lround(-eventPtr->u.wheel.dx * 120));
}

/*
 *----------------------------------------------------------------------
 *
 * Keyboard events --
 *
 *	The keycode of an X event is the key code of the Be API plus 8. The
 *	keysym and the characters of a key are computed when the event is
 *	generated, and stored in the TkKeyEvent.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * QueueFocusEvent --
 *
 *	Queue a FocusIn or FocusOut event.
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
    int type,			/* FocusIn or FocusOut. */
    Window w)			/* The window. */
{
    QueueFocusDetail(type, w, NotifyNonlinear);
}

static void
QueueFocusDetail(
    int type,			/* FocusIn or FocusOut. */
    Window w,			/* The window. */
    int detail)			/* NotifyNonlinear etc. */
{
    XEvent event;

    memset(&event, 0, sizeof(XEvent));
    event.xfocus.type = type;
    event.xfocus.display = tkXeDisplay->display;
    event.xfocus.window = w;
    event.xfocus.mode = NotifyNormal;
    event.xfocus.detail = detail;
    TkXeQueueEvent(&event);
}

/*
 *----------------------------------------------------------------------
 *
 * IsAncestor --
 *
 *	Whether a window is an ancestor of another.
 *
 * Results:
 *	True if it is.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static bool
IsAncestor(
    Window ancestor,		/* The window which may be an ancestor. */
    Window w)			/* The other window. */
{
    TkXeWindow *winPtr = TkXeGetWindow(w);

    for (winPtr = winPtr ? winPtr->parent : NULL; winPtr;
	    winPtr = winPtr->parent) {
	if (winPtr->id == ancestor) {
	    return true;
	}
    }
    return false;
}

/*
 *----------------------------------------------------------------------
 *
 * QueueKeyEvent --
 *
 *	Queue a key event for the window which has the focus, with its
 *	keysym and, for a KeyPress, its characters.
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
QueueKeyEvent(
    int type,			/* KeyPress or KeyRelease. */
    KeySym keysym,		/* The keysym of the key. */
    unsigned int keycode,	/* The X keycode, or 0. */
    const char *chars,		/* The characters of a KeyPress, in
				 * UTF-8. */
    int length)			/* Their length in bytes. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    TkXeSurface *surfPtr = keyboardSurface;
    union {
	XEvent x;
	TkKeyEvent k;
    } event;

    if (surfPtr == NULL) {
	return;
    }
    memset(&event, 0, sizeof(event));
    event.x.xkey.type = type;
    event.x.xkey.display = dispPtr->display;
    event.x.xkey.window = TkXeGetWindow(focusWindow)
	    ? focusWindow : surfPtr->winPtr->id;
    event.x.xkey.root = RootWindow(dispPtr->display, 0);
    event.x.xkey.time = dispPtr->lastTime;
    event.x.xkey.x_root = (int)floor(dispPtr->pointerX);
    event.x.xkey.y_root = (int)floor(dispPtr->pointerY);
    event.x.xkey.x = event.x.xkey.x_root - surfPtr->winPtr->x;
    event.x.xkey.y = event.x.xkey.y_root - surfPtr->winPtr->y;
    event.x.xkey.state = dispPtr->modState | dispPtr->buttonState;
    event.x.xkey.keycode = (keycode <= 255) ? keycode : 0;
    event.x.xkey.same_screen = True;
    event.k.keysym = keysym;
    if (type == KeyPress) {
	event.k.charValuePtr = (char *)Tcl_Alloc(length + 1);
	memcpy(event.k.charValuePtr, chars, length);
	event.k.charValuePtr[length] = '\0';
	event.k.charValueLen = length;
    }
    TkXeQueueEvent(&event.x);
}

/*
 *----------------------------------------------------------------------
 *
 * HandleKey --
 *
 *	Handle a key event of a window.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A key event is queued, and the modifier state is updated.
 *
 *----------------------------------------------------------------------
 */

static void
HandleKey(
    TkHaikuEvent *eventPtr)	/* A TKHAIKU_KEY event. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    uint32_t modifiers = eventPtr->u.key.modifiers;
    KeySym keysym;
    const char *chars = "";
    char buf[8];
    int len = 0;

    dispPtr->modState = TkHaikuModState(modifiers);
    keysym = TkHaikuKeySym(eventPtr->u.key.key, modifiers,
	    eventPtr->u.key.bytes);
    if (keysym == NoSymbol) {
	return;
    }
    if (eventPtr->u.key.down) {
	unsigned char c = UCHAR(eventPtr->u.key.bytes[0]);

	if ((dispPtr->modState & ControlMask) && keysym < 0x80
		&& isalpha((int)keysym)) {
	    buf[0] = (char)(keysym & 0x1F);
	    chars = buf;
	    len = 1;
	} else if (c >= 0x20 && c != 0x7F
		&& !(keysym >= XK_KP_Space && keysym <= XK_KP_Equal
		&& keysym != XK_KP_Enter && c < 0x20)) {
	    chars = eventPtr->u.key.bytes;
	    len = (int)strlen(chars);
	} else if (keysym == XK_Return || keysym == XK_KP_Enter) {
	    chars = "\r";
	    len = 1;
	} else if (keysym == XK_Tab || keysym == XK_ISO_Left_Tab) {
	    chars = "\t";
	    len = 1;
	} else if (keysym == XK_BackSpace) {
	    chars = "\b";
	    len = 1;
	} else if (keysym == XK_Escape) {
	    chars = "\033";
	    len = 1;
	} else if (keysym == XK_Delete) {
	    chars = "\177";
	    len = 1;
	}
    }
    QueueKeyEvent(eventPtr->u.key.down ? KeyPress : KeyRelease, keysym,
	    (unsigned)eventPtr->u.key.key + 8, chars, len);
}

/*
 *----------------------------------------------------------------------
 *
 * HandleModifiers --
 *
 *	The modifiers changed. (The presses and releases of the modifier
 *	keys come as key events, as unmapped keys.)
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The modifier state is updated.
 *
 *----------------------------------------------------------------------
 */

static void
HandleModifiers(
    TkHaikuEvent *eventPtr)	/* A TKHAIKU_MODIFIERS event. */
{
    tkXeDisplay->modState = TkHaikuModState(eventPtr->u.modifiers.modifiers);
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuSetKeyboardSurface, TkHaikuKeyboardSurface --
 *
 *	Give the keyboard to a toplevel, as a window manager does when the
 *	user activates it, or get the toplevel which has it.
 *
 * Results:
 *	TkHaikuKeyboardSurface returns the surface, or NULL.
 *
 * Side effects:
 *	FocusOut and FocusIn events are queued.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuSetKeyboardSurface(
    TkXeSurface *surfPtr)	/* The surface of the toplevel, or NULL. */
{
    if (surfPtr == keyboardSurface) {
	return;
    }
    if (TkXeGetWindow(focusWindow)) {
	QueueFocusEvent(FocusOut, focusWindow);
    }
    keyboardSurface = surfPtr;
    focusWindow = surfPtr ? surfPtr->winPtr->id : None;
    if (surfPtr) {
	QueueFocusEvent(FocusIn, focusWindow);
    }
}

TkXeSurface *
TkHaikuKeyboardSurface(void)
{
    return keyboardSurface;
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuUpdateScreen --
 *
 *	Set the size of the screen of the display, and the scale, from the
 *	screen of Haiku and the size of its fonts. TK_HAIKU_SCALE sets the
 *	scale, for testing.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The screen of the display changes.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuUpdateScreen(void)
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    const char *forced = getenv("TK_HAIKU_SCALE");
    int width, height;
    double dpi, scale = forced ? atof(forced) : TkHaikuFontScale();

    if (scale < 1.0) {
	scale = 1.0;
    }
    if (!forced) {
	/*
	 * Steps of a quarter, as the fonts the user can choose are.
	 */

	scale = floor(scale * 4 + 0.5) / 4;
    }
    dispPtr->scale = scale;
    TkHaikuScreenSize(&width, &height, &dpi);
    if (dispPtr->display) {
	int w = (int)floor(width / scale), h = (int)floor(height / scale);

	TkXeSetScreenSize(dispPtr->display, w, h,
		(int)lround(w * 25.4 / 96.0), (int)lround(h * 25.4 / 96.0));
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ConfigureFonts --
 *
 *	Make the generic families of fontconfig, which the named fonts of Tk
 *	use, those of the fonts the user chose in the preferences of Haiku:
 *	sans-serif is the plain font, and monospace the fixed font. The
 *	families of the core fonts of X which scripts name (Helvetica,
 *	Courier, Times), which other systems map to fonts of the same metrics
 *	that Haiku does not have, are mapped to fonts of the same kind: the
 *	configuration of fontconfig on Haiku maps them all, monospace too, to
 *	Noto Sans.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Rules are added to the current configuration of fontconfig.
 *
 *----------------------------------------------------------------------
 */

static void
ConfigureFonts(void)
{
    static const struct {
	const char *family;
	int kind;		/* 0: plain, 1: fixed, 2: serif. */
    } aliases[] = {
	{"sans-serif", 0}, {"Helvetica", 0}, {"Arial", 0},
	{"monospace", 1}, {"Courier", 1}, {"Courier New", 1}, {"fixed", 1},
	{"serif", 2}, {"Times", 2}, {"Times New Roman", 2},
	{NULL, 0}
    };
    char plain[64], fixed[64];
    const char *targets[3];
    Tcl_DString ds;
    int i;

    TkHaikuFontFamilies(plain, fixed, sizeof(plain));
    if (strpbrk(plain, "<>&\"'") || strpbrk(fixed, "<>&\"'")) {
	return;
    }
    targets[0] = plain;
    targets[1] = fixed;
    targets[2] = "Noto Serif";
    Tcl_DStringInit(&ds);
    Tcl_DStringAppend(&ds, "<?xml version=\"1.0\"?>\n"
	    "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n<fontconfig>\n",
	    -1);
    for (i = 0; aliases[i].family; i++) {
	Tcl_DStringAppend(&ds, "<alias binding=\"same\"><family>", -1);
	Tcl_DStringAppend(&ds, aliases[i].family, -1);
	Tcl_DStringAppend(&ds, "</family><prefer><family>", -1);
	Tcl_DStringAppend(&ds, targets[aliases[i].kind], -1);
	Tcl_DStringAppend(&ds, "</family></prefer></alias>\n", -1);
    }
    Tcl_DStringAppend(&ds, "</fontconfig>\n", -1);
    FcInit();
    FcConfigParseAndLoadFromMemory(FcConfigGetCurrent(),
	    (const FcChar8 *)Tcl_DStringValue(&ds), FcFalse);
    Tcl_DStringFree(&ds);
}

/*
 *----------------------------------------------------------------------
 *
 * TkpOpenDisplay --
 *
 *	Open the display, which is the screen of Haiku. There is only one per
 *	process.
 *
 * Results:
 *	The display, or NULL.
 *
 * Side effects:
 *	The application of the Be API is started.
 *
 *----------------------------------------------------------------------
 */

TkDisplay *
TkpOpenDisplay(
    const char *displayNameStr)	/* The name of the display. */
{
    TkDisplay *dispPtr;
    int width, height, mouseX, mouseY;
    uint32_t buttons;
    double dpi;

    if (tkXeDisplay) {
	/*
	 * Only one display per process; any name refers to it.
	 */

	for (dispPtr = TkGetDisplayList(); dispPtr;
		dispPtr = dispPtr->nextPtr) {
	    if (dispPtr->display == tkXeDisplay->display) {
		return NULL;
	    }
	}
    } else {
	if (pipe(queuePipe) < 0) {
	    return NULL;
	}
	fcntl(queuePipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(queuePipe[1], F_SETFD, FD_CLOEXEC);
	fcntl(queuePipe[0], F_SETFL, O_NONBLOCK);
	fcntl(queuePipe[1], F_SETFL, O_NONBLOCK);
	if (!TkHaikuStartApp("application/x-vnd.tcl-tk")) {
	    close(queuePipe[0]);
	    close(queuePipe[1]);
	    queuePipe[0] = queuePipe[1] = -1;
	    return NULL;
	}
	tkXeDisplay = (TkXeDisplay *)Tcl_Alloc(sizeof(TkXeDisplay));
	memset(tkXeDisplay, 0, sizeof(TkXeDisplay));
	tkXeDisplay->eventFd = queuePipe[0];
	tkXeDisplay->scale = 1.0;
	ConfigureFonts();
    }

    TkHaikuScreenSize(&width, &height, &dpi);
    TkHaikuGetMouse(&mouseX, &mouseY, &buttons);
    tkXeDisplay->pointerX = mouseX;
    tkXeDisplay->pointerY = mouseY;
    tkXeDisplay->display = TkXeOpenXDisplay(
	    displayNameStr ? displayNameStr : "haiku", tkXeDisplay->eventFd,
	    width, height, 0, 0);
    TkHaikuUpdateScreen();

    dispPtr = (TkDisplay *)Tcl_Alloc(sizeof(TkDisplay));
    memset(dispPtr, 0, sizeof(TkDisplay));
    dispPtr->display = tkXeDisplay->display;
    Tcl_CreateFileHandler(tkXeDisplay->eventFd, TCL_READABLE,
	    DisplayFileProc, NULL);
    TkpInitKeymapInfo(dispPtr);
    return dispPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkpCloseDisplay --
 *
 *	Close the display.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The pipe of the events is no longer watched.
 *
 *----------------------------------------------------------------------
 */

void
TkpCloseDisplay(
    TkDisplay *dispPtr)		/* The display. */
{
    TkSendCleanup(dispPtr);
    TkWmCleanup(dispPtr);
    if (tkXeDisplay && dispPtr->display == tkXeDisplay->display
	    && tkXeDisplay->eventFd >= 0) {
	Tcl_DeleteFileHandler(tkXeDisplay->eventFd);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * The event source --
 *
 *	The events of the glue are handled when the pipe is readable.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * TkCreateXEventSource --
 *
 *	Create the event source of the display, once per thread.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event source and an exit handler are created.
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
 *	Delete the event source when Tk exits.
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
 *	The events come through a pipe, watched by a file handler: the event
 *	source has nothing to do.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
DisplaySetupProc(
    TCL_UNUSED(void *),
    TCL_UNUSED(int))
{
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
 * DisplayFileProc --
 *
 *	Called when the pipe of the events is readable.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The events are handled.
 *
 *----------------------------------------------------------------------
 */

static void
DisplayFileProc(
    TCL_UNUSED(void *),
    TCL_UNUSED(int))
{
    ProcessQueue();
}

/*
 *----------------------------------------------------------------------
 *
 * TkUnixDoOneXEvent --
 *
 *	Wait for and handle one window event, or until the time given has
 *	passed. Used by tkUnixWm.c to wait for the window manager.
 *
 * Results:
 *	True if an event was handled, false if the time passed.
 *
 * Side effects:
 *	Events are handled.
 *
 *----------------------------------------------------------------------
 */

bool
TkUnixDoOneXEvent(
    Tcl_Time *timePtr)		/* When to give up, or NULL. */
{
    struct pollfd pfd;
    int timeout = -1;

    if (Tcl_ServiceEvent(TCL_WINDOW_EVENTS)) {
	return true;
    }
    if (tkXeDisplay == NULL) {
	return false;
    }
    ProcessQueue();
    if (Tcl_ServiceEvent(TCL_WINDOW_EVENTS)) {
	return true;
    }
    if (timePtr) {
	Tcl_Time now;

	Tcl_GetTime(&now);
	timeout = (int)((timePtr->sec - now.sec) * 1000
		+ (timePtr->usec - now.usec) / 1000);
	if (timeout < 0) {
	    timeout = 0;
	}
    }
    pfd.fd = tkXeDisplay->eventFd;
    pfd.events = POLLIN;
    if (pfd.fd >= 0 && poll(&pfd, 1, timeout) > 0) {
	ProcessQueue();
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
 *	The time of the last input event.
 *
 * Results:
 *	The time, in milliseconds.
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
 * TkXeBell --
 *
 *	Ring the bell: the beep of the system.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A sound is played.
 *
 *----------------------------------------------------------------------
 */

void
TkXeBell(void)
{
    TkHaikuBeep();
}

/*
 *----------------------------------------------------------------------
 *
 * Focus --
 *
 *	The focus moves between the windows of the toplevel which has the
 *	keyboard. Setting it in another toplevel activates that toplevel.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * XSetInputFocus --
 *
 *	Move the focus to a window, activating its toplevel if another one
 *	has the keyboard (an override-redirect window gets the keyboard
 *	without being activated).
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	FocusOut and FocusIn events are queued.
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
    if (topPtr == NULL || topPtr->surface == NULL || !topPtr->mapped
	    || topPtr->surface->window == NULL || topPtr->surface->iconic) {
	return Success;
    }
    if (topPtr->surface != keyboardSurface) {
	/*
	 * The toplevel is activated, and the focus goes from the window
	 * which had it straight to the new one, as with an X server.
	 */

	if (!topPtr->surface->popup) {
	    TkHaikuWindowActivate(topPtr->surface->window);
	}
	keyboardSurface = topPtr->surface;
    }
    if (focus != focusWindow) {
	/*
	 * As with X, the details tell whether the focus moves to an
	 * inferior (an embedded application, for which the container keeps
	 * it) or an ancestor.
	 */

	int outDetail = NotifyNonlinear, inDetail = NotifyNonlinear;

	if (TkXeGetWindow(focusWindow) && IsAncestor(focusWindow, focus)) {
	    outDetail = NotifyInferior;
	    inDetail = NotifyAncestor;
	} else if (TkXeGetWindow(focusWindow)
		&& IsAncestor(focus, focusWindow)) {
	    outDetail = NotifyAncestor;
	    inDetail = NotifyInferior;
	}
	if (TkXeGetWindow(focusWindow)) {
	    QueueFocusDetail(FocusOut, focusWindow, outDetail);
	}
	focusWindow = focus;
	QueueFocusDetail(FocusIn, focusWindow, inDetail);
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
 *	Success.
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
 *	The grabs of Tk work on the windows of the application, which get the
 *	keyboard through the toplevel which has it.
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

/*
 *----------------------------------------------------------------------
 *
 * TkpSetCapture, TkpGetCapture --
 *
 *	Set or get the window which captured the pointer, for tkPointer.c.
 *	While the pointer is captured, a BWindow of the application gets the
 *	pointer events of the whole screen, so that a click outside the
 *	application (which dismisses a menu) is seen, as with a grab of X.
 *
 * Results:
 *	TkpGetCapture returns the window, or NULL.
 *
 * Side effects:
 *	The pointer events of a BWindow change.
 *
 *----------------------------------------------------------------------
 */

void
TkpSetCapture(
    TkWindow *winPtr)		/* The window, or NULL. */
{
    captureWinPtr = winPtr;
    TkHaikuUpdateCapture();
}

Tk_Window
TkpGetCapture(void)
{
    return (Tk_Window)captureWinPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuUpdateCapture --
 *
 *	Give the pointer events of the whole screen to the BWindow of the
 *	window which captured the pointer (or of its toplevel, or of any
 *	shown window), or to none.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The pointer events of BWindows change.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuUpdateCapture(void)
{
    TkXeSurface *surfPtr = NULL;

    if (captureWinPtr && tkXeDisplay) {
	TkXeWindow *winPtr = TkXeGetWindow(captureWinPtr->window);
	TkXeWindow *topPtr = winPtr ? TkXeTopWindow(winPtr) : NULL;

	if (topPtr && topPtr->surface && topPtr->surface->shown) {
	    surfPtr = topPtr->surface;
	} else if (keyboardSurface && keyboardSurface->shown) {
	    surfPtr = keyboardSurface;
	} else {
	    for (surfPtr = tkXeDisplay->surfaces; surfPtr;
		    surfPtr = surfPtr->nextPtr) {
		if (surfPtr->shown) {
		    break;
		}
	    }
	}
    }
    if (surfPtr == captureSurface) {
	return;
    }
    if (captureSurface && captureSurface->window) {
	TkHaikuWindowCapture(captureSurface->window, false);
    }
    captureSurface = surfPtr;
    if (surfPtr && surfPtr->window) {
	TkHaikuWindowCapture(surfPtr->window, true);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuForgetSurface --
 *
 *	Forget a surface which is destroyed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The surface no longer has the keyboard or the pointer events.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuForgetSurface(
    TkXeSurface *surfPtr)	/* The surface. */
{
    if (captureSurface == surfPtr) {
	captureSurface = NULL;
    }
    if (keyboardSurface == surfPtr) {
	keyboardSurface = NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * XQueryPointer --
 *
 *	Where the pointer is, and the state of the buttons and modifiers. Any of the
 *	pointers may be NULL: Tk passes NULL for what it does not need, as on
 *	Aqua and Windows.
 *
 * Results:
 *	True.
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
    int x, y, rootX, rootY;
    uint32_t buttons;
    Window root = RootWindow(display, 0), child = None;

    TkXeNextRequest(display);
    TkHaikuGetMouse(&x, &y, &buttons);
    rootX = (int)floor(TkHaikuFromScreen(x));
    rootY = (int)floor(TkHaikuFromScreen(y));
    if (tkXeDisplay->warpPending
	    && TkHaikuSystemTime() < tkXeDisplay->warpTime + 500) {
	rootX = tkXeDisplay->warpX;
	rootY = tkXeDisplay->warpY;
    }
    if (rootPtr) {
	*rootPtr = root;
    }
    if (rootXPtr) {
	*rootXPtr = rootX;
    }
    if (rootYPtr) {
	*rootYPtr = rootY;
    }
    XTranslateCoordinates(display, root, (w != None) ? w : root, rootX,
	    rootY, &x, &y, &child);
    if (childPtr) {
	*childPtr = child;
    }
    if (winXPtr) {
	*winXPtr = x;
    }
    if (winYPtr) {
	*winYPtr = y;
    }
    if (maskPtr) {
	*maskPtr = TkHaikuModState(TkHaikuModifiers())
		| ((buttons & TKHAIKU_BUTTON1) ? Button1Mask : 0)
		| ((buttons & TKHAIKU_BUTTON3) ? Button2Mask : 0)
		| ((buttons & TKHAIKU_BUTTON2) ? Button3Mask : 0);
    }
    return True;
}

/*
 *----------------------------------------------------------------------
 *
 * XWarpPointer, TkpWarpPointer --
 *
 *	Move the pointer.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	The pointer moves, and Tk is told where it is.
 *
 *----------------------------------------------------------------------
 */

int
XWarpPointer(
    Display *display,
    TCL_UNUSED(Window),
    Window destWindow,
    TCL_UNUSED(int),
    TCL_UNUSED(int),
    TCL_UNUSED(unsigned int),
    TCL_UNUSED(unsigned int),
    int destX, int destY)
{
    int x = destX, y = destY;

    TkXeNextRequest(display);
    if (destWindow != None) {
	Window child;

	XTranslateCoordinates(display, destWindow, RootWindow(display, 0),
		destX, destY, &x, &y, &child);
    }
    WarpTo(x, y);
    return Success;
}

void
TkpWarpPointer(
    TkDisplay *dispPtr)		/* The display. */
{
    int x = dispPtr->warpX, y = dispPtr->warpY;

    if (dispPtr->warpWindow) {
	int rootX, rootY;

	Tk_GetRootCoords(dispPtr->warpWindow, &rootX, &rootY);
	x += rootX;
	y += rootY;
    }
    WarpTo(x, y);
    UpdatePointer();
}

/*
 *----------------------------------------------------------------------
 *
 * WarpTo --
 *
 *	Move the pointer, and remember where, until it is there.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The pointer moves (later); the events of the pointer from before are
 *	dropped.
 *
 *----------------------------------------------------------------------
 */

static void
WarpTo(
    int x, int y)		/* In root coordinates. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;

    TkHaikuSetMousePosition(TkHaikuToScreen(x), TkHaikuToScreen(y));
    dispPtr->warpTime = TkHaikuSystemTime();
    dispPtr->warpPending = true;
    dispPtr->warpX = x;
    dispPtr->warpY = y;
    dispPtr->pointerX = x;
    dispPtr->pointerY = y;
    dispPtr->pointerSurface = TkHaikuSurfaceAt(x, y, NULL);
    dispPtr->pointerInside = (dispPtr->pointerSurface != NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * XSendEvent --
 *
 *	Events can only be sent to the windows of this process, and client
 *	messages to the root window go to the window manager.
 *
 * Results:
 *	Nonzero.
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
