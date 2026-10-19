/*
 * tkXemuWindow.c --
 *
 *	The window part of the X server emulation: the window tree, the
 *	visible regions of windows and Expose events. The surfaces of the
 *	children of the root window, which present the images Tk draws into,
 *	are given by the port (tkWaylandSurface.c).
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkXemuInt.h"

/*
 * The visible regions of windows are cached until the window tree changes.
 */

static unsigned long clipGeneration = 1;

/*
 * Ids of windows and pixmaps.
 */

static Tcl_HashTable idTable;
static int idTableInitialized = 0;
static XID nextId = 0x200000;

static void		QueueStructureEvent(TkXeWindow *winPtr,
			    XEvent *eventPtr);
static void		UnlinkWindow(TkXeWindow *winPtr);
static void		LinkWindow(TkXeWindow *winPtr, TkXeWindow *parentPtr,
			    TkXeWindow *belowPtr);

/*
 *----------------------------------------------------------------------
 *
 * TkXeNewId --
 *
 *	Give a new id to a window or a pixmap. Ids are never reused, so that
 *	the requests and events for destroyed windows are ignored.
 *
 * Results:
 *	The new id.
 *
 * Side effects:
 *	The id is recorded, so that TkXeLookupId finds the structure.
 *
 *----------------------------------------------------------------------
 */

