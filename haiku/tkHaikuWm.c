/*
 * tkHaikuWm.c --
 *
 *	The window manager of the emulated X display on Haiku. As in the
 *	Wayland and Android ports, the "wm" command is implemented by the X11
 *	code (tkUnixWm.c), which gives hints to the window manager with ICCCM
 *	and EWMH properties and client messages, and this file plays the part
 *	of the window manager, with the app_server: toplevels get a decorated
 *	BWindow, override-redirect windows a borderless one above the others.
 *
 *	Haiku lets applications place their windows, so the X windows are
 *	where the BWindows are, and root coordinates are those of the screen.
 *	The decorations are drawn around the BWindow without reparenting it,
 *	and reported in _NET_FRAME_EXTENTS, so that [wm geometry] gives the
 *	position of the decorations, as with X window managers. When the user
 *	moves or resizes a BWindow, its X window is moved or resized the same,
 *	which tells Tk with a ConfigureNotify event.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkHaikuInt.h"

/*
 * The atoms of the properties and messages the window manager handles.
 */

static struct {
    Atom WM_PROTOCOLS, WM_DELETE_WINDOW, WM_CHANGE_STATE, _NET_WM_NAME,
	UTF8_STRING, _NET_WM_STATE, _NET_WM_STATE_MAXIMIZED_VERT,
	_NET_WM_STATE_MAXIMIZED_HORZ, _NET_WM_STATE_FULLSCREEN,
	_NET_WM_STATE_ABOVE, _NET_ACTIVE_WINDOW, _NET_FRAME_EXTENTS,
	_NET_WORKAREA, _NET_CURRENT_DESKTOP;
} atoms;

/*
 * Where the next toplevel without a position is placed: toplevels cascade
 * from the corner of the work area.
 */

static int cascade = 0;

/*
 * While the window manager restacks the X windows as the app_server
 * stacked the BWindows, the BWindows are not restacked.
 */

static bool followingStack = false;

/*
 * Whether no BWindow of the application may be active.
 */

static bool deactivatedPending = false;

static void		ApplyNetWmState(TkXeSurface *surfPtr);
static void		ApplySizeHints(TkXeSurface *surfPtr);
static void		ApplyTitle(TkXeSurface *surfPtr);
static void		ChangeState(TkXeSurface *surfPtr, Atom state,
			    long action);
static void		DeleteWindow(TkXeSurface *surfPtr);
static void		DeactivatedTimer(void *clientData);
static void		FollowActivation(void);
static void		IconifyIdle(void *clientData);
static void		FollowFrame(TkXeSurface *surfPtr);
static long *		GetLongs(TkXeWindow *winPtr, Atom property,
			    unsigned long minItems);
static bool		HasState(TkXeWindow *winPtr, Atom state);
static void		InitAtoms(Display *display);
static void		Place(TkXeSurface *surfPtr);
static void		SetFrameExtents(TkXeSurface *surfPtr);
static void		SetNetWmState(TkXeSurface *surfPtr);
static TkXeSurface *	TopToplevel(TkXeSurface *exceptPtr);
static void		WmMoveResize(TkXeSurface *surfPtr, int x, int y,
			    int width, int height);

/*
 *----------------------------------------------------------------------
 *
 * InitAtoms --
 *
 *	Intern the atoms of the window manager, once.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Fills the atoms structure.
 *
 *----------------------------------------------------------------------
 */

static void
InitAtoms(
    Display *display)		/* The display. */
{
    if (atoms.WM_PROTOCOLS != None) {
	return;
    }
#define ATOM(name) atoms.name = XInternAtom(display, #name, False)
    ATOM(WM_PROTOCOLS);
    ATOM(WM_DELETE_WINDOW);
    ATOM(WM_CHANGE_STATE);
    ATOM(_NET_WM_NAME);
    ATOM(UTF8_STRING);
    ATOM(_NET_WM_STATE);
    ATOM(_NET_WM_STATE_MAXIMIZED_VERT);
    ATOM(_NET_WM_STATE_MAXIMIZED_HORZ);
    ATOM(_NET_WM_STATE_FULLSCREEN);
    ATOM(_NET_WM_STATE_ABOVE);
    ATOM(_NET_ACTIVE_WINDOW);
    ATOM(_NET_FRAME_EXTENTS);
    ATOM(_NET_WORKAREA);
    ATOM(_NET_CURRENT_DESKTOP);
#undef ATOM
}

/*
 *----------------------------------------------------------------------
 *
 * GetLongs, HasState --
 *
 *	Read the hints in the properties of a window.
 *
 * Results:
 *	GetLongs returns the data of a property of format 32 with at least
 *	minItems items, or NULL. HasState returns whether _NET_WM_STATE
 *	contains a state.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static long *
GetLongs(
    TkXeWindow *winPtr,		/* The window. */
    Atom property,		/* The property. */
    unsigned long minItems)	/* The number of items needed. */
{
    TkXeProperty *propPtr = TkXeGetProperty(winPtr, property);

    if (propPtr == NULL || propPtr->format != 32
	    || propPtr->nitems < minItems) {
	return NULL;
    }
    return (long *)(void *)propPtr->data;
}

