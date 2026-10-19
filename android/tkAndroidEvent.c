/*
 * tkAndroidEvent.c --
 *
 *	The display of the Android port, and the translation of the events
 *	of the activity into X events: touches and the mouse, keys, focus,
 *	and the changes of the window of the activity.
 *
 *	The callbacks of the activity run in the main thread of the
 *	application, and post events with TkAndroidPostEvent, which queues
 *	them and writes a byte to a pipe. Tcl watches the pipe with a file
 *	handler, which handles the queued events in the thread of Tk.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#define _GNU_SOURCE		/* For pipe2. */
#include "tkAndroidInt.h"
#include <android/configuration.h>
#include <android/input.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>

TkXeDisplay *tkXeDisplay = NULL;
ANativeActivity *tkAndroidActivity = NULL;

/*
 * The queue of the events posted by the glue. It is protected by a mutex,
 * since events are posted from the main thread of the application.
 */

typedef struct QueuedEvent {
    TkAndroidEvent event;
    unsigned long serial;	/* Number of the event, for
				 * TkAndroidPostEventAndWait. */
    struct QueuedEvent *nextPtr;
} QueuedEvent;

static pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queueCond = PTHREAD_COND_INITIALIZER;
static QueuedEvent *queueHead = NULL;
static QueuedEvent *queueTail = NULL;
static unsigned long postedSerial = 0;
				/* Serial of the last posted event. */
static unsigned long handledSerial = 0;
				/* Serial of the last handled event. */
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
 * The window which has captured the pointer.
 */

static TkWindow *captureWinPtr = NULL;

static void		DisplayExitHandler(void *clientData);
static void		DisplayFileProc(void *clientData, int flags);
static void		DisplaySetupProc(void *clientData, int flags);
static void		DisplayCheckProc(void *clientData, int flags);
static void		HandleEvent(TkAndroidEvent *eventPtr);
static void		HandleKey(TkAndroidEvent *eventPtr);
static void		HandlePointer(TkAndroidEvent *eventPtr);
static void		HandleScroll(TkAndroidEvent *eventPtr);
static void		HandleText(TkAndroidEvent *eventPtr);
static void		ProcessQueue(void);
static void		QueueFocusEvent(int type, Window w);
static void		QueueKeyEvent(int type, KeySym keysym,
			    unsigned int keycode, const char *chars,
			    int length);
static void		KeyboardProc(Tk_Window tkwin, int show, int type);
static Tk_Window	WindowAtPoint(int x, int y);

/*
 *----------------------------------------------------------------------
 *
 * Post --
 *
 *	Queue an event of the activity, and wake Tk up. Called with the
 *	mutex of the queue locked.
 *
 * Results:
 *	The serial number of the event.
 *
 * Side effects:
 *	The pipe of the queue is created the first time, and written to.
 *
 *----------------------------------------------------------------------
 */