XID
TkXeNewId(
    void *ptr)
{
    Tcl_HashEntry *hPtr;
    int isNew;
    XID id = nextId++;

    if (!idTableInitialized) {
	Tcl_InitHashTable(&idTable, TCL_ONE_WORD_KEYS);
	idTableInitialized = 1;
    }
    hPtr = Tcl_CreateHashEntry(&idTable, INT2PTR(id), &isNew);
    Tcl_SetHashValue(hPtr, ptr);
    return id;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeFreeId --
 *
 *	Forget the id of a destroyed window or pixmap.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	TkXeLookupId no longer finds the id.
 *
 *----------------------------------------------------------------------
 */

void
TkXeFreeId(
    XID id)
{
    Tcl_HashEntry *hPtr;

    if (idTableInitialized
	    && (hPtr = Tcl_FindHashEntry(&idTable, INT2PTR(id))) != NULL) {
	Tcl_DeleteHashEntry(hPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeLookupId --
 *
 *	Find the window or pixmap which has an id.
 *
 * Results:
 *	The structure of the window or pixmap (whose first field tells which),
 *	or NULL if the id is unknown.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void *
TkXeLookupId(
    XID id)
{
    Tcl_HashEntry *hPtr;

    if (!idTableInitialized || id == None) {
	return NULL;
    }
    hPtr = Tcl_FindHashEntry(&idTable, INT2PTR(id));
    return hPtr ? Tcl_GetHashValue(hPtr) : NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * NewWindow --
 *
 *	Allocate and initialize a window structure.
 *
 * Results:
 *	The new window, which is not linked in the window tree.
 *
 * Side effects:
 *	Memory is allocated, and the window gets a new id.
 *
 *----------------------------------------------------------------------
 */

static TkXeWindow *
NewWindow(
    Display *display)
{
    TkXeWindow *winPtr = (TkXeWindow *)Tcl_Alloc(sizeof(TkXeWindow));

    memset(winPtr, 0, sizeof(TkXeWindow));
    winPtr->type = TKXE_WINDOW;
    winPtr->id = TkXeNewId(winPtr);
    winPtr->display = display;
    winPtr->bitGravity = ForgetGravity;
    winPtr->backgroundType = BG_NONE;
    winPtr->visibility = -1;
    pixman_region32_init(&winPtr->exposed);
    pixman_region32_init(&winPtr->clip);
    return winPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeCreateRootWindow --
 *
 *	Create the root window of the emulated display.
 *
 * Results:
 *	The root window.
 *
 * Side effects:
 *	Memory is allocated.
 *
 *----------------------------------------------------------------------
 */

TkXeWindow *
TkXeCreateRootWindow(
    Display *display,
    int width, int height)
{
    TkXeWindow *rootPtr = NewWindow(display);

    rootPtr->width = width;
    rootPtr->height = height;
    rootPtr->mapped = true;
    rootPtr->backgroundType = BG_PIXEL;
    return rootPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * LinkWindow, UnlinkWindow --
 *
 *	Insert a window in the stacking order of its parent, above the given
 *	sibling (at the bottom if it is NULL), or remove it from the stacking
 *	order of its parent.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The links of the window, of its siblings and of its parent are
 *	changed.
 *
 *----------------------------------------------------------------------
 */

static void
LinkWindow(
    TkXeWindow *winPtr,
    TkXeWindow *parentPtr,
    TkXeWindow *belowPtr)
{
    winPtr->parent = parentPtr;
    winPtr->below = belowPtr;
    if (belowPtr) {
	winPtr->above = belowPtr->above;
	belowPtr->above = winPtr;
    } else {
	winPtr->above = parentPtr->firstChild;
	parentPtr->firstChild = winPtr;
    }
    if (winPtr->above) {
	winPtr->above->below = winPtr;
    } else {
	parentPtr->lastChild = winPtr;
    }
}

static void
UnlinkWindow(
    TkXeWindow *winPtr)
{
    TkXeWindow *parentPtr = winPtr->parent;

    if (parentPtr == NULL) {
	return;
    }
    if (winPtr->below) {
	winPtr->below->above = winPtr->above;
    } else {
	parentPtr->firstChild = winPtr->above;
    }
    if (winPtr->above) {
	winPtr->above->below = winPtr->below;
    } else {
	parentPtr->lastChild = winPtr->below;
    }
    winPtr->parent = winPtr->above = winPtr->below = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeTopWindow --
 *
 *	Find the child of the root window which contains a window.
 *
 * Results:
 *	The child of the root window, or NULL for the root window.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

TkXeWindow *
TkXeTopWindow(
    TkXeWindow *winPtr)
{
    if (winPtr->parent == NULL) {
	return NULL;
    }
    while (winPtr->parent->parent) {
	winPtr = winPtr->parent;
    }
    return winPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeGetSurface --
 *
 *	Find the surface which shows a window.
 *
 * Results:
 *	The surface of the child of the root window which contains the window,
 *	or NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

TkXeSurface *
TkXeGetSurface(
    Window w)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);

    if (winPtr == NULL || (winPtr = TkXeTopWindow(winPtr)) == NULL) {
	return NULL;
    }
    return winPtr->surface;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeIsViewable --
 *
 *	Tell whether a window and all of its ancestors are mapped.
 *
 * Results:
 *	True if the window is viewable, false otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

bool
TkXeIsViewable(
    TkXeWindow *winPtr)
{
    for (; winPtr; winPtr = winPtr->parent) {
	if (!winPtr->mapped) {
	    return false;
	}
    }
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWindowOrigin --
 *
 *	Compute the position of the inside of a window in the surface of its
 *	top window. The outer corner of the top window is at 0,0.
 *
 * Results:
 *	The position is stored in *xPtr and *yPtr.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWindowOrigin(
    TkXeWindow *winPtr,
    int *xPtr, int *yPtr)
{
    int x = winPtr->borderWidth, y = winPtr->borderWidth;

    while (winPtr->parent && winPtr->parent->parent) {
	x += winPtr->x + winPtr->parent->borderWidth;
	y += winPtr->y + winPtr->parent->borderWidth;
	winPtr = winPtr->parent;
    }
    *xPtr = x;
    *yPtr = y;
}

/*
 *----------------------------------------------------------------------
 *
 * SubtractOuter --
 *
 *	Remove the outside of a window (including its border) from a region,
 *	in the coordinates of the surface.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The region is changed.
 *
 *----------------------------------------------------------------------
 */

static void
SubtractOuter(
    pixman_region32_t *rgn,
    TkXeWindow *winPtr,
    int parentX, int parentY)	/* Position of the inside of the parent. */
{
    pixman_region32_t box;

    pixman_region32_init_rect(&box, parentX + winPtr->x,
	    parentY + winPtr->y,
	    (unsigned)(winPtr->width + 2 * winPtr->borderWidth),
	    (unsigned)(winPtr->height + 2 * winPtr->borderWidth));
    pixman_region32_subtract(rgn, rgn, &box);
    pixman_region32_fini(&box);
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeVisibleRegion --
 *
 *	Compute the part of a window which can be drawn into, in the
 *	coordinates of its surface: the inside of the window, clipped by its
 *	ancestors, minus the mapped siblings above it and above its ancestors,
 *	and minus its mapped children unless includeInferiors is set.
 *
 * Results:
 *	The region is stored in rgn, which must be initialized.
 *
 * Side effects:
 *	The region is cached in the window until the window tree changes.
 *
 *----------------------------------------------------------------------
 */

void
TkXeVisibleRegion(
    TkXeWindow *winPtr,
    bool includeInferiors,
    pixman_region32_t *rgn)
{
    TkXeWindow *ancestorPtr, *childPtr;
    int x, y;

    if (winPtr->clipGeneration == clipGeneration
	    && winPtr->clipInferiors == includeInferiors) {
	pixman_region32_copy(rgn, &winPtr->clip);
	return;
    }

    if (winPtr->parent == NULL || !TkXeIsViewable(winPtr)
	    || winPtr->inputOnly) {
	pixman_region32_clear(rgn);
	goto done;
    }

    TkXeWindowOrigin(winPtr, &x, &y);
    pixman_region32_fini(rgn);
    pixman_region32_init_rect(rgn, x, y, (unsigned)winPtr->width,
	    (unsigned)winPtr->height);

    /*
     * Clip by the ancestors, and remove the siblings above the window and
     * above each of its ancestors.
     */

    for (ancestorPtr = winPtr; ancestorPtr->parent->parent;
	    ancestorPtr = ancestorPtr->parent) {
	TkXeWindow *parentPtr = ancestorPtr->parent;
	TkXeWindow *siblingPtr;
	int px, py;

	TkXeWindowOrigin(parentPtr, &px, &py);
	pixman_region32_intersect_rect(rgn, rgn, px, py,
		(unsigned)parentPtr->width, (unsigned)parentPtr->height);
	for (siblingPtr = ancestorPtr->above; siblingPtr;
		siblingPtr = siblingPtr->above) {
	    if (siblingPtr->mapped && !siblingPtr->inputOnly) {
		SubtractOuter(rgn, siblingPtr, px, py);
	    }
	}
    }

    if (!includeInferiors) {
	for (childPtr = winPtr->firstChild; childPtr;
		childPtr = childPtr->above) {
	    if (childPtr->mapped && !childPtr->inputOnly) {
		SubtractOuter(rgn, childPtr, x, y);
	    }
	}
    }

  done:
    pixman_region32_copy(&winPtr->clip, rgn);
    winPtr->clipGeneration = clipGeneration;
    winPtr->clipInferiors = includeInferiors;
}

/*
 *----------------------------------------------------------------------
 *
 * ExposeIdle --
 *
 *	Idle handler which generates the Expose events of the surfaces whose
 *	windows changed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	See TkXeProcessExposures.
 *
 *----------------------------------------------------------------------
 */

static void
ExposeIdle(
    void *clientData)
{
    TkXeSurface *surfPtr;

    (void)clientData;
    for (surfPtr = tkXeDisplay ? tkXeDisplay->surfaces : NULL; surfPtr;
	    surfPtr = surfPtr->nextPtr) {
	TkXeProcessExposures(surfPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeTreeChanged --
 *
 *	Called when the window tree changes in a way which can change what is
 *	visible.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The cached visible regions are invalidated, and Expose events are
 *	scheduled for the surface of the window.
 *
 *----------------------------------------------------------------------
 */

void
TkXeTreeChanged(
    TkXeWindow *winPtr)
{
    TkXeWindow *topPtr = TkXeTopWindow(winPtr);
    bool scheduled = false;
    TkXeSurface *surfPtr;

    clipGeneration++;
    if (topPtr == NULL || topPtr->surface == NULL) {
	return;
    }
    for (surfPtr = tkXeDisplay->surfaces; surfPtr; surfPtr = surfPtr->nextPtr) {
	scheduled |= surfPtr->exposePending;
    }
    topPtr->surface->exposePending = true;
    if (!scheduled) {
	Tcl_DoWhenIdle(ExposeIdle, NULL);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ExposeWindow --
 *
 *	Generate the Expose events of a window and of its descendants for
 *	their parts which became visible since the last call, after painting
 *	them with the background of their window, as the X server does, and
 *	VisibilityNotify events when their visibility changed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The backgrounds are painted, events are queued, and what is visible of
 *	the windows is recorded.
 *
 *----------------------------------------------------------------------
 */

static void
ExposeWindow(
    TkXeWindow *winPtr)
{
    pixman_region32_t visible, newly;
    TkXeWindow *childPtr;
    int x, y;

    pixman_region32_init(&visible);
    TkXeVisibleRegion(winPtr, false, &visible);
    TkXeWindowOrigin(winPtr, &x, &y);
    pixman_region32_init(&newly);
    if (x == winPtr->exposedX && y == winPtr->exposedY
	    && ((winPtr->width == winPtr->exposedWidth
		    && winPtr->height == winPtr->exposedHeight)
		|| winPtr->bitGravity != ForgetGravity)) {
	pixman_region32_subtract(&newly, &visible, &winPtr->exposed);
    } else {
	pixman_region32_copy(&newly, &visible);
    }

    if (pixman_region32_not_empty(&newly)) {
	TkXePaintBackground(winPtr, &newly);
	if (winPtr->eventMask & ExposureMask) {
	    int i, n;
	    pixman_box32_t *boxes = pixman_region32_rectangles(&newly, &n);

	    for (i = 0; i < n; i++) {
		XEvent event;

		memset(&event, 0, sizeof(XEvent));
		event.xexpose.type = Expose;
		event.xexpose.display = winPtr->display;
		event.xexpose.window = winPtr->id;
		event.xexpose.x = boxes[i].x1 - x;
		event.xexpose.y = boxes[i].y1 - y;
		event.xexpose.width = boxes[i].x2 - boxes[i].x1;
		event.xexpose.height = boxes[i].y2 - boxes[i].y1;
		event.xexpose.count = n - i - 1;
		TkXeQueueEvent(&event);
	    }
	}
    }
    /*
     * Send VisibilityNotify when the visibility of a viewable window
     * changes.
     */

    if (TkXeIsViewable(winPtr) && !winPtr->inputOnly) {
	int state;

	if (!pixman_region32_not_empty(&visible)) {
	    state = VisibilityFullyObscured;
	} else if (pixman_region32_n_rects(&visible) == 1
		&& pixman_region32_extents(&visible)->x2
		    - pixman_region32_extents(&visible)->x1 == winPtr->width
		&& pixman_region32_extents(&visible)->y2
		    - pixman_region32_extents(&visible)->y1 == winPtr->height) {
	    state = VisibilityUnobscured;
	} else {
	    state = VisibilityPartiallyObscured;
	}
	if (state != winPtr->visibility
		&& (winPtr->eventMask & VisibilityChangeMask)) {
	    XEvent event;

	    memset(&event, 0, sizeof(XEvent));
	    event.xvisibility.type = VisibilityNotify;
	    event.xvisibility.display = winPtr->display;
	    event.xvisibility.window = winPtr->id;
	    event.xvisibility.state = state;
	    TkXeQueueEvent(&event);
	}
	winPtr->visibility = state;
    } else {
	winPtr->visibility = -1;
    }

    pixman_region32_copy(&winPtr->exposed, &visible);
    winPtr->exposedX = x;
    winPtr->exposedY = y;
    winPtr->exposedWidth = winPtr->width;
    winPtr->exposedHeight = winPtr->height;
    pixman_region32_fini(&newly);
    pixman_region32_fini(&visible);

    for (childPtr = winPtr->firstChild; childPtr; childPtr = childPtr->above) {
	ExposeWindow(childPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeProcessExposures --
 *
 *	Generate the Expose events of the windows of a surface, if its window
 *	tree changed. This is called before anything is drawn in the surface,
 *	so that the order of drawing and of painting backgrounds is the same
 *	as with X.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	See ExposeWindow.
 *
 *----------------------------------------------------------------------
 */

void
TkXeProcessExposures(
    TkXeSurface *surfPtr)
{
    if (!surfPtr->exposePending || surfPtr->backing == NULL) {
	return;
    }
    surfPtr->exposePending = false;
    ExposeWindow(surfPtr->winPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeQueueEvent --
 *
 *	Queue an event generated by the Xlib emulation, as if it came from the
 *	X server.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The event gets the serial of the last request, and is queued at the
 *	tail of the Tcl event queue.
 *
 *----------------------------------------------------------------------
 */

void
TkXeQueueEvent(
    XEvent *eventPtr)
{
    Display *display = eventPtr->xany.display;

    /*
     * The event follows all the requests made so far.
     */

    LastKnownRequestProcessed(display) = NextRequest(display) - 1;
    eventPtr->xany.serial = LastKnownRequestProcessed(display);
    eventPtr->xany.send_event = False;
    Tk_QueueWindowEvent(eventPtr, TCL_QUEUE_TAIL);
}

/*
 *----------------------------------------------------------------------
 *
 * QueueStructureEvent --
 *
 *	Send a StructureNotify event to a window and a SubstructureNotify
 *	event to its parent, as the X server does. The event and window fields
 *	of all these events are at the same place.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Events are queued, as the event masks of the windows ask.
 *
 *----------------------------------------------------------------------
 */

static void
QueueStructureEvent(
    TkXeWindow *winPtr,
    XEvent *eventPtr)
{
    eventPtr->xany.display = winPtr->display;
    if (winPtr->eventMask & StructureNotifyMask) {
	eventPtr->xmap.event = winPtr->id;
	TkXeQueueEvent(eventPtr);
    }
    if (winPtr->parent
	    && (winPtr->parent->eventMask & SubstructureNotifyMask)) {
	eventPtr->xmap.event = winPtr->parent->id;
	TkXeQueueEvent(eventPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * QueueConfigureNotify --
 *
 *	Send the ConfigureNotify events of a window which was moved, resized
 *	or restacked.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Events are queued.
 *
 *----------------------------------------------------------------------
 */

static void
QueueConfigureNotify(
    TkXeWindow *winPtr)
{
    XEvent event;

    memset(&event, 0, sizeof(XEvent));
    event.xconfigure.type = ConfigureNotify;
    event.xconfigure.window = winPtr->id;
    event.xconfigure.x = winPtr->x;
    event.xconfigure.y = winPtr->y;
    event.xconfigure.width = winPtr->width;
    event.xconfigure.height = winPtr->height;
    event.xconfigure.border_width = winPtr->borderWidth;
    event.xconfigure.above = winPtr->below ? winPtr->below->id : None;
    event.xconfigure.override_redirect = winPtr->overrideRedirect;
    QueueStructureEvent(winPtr, &event);
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeForgetExposed --
 *
 *	Forget what was drawn in a window and its descendants, so that they
 *	get Expose events for all of their visible parts.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The exposed regions of the windows are cleared.
 *
 *----------------------------------------------------------------------
 */

void
TkXeForgetExposed(
    TkXeWindow *winPtr)
{
    TkXeWindow *childPtr;

    pixman_region32_clear(&winPtr->exposed);
    winPtr->exposedWidth = -1;
    for (childPtr = winPtr->firstChild; childPtr; childPtr = childPtr->above) {
	TkXeForgetExposed(childPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWindowAtPoint --
 *
 *	Find the deepest mapped descendant of a window which contains a point,
 *	given relative to the inside of the window.
 *
 * Results:
 *	The descendant, or the window itself.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

TkXeWindow *
TkXeWindowAtPoint(
    TkXeWindow *winPtr,
    int x, int y)
{
    TkXeWindow *childPtr;

  again:
    for (childPtr = winPtr->lastChild; childPtr; childPtr = childPtr->below) {
	int cx = x - childPtr->x - childPtr->borderWidth;
	int cy = y - childPtr->y - childPtr->borderWidth;

	if (childPtr->mapped && cx >= -childPtr->borderWidth
		&& cy >= -childPtr->borderWidth
		&& cx < childPtr->width + childPtr->borderWidth
		&& cy < childPtr->height + childPtr->borderWidth) {
	    winPtr = childPtr;
	    x = cx;
	    y = cy;
	    goto again;
	}
    }
    return winPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * ChangeAttributes --
 *
 *	Change the attributes of a window given by valueMask.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The window structure is changed.
 *
 *----------------------------------------------------------------------
 */

static void
ChangeAttributes(
    TkXeWindow *winPtr,
    unsigned long valueMask,
    XSetWindowAttributes *attrPtr)
{
    if (valueMask & CWBackPixmap) {
	if (attrPtr->background_pixmap == None) {
	    winPtr->backgroundType = BG_NONE;
	} else if (attrPtr->background_pixmap == ParentRelative) {
	    winPtr->backgroundType = BG_PARENT;
	} else {
	    winPtr->backgroundType = BG_PIXMAP;
	    winPtr->backgroundPixmap = attrPtr->background_pixmap;
	}
    }
    if (valueMask & CWBackPixel) {
	winPtr->backgroundType = BG_PIXEL;
	winPtr->backgroundPixel = attrPtr->background_pixel;
    }
    if (valueMask & CWBorderPixel) {
	winPtr->borderPixel = attrPtr->border_pixel;
    }
    if (valueMask & CWBitGravity) {
	winPtr->bitGravity = attrPtr->bit_gravity;
    }
    if (valueMask & CWOverrideRedirect) {
	winPtr->overrideRedirect = attrPtr->override_redirect;
    }
    if (valueMask & CWEventMask) {
	winPtr->eventMask = attrPtr->event_mask;
    }
    if (valueMask & CWColormap) {
	winPtr->colormap = attrPtr->colormap;
    }
    if (valueMask & CWCursor) {
	winPtr->cursor = attrPtr->cursor;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * XCreateWindow --
 *
 *	Create a window, on top of its siblings. The children of the root
 *	window get a surface.
 *
 * Results:
 *	The id of the window, or None if the parent does not exist.
 *
 * Side effects:
 *	A CreateNotify event is queued if the parent asks for it.
 *
 *----------------------------------------------------------------------
 */

Window
XCreateWindow(
    Display *display,
    Window parent,
    int x, int y,
    unsigned int width, unsigned int height,
    unsigned int borderWidth,
    TCL_UNUSED(int),		/* depth */
    unsigned int windowClass,
    TCL_UNUSED(Visual *),
    unsigned long valueMask,
    XSetWindowAttributes *attrPtr)
{
    TkXeWindow *parentPtr = TkXeGetWindow(parent);
    TkXeWindow *winPtr;

    TkXeNextRequest(display);
    if (parentPtr == NULL) {
	return None;
    }
    winPtr = NewWindow(display);
    winPtr->x = x;
    winPtr->y = y;
    winPtr->width = (int)width;
    winPtr->height = (int)height;
    winPtr->borderWidth = (int)borderWidth;
    winPtr->inputOnly = (windowClass == InputOnly);
    if (attrPtr) {
	ChangeAttributes(winPtr, valueMask, attrPtr);
    }
    LinkWindow(winPtr, parentPtr, parentPtr->lastChild);
    if (parentPtr->parent == NULL) {
	TkXeNewSurface(winPtr);
    }

    if (parentPtr->eventMask & SubstructureNotifyMask) {
	XEvent event;

	memset(&event, 0, sizeof(XEvent));
	event.xcreatewindow.type = CreateNotify;
	event.xcreatewindow.display = display;
	event.xcreatewindow.parent = parent;
	event.xcreatewindow.window = winPtr->id;
	event.xcreatewindow.x = x;
	event.xcreatewindow.y = y;
	event.xcreatewindow.width = (int)width;
	event.xcreatewindow.height = (int)height;
	event.xcreatewindow.border_width = (int)borderWidth;
	event.xcreatewindow.override_redirect = winPtr->overrideRedirect;
	TkXeQueueEvent(&event);
    }
    return winPtr->id;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeWindow --
 *
 *	Free a window and its descendants.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The windows are freed with their surfaces and properties, and Tk
 *	forgets them as windows under the pointer.
 *
 *----------------------------------------------------------------------
 */

static void
FreeWindow(
    TkXeWindow *winPtr)
{
    TkWindow *tkwinPtr;

    while (winPtr->lastChild) {
	TkXeWindow *childPtr = winPtr->lastChild;

	UnlinkWindow(childPtr);
	FreeWindow(childPtr);
    }
    tkwinPtr = (TkWindow *)Tk_IdToWindow(winPtr->display, winPtr->id);
    if (tkwinPtr) {
	TkPointerDeadWindow(tkwinPtr);
    }
    if (winPtr->surface) {
	TkXeDestroySurface(winPtr->surface);
    }
    TkXeFreeProperties(winPtr);
    TkXeFreeId(winPtr->id);
    pixman_region32_fini(&winPtr->exposed);
    pixman_region32_fini(&winPtr->clip);
    Tcl_Free(winPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * XDestroyWindow --
 *
 *	Destroy a window and its descendants.
 *
 * Results:
 *	Success, or BadWindow if the window does not exist or is the root
 *	window.
 *
 * Side effects:
 *	The window is unmapped first, and a DestroyNotify event is queued if
 *	its parent asks for it.
 *
 *----------------------------------------------------------------------
 */

int
XDestroyWindow(
    Display *display,
    Window w)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);
    TkXeWindow *parentPtr;

    TkXeNextRequest(display);
    if (winPtr == NULL || winPtr->parent == NULL) {
	return BadWindow;
    }
    parentPtr = winPtr->parent;
    if (winPtr->mapped) {
	XUnmapWindow(display, w);
    }
    if (parentPtr->eventMask & SubstructureNotifyMask) {
	XEvent event;

	memset(&event, 0, sizeof(XEvent));
	event.xdestroywindow.type = DestroyNotify;
	event.xdestroywindow.display = display;
	event.xdestroywindow.event = parentPtr->id;
	event.xdestroywindow.window = w;
	TkXeQueueEvent(&event);
    }
    UnlinkWindow(winPtr);
    FreeWindow(winPtr);
    clipGeneration++;
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XMapWindow --
 *
 *	Map a window. A child of the root window is shown by the window
 *	manager of the port.
 *
 * Results:
 *	Success, or BadWindow if the window does not exist.
 *
 * Side effects:
 *	Expose events are scheduled, and a MapNotify event is queued.
 *
 *----------------------------------------------------------------------
 */

int
XMapWindow(
    Display *display,
    Window w)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);
    XEvent event;

    TkXeNextRequest(display);
    if (winPtr == NULL) {
	return BadWindow;
    }
    if (winPtr->mapped || winPtr->parent == NULL) {
	return Success;
    }
    winPtr->mapped = true;
    if (winPtr->surface && !winPtr->surface->iconic) {
	TkXeResizeBacking(winPtr->surface);
	pixman_region32_clear(&winPtr->exposed);
	winPtr->exposedWidth = -1;
    }
    TkXeTreeChanged(winPtr);
    if (winPtr->surface) {
	TkXeWmMapSurface(winPtr->surface);
    }

    memset(&event, 0, sizeof(XEvent));
    event.xmap.type = MapNotify;
    event.xmap.window = w;
    event.xmap.override_redirect = winPtr->overrideRedirect;
    QueueStructureEvent(winPtr, &event);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XMapRaised --
 *
 *	Raise and map a window.
 *
 * Results:
 *	As XMapWindow.
 *
 * Side effects:
 *	See XRaiseWindow and XMapWindow.
 *
 *----------------------------------------------------------------------
 */

int
XMapRaised(
    Display *display,
    Window w)
{
    XRaiseWindow(display, w);
    return XMapWindow(display, w);
}

/*
 *----------------------------------------------------------------------
 *
 * XUnmapWindow --
 *
 *	Unmap a window. The surface of a child of the root window loses its
 *	role, unless the window is iconic.
 *
 * Results:
 *	Success, or BadWindow if the window does not exist.
 *
 * Side effects:
 *	Expose events are scheduled for what becomes visible, and an
 *	UnmapNotify event is queued.
 *
 *----------------------------------------------------------------------
 */

int
XUnmapWindow(
    Display *display,
    Window w)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);
    XEvent event;

    TkXeNextRequest(display);
    if (winPtr == NULL) {
	return BadWindow;
    }
    if (!winPtr->mapped) {
	return Success;
    }
    TkXeTreeChanged(winPtr);
    winPtr->mapped = false;
    if (winPtr->surface && !winPtr->surface->iconic) {
	TkXeSurfaceUnmapped(winPtr->surface);
    }

    memset(&event, 0, sizeof(XEvent));
    event.xunmap.type = UnmapNotify;
    event.xunmap.window = w;
    event.xunmap.from_configure = False;
    QueueStructureEvent(winPtr, &event);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XWithdrawWindow --
 *
 *	Withdraw a toplevel, which unmaps it.
 *
 * Results:
 *	Non-zero if the window was unmapped.
 *
 * Side effects:
 *	See XUnmapWindow.
 *
 *----------------------------------------------------------------------
 */

Status
XWithdrawWindow(
    Display *display,
    Window w,
    TCL_UNUSED(int))
{
    return XUnmapWindow(display, w) == Success;
}

/*
 *----------------------------------------------------------------------
 *
 * Restack --
 *
 *	Move a window in the stacking order of its parent, relative to a
 *	sibling or to all of them.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The links of the window and its siblings are changed.
 *
 *----------------------------------------------------------------------
 */

static void
Restack(
    TkXeWindow *winPtr,
    TkXeWindow *siblingPtr,
    int stackMode)
{
    TkXeWindow *parentPtr = winPtr->parent;

    if (parentPtr == NULL || (siblingPtr && siblingPtr->parent != parentPtr)) {
	return;
    }
    UnlinkWindow(winPtr);
    switch (stackMode) {
    case Above:
	LinkWindow(winPtr, parentPtr,
		siblingPtr ? siblingPtr : parentPtr->lastChild);
	break;
    case Below:
	LinkWindow(winPtr, parentPtr, siblingPtr ? siblingPtr->below : NULL);
	break;
    default:
	LinkWindow(winPtr, parentPtr, parentPtr->lastChild);
	break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * XConfigureWindow --
 *
 *	Move, resize, restack or change the border width of a window.
 *
 * Results:
 *	Success, or BadWindow if the window does not exist.
 *
 * Side effects:
 *	The window and its surface are changed, Expose events are scheduled,
 *	the window manager of the port is told when a child of the root window
 *	is resized, and ConfigureNotify events are queued.
 *
 *----------------------------------------------------------------------
 */

int
XConfigureWindow(
    Display *display,
    Window w,
    unsigned int valueMask,
    XWindowChanges *values)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);

    TkXeNextRequest(display);
    if (winPtr == NULL) {
	return BadWindow;
    }
    if (winPtr->mapped) {
	TkXeTreeChanged(winPtr);
    }
    if (valueMask & CWX) {
	winPtr->x = values->x;
    }
    if (valueMask & CWY) {
	winPtr->y = values->y;
    }
    if (valueMask & CWWidth) {
	winPtr->width = values->width;
    }
    if (valueMask & CWHeight) {
	winPtr->height = values->height;
    }
    if (valueMask & CWBorderWidth) {
	winPtr->borderWidth = values->border_width;
    }
    if (valueMask & CWStackMode) {
	Restack(winPtr, (valueMask & CWSibling)
		? TkXeGetWindow(values->sibling) : NULL, values->stack_mode);
    }
    if (winPtr->surface && winPtr->mapped) {
	TkXeResizeBacking(winPtr->surface);
	if (valueMask & (CWWidth | CWHeight)) {
	    TkXeWmSurfaceResized(winPtr->surface);
	}
    }
    if (winPtr->mapped) {
	TkXeTreeChanged(winPtr);
    } else {
	clipGeneration++;
    }
    QueueConfigureNotify(winPtr);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XMoveWindow, XResizeWindow, XMoveResizeWindow,
 * XSetWindowBorderWidth --
 *
 *	Change the position, the size or the border width of a window.
 *
 * Results:
 *	As XConfigureWindow.
 *
 * Side effects:
 *	See XConfigureWindow.
 *
 *----------------------------------------------------------------------
 */

int
XMoveWindow(
    Display *display,
    Window w,
    int x, int y)
{
    XWindowChanges values;

    values.x = x;
    values.y = y;
    return XConfigureWindow(display, w, CWX | CWY, &values);
}

int
XResizeWindow(
    Display *display,
    Window w,
    unsigned int width, unsigned int height)
{
    XWindowChanges values;

    values.width = (int)width;
    values.height = (int)height;
    return XConfigureWindow(display, w, CWWidth | CWHeight, &values);
}

int
XMoveResizeWindow(
    Display *display,
    Window w,
    int x, int y,
    unsigned int width, unsigned int height)
{
    XWindowChanges values;

    values.x = x;
    values.y = y;
    values.width = (int)width;
    values.height = (int)height;
    return XConfigureWindow(display, w, CWX | CWY | CWWidth | CWHeight,
	    &values);
}

int
XSetWindowBorderWidth(
    Display *display,
    Window w,
    unsigned int width)
{
    XWindowChanges values;

    values.border_width = (int)width;
    return XConfigureWindow(display, w, CWBorderWidth, &values);
}

/*
 *----------------------------------------------------------------------
 *
 * XRaiseWindow --
 *
 *	Raise a window. Toplevels are also activated by the window manager of
 *	the port (a Wayland client cannot raise its toplevels; it can ask the
 *	compositor to activate them).
 *
 * Results:
 *	As XConfigureWindow.
 *
 * Side effects:
 *	Mapped toplevels are activated; see XConfigureWindow.
 *
 *----------------------------------------------------------------------
 */

int
XRaiseWindow(
    Display *display,
    Window w)
{
    XWindowChanges values;
    TkXeWindow *winPtr = TkXeGetWindow(w);

    if (winPtr && winPtr->surface && winPtr->mapped
	    && !winPtr->overrideRedirect) {
	TkXeWmActivate(winPtr->surface);
    }
    values.stack_mode = Above;
    return XConfigureWindow(display, w, CWStackMode, &values);
}

/*
 *----------------------------------------------------------------------
 *
 * XLowerWindow --
 *
 *	Lower a window below its siblings.
 *
 * Results:
 *	As XConfigureWindow.
 *
 * Side effects:
 *	See XConfigureWindow.
 *
 *----------------------------------------------------------------------
 */

int
XLowerWindow(
    Display *display,
    Window w)
{
    XWindowChanges values;

    values.stack_mode = Below;
    return XConfigureWindow(display, w, CWStackMode, &values);
}

/*
 *----------------------------------------------------------------------
 *
 * XReparentWindow --
 *
 *	Give a window another parent. A window which becomes a child of the
 *	root window gets a surface, and one which stops being one loses it.
 *
 * Results:
 *	Success, or BadWindow if a window does not exist.
 *
 * Side effects:
 *	The window is unmapped during the change and mapped again, and
 *	ReparentNotify events are queued.
 *
 *----------------------------------------------------------------------
 */

int
XReparentWindow(
    Display *display,
    Window w,
    Window parent,
    int x, int y)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);
    TkXeWindow *parentPtr = TkXeGetWindow(parent);
    bool wasMapped;
    XEvent event;

    TkXeNextRequest(display);
    if (winPtr == NULL || parentPtr == NULL || winPtr->parent == NULL) {
	return BadWindow;
    }
    wasMapped = winPtr->mapped;
    if (wasMapped) {
	XUnmapWindow(display, w);
    }
    if (winPtr->surface && parentPtr->parent) {
	TkXeDestroySurface(winPtr->surface);
    }
    UnlinkWindow(winPtr);
    winPtr->x = x;
    winPtr->y = y;
    LinkWindow(winPtr, parentPtr, parentPtr->lastChild);
    if (parentPtr->parent == NULL && winPtr->surface == NULL) {
	TkXeNewSurface(winPtr);
    }
    clipGeneration++;

    memset(&event, 0, sizeof(XEvent));
    event.xreparent.type = ReparentNotify;
    event.xreparent.display = display;
    event.xreparent.window = w;
    event.xreparent.parent = parent;
    event.xreparent.x = x;
    event.xreparent.y = y;
    event.xreparent.override_redirect = winPtr->overrideRedirect;
    if (winPtr->eventMask & StructureNotifyMask) {
	event.xreparent.event = w;
	TkXeQueueEvent(&event);
    }
    if (parentPtr->eventMask & SubstructureNotifyMask) {
	event.xreparent.event = parent;
	TkXeQueueEvent(&event);
    }

    if (wasMapped) {
	XMapWindow(display, w);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XChangeWindowAttributes --
 *
 *	Change the attributes of a window.
 *
 * Results:
 *	Success, or BadWindow if the window does not exist.
 *
 * Side effects:
 *	The window structure is changed, and the cursor updated when it is one
 *	of the changes.
 *
 *----------------------------------------------------------------------
 */

int
XChangeWindowAttributes(
    Display *display,
    Window w,
    unsigned long valueMask,
    XSetWindowAttributes *attrPtr)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);

    TkXeNextRequest(display);
    if (winPtr == NULL) {
	return BadWindow;
    }
    ChangeAttributes(winPtr, valueMask, attrPtr);
    if (valueMask & CWCursor) {
	TkXeUpdateCursor();
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XSelectInput, XSetWindowBackground,
 * XSetWindowBackgroundPixmap, XSetWindowBorder --
 *
 *	Change one attribute of a window.
 *
 * Results:
 *	As XChangeWindowAttributes.
 *
 * Side effects:
 *	See XChangeWindowAttributes.
 *
 *----------------------------------------------------------------------
 */

int
XSelectInput(
    Display *display,
    Window w,
    long eventMask)
{
    XSetWindowAttributes attr;

    attr.event_mask = eventMask;
    return XChangeWindowAttributes(display, w, CWEventMask, &attr);
}

int
XSetWindowBackground(
    Display *display,
    Window w,
    unsigned long pixel)
{
    XSetWindowAttributes attr;

    attr.background_pixel = pixel;
    return XChangeWindowAttributes(display, w, CWBackPixel, &attr);
}

int
XSetWindowBackgroundPixmap(
    Display *display,
    Window w,
    Pixmap pixmap)
{
    XSetWindowAttributes attr;

    attr.background_pixmap = pixmap;
    return XChangeWindowAttributes(display, w, CWBackPixmap, &attr);
}

int
XSetWindowBorder(
    Display *display,
    Window w,
    unsigned long pixel)
{
    XSetWindowAttributes attr;

    attr.border_pixel = pixel;
    return XChangeWindowAttributes(display, w, CWBorderPixel, &attr);
}

/*
 *----------------------------------------------------------------------
 *
 * XSetWindowBorderPixmap --
 *
 *	Borders of pixmaps are not drawn by the emulation: Tk does not use
 *	them.
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
XSetWindowBorderPixmap(
    Display *display,
    TCL_UNUSED(Window),
    TCL_UNUSED(Pixmap))
{
    TkXeNextRequest(display);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XSetWindowColormap, XUndefineCursor --
 *
 *	Change one attribute of a window.
 *
 * Results:
 *	As XChangeWindowAttributes.
 *
 * Side effects:
 *	See XChangeWindowAttributes.
 *
 *----------------------------------------------------------------------
 */

int
XSetWindowColormap(
    Display *display,
    Window w,
    Colormap colormap)
{
    XSetWindowAttributes attr;

    attr.colormap = colormap;
    return XChangeWindowAttributes(display, w, CWColormap, &attr);
}

int
XUndefineCursor(
    Display *display,
    Window w)
{
    XSetWindowAttributes attr;

    attr.cursor = None;
    return XChangeWindowAttributes(display, w, CWCursor, &attr);
}

/*
 *----------------------------------------------------------------------
 *
 * XClearWindow --
 *
 *	Paint the visible part of a window with its background.
 *
 * Results:
 *	Success, or BadWindow if the window does not exist.
 *
 * Side effects:
 *	The pending Expose events of its surface are generated first, then the
 *	window is painted.
 *
 *----------------------------------------------------------------------
 */

int
XClearWindow(
    Display *display,
    Window w)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);
    pixman_region32_t rgn;

    TkXeNextRequest(display);
    if (winPtr == NULL) {
	return BadWindow;
    }
    if (winPtr->parent && winPtr->parent->parent == NULL) {
	TkXeProcessExposures(winPtr->surface);
    } else if (TkXeTopWindow(winPtr) && TkXeTopWindow(winPtr)->surface) {
	TkXeProcessExposures(TkXeTopWindow(winPtr)->surface);
    }
    pixman_region32_init(&rgn);
    TkXeVisibleRegion(winPtr, false, &rgn);
    TkXePaintBackground(winPtr, &rgn);
    pixman_region32_fini(&rgn);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XGetWindowAttributes --
 *
 *	Get the attributes of a window.
 *
 * Results:
 *	Non-zero, or zero if the window does not exist.
 *
 * Side effects:
 *	The attributes are stored in *attrPtr.
 *
 *----------------------------------------------------------------------
 */

int
XGetWindowAttributes(
    Display *display,
    Window w,
    XWindowAttributes *attrPtr)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);

    TkXeNextRequest(display);
    memset(attrPtr, 0, sizeof(XWindowAttributes));
    if (winPtr == NULL) {
	return 0;
    }
    attrPtr->x = winPtr->x;
    attrPtr->y = winPtr->y;
    attrPtr->width = winPtr->width;
    attrPtr->height = winPtr->height;
    attrPtr->border_width = winPtr->borderWidth;
    attrPtr->depth = 24;
    attrPtr->visual = DefaultVisual(display, 0);
    attrPtr->root = RootWindow(display, 0);
    attrPtr->c_class = winPtr->inputOnly ? InputOnly : InputOutput;
    attrPtr->bit_gravity = winPtr->bitGravity;
    attrPtr->win_gravity = NorthWestGravity;
    attrPtr->backing_store = NotUseful;
    attrPtr->colormap = DefaultColormap(display, 0);
    attrPtr->map_installed = True;
    attrPtr->map_state = !winPtr->mapped ? IsUnmapped
	    : TkXeIsViewable(winPtr) ? IsViewable : IsUnviewable;
    attrPtr->all_event_masks = winPtr->eventMask;
    attrPtr->your_event_mask = winPtr->eventMask;
    attrPtr->override_redirect = winPtr->overrideRedirect;
    attrPtr->screen = DefaultScreenOfDisplay(display);
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * XGetGeometry --
 *
 *	Get the position, size, border width and depth of a window or a
 *	pixmap.
 *
 * Results:
 *	Non-zero, or zero if the drawable does not exist.
 *
 * Side effects:
 *	The values are stored in the given variables.
 *
 *----------------------------------------------------------------------
 */

Status
XGetGeometry(
    Display *display,
    Drawable d,
    Window *rootPtr,
    int *xPtr, int *yPtr,
    unsigned int *widthPtr, unsigned int *heightPtr,
    unsigned int *borderWidthPtr,
    unsigned int *depthPtr)
{
    void *p = TkXeLookupId(d);

    TkXeNextRequest(display);
    *rootPtr = RootWindow(display, 0);
    if (p == NULL) {
	return 0;
    }
    if (*(int *)p == TKXE_WINDOW) {
	TkXeWindow *winPtr = (TkXeWindow *)p;

	*xPtr = winPtr->x;
	*yPtr = winPtr->y;
	*widthPtr = (unsigned)winPtr->width;
	*heightPtr = (unsigned)winPtr->height;
	*borderWidthPtr = (unsigned)winPtr->borderWidth;
	*depthPtr = 24;
    } else {
	TkXePixmap *pixPtr = (TkXePixmap *)p;

	*xPtr = *yPtr = 0;
	*widthPtr = (unsigned)pixPtr->width;
	*heightPtr = (unsigned)pixPtr->height;
	*borderWidthPtr = 0;
	*depthPtr = (unsigned)pixPtr->depth;
    }
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * XQueryTree --
 *
 *	Get the parent and the children of a window, from bottom to top.
 *
 * Results:
 *	Non-zero, or zero if the window does not exist.
 *
 * Side effects:
 *	*childrenPtr is set to an array of ids allocated with Tcl_Alloc, which
 *	the caller frees (with XFree), or NULL if there are no children.
 *
 *----------------------------------------------------------------------
 */

Status
XQueryTree(
    Display *display,
    Window w,
    Window *rootPtr,
    Window *parentPtr,
    Window **childrenPtr,
    unsigned int *countPtr)
{
    TkXeWindow *winPtr = TkXeGetWindow(w), *childPtr;
    unsigned int n = 0;

    TkXeNextRequest(display);
    *rootPtr = RootWindow(display, 0);
    *parentPtr = None;
    *childrenPtr = NULL;
    *countPtr = 0;
    if (winPtr == NULL) {
	return 0;
    }
    if (winPtr->parent) {
	*parentPtr = winPtr->parent->id;
    }
    for (childPtr = winPtr->firstChild; childPtr; childPtr = childPtr->above) {
	n++;
    }
    if (n > 0) {
	Window *children = (Window *)Tcl_Alloc(n * sizeof(Window));

	n = 0;
	for (childPtr = winPtr->firstChild; childPtr;
		childPtr = childPtr->above) {
	    children[n++] = childPtr->id;
	}
	*childrenPtr = children;
    }
    *countPtr = n;
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * RootPosition --
 *
 *	Compute the position of the inside of a window in root coordinates.
 *	The root coordinates of a toplevel are the position of its wrapper
 *	window. Wayland clients do not know where their windows are on the
 *	screen, and these are the coordinates Tk thinks it has, which keeps
 *	them consistent within the application.
 *
 * Results:
 *	The position is stored in *xPtr and *yPtr.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
RootPosition(
    TkXeWindow *winPtr,
    int *xPtr, int *yPtr)
{
    int x = 0, y = 0;

    for (; winPtr && winPtr->parent; winPtr = winPtr->parent) {
	x += winPtr->x + winPtr->borderWidth;
	y += winPtr->y + winPtr->borderWidth;
    }
    *xPtr = x;
    *yPtr = y;
}

/*
 *----------------------------------------------------------------------
 *
 * XTranslateCoordinates --
 *
 *	Convert a position relative to a window to one relative to another
 *	window, and find the child of that window which contains it.
 *
 * Results:
 *	True, or False if a window does not exist.
 *
 * Side effects:
 *	The position is stored in *dstXPtr and *dstYPtr, and the child in
 *	*childPtr.
 *
 *----------------------------------------------------------------------
 */

Bool
XTranslateCoordinates(
    Display *display,
    Window src,
    Window dst,
    int srcX, int srcY,
    int *dstXPtr, int *dstYPtr,
    Window *childPtr)
{
    TkXeWindow *srcPtr = TkXeGetWindow(src), *dstPtr = TkXeGetWindow(dst);
    int sx, sy, dx, dy;

    TkXeNextRequest(display);
    *childPtr = None;
    if (srcPtr == NULL || dstPtr == NULL) {
	*dstXPtr = srcX;
	*dstYPtr = srcY;
	return False;
    }
    RootPosition(srcPtr, &sx, &sy);
    RootPosition(dstPtr, &dx, &dy);
    *dstXPtr = srcX + sx - dx;
    *dstYPtr = srcY + sy - dy;
    if (*dstXPtr >= 0 && *dstYPtr >= 0 && *dstXPtr < dstPtr->width
	    && *dstYPtr < dstPtr->height) {
	TkXeWindow *childWinPtr;

	for (childWinPtr = dstPtr->lastChild; childWinPtr;
		childWinPtr = childWinPtr->below) {
	    if (childWinPtr->mapped && *dstXPtr >= childWinPtr->x
		    && *dstYPtr >= childWinPtr->y
		    && *dstXPtr < childWinPtr->x + childWinPtr->width
			+ 2 * childWinPtr->borderWidth
		    && *dstYPtr < childWinPtr->y + childWinPtr->height
			+ 2 * childWinPtr->borderWidth) {
		*childPtr = childWinPtr->id;
		break;
	    }
	}
    }
    return True;
}

/*
 *----------------------------------------------------------------------
 *
 * XFlush, XSync, TkpSync --
 *
 *	Send the pending requests to the compositor. With XSync and TkpSync,
 *	all the requests made so far are also considered processed.
 *
 * Results:
 *	Success, or nothing for TkpSync.
 *
 * Side effects:
 *	The connection is flushed.
 *
 *----------------------------------------------------------------------
 */

int
XFlush(
    TCL_UNUSED(Display *))
{
    TkXeFlush();
    return Success;
}

int
XSync(
    Display *display,
    Bool discard)
{
    (void)discard;
    TkXeNextRequest(display);
    LastKnownRequestProcessed(display) = NextRequest(display) - 1;
    TkXeFlush();
    return Success;
}

void
TkpSync(
    Display *display)
{
    XSync(display, False);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