static bool
HasState(
    TkXeWindow *winPtr,		/* The window. */
    Atom state)			/* The state. */
{
    TkXeProperty *propPtr = TkXeGetProperty(winPtr, atoms._NET_WM_STATE);
    unsigned long i;

    if (propPtr == NULL || propPtr->format != 32) {
	return false;
    }
    for (i = 0; i < propPtr->nitems; i++) {
	if ((Atom)((long *)(void *)propPtr->data)[i] == state) {
	    return true;
	}
    }
    return false;
}

/*
 *----------------------------------------------------------------------
 *
 * ApplyTitle --
 *
 *	Set the title of the BWindow of a toplevel from its _NET_WM_NAME
 *	property, or else its WM_NAME property.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The title changes.
 *
 *----------------------------------------------------------------------
 */

static void
ApplyTitle(
    TkXeSurface *surfPtr)	/* The surface of the toplevel. */
{
    TkXeProperty *propPtr = TkXeGetProperty(surfPtr->winPtr,
	    atoms._NET_WM_NAME);

    if (propPtr == NULL || propPtr->format != 8) {
	propPtr = TkXeGetProperty(surfPtr->winPtr, XA_WM_NAME);
    }
    if (propPtr && propPtr->format == 8) {
	char *title = (char *)Tcl_Alloc(propPtr->nitems + 1);

	memcpy(title, propPtr->data, propPtr->nitems);
	title[propPtr->nitems] = '\0';
	TkHaikuWindowSetTitle(surfPtr->window, title);
	Tcl_Free(title);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ApplySizeHints --
 *
 *	Set the size limits of the BWindow of a toplevel from its
 *	WM_NORMAL_HINTS property. A toplevel of fixed size cannot be resized
 *	by the user.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The size limits and the flags of the BWindow change.
 *
 *----------------------------------------------------------------------
 */

static void
ApplySizeHints(
    TkXeSurface *surfPtr)	/* The surface of the toplevel. */
{
    long *hints = GetLongs(surfPtr->winPtr, XA_WM_NORMAL_HINTS, SH_NUM);
    int minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;

    if (hints && (hints[SH_FLAGS] & PMinSize)) {
	minWidth = TkHaikuToScreen((double)hints[SH_MIN_WIDTH]);
	minHeight = TkHaikuToScreen((double)hints[SH_MIN_HEIGHT]);
    }
    if (hints && (hints[SH_FLAGS] & PMaxSize)) {
	maxWidth = TkHaikuToScreen((double)hints[SH_MAX_WIDTH]);
	maxHeight = TkHaikuToScreen((double)hints[SH_MAX_HEIGHT]);
	if (maxWidth < minWidth) {
	    maxWidth = minWidth;
	}
	if (maxHeight < minHeight) {
	    maxHeight = minHeight;
	}
    }
    TkHaikuWindowSetSizeLimits(surfPtr->window, minWidth, minHeight,
	    maxWidth, maxHeight);
}

/*
 *----------------------------------------------------------------------
 *
 * SetFrameExtents --
 *
 *	Tell Tk the size of the decorations of a toplevel, in its
 *	_NET_FRAME_EXTENTS property, as X window managers which do not
 *	reparent do.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The property changes, if the decorations did.
 *
 *----------------------------------------------------------------------
 */

static void
SetFrameExtents(
    TkXeSurface *surfPtr)	/* The surface of the toplevel. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    long extents[4];
    int left = 0, top = 0, right = 0, bottom = 0;
    long *old = GetLongs(winPtr, atoms._NET_FRAME_EXTENTS, 4);

    if (!surfPtr->fullscreen) {
	TkHaikuWindowFrameExtents(surfPtr->window, &left, &top, &right,
		&bottom);
    }
    extents[0] = (long)ceil(TkHaikuFromScreen(left));
    extents[1] = (long)ceil(TkHaikuFromScreen(right));
    extents[2] = (long)ceil(TkHaikuFromScreen(top));
    extents[3] = (long)ceil(TkHaikuFromScreen(bottom));
    if (old && !memcmp(old, extents, sizeof(extents))) {
	return;
    }
    XChangeProperty(winPtr->display, winPtr->id, atoms._NET_FRAME_EXTENTS,
	    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)extents, 4);
}

/*
 *----------------------------------------------------------------------
 *
 * SetNetWmState --
 *
 *	Set _NET_WM_STATE to the state of a toplevel, as a window manager
 *	does, which tells tkUnixWm.c with a PropertyNotify event.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The property is changed, if it differs.
 *
 *----------------------------------------------------------------------
 */

static void
SetNetWmState(
    TkXeSurface *surfPtr)	/* The surface of the toplevel. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    TkXeProperty *propPtr = TkXeGetProperty(winPtr, atoms._NET_WM_STATE);
    long states[4];
    int n = 0;

    if (surfPtr->maximized) {
	states[n++] = (long)atoms._NET_WM_STATE_MAXIMIZED_VERT;
	states[n++] = (long)atoms._NET_WM_STATE_MAXIMIZED_HORZ;
    }
    if (surfPtr->fullscreen) {
	states[n++] = (long)atoms._NET_WM_STATE_FULLSCREEN;
    }
    if (surfPtr->level == TKHAIKU_ABOVE) {
	states[n++] = (long)atoms._NET_WM_STATE_ABOVE;
    }
    if (propPtr && propPtr->format == 32 && propPtr->nitems == (unsigned)n
	    && !memcmp(propPtr->data, states, n * sizeof(long))) {
	return;
    }
    XChangeProperty(winPtr->display, winPtr->id, atoms._NET_WM_STATE,
	    XA_ATOM, 32, PropModeReplace, (unsigned char *)states, n);
}

/*
 *----------------------------------------------------------------------
 *
 * Place --
 *
 *	Give a toplevel the geometry of its state: the work area if it is
 *	maximized, the screen if it is fullscreen, otherwise the geometry it
 *	had before.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The X window is moved and resized, and the BWindow with it, which
 *	tells Tk with a ConfigureNotify event.
 *
 *----------------------------------------------------------------------
 */

static void
Place(
    TkXeSurface *surfPtr)	/* The surface of the toplevel. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    int x, y, width, height;

    if (surfPtr->fullscreen) {
	Screen *screen = ScreenOfDisplay(winPtr->display, 0);

	x = y = 0;
	width = screen->width;
	height = screen->height;
    } else if (surfPtr->maximized) {
	long *frame = GetLongs(winPtr, atoms._NET_FRAME_EXTENTS, 4);
	int ax, ay, aw, ah;
	int wx, wy, ww, wh;

	TkHaikuWorkArea(&wx, &wy, &ww, &wh);
	ax = (int)ceil(TkHaikuFromScreen(wx));
	ay = (int)ceil(TkHaikuFromScreen(wy));
	aw = (int)floor(TkHaikuFromScreen(ww));
	ah = (int)floor(TkHaikuFromScreen(wh));
	x = ax + (frame ? (int)frame[0] : 0);
	y = ay + (frame ? (int)frame[2] : 0);
	width = aw - (frame ? (int)(frame[0] + frame[1]) : 0);
	height = ah - (frame ? (int)(frame[2] + frame[3]) : 0);
    } else {
	x = surfPtr->savedX;
	y = surfPtr->savedY;
	width = surfPtr->savedWidth;
	height = surfPtr->savedHeight;
    }
    if (width < 1 || height < 1) {
	return;
    }
    WmMoveResize(surfPtr, x, y, width, height);
}

/*
 *----------------------------------------------------------------------
 *
 * WmMoveResize --
 *
 *	Move and resize the X window of a surface as the window manager:
 *	the position is that of the window, not of its decorations.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The X window and the BWindow are moved and resized, which tells Tk
 *	with a ConfigureNotify event.
 *
 *----------------------------------------------------------------------
 */

static void
WmMoveResize(
    TkXeSurface *surfPtr,	/* The surface. */
    int x, int y,		/* The position of the window. */
    int width, int height)	/* Its size. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;

    if (x != winPtr->x || y != winPtr->y || width != winPtr->width
	    || height != winPtr->height) {
	surfPtr->inConfigure = true;
	XMoveResizeWindow(winPtr->display, winPtr->id, x, y, (unsigned)width,
		(unsigned)height);
	surfPtr->inConfigure = false;
    }
    surfPtr->wmX = x;
    surfPtr->wmY = y;
    TkHaikuSyncFrame(surfPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuWmConfigured --
 *
 *	Called when the X window of a surface was configured. A toplevel
 *	which the client moved is placed as X window managers place it: the
 *	position asked for is that of its decorations (the gravity is
 *	NorthWest), so the window goes inside them.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The X window may be moved again, which tells Tk with a ConfigureNotify
 *	event.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuWmConfigured(
    TkXeSurface *surfPtr)	/* The surface. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    long *frame;

    if (surfPtr->inConfigure) {
	surfPtr->wmX = winPtr->x;
	surfPtr->wmY = winPtr->y;
	return;
    }
    if (surfPtr->popup || surfPtr->window == NULL || !winPtr->mapped
	    || surfPtr->iconic || surfPtr->fullscreen || !surfPtr->placed
	    || (winPtr->x == surfPtr->wmX && winPtr->y == surfPtr->wmY)) {
	return;
    }
    InitAtoms(winPtr->display);
    frame = GetLongs(winPtr, atoms._NET_FRAME_EXTENTS, 4);
    WmMoveResize(surfPtr, winPtr->x + (frame ? (int)frame[0] : 0),
	    winPtr->y + (frame ? (int)frame[2] : 0), winPtr->width,
	    winPtr->height);
}

/*
 *----------------------------------------------------------------------
 *
 * ChangeState --
 *
 *	Add, remove or toggle a state of a toplevel: maximized, fullscreen or
 *	above the others.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The toplevel may change place, decorations and level; _NET_WM_STATE
 *	is updated.
 *
 *----------------------------------------------------------------------
 */

static void
ChangeState(
    TkXeSurface *surfPtr,	/* The surface of the toplevel. */
    Atom state,			/* The state. */
    long action)		/* 0 to remove, 1 to add, 2 to toggle. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    bool *flagPtr;
    bool on;

    if (state == atoms._NET_WM_STATE_ABOVE) {
	on = (action == 2) ? surfPtr->level != TKHAIKU_ABOVE : (action == 1);
	surfPtr->level = on ? TKHAIKU_ABOVE : TKHAIKU_NORMAL;
	if (surfPtr->window && !surfPtr->fullscreen) {
	    TkHaikuWindowSetLevel(surfPtr->window, surfPtr->level);
	}
	SetNetWmState(surfPtr);
	return;
    }
    if (state == atoms._NET_WM_STATE_FULLSCREEN) {
	flagPtr = &surfPtr->fullscreen;
    } else if (state == atoms._NET_WM_STATE_MAXIMIZED_VERT
	    || state == atoms._NET_WM_STATE_MAXIMIZED_HORZ) {
	flagPtr = &surfPtr->maximized;
    } else {
	return;
    }
    on = (action == 2) ? !*flagPtr : (action == 1);
    if (on == *flagPtr) {
	return;
    }
    if (on && !surfPtr->maximized && !surfPtr->fullscreen) {
	surfPtr->savedX = winPtr->x;
	surfPtr->savedY = winPtr->y;
	surfPtr->savedWidth = winPtr->width;
	surfPtr->savedHeight = winPtr->height;
    }
    *flagPtr = on;
    if (surfPtr->window && flagPtr == &surfPtr->fullscreen) {
	TkHaikuWindowSetDecorated(surfPtr->window, !on);
	TkHaikuWindowSetLevel(surfPtr->window,
		on ? TKHAIKU_ABOVE : surfPtr->level);
	SetFrameExtents(surfPtr);
    }
    SetNetWmState(surfPtr);
    if (winPtr->mapped) {
	Place(surfPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ApplyNetWmState --
 *
 *	Apply the states in _NET_WM_STATE, which Tk sets before a toplevel is
 *	mapped.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	See ChangeState.
 *
 *----------------------------------------------------------------------
 */

static void
ApplyNetWmState(
    TkXeSurface *surfPtr)	/* The surface of the toplevel. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;

    ChangeState(surfPtr, atoms._NET_WM_STATE_ABOVE,
	    HasState(winPtr, atoms._NET_WM_STATE_ABOVE));
    ChangeState(surfPtr, atoms._NET_WM_STATE_FULLSCREEN,
	    HasState(winPtr, atoms._NET_WM_STATE_FULLSCREEN));
    ChangeState(surfPtr, atoms._NET_WM_STATE_MAXIMIZED_VERT,
	    HasState(winPtr, atoms._NET_WM_STATE_MAXIMIZED_VERT)
	    || HasState(winPtr, atoms._NET_WM_STATE_MAXIMIZED_HORZ));
}

/*
 *----------------------------------------------------------------------
 *
 * TopToplevel --
 *
 *	Find the top-most mapped toplevel (a child of the root window which
 *	is not override-redirect).
 *
 * Results:
 *	Its surface, or NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static TkXeSurface *
TopToplevel(
    TkXeSurface *exceptPtr)	/* A surface to ignore, or NULL. */
{
    TkXeWindow *rootPtr, *childPtr;

    rootPtr = TkXeGetWindow(RootWindow(tkXeDisplay->display, 0));
    for (childPtr = rootPtr->lastChild; childPtr;
	    childPtr = childPtr->below) {
	if (childPtr->mapped && !childPtr->overrideRedirect
		&& childPtr->surface && childPtr->surface != exceptPtr
		&& !childPtr->surface->iconic) {
	    return childPtr->surface;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWmMapSurface --
 *
 *	Called when a child of the root window is mapped: create its BWindow
 *	if it has none, apply the hints of a toplevel, place it if Tk gave
 *	no position, and show it. A toplevel whose initial state is iconic is
 *	minimized. A minimized toplevel which is mapped is restored.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The BWindow is created and shown; the toplevel gets the keyboard.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWmMapSurface(
    TkXeSurface *surfPtr)	/* The surface of the mapped window. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    long *hints;
    bool popup = winPtr->overrideRedirect;

    InitAtoms(winPtr->display);
    if (surfPtr->iconic) {
	surfPtr->iconic = false;
	if (surfPtr->window) {
	    TkHaikuWmConfigured(surfPtr);
	    TkHaikuWindowMinimize(surfPtr->window, false);
	    TkHaikuShowSurface(surfPtr, true);
	    TkXeWmActivate(surfPtr);
	    return;
	}
    }
    if (surfPtr->window && surfPtr->popup != popup) {
	TkHaikuWindowDestroy(surfPtr->window);
	surfPtr->window = NULL;
	surfPtr->shown = false;
    }
    if (surfPtr->window == NULL) {
	surfPtr->popup = popup;
	surfPtr->window = TkHaikuWindowCreate(winPtr->id,
		popup ? TKHAIKU_POPUP : TKHAIKU_TOPLEVEL,
		TkHaikuToScreen(winPtr->x), TkHaikuToScreen(winPtr->y),
		surfPtr->bufferWidth, surfPtr->bufferHeight);
    }
    if (!popup) {
	ApplyTitle(surfPtr);
	ApplySizeHints(surfPtr);
	SetFrameExtents(surfPtr);
	hints = GetLongs(winPtr, XA_WM_NORMAL_HINTS, SH_NUM);
	if (!surfPtr->placed) {
	    long *frame = GetLongs(winPtr, atoms._NET_FRAME_EXTENTS, 4);
	    int left = frame ? (int)frame[0] : 0;
	    int top = frame ? (int)frame[2] : 0;
	    int x = winPtr->x, y = winPtr->y;

	    if (!(hints && (hints[SH_FLAGS] & (USPosition | PPosition)))) {
		int wx, wy, ww, wh;

		/*
		 * Tk gave no position: cascade the toplevel from the corner
		 * of the work area, as X window managers place windows.
		 */

		TkHaikuWorkArea(&wx, &wy, &ww, &wh);
		x = (int)ceil(TkHaikuFromScreen(wx)) + 24 * cascade;
		y = (int)ceil(TkHaikuFromScreen(wy)) + 24 * cascade;
		cascade = (cascade + 1) % 10;
	    }

	    /*
	     * The position is that of the decorations.
	     */

	    surfPtr->placed = true;
	    WmMoveResize(surfPtr, x + left, y + top, winPtr->width,
		    winPtr->height);
	}
	ApplyNetWmState(surfPtr);
    }
    TkHaikuSyncFrame(surfPtr);
    TkHaikuShowSurface(surfPtr, true);
    if (!popup) {
	hints = GetLongs(winPtr, XA_WM_HINTS, 9);
	if (hints && (hints[0] & StateHint) && hints[2] == IconicState) {
	    /*
	     * Minimized at idle time, after Tk got the MapNotify event, as
	     * X window managers do.
	     */

	    Tcl_DoWhenIdle(IconifyIdle, (void *)(intptr_t)winPtr->id);
	    return;
	}
	TkHaikuSetKeyboardSurface(surfPtr);
    }
    TkHaikuUpdateCapture();
}

/*
 *----------------------------------------------------------------------
 *
 * IconifyIdle --
 *
 *	Minimize a toplevel mapped in the iconic state, if it is still
 *	mapped: the BWindow goes to the Deskbar and the X window is unmapped,
 *	as X window managers do.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The window is unmapped.
 *
 *----------------------------------------------------------------------
 */

static void
IconifyIdle(
    void *clientData)		/* The id of the window. */
{
    TkXeWindow *winPtr = TkXeGetWindow((Window)(intptr_t)clientData);
    TkXeSurface *surfPtr;

    if (winPtr == NULL || !winPtr->mapped || (surfPtr = winPtr->surface)
	    == NULL || surfPtr->window == NULL) {
	return;
    }
    surfPtr->iconic = true;
    TkHaikuWindowMinimize(surfPtr->window, true);
    XUnmapWindow(winPtr->display, winPtr->id);
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuWmUnmapSurface --
 *
 *	Called when a child of the root window is unmapped or destroyed: the
 *	keyboard goes to the top-most toplevel left.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May move the keyboard focus, and the capture of the pointer.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuWmUnmapSurface(
    TkXeSurface *surfPtr)	/* The surface of the unmapped window. */
{
    if (TkHaikuKeyboardSurface() == surfPtr) {
	TkXeProperty *propPtr = TkXeGetProperty(surfPtr->winPtr,
		XA_WM_TRANSIENT_FOR);
	TkXeSurface *topPtr = NULL;

	/*
	 * The keyboard goes back to the master of a dialog, else to the
	 * top-most toplevel.
	 */

	if (propPtr && propPtr->format == 32 && propPtr->nitems >= 1) {
	    TkXeWindow *masterPtr = TkXeGetWindow(
		    (Window)((long *)(void *)propPtr->data)[0]);

	    if (masterPtr && masterPtr->surface && masterPtr->mapped
		    && !masterPtr->surface->iconic) {
		topPtr = masterPtr->surface;
	    }
	}
	if (topPtr == NULL) {
	    topPtr = TopToplevel(surfPtr);
	}
	TkHaikuSetKeyboardSurface(topPtr);
	if (topPtr && topPtr->window) {
	    TkHaikuWindowActivate(topPtr->window);
	}
    }
    TkHaikuUpdateCapture();
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWmSurfaceResized --
 *
 *	Called when Tk resizes a child of the root window. The window manager
 *	lets it.
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
TkXeWmSurfaceResized(
    TCL_UNUSED(TkXeSurface *))
{
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWmPropertyChanged --
 *
 *	Called when a property of a child of the root window changes.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The title or the size limits of the BWindow may change.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWmPropertyChanged(
    TkXeSurface *surfPtr,	/* The surface of the window. */
    Atom property)		/* The property which changed. */
{
    InitAtoms(surfPtr->winPtr->display);
    if (surfPtr->window == NULL || surfPtr->popup) {
	return;
    }
    if (property == XA_WM_NAME || property == atoms._NET_WM_NAME) {
	ApplyTitle(surfPtr);
    } else if (property == XA_WM_NORMAL_HINTS) {
	ApplySizeHints(surfPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWmClientMessage --
 *
 *	Handle a client message sent to the root window, which is for the
 *	window manager.
 *
 * Results:
 *	True if the message was for the window manager.
 *
 * Side effects:
 *	The window may be minimized, activated, maximized, made fullscreen
 *	or put above the others.
 *
 *----------------------------------------------------------------------
 */

bool
TkXeWmClientMessage(
    XEvent *eventPtr)		/* The message. */
{
    TkXeWindow *winPtr;
    TkXeSurface *surfPtr;

    if (eventPtr->type != ClientMessage) {
	return false;
    }
    InitAtoms(eventPtr->xany.display);
    winPtr = TkXeGetWindow(eventPtr->xclient.window);
    if (winPtr == NULL || (surfPtr = winPtr->surface) == NULL) {
	return true;
    }
    if (eventPtr->xclient.message_type == atoms.WM_CHANGE_STATE
	    && eventPtr->xclient.data.l[0] == IconicState) {
	/*
	 * Minimize: the BWindow goes to the Deskbar, and the X window is
	 * unmapped, as X window managers do.
	 */

	if (winPtr->mapped && surfPtr->window && !surfPtr->popup) {
	    surfPtr->iconic = true;
	    TkHaikuWindowMinimize(surfPtr->window, true);
	    XUnmapWindow(winPtr->display, winPtr->id);
	}
    } else if (eventPtr->xclient.message_type == atoms._NET_ACTIVE_WINDOW) {
	TkXeWmActivate(surfPtr);
    } else if (eventPtr->xclient.message_type == atoms._NET_WM_STATE
	    && !winPtr->overrideRedirect) {
	int i;

	for (i = 1; i <= 2; i++) {
	    Atom state = (Atom)eventPtr->xclient.data.l[i];

	    if (state == atoms._NET_WM_STATE_MAXIMIZED_HORZ
		    && (Atom)eventPtr->xclient.data.l[1]
			    == atoms._NET_WM_STATE_MAXIMIZED_VERT) {
		continue;
	    }
	    if (state != None) {
		ChangeState(surfPtr, state, eventPtr->xclient.data.l[0]);
	    }
	}
    }
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWmActivate --
 *
 *	Activate a toplevel: its BWindow is raised and gets the keyboard. An
 *	override-redirect window only gets the keyboard.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Moves the keyboard focus.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWmActivate(
    TkXeSurface *surfPtr)	/* The surface of the toplevel. */
{
    if (surfPtr == NULL || !surfPtr->winPtr->mapped
	    || surfPtr->window == NULL || surfPtr->iconic) {
	return;
    }

    /*
     * An override-redirect window (the dropdown of a combobox) gets the
     * keyboard from the BWindow which has it, without taking the focus.
     */

    if (!surfPtr->popup) {
	TkHaikuWindowActivate(surfPtr->window);
    }
    TkHaikuSetKeyboardSurface(surfPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * FollowFrame --
 *
 *	Move and resize the X window of a surface to its BWindow, which the
 *	user moved or resized: the frame is read again, so that events about
 *	moves Tk made itself, which are behind, change nothing.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The X window may be moved and resized, which tells Tk with a
 *	ConfigureNotify event.
 *
 *----------------------------------------------------------------------
 */

static void
FollowFrame(
    TkXeSurface *surfPtr)	/* The surface. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    int x, y, width, height, lx, ly, lw, lh;

    if (surfPtr->window == NULL || !winPtr->mapped || surfPtr->iconic) {
	return;
    }
    TkHaikuWindowGetFrame(surfPtr->window, &x, &y, &width, &height);
    if (width == surfPtr->clampedWidth && height == surfPtr->clampedHeight) {
	/*
	 * The size the app_server gave instead of the one Tk asked for.
	 */

	width = surfPtr->bufferWidth;
	height = surfPtr->bufferHeight;
    } else {
	surfPtr->clampedWidth = surfPtr->clampedHeight = 0;
    }
    lx = (int)lround(TkHaikuFromScreen(x));
    ly = (int)lround(TkHaikuFromScreen(y));
    lw = (width == surfPtr->bufferWidth) ? winPtr->width
	    : (int)lround(TkHaikuFromScreen(width));
    lh = (height == surfPtr->bufferHeight) ? winPtr->height
	    : (int)lround(TkHaikuFromScreen(height));
    if (lx == winPtr->x && ly == winPtr->y && lw == winPtr->width
	    && lh == winPtr->height) {
	return;
    }
    surfPtr->inConfigure = true;
    XMoveResizeWindow(winPtr->display, winPtr->id, lx, ly, (unsigned)lw,
	    (unsigned)lh);
    surfPtr->inConfigure = false;
}

/*
 *----------------------------------------------------------------------
 *
 * FollowActivation --
 *
 *	Called when a BWindow was activated or deactivated: the toplevel
 *	whose BWindow is active now gets the keyboard, and its X window is
 *	raised, as the app_server raised the BWindow. The BWindows are asked,
 *	rather than the events believed, since the events of different
 *	windows come from different threads, in any order.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The keyboard focus may move, and an X window be raised.
 *
 *----------------------------------------------------------------------
 */

static void
FollowActivation(void)
{
    TkXeSurface *surfPtr, *activePtr = NULL, *keyboardPtr;

    for (surfPtr = tkXeDisplay->surfaces; surfPtr;
	    surfPtr = surfPtr->nextPtr) {
	if (!surfPtr->popup && surfPtr->window && surfPtr->winPtr->mapped
		&& !surfPtr->iconic
		&& TkHaikuWindowIsActive(surfPtr->window)) {
	    activePtr = surfPtr;
	    break;
	}
    }
    keyboardPtr = TkHaikuKeyboardSurface();
    if (activePtr == NULL) {
	/*
	 * Another application is active, or another BWindow is about to be:
	 * the keyboard is taken from Tk if none is soon.
	 */

	if (!deactivatedPending) {
	    deactivatedPending = true;
	    Tcl_CreateTimerHandler(200, DeactivatedTimer, NULL);
	}
	return;
    }
    if (keyboardPtr == NULL || !keyboardPtr->popup) {
	TkHaikuSetKeyboardSurface(activePtr);
    }
    if (activePtr->winPtr->above != NULL) {
	XWindowChanges changes;

	followingStack = true;
	changes.stack_mode = Above;
	XConfigureWindow(activePtr->winPtr->display, activePtr->winPtr->id,
		CWStackMode, &changes);
	followingStack = false;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * DeactivatedTimer --
 *
 *	Called a moment after no BWindow of the application was active: if
 *	none is still, the application lost the keyboard.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The keyboard focus may leave Tk.
 *
 *----------------------------------------------------------------------
 */

static void
DeactivatedTimer(
    TCL_UNUSED(void *))
{
    TkXeSurface *surfPtr;

    deactivatedPending = false;
    for (surfPtr = tkXeDisplay->surfaces; surfPtr;
	    surfPtr = surfPtr->nextPtr) {
	if (!surfPtr->popup && surfPtr->window && surfPtr->winPtr->mapped
		&& TkHaikuWindowIsActive(surfPtr->window)) {
	    return;
	}
    }
    TkHaikuSetKeyboardSurface(NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * DeleteWindow --
 *
 *	Ask a toplevel to close, with WM_DELETE_WINDOW.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A ClientMessage event is queued.
 *
 *----------------------------------------------------------------------
 */

static void
DeleteWindow(
    TkXeSurface *surfPtr)	/* The surface of the toplevel. */
{
    XEvent event;

    memset(&event, 0, sizeof(XEvent));
    event.xclient.type = ClientMessage;
    event.xclient.display = tkXeDisplay->display;
    event.xclient.window = surfPtr->winPtr->id;
    event.xclient.message_type = atoms.WM_PROTOCOLS;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long)atoms.WM_DELETE_WINDOW;
    event.xclient.data.l[1] = (long)TkXeCurrentTime();
    TkXeQueueEvent(&event);
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuWmEvent --
 *
 *	Handle what the user did to a BWindow: activating, moving, resizing,
 *	closing, minimizing or zooming it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	X events are generated as an X window manager would.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuWmEvent(
    TkXeSurface *surfPtr,	/* The surface of the BWindow. */
    TkHaikuEvent *eventPtr)	/* The event. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;

    InitAtoms(winPtr->display);
    switch (eventPtr->type) {
    case TKHAIKU_ACTIVATED:
	FollowActivation();
	break;
    case TKHAIKU_MOVED:
    case TKHAIKU_RESIZED:
	FollowFrame(surfPtr);
	break;
    case TKHAIKU_CLOSE:
	DeleteWindow(surfPtr);
	break;
    case TKHAIKU_MINIMIZED:
	if (eventPtr->u.minimized && !surfPtr->iconic && winPtr->mapped) {
	    surfPtr->iconic = true;
	    XUnmapWindow(winPtr->display, winPtr->id);
	} else if (!eventPtr->u.minimized && surfPtr->iconic) {
	    XMapWindow(winPtr->display, winPtr->id);
	}
	break;
    case TKHAIKU_ZOOM:
	if (!surfPtr->popup) {
	    ChangeState(surfPtr, atoms._NET_WM_STATE_MAXIMIZED_VERT, 2);
	}
	break;
    default:
	break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuWmRestack --
 *
 *	Called when the X windows may have been restacked: restack the
 *	BWindows of the toplevels the same, unless the window manager is
 *	following the app_server.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	BWindows may be sent behind others.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuWmRestack(void)
{
    TkXeWindow *rootPtr, *childPtr;
    TkXeSurface *abovePtr = NULL;
    int index = 0;
    bool changed = false;

    if (tkXeDisplay->display == NULL) {
	return;
    }
    rootPtr = TkXeGetWindow(RootWindow(tkXeDisplay->display, 0));
    for (childPtr = rootPtr->lastChild; childPtr;
	    childPtr = childPtr->below) {
	TkXeSurface *surfPtr = childPtr->surface;

	if (surfPtr == NULL || !childPtr->mapped || surfPtr->popup
		|| surfPtr->window == NULL || surfPtr->iconic) {
	    continue;
	}
	if (surfPtr->stackIndex != index) {
	    changed = true;
	}
	surfPtr->stackIndex = index++;
    }
    if (!changed || followingStack) {
	return;
    }

    /*
     * From the top down, each toplevel is sent behind the one above it.
     */

    for (childPtr = rootPtr->lastChild; childPtr;
	    childPtr = childPtr->below) {
	TkXeSurface *surfPtr = childPtr->surface;

	if (surfPtr == NULL || !childPtr->mapped || surfPtr->popup
		|| surfPtr->window == NULL || surfPtr->iconic) {
	    continue;
	}
	if (abovePtr) {
	    TkHaikuWindowSendBehind(surfPtr->window, abovePtr->window);
	}
	abovePtr = surfPtr;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuWmScreenChanged --
 *
 *	Set the work area in the _NET_WORKAREA property of the root window,
 *	and place the maximized and fullscreen toplevels again, when the
 *	screen changed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The property changes; windows may move.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuWmScreenChanged(void)
{
    Display *display = tkXeDisplay->display;
    TkXeSurface *surfPtr;
    long area[4], desktop = 0;
    int x, y, width, height;

    InitAtoms(display);
    TkHaikuWorkArea(&x, &y, &width, &height);
    area[0] = (long)ceil(TkHaikuFromScreen(x));
    area[1] = (long)ceil(TkHaikuFromScreen(y));
    area[2] = (long)floor(TkHaikuFromScreen(width));
    area[3] = (long)floor(TkHaikuFromScreen(height));
    XChangeProperty(display, RootWindow(display, 0), atoms._NET_WORKAREA,
	    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)area, 4);
    XChangeProperty(display, RootWindow(display, 0),
	    atoms._NET_CURRENT_DESKTOP, XA_CARDINAL, 32, PropModeReplace,
	    (unsigned char *)&desktop, 1);
    for (surfPtr = tkXeDisplay->surfaces; surfPtr;
	    surfPtr = surfPtr->nextPtr) {
	if (surfPtr->winPtr->mapped
		&& (surfPtr->maximized || surfPtr->fullscreen)) {
	    Place(surfPtr);
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuWmQuit --
 *
 *	The user asked the application to quit (from the Deskbar): ask its
 *	toplevels to close, with WM_DELETE_WINDOW.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	ClientMessage events are queued.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuWmQuit(void)
{
    TkXeSurface *surfPtr;

    InitAtoms(tkXeDisplay->display);
    for (surfPtr = tkXeDisplay->surfaces; surfPtr;
	    surfPtr = surfPtr->nextPtr) {
	if (!surfPtr->popup && (surfPtr->winPtr->mapped || surfPtr->iconic)) {
	    DeleteWindow(surfPtr);
	}
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