static unsigned long
Post(
    const TkAndroidEvent *eventPtr)	/* The event. Called with the mutex
					 * locked. */
{
    QueuedEvent *qPtr = (QueuedEvent *)malloc(sizeof(QueuedEvent));
    char byte = 0;

    if (queuePipe[0] < 0) {
	if (pipe2(queuePipe, O_CLOEXEC | O_NONBLOCK) < 0) {
	    queuePipe[0] = queuePipe[1] = -1;
	}
    }
    qPtr->event = *eventPtr;
    qPtr->serial = ++postedSerial;
    qPtr->nextPtr = NULL;
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
    return qPtr->serial;
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidPostEvent, TkAndroidPostEventAndWait --
 *
 *	Post an event of the activity to Tk, from any thread.
 *	TkAndroidPostEventAndWait waits until Tk has handled it, or until
 *	the timeout (in milliseconds) has passed.
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
TkAndroidPostEvent(
    const TkAndroidEvent *eventPtr)	/* The event. */
{
    pthread_mutex_lock(&queueMutex);
    Post(eventPtr);
    pthread_mutex_unlock(&queueMutex);
}

void
TkAndroidPostEventAndWait(
    const TkAndroidEvent *eventPtr,	/* The event. */
    int timeout)			/* In milliseconds. */
{
    struct timespec deadline;
    unsigned long serial;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout / 1000;
    deadline.tv_nsec += (long)(timeout % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
	deadline.tv_sec++;
	deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&queueMutex);
    serial = Post(eventPtr);
    while (handledSerial < serial) {
	if (pthread_cond_timedwait(&queueCond, &queueMutex, &deadline)) {
	    break;
	}
    }
    pthread_mutex_unlock(&queueMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * ProcessQueue, IsMove --
 *
 *	Handle the events posted by the glue. Consecutive moves of a finger
 *	are compressed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Whatever the events do. Threads waiting for them are woken up.
 *
 *----------------------------------------------------------------------
 */

static bool
IsMove(
    const TkAndroidEvent *eventPtr)	/* An event. */
{
    return eventPtr->type == TKANDROID_POINTER
	    && !eventPtr->u.pointer.isMouse
	    && eventPtr->u.pointer.action == TKANDROID_MOVE;
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
		&& queueHead->event.u.pointer.id == qPtr->event.u.pointer.id;
	pthread_mutex_unlock(&queueMutex);
	if (qPtr == NULL) {
	    break;
	}

	/*
	 * A move of a finger followed by another is not handled, as Tk
	 * compresses the motion of the mouse: handling every move of a fast
	 * finger would keep Tk from redisplaying until it is lifted.
	 */

	if (!superseded) {
	    HandleEvent(&qPtr->event);
	}
	pthread_mutex_lock(&queueMutex);
	handledSerial = qPtr->serial;
	pthread_cond_broadcast(&queueCond);
	pthread_mutex_unlock(&queueMutex);
	free(qPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * HandleEvent --
 *
 *	Handle an event of the activity.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	X events are queued, the window changes, ...
 *
 *----------------------------------------------------------------------
 */

static void
HandleEvent(
    TkAndroidEvent *eventPtr)	/* The event. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;

    if (eventPtr->time) {
	dispPtr->lastTime = (Time)eventPtr->time;
    }
    switch (eventPtr->type) {
    case TKANDROID_WINDOW_CREATED:
	TkAndroidSetWindow(eventPtr->u.window);
	break;
    case TKANDROID_WINDOW_DESTROYED:
	if (dispPtr->window == eventPtr->u.window) {
	    TkAndroidSetWindow(NULL);
	}
	ANativeWindow_release(eventPtr->u.window);
	break;
    case TKANDROID_WINDOW_RESIZED:
    case TKANDROID_REDRAW:
	if (dispPtr->window) {
	    TkAndroidSetWindow(dispPtr->window);
	}
	break;
    case TKANDROID_CONTENT_RECT:
	dispPtr->content[0] = eventPtr->u.rect.left;
	dispPtr->content[1] = eventPtr->u.rect.top;
	dispPtr->content[2] = eventPtr->u.rect.right;
	dispPtr->content[3] = eventPtr->u.rect.bottom;
	if (dispPtr->display) {
	    TkAndroidWmRelayout();
	}
	break;
    case TKANDROID_FOCUS:
	dispPtr->hasFocus = (eventPtr->u.hasFocus != 0);
	if (dispPtr->hasFocus && dispPtr->display) {
	    TkAndroidClipboardCheck();
	}
	if (TkXeGetWindow(focusWindow)) {
	    QueueFocusEvent(dispPtr->hasFocus ? FocusIn : FocusOut,
		    focusWindow);
	}
	break;
    case TKANDROID_CONFIG:
	dispPtr->density = eventPtr->u.density;
	TkAndroidUpdateScale();
	break;
    case TKANDROID_PAUSE:
	dispPtr->visible = false;
	break;
    case TKANDROID_RESUME:
	dispPtr->visible = true;
	if (dispPtr->display) {
	    Screen *screen = ScreenOfDisplay(dispPtr->display, 0);

	    TkAndroidDamageScreen(0, 0, screen->width, screen->height);
	}
	break;
    case TKANDROID_POINTER:
	HandlePointer(eventPtr);
	break;
    case TKANDROID_SCROLL:
	HandleScroll(eventPtr);
	break;
    case TKANDROID_KEY:
	HandleKey(eventPtr);
	break;
    case TKANDROID_TEXT:
	HandleText(eventPtr);
	break;
    case TKANDROID_DELETE: {
	int i;

	for (i = 0; i < eventPtr->u.del.before; i++) {
	    TkAndroidQueueKey(XK_BackSpace);
	}
	for (i = 0; i < eventPtr->u.del.after; i++) {
	    TkAndroidQueueKey(XK_Delete);
	}
	break;
    }
    case TKANDROID_BACK:
	TkAndroidWmBack();
	break;
    case TKANDROID_KEYBOARD:
	if (dispPtr->display) {
	    TkKeyboardChanged(NULL, eventPtr->u.keyboardShown);
	}
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
 *	grabs, as on Aqua, Windows and Wayland. Touches are given to the
 *	gesture recognizer of tkTouch.c, which turns them into clicks,
 *	scrolling and pinches.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * WindowAtPoint --
 *
 *	Find the Tk window at a point of the screen.
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
    TkXeSurface *surfPtr = TkAndroidSurfaceAt(x, y);
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
UpdatePointer(
    unsigned int buttons)	/* Button1Mask etc. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    int x = (int)floor(dispPtr->pointerX);
    int y = (int)floor(dispPtr->pointerY);

    dispPtr->buttonState = buttons;
    Tk_UpdatePointer(dispPtr->pointerInside ? WindowAtPoint(x, y) : NULL,
	    x, y, dispPtr->modState | buttons);
}

/*
 *----------------------------------------------------------------------
 *
 * HandlePointer --
 *
 *	Handle a touch or mouse event of the activity.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Pointer events are generated; the soft keyboard may be shown or
 *	hidden when a finger is lifted.
 *
 *----------------------------------------------------------------------
 */

static void
HandlePointer(
    TkAndroidEvent *eventPtr)	/* A TKANDROID_POINTER event. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    unsigned int buttons = 0;
    int type;

    if (dispPtr->display == NULL) {
	return;
    }
    dispPtr->pointerX = eventPtr->u.pointer.x / dispPtr->scale;
    dispPtr->pointerY = eventPtr->u.pointer.y / dispPtr->scale;
    if (eventPtr->u.pointer.isMouse) {
	int b = eventPtr->u.pointer.buttons;

	if (b & AMOTION_EVENT_BUTTON_PRIMARY) {
	    buttons |= Button1Mask;
	}
	if (b & AMOTION_EVENT_BUTTON_TERTIARY) {
	    buttons |= Button2Mask;
	}
	if (b & AMOTION_EVENT_BUTTON_SECONDARY) {
	    buttons |= Button3Mask;
	}
	dispPtr->pointerInside =
		(eventPtr->u.pointer.action != TKANDROID_LEAVE);
	UpdatePointer(buttons);
	return;
    }

    dispPtr->pointerInside = true;
    switch (eventPtr->u.pointer.action) {
    case TKANDROID_DOWN:
	type = TK_TOUCH_BEGIN;
	break;
    case TKANDROID_MOVE:
	type = TK_TOUCH_MOVE;
	break;
    case TKANDROID_UP:
	type = TK_TOUCH_END;
	break;
    default:
	type = TK_TOUCH_CANCEL;
	break;
    }
    TkTouchEvent(type, WindowAtPoint((int)floor(dispPtr->pointerX),
	    (int)floor(dispPtr->pointerY)), eventPtr->u.pointer.id,
	    dispPtr->pointerX, dispPtr->pointerY,
	    (unsigned long)eventPtr->time, dispPtr->modState);

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
 * HandleScroll --
 *
 *	Handle a mouse wheel event of the activity.
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
HandleScroll(
    TkAndroidEvent *eventPtr)	/* A TKANDROID_SCROLL event. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    unsigned int state;

    if (dispPtr->display == NULL) {
	return;
    }
    dispPtr->pointerX = eventPtr->u.scroll.x / dispPtr->scale;
    dispPtr->pointerY = eventPtr->u.scroll.y / dispPtr->scale;
    state = dispPtr->modState | dispPtr->buttonState;
    QueueWheelEvent(state, (int)lround(eventPtr->u.scroll.dy * 120));
    QueueWheelEvent(state | ShiftMask,
	    (int)lround(-eventPtr->u.scroll.dx * 120));
}

/*
 *----------------------------------------------------------------------
 *
 * Keyboard events --
 *
 *	The keycode of an X event is the Android keycode plus 8. The keysym
 *	and the character of a key are computed when the event is generated,
 *	with the character the glue got from the key character map of the
 *	device, and stored in the TkKeyEvent.
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
 * QueueKeyEvent --
 *
 *	Queue a key event for the window which has the focus, with its
 *	keysym and, for a KeyPress, its characters, as an input method
 *	gives them.
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
    const char *chars,		/* The characters of a KeyPress, in UTF-8. */
    int length)			/* Their length in bytes. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    TkXeSurface *surfPtr = keyboardSurface;
    union {
	XEvent x;
	TkKeyEvent k;
    } event;

    if (surfPtr == NULL || dispPtr->display == NULL) {
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
 *	Handle a key event of the activity.
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
    TkAndroidEvent *eventPtr)	/* A TKANDROID_KEY event. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    int keyCode = eventPtr->u.key.keyCode, unicode = eventPtr->u.key.unicode;
    KeySym keysym;
    char buf[8];
    int len = 0;

    dispPtr->modState = TkAndroidModState(eventPtr->u.key.metaState);
    keysym = TkAndroidKeySym(keyCode, eventPtr->u.key.metaState, unicode);
    if (keysym == NoSymbol) {
	return;
    }
    if (eventPtr->u.key.down && unicode >= 0x20 && unicode != 0x7F
	    && !(dispPtr->modState & ControlMask)) {
	len = Tcl_UniCharToUtf(unicode, buf);
    } else if (eventPtr->u.key.down && (dispPtr->modState & ControlMask)
	    && keysym >= 'a' && keysym <= 'z') {
	buf[0] = (char)(keysym & 0x1F);
	len = 1;
    }
    QueueKeyEvent(eventPtr->u.key.down ? KeyPress : KeyRelease, keysym,
	    (unsigned)keyCode + 8, buf, len);
}

/*
 *----------------------------------------------------------------------
 *
 * HandleText --
 *
 *	Text committed by the soft keyboard is typed: each character is the
 *	press and release of a key with its keysym (the Unicode keysym of
 *	the character, as X gives with xkb).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Key events are queued; the text is freed.
 *
 *----------------------------------------------------------------------
 */

static void
HandleText(
    TkAndroidEvent *eventPtr)	/* A TKANDROID_TEXT event. */
{
    Tcl_DString ds;
    const char *p, *end;

    Tcl_DStringInit(&ds);
    Tcl_Char16ToUtfDString(eventPtr->u.text.chars, eventPtr->u.text.length,
	    &ds);
    free(eventPtr->u.text.chars);
    p = Tcl_DStringValue(&ds);
    end = p + Tcl_DStringLength(&ds);
    while (p < end) {
	int c, len = Tcl_UtfToUniChar(p, &c);
	KeySym keysym;

	if (c == '\n') {
	    TkAndroidQueueKey(XK_Return);
	} else if (c >= 0x20) {
	    keysym = (c <= 0xFF) ? (KeySym)c : (KeySym)c + 0x1000000;
	    QueueKeyEvent(KeyPress, keysym, TkAndroidKeyCode(keysym), p, len);
	    QueueKeyEvent(KeyRelease, keysym, TkAndroidKeyCode(keysym), NULL,
		    0);
	}
	p += len;
    }
    Tcl_DStringFree(&ds);
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidQueueKey --
 *
 *	Queue the press and release of a key which is not on the keyboard
 *	(Escape, for the back button).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Key events are queued.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidQueueKey(
    KeySym keysym)		/* The keysym of the key. */
{
    char c = (keysym == XK_Escape) ? 0x1B : (keysym == XK_BackSpace) ? 0x08
	    : (keysym == XK_Delete) ? 0x7F : (keysym == XK_Return) ? 0x0D : 0;
    unsigned int keycode = TkAndroidKeyCode(keysym);

    QueueKeyEvent(KeyPress, keysym, keycode, &c, c ? 1 : 0);
    QueueKeyEvent(KeyRelease, keysym, keycode, NULL, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidSetKeyboardSurface, TkAndroidKeyboardSurface --
 *
 *	Give the keyboard to a toplevel, as a window manager does, or get
 *	the toplevel which has it.
 *
 * Results:
 *	TkAndroidKeyboardSurface returns the surface, or NULL.
 *
 * Side effects:
 *	FocusOut and FocusIn events are queued.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidSetKeyboardSurface(
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
TkAndroidKeyboardSurface(void)
{
    return keyboardSurface;
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidGetEnv --
 *
 *	The JNI environment of the thread of Tk, which is attached to the
 *	Java VM the first time.
 *
 * Results:
 *	The environment, or NULL.
 *
 * Side effects:
 *	The thread may be attached to the VM.
 *
 *----------------------------------------------------------------------
 */

JNIEnv *
TkAndroidGetEnv(void)
{
    JavaVM *vm;
    JNIEnv *env = NULL;

    if (tkAndroidActivity == NULL) {
	return NULL;
    }
    vm = tkAndroidActivity->vm;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED
	    && (*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
	return NULL;
    }
    return env;
}

/*
 *----------------------------------------------------------------------
 *
 * KeyboardProc --
 *
 *	Called by tkKeyboard.c to show or hide the soft keyboard, which the
 *	activity of Tk (TkActivity.java) does for a view which edits text,
 *	with the type of the text (NativeActivity cannot, since Android 11).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The keyboard is shown or hidden; the work area changes.
 *
 *----------------------------------------------------------------------
 */

static void
KeyboardProc(
    TCL_UNUSED(Tk_Window),
    int show,			/* Whether to show the keyboard. */
    int type)			/* TK_KEYBOARD_*. */
{
    JNIEnv *env;
    jclass cls;
    jmethodID method;

    if (tkAndroidActivity == NULL) {
	return;
    }
    env = TkAndroidGetEnv();
    if (env) {
	cls = (*env)->GetObjectClass(env, tkAndroidActivity->clazz);
	method = (*env)->GetMethodID(env, cls, "showKeyboard", "(ZI)V");
	(*env)->DeleteLocalRef(env, cls);
	if (method) {
	    (*env)->CallVoidMethod(env, tkAndroidActivity->clazz, method,
		    (jboolean)(show != 0), (jint)type);
	    if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
	    }
	    return;
	}
	(*env)->ExceptionClear(env);
    }
    if (show) {
	ANativeActivity_showSoftInput(tkAndroidActivity,
		ANATIVEACTIVITY_SHOW_SOFT_INPUT_IMPLICIT);
    } else {
	ANativeActivity_hideSoftInput(tkAndroidActivity,
		ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkpOpenDisplay --
 *
 *	Open the display, which is the window of the activity. There is
 *	only one per process.
 *
 * Results:
 *	The display, or NULL.
 *
 * Side effects:
 *	The events posted so far are handled, so that the size of the
 *	window is known.
 *
 *----------------------------------------------------------------------
 */

TkDisplay *
TkpOpenDisplay(
    const char *displayNameStr)	/* The name of the display. */
{
    TkDisplay *dispPtr;
    const char *scale = getenv("TK_ANDROID_SCALE");

    if (tkXeDisplay) {
	/*
	 * Only one display per process; any name refers to it.
	 */

	for (dispPtr = TkGetDisplayList(); dispPtr; dispPtr = dispPtr->nextPtr) {
	    if (dispPtr->display == tkXeDisplay->display) {
		return NULL;
	    }
	}
    } else {
	tkXeDisplay = (TkXeDisplay *)Tcl_Alloc(sizeof(TkXeDisplay));
	memset(tkXeDisplay, 0, sizeof(TkXeDisplay));
	pixman_region32_init(&tkXeDisplay->damage);
	tkXeDisplay->visible = true;
	tkXeDisplay->hasFocus = true;
	tkXeDisplay->forcedScale = scale ? atof(scale) : 0.0;
	if (tkAndroidActivity) {
	    AConfiguration *config = AConfiguration_new();

	    AConfiguration_fromAssetManager(config,
		    tkAndroidActivity->assetManager);
	    tkXeDisplay->density = AConfiguration_getDensity(config);
	    AConfiguration_delete(config);
	}
	TkAndroidUpdateScale();
	pthread_mutex_lock(&queueMutex);
	if (queuePipe[0] < 0 && pipe2(queuePipe, O_CLOEXEC | O_NONBLOCK) < 0) {
	    queuePipe[0] = queuePipe[1] = -1;
	}
	pthread_mutex_unlock(&queueMutex);
	tkXeDisplay->eventFd = queuePipe[0];
    }

    tkXeDisplay->display = TkXeOpenXDisplay(
	    displayNameStr ? displayNameStr : "android", tkXeDisplay->eventFd,
	    360, 640, 0, 0);
    TkXeSetScreenSize(tkXeDisplay->display, 360, 640, 0, 0);
    ProcessQueue();
    TkAndroidUpdateScale();

    dispPtr = (TkDisplay *)Tcl_Alloc(sizeof(TkDisplay));
    memset(dispPtr, 0, sizeof(TkDisplay));
    dispPtr->display = tkXeDisplay->display;
    if (tkXeDisplay->eventFd >= 0) {
	Tcl_CreateFileHandler(tkXeDisplay->eventFd, TCL_READABLE,
		DisplayFileProc, NULL);
    }
    TkpInitKeymapInfo(dispPtr);
    TkSetKeyboardProc(KeyboardProc);
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
 *	The pipe of the events of the activity is no longer watched.
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
 * TkClipCleanup --
 *
 *	Nothing to clean up: the clipboard of Android keeps its text.
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
 *	The events of the activity are handled when the pipe is readable.
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
 *	The events of the activity come through a pipe, watched by a file
 *	handler: the event source has nothing to do.
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
 *	Called when the pipe of the events of the activity is readable.
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
 * Keysyms --
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * TkpInitKeymapInfo --
 *
 *	Set the modifier information of a display: the keys of Android which
 *	are modifiers, and the masks of Meta and Alt.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The fields of the display are set.
 *
 *----------------------------------------------------------------------
 */

void
TkpInitKeymapInfo(
    TkDisplay *dispPtr)		/* The display. */
{
    static const KeySym modKeysyms[] = {
	XK_Shift_L, XK_Shift_R, XK_Control_L, XK_Control_R, XK_Caps_Lock,
	XK_Meta_L, XK_Meta_R, XK_Alt_L, XK_Alt_R, XK_Num_Lock
    };
    size_t i;

    dispPtr->bindInfoStale = 0;
    dispPtr->lockUsage = LU_CAPS;
    dispPtr->modeModMask = 0;
    dispPtr->metaModMask = Mod1Mask;
    dispPtr->altModMask = Mod1Mask;
    if (dispPtr->modKeyCodes != NULL) {
	Tcl_Free(dispPtr->modKeyCodes);
    }
    dispPtr->modKeyCodes = (KeyCode *)Tcl_Alloc(
	    sizeof(modKeysyms) / sizeof(modKeysyms[0]) * sizeof(KeyCode));
    dispPtr->numModKeyCodes = 0;
    for (i = 0; i < sizeof(modKeysyms) / sizeof(modKeysyms[0]); i++) {
	KeyCode code = TkAndroidKeyCode(modKeysyms[i]);

	if (code != 0) {
	    dispPtr->modKeyCodes[dispPtr->numModKeyCodes++] = code;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkpGetKeySym --
 *
 *	The keysym of a key event: the one stored when it was generated, or
 *	that of its keycode for events which were not.
 *
 * Results:
 *	The keysym, or NoSymbol.
 *
 * Side effects:
 *	May update the keymap information.
 *
 *----------------------------------------------------------------------
 */

KeySym
TkpGetKeySym(
    TkDisplay *dispPtr,		/* The display. */
    XEvent *eventPtr)		/* A key event. */
{
    TkKeyEvent *kePtr = (TkKeyEvent *)eventPtr;

    if (dispPtr->bindInfoStale) {
	TkpInitKeymapInfo(dispPtr);
    }
    if (kePtr->keysym != NoSymbol) {
	return kePtr->keysym;
    }
    if (eventPtr->xkey.keycode < 8) {
	return NoSymbol;
    }
    return TkAndroidKeySym((int)eventPtr->xkey.keycode - 8,
	    (eventPtr->xkey.state & ShiftMask) ? AMETA_SHIFT_ON : 0, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * TkpGetString --
 *
 *	The characters of a key event: those stored when it was generated,
 *	or those of its keysym for events which were not.
 *
 * Results:
 *	The string, in the DString.
 *
 * Side effects:
 *	The string is stored in the event.
 *
 *----------------------------------------------------------------------
 */

const char *
TkpGetString(
    TCL_UNUSED(TkWindow *),
    XEvent *eventPtr,		/* A key event. */
    Tcl_DString *dsPtr)		/* Initialized; returns the string. */
{
    TkKeyEvent *kePtr = (TkKeyEvent *)eventPtr;

    if (kePtr->charValuePtr == NULL) {
	char buf[8];
	int len = 0;

	if (eventPtr->type == KeyPress) {
	    KeySym keysym = kePtr->keysym;

	    if (keysym >= 0x20 && keysym <= 0xFF && keysym != 0x7F) {
		len = Tcl_UniCharToUtf((int)keysym, buf);
	    } else if (keysym >= 0x1000100 && keysym <= 0x110FFFF) {
		len = Tcl_UniCharToUtf((int)(keysym - 0x1000000), buf);
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
 *	Set the keycode of an event generated with [event generate].
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The event is changed.
 *
 *----------------------------------------------------------------------
 */

void
TkpSetKeycodeAndState(
    TCL_UNUSED(Tk_Window),
    KeySym keySym,		/* The keysym of the event. */
    XEvent *eventPtr)		/* The event to fill. */
{
    eventPtr->xkey.keycode = (keySym == NoSymbol) ? 0
	    : TkAndroidKeyCode(keySym);
    ((TkKeyEvent *)eventPtr)->keysym = keySym;
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
 *	has the keyboard.
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
    if (topPtr == NULL || topPtr->surface == NULL) {
	return Success;
    }
    if (topPtr->surface != keyboardSurface) {
	TkXeWmActivate(topPtr->surface);
	if (topPtr->surface != keyboardSurface) {
	    return Success;
	}
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
 *	There is nothing else to take the keyboard: the grabs of Tk work on
 *	the windows of the application.
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
 *
 * Results:
 *	TkpGetCapture returns the window, or NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
TkpSetCapture(
    TkWindow *winPtr)		/* The window, or NULL. */
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
 *	Where the pointer is (where the screen was last touched), and the
 *	state of the buttons and modifiers.
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
    int rootX = (int)floor(tkXeDisplay->pointerX);
    int rootY = (int)floor(tkXeDisplay->pointerY);
    int x, y;

    TkXeNextRequest(display);
    *rootPtr = RootWindow(display, 0);
    *childPtr = None;
    *rootXPtr = rootX;
    *rootYPtr = rootY;
    XTranslateCoordinates(display, *rootPtr, w, rootX, rootY, &x, &y,
	    childPtr);
    *winXPtr = x;
    *winYPtr = y;
    *maskPtr = tkXeDisplay->modState | tkXeDisplay->buttonState;
    return True;
}

/*
 *----------------------------------------------------------------------
 *
 * XWarpPointer, TkpWarpPointer --
 *
 *	A finger cannot be moved.
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
