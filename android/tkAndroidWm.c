/*
 * tkAndroidWm.c --
 *
 *	The window manager of the emulated X display on Android. As in the
 *	Wayland port, the "wm" command is implemented by the X11 code
 *	(tkUnixWm.c), which gives hints to the window manager with ICCCM and
 *	EWMH properties and client messages, and this file plays the part of
 *	the window manager.
 *
 *	There is only one window, the window of the activity, and windows
 *	do not overlap on a phone, so the policy is simple: toplevels are
 *	maximized (they fill the part of the screen which is not covered by
 *	the system bars and the soft keyboard), except dialogs (transient
 *	windows, and windows which cannot be resized), which are centered.
 *	Override-redirect windows (menus, tooltips, combobox dropdowns) stay
 *	where Tk puts them. The back button closes the top-most toplevel, or
 *	dismisses the menu which is posted.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkAndroidInt.h"

/*
 * The atoms of the properties and messages the window manager handles.
 */

static struct {
    Atom WM_PROTOCOLS, WM_DELETE_WINDOW, _NET_WM_STATE,
	_NET_WM_STATE_MAXIMIZED_VERT, _NET_WM_STATE_MAXIMIZED_HORZ,
	_NET_WM_STATE_FULLSCREEN, _NET_ACTIVE_WINDOW, _NET_WM_WINDOW_OPACITY,
	_NET_WM_WINDOW_TYPE, _NET_WM_WINDOW_TYPE_DIALOG;
} atoms;

static void		InitAtoms(Display *display);
static long *		GetLongs(TkXeWindow *winPtr, Atom property,
			    unsigned long minItems);
static bool		HasState(TkXeWindow *winPtr, Atom state);
static int		Placement(TkXeSurface *surfPtr);
static void		Place(TkXeSurface *surfPtr);
static void		SetNetWmState(TkXeSurface *surfPtr);
static TkXeSurface *	TopToplevel(TkXeSurface *exceptPtr);

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
    ATOM(_NET_WM_STATE);
    ATOM(_NET_WM_STATE_MAXIMIZED_VERT);
    ATOM(_NET_WM_STATE_MAXIMIZED_HORZ);
    ATOM(_NET_WM_STATE_FULLSCREEN);
    ATOM(_NET_ACTIVE_WINDOW);
    ATOM(_NET_WM_WINDOW_OPACITY);
    ATOM(_NET_WM_WINDOW_TYPE);
    ATOM(_NET_WM_WINDOW_TYPE_DIALOG);
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
 * TkAndroidWmWorkArea --
 *
 *	The part of the screen in which toplevels are placed: the part of the
 *	window of the activity which is not covered by the system bars and
 *	the soft keyboard.
 *
 * Results:
 *	The rectangle, in root coordinates.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidWmWorkArea(
    int *xPtr, int *yPtr,	/* Returns the position of the work area. */
    int *widthPtr, int *heightPtr)
				/* Returns its size. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    double s = dispPtr->scale;
    Screen *screen = ScreenOfDisplay(dispPtr->display, 0);
    int x1 = (int)ceil(dispPtr->content[0] / s);
    int y1 = (int)ceil(dispPtr->content[1] / s);
    int x2 = (int)floor(dispPtr->content[2] / s);
    int y2 = (int)floor(dispPtr->content[3] / s);

    if (x2 > screen->width) {
	x2 = screen->width;
    }
    if (y2 > screen->height) {
	y2 = screen->height;
    }
    if (x2 - x1 < 1 || y2 - y1 < 1) {
	x1 = y1 = 0;
	x2 = screen->width;
	y2 = screen->height;
    }
    *xPtr = x1;
    *yPtr = y1;
    *widthPtr = x2 - x1;
    *heightPtr = y2 - y1;
}

/*
 *----------------------------------------------------------------------
 *
 * Placement --
 *
 *	Decide how a toplevel is placed, from its hints.
 *
 * Results:
 *	One of the TKANDROID_PLACE_* values.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
Placement(
    TkXeSurface *surfPtr)	/* The surface of the toplevel. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    long *hints;

    if (winPtr->overrideRedirect) {
	return TKANDROID_PLACE_NONE;
    }
    if (HasState(winPtr, atoms._NET_WM_STATE_FULLSCREEN)) {
	return TKANDROID_PLACE_FULLSCREEN;
    }
    if (HasState(winPtr, atoms._NET_WM_STATE_MAXIMIZED_VERT)
	    || HasState(winPtr, atoms._NET_WM_STATE_MAXIMIZED_HORZ)) {
	return TKANDROID_PLACE_MAXIMIZED;
    }
    if (TkXeGetProperty(winPtr, XA_WM_TRANSIENT_FOR)) {
	return TKANDROID_PLACE_DIALOG;
    }
    hints = GetLongs(winPtr, atoms._NET_WM_WINDOW_TYPE, 1);
    if (hints && (Atom)hints[0] == atoms._NET_WM_WINDOW_TYPE_DIALOG) {
	return TKANDROID_PLACE_DIALOG;
    }
    hints = GetLongs(winPtr, XA_WM_NORMAL_HINTS, SH_NUM);
    if (hints && (hints[SH_FLAGS] & PMinSize) && (hints[SH_FLAGS] & PMaxSize)
	    && hints[SH_MIN_WIDTH] == hints[SH_MAX_WIDTH]
	    && hints[SH_MIN_HEIGHT] == hints[SH_MAX_HEIGHT]) {
	return TKANDROID_PLACE_DIALOG;
    }
    return TKANDROID_PLACE_MAXIMIZED;
}

/*
 *----------------------------------------------------------------------
 *
 * Place --
 *
 *	Give a toplevel the position and size its placement gives it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The window is moved and resized, which tells Tk with a
 *	ConfigureNotify event, as with an X window manager.
 *
 *----------------------------------------------------------------------
 */

static void
Place(
    TkXeSurface *surfPtr)	/* The surface of the toplevel. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    int ax, ay, aw, ah, x, y, width, height;

    if (surfPtr->placement == TKANDROID_PLACE_NONE || !winPtr->mapped) {
	return;
    }
    TkAndroidWmWorkArea(&ax, &ay, &aw, &ah);
    switch (surfPtr->placement) {
    case TKANDROID_PLACE_FULLSCREEN: {
	Screen *screen = ScreenOfDisplay(winPtr->display, 0);

	x = y = 0;
	width = screen->width;
	height = screen->height;
	break;
    }
    case TKANDROID_PLACE_MAXIMIZED:
	x = ax;
	y = ay;
	width = aw;
	height = ah;
	break;
    default:
	width = winPtr->width;
	height = winPtr->height;
	if (width > aw) {
	    width = aw;
	}
	if (height > ah) {
	    height = ah;
	}
	x = ax + (aw - width) / 2;
	y = ay + (ah - height) / 2;
	break;
    }
    if (x == winPtr->x && y == winPtr->y && width == winPtr->width
	    && height == winPtr->height) {
	return;
    }
    surfPtr->inConfigure = true;
    XMoveResizeWindow(winPtr->display, winPtr->id, x, y, (unsigned)width,
	    (unsigned)height);
    surfPtr->inConfigure = false;
}

/*
 *----------------------------------------------------------------------
 *
 * SetNetWmState --
 *
 *	Set _NET_WM_STATE to the placement of a toplevel, as a window manager
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
    long states[2];
    int n = 0;

    if (surfPtr->placement == TKANDROID_PLACE_MAXIMIZED) {
	states[n++] = (long)atoms._NET_WM_STATE_MAXIMIZED_VERT;
	states[n++] = (long)atoms._NET_WM_STATE_MAXIMIZED_HORZ;
    } else if (surfPtr->placement == TKANDROID_PLACE_FULLSCREEN) {
	states[n++] = (long)atoms._NET_WM_STATE_FULLSCREEN;
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
		&& childPtr->surface && childPtr->surface != exceptPtr) {
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
 *	Called when a child of the root window is mapped: place it, and give
 *	the keyboard to it unless it is override-redirect.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The window may be moved and resized.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWmMapSurface(
    TkXeSurface *surfPtr)	/* The surface of the mapped window. */
{
    InitAtoms(surfPtr->winPtr->display);
    surfPtr->placement = Placement(surfPtr);
    if (surfPtr->placement == TKANDROID_PLACE_NONE) {
	return;
    }
    SetNetWmState(surfPtr);
    Place(surfPtr);
    TkAndroidSetKeyboardSurface(surfPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidWmUnmapSurface --
 *
 *	Called when a child of the root window is unmapped or destroyed: the
 *	keyboard goes to the top-most toplevel left.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May move the keyboard focus.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidWmUnmapSurface(
    TkXeSurface *surfPtr)	/* The surface of the unmapped window. */
{
    if (TkAndroidKeyboardSurface() == surfPtr) {
	TkAndroidSetKeyboardSurface(TopToplevel(surfPtr));
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWmSurfaceResized --
 *
 *	Called when Tk resizes a child of the root window. A maximized
 *	window keeps the size of the work area: the window manager sets it
 *	back, which Tk takes as a resize by the user, as with X.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The window may be resized.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWmSurfaceResized(
    TkXeSurface *surfPtr)	/* The surface of the resized window. */
{
    if (surfPtr->inConfigure || surfPtr->placement == TKANDROID_PLACE_NONE) {
	return;
    }
    Place(surfPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidWmRelayout --
 *
 *	Place all the toplevels again, when the work area changes (the
 *	screen rotated, or the soft keyboard was shown or hidden).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Windows may be moved and resized.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidWmRelayout(void)
{
    TkXeSurface *surfPtr;

    for (surfPtr = tkXeDisplay->surfaces; surfPtr; surfPtr = surfPtr->nextPtr) {
	if (surfPtr->winPtr->mapped) {
	    Place(surfPtr);
	}
    }
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
 *	The opacity of the window, or its placement, may change.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWmPropertyChanged(
    TkXeSurface *surfPtr,	/* The surface of the window. */
    Atom property)		/* The property which changed. */
{
    InitAtoms(surfPtr->winPtr->display);
    if (property == atoms._NET_WM_WINDOW_OPACITY) {
	long *opacity = GetLongs(surfPtr->winPtr, property, 1);

	TkAndroidSurfaceSetOpacity(surfPtr, opacity
		? (unsigned long)opacity[0] / (double)0xFFFFFFFFul : 1.0);
    } else if ((property == XA_WM_TRANSIENT_FOR
	    || property == XA_WM_NORMAL_HINTS)
	    && surfPtr->winPtr->mapped
	    && surfPtr->placement != TKANDROID_PLACE_NONE) {
	int placement = Placement(surfPtr);

	if (placement != surfPtr->placement) {
	    surfPtr->placement = placement;
	    SetNetWmState(surfPtr);
	    Place(surfPtr);
	}
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
 *	The window may be activated, maximized or made fullscreen.
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
    if (eventPtr->xclient.message_type == atoms._NET_ACTIVE_WINDOW) {
	TkXeWmActivate(surfPtr);
    } else if (eventPtr->xclient.message_type == atoms._NET_WM_STATE
	    && surfPtr->placement != TKANDROID_PLACE_NONE) {
	long action = eventPtr->xclient.data.l[0];
	int i, placement = surfPtr->placement;

	for (i = 1; i <= 2; i++) {
	    Atom state = (Atom)eventPtr->xclient.data.l[i];
	    bool on = (action == 1);

	    if (action == 2) {
		on = !HasState(winPtr, state);
	    }
	    if (state == atoms._NET_WM_STATE_FULLSCREEN) {
		placement = on ? TKANDROID_PLACE_FULLSCREEN
			: TKANDROID_PLACE_MAXIMIZED;
	    } else if ((state == atoms._NET_WM_STATE_MAXIMIZED_VERT
		    || state == atoms._NET_WM_STATE_MAXIMIZED_HORZ)
		    && placement != TKANDROID_PLACE_FULLSCREEN) {
		placement = on ? TKANDROID_PLACE_MAXIMIZED
			: TKANDROID_PLACE_DIALOG;
	    }
	}
	if (placement != surfPtr->placement) {
	    surfPtr->placement = placement;
	    SetNetWmState(surfPtr);
	    Place(surfPtr);
	}
    }
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWmActivate --
 *
 *	Activate a toplevel: it gets the keyboard. (Raising it is done by
 *	the caller.)
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
    if (surfPtr && surfPtr->winPtr->mapped
	    && !surfPtr->winPtr->overrideRedirect) {
	TkAndroidSetKeyboardSurface(surfPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidWmBack --
 *
 *	Handle the back button or gesture: dismiss the menu or dropdown
 *	which is posted (with the Escape key its bindings handle), or else
 *	ask the top-most toplevel to close, with WM_DELETE_WINDOW.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Events are queued.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidWmBack(void)
{
    TkXeWindow *rootPtr, *childPtr;
    TkXeSurface *surfPtr;
    XEvent event;

    if (tkXeDisplay->display == NULL) {
	return;
    }
    InitAtoms(tkXeDisplay->display);
    rootPtr = TkXeGetWindow(RootWindow(tkXeDisplay->display, 0));
    for (childPtr = rootPtr->lastChild; childPtr;
	    childPtr = childPtr->below) {
	if (childPtr->mapped && childPtr->overrideRedirect) {
	    TkAndroidQueueKey(XK_Escape);
	    return;
	}
    }
    surfPtr = TopToplevel(NULL);
    if (surfPtr == NULL) {
	return;
    }
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
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
