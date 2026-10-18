/*
 * tkWaylandWindow.c --
 *
 *	The window part of the Xlib emulation of the Wayland port: the
 *	window tree, the visible regions of windows, Expose events, and the
 *	surfaces of the children of the root window, which present the
 *	images Tk draws into.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#define _GNU_SOURCE		/* For memfd_create. */
#include "tkWaylandInt.h"
#include <sys/mman.h>

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

static void		FrameDone(void *data, struct wl_callback *callback,
			    uint32_t time);
static void		PresentIdle(void *clientData);
static void		Present(TkWlSurface *surfPtr);
static void		QueueStructureEvent(TkWlWindow *winPtr,
			    XEvent *eventPtr);
static void		SchedulePresent(TkWlSurface *surfPtr);
static void		UnlinkWindow(TkWlWindow *winPtr);
static void		LinkWindow(TkWlWindow *winPtr, TkWlWindow *parentPtr,
			    TkWlWindow *belowPtr);

static const struct wl_callback_listener frameListener = {
    .done = FrameDone
};

/*
 *----------------------------------------------------------------------
 *
 * TkWlNewId, TkWlFreeId, TkWlLookupId --
 *
 *	Ids of windows and pixmaps are never reused, so that the requests
 *	and events for destroyed windows are ignored.
 *
 *----------------------------------------------------------------------
 */

XID
TkWlNewId(
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

void
TkWlFreeId(
    XID id)
{
    Tcl_HashEntry *hPtr;

    if (idTableInitialized
	    && (hPtr = Tcl_FindHashEntry(&idTable, INT2PTR(id))) != NULL) {
	Tcl_DeleteHashEntry(hPtr);
    }
}

void *
TkWlLookupId(
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
 *----------------------------------------------------------------------
 */

static TkWlWindow *
NewWindow(
    Display *display)
{
    TkWlWindow *winPtr = (TkWlWindow *)Tcl_Alloc(sizeof(TkWlWindow));

    memset(winPtr, 0, sizeof(TkWlWindow));
    winPtr->type = TKWL_WINDOW;
    winPtr->id = TkWlNewId(winPtr);
    winPtr->display = display;
    winPtr->bitGravity = ForgetGravity;
    winPtr->backgroundType = BG_NONE;
    winPtr->visibility = -1;
    pixman_region32_init(&winPtr->exposed);
    pixman_region32_init(&winPtr->clip);
    return winPtr;
}

TkWlWindow *
TkWlCreateRootWindow(
    Display *display,
    int width, int height)
{
    TkWlWindow *rootPtr = NewWindow(display);

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
 *	sibling (at the bottom if it is NULL), or remove it.
 *
 *----------------------------------------------------------------------
 */

static void
LinkWindow(
    TkWlWindow *winPtr,
    TkWlWindow *parentPtr,
    TkWlWindow *belowPtr)
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
    TkWlWindow *winPtr)
{
    TkWlWindow *parentPtr = winPtr->parent;

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
 * TkWlTopWindow --
 *
 *	Return the child of the root window which contains a window, or NULL
 *	for the root window.
 *
 *----------------------------------------------------------------------
 */

TkWlWindow *
TkWlTopWindow(
    TkWlWindow *winPtr)
{
    if (winPtr->parent == NULL) {
	return NULL;
    }
    while (winPtr->parent->parent) {
	winPtr = winPtr->parent;
    }
    return winPtr;
}

TkWlSurface *
TkWlGetSurface(
    Window w)
{
    TkWlWindow *winPtr = TkWlGetWindow(w);

    if (winPtr == NULL || (winPtr = TkWlTopWindow(winPtr)) == NULL) {
	return NULL;
    }
    return winPtr->surface;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlIsViewable --
 *
 *	Whether a window and all of its ancestors are mapped.
 *
 *----------------------------------------------------------------------
 */

bool
TkWlIsViewable(
    TkWlWindow *winPtr)
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
 * TkWlWindowOrigin --
 *
 *	Return the position of the inside of a window in the surface of its
 *	top window. The outer corner of the top window is at 0,0.
 *
 *----------------------------------------------------------------------
 */

void
TkWlWindowOrigin(
    TkWlWindow *winPtr,
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
 * TkWlVisibleRegion --
 *
 *	Compute the part of a window which can be drawn into, in the
 *	coordinates of its surface: the inside of the window, clipped by its
 *	ancestors, minus the mapped siblings above it and above its
 *	ancestors, and minus its mapped children unless includeInferiors is
 *	set.
 *
 * Results:
 *	The region is stored in rgn, which must be initialized.
 *
 *----------------------------------------------------------------------
 */

static void
SubtractOuter(
    pixman_region32_t *rgn,
    TkWlWindow *winPtr,
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

void
TkWlVisibleRegion(
    TkWlWindow *winPtr,
    bool includeInferiors,
    pixman_region32_t *rgn)
{
    TkWlWindow *ancestorPtr, *childPtr;
    int x, y;

    if (winPtr->clipGeneration == clipGeneration
	    && winPtr->clipInferiors == includeInferiors) {
	pixman_region32_copy(rgn, &winPtr->clip);
	return;
    }

    if (winPtr->parent == NULL || !TkWlIsViewable(winPtr)
	    || winPtr->inputOnly) {
	pixman_region32_clear(rgn);
	goto done;
    }

    TkWlWindowOrigin(winPtr, &x, &y);
    pixman_region32_fini(rgn);
    pixman_region32_init_rect(rgn, x, y, (unsigned)winPtr->width,
	    (unsigned)winPtr->height);

    /*
     * Clip by the ancestors, and remove the siblings above the window and
     * above each of its ancestors.
     */

    for (ancestorPtr = winPtr; ancestorPtr->parent->parent;
	    ancestorPtr = ancestorPtr->parent) {
	TkWlWindow *parentPtr = ancestorPtr->parent;
	TkWlWindow *siblingPtr;
	int px, py;

	TkWlWindowOrigin(parentPtr, &px, &py);
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
 * TkWlTreeChanged --
 *
 *	Called when the window tree changes in a way which can change what is
 *	visible: invalidates the visible regions, and schedules Expose events
 *	for the surface of the window.
 *
 *----------------------------------------------------------------------
 */

static void
ExposeIdle(
    void *clientData)
{
    TkWlSurface *surfPtr;

    (void)clientData;
    for (surfPtr = tkWlDisplay ? tkWlDisplay->surfaces : NULL; surfPtr;
	    surfPtr = surfPtr->nextPtr) {
	TkWlProcessExposures(surfPtr);
    }
}

void
TkWlTreeChanged(
    TkWlWindow *winPtr)
{
    TkWlWindow *topPtr = TkWlTopWindow(winPtr);
    bool scheduled = false;
    TkWlSurface *surfPtr;

    clipGeneration++;
    if (topPtr == NULL || topPtr->surface == NULL) {
	return;
    }
    for (surfPtr = tkWlDisplay->surfaces; surfPtr; surfPtr = surfPtr->nextPtr) {
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
 * TkWlProcessExposures --
 *
 *	Generate Expose events for the parts of the windows of a surface
 *	which became visible since the last call, after painting them with
 *	the background of their window, as the X server does. This is called
 *	before anything is drawn in the surface, so that the order of drawing
 *	and of painting backgrounds is the same as with X.
 *
 *----------------------------------------------------------------------
 */

static void
ExposeWindow(
    TkWlWindow *winPtr)
{
    pixman_region32_t visible, newly;
    TkWlWindow *childPtr;
    int x, y;

    pixman_region32_init(&visible);
    TkWlVisibleRegion(winPtr, false, &visible);
    TkWlWindowOrigin(winPtr, &x, &y);
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
	TkWlPaintBackground(winPtr, &newly);
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
		TkWlQueueEvent(&event);
	    }
	}
    }
    /*
     * Send VisibilityNotify when the visibility of a viewable window
     * changes.
     */

    if (TkWlIsViewable(winPtr) && !winPtr->inputOnly) {
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
	    TkWlQueueEvent(&event);
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

void
TkWlProcessExposures(
    TkWlSurface *surfPtr)
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
 * TkWlQueueEvent --
 *
 *	Queue an event generated by the Xlib emulation, as if it came from the
 *	X server.
 *
 *----------------------------------------------------------------------
 */

void
TkWlQueueEvent(
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
 * Send a StructureNotify event to the window and a SubstructureNotify event
 * to its parent, as the X server does. The event and window fields of all
 * these events are at the same place.
 */

static void
QueueStructureEvent(
    TkWlWindow *winPtr,
    XEvent *eventPtr)
{
    eventPtr->xany.display = winPtr->display;
    if (winPtr->eventMask & StructureNotifyMask) {
	eventPtr->xmap.event = winPtr->id;
	TkWlQueueEvent(eventPtr);
    }
    if (winPtr->parent && (winPtr->parent->eventMask & SubstructureNotifyMask)) {
	eventPtr->xmap.event = winPtr->parent->id;
	TkWlQueueEvent(eventPtr);
    }
}

static void
QueueConfigureNotify(
    TkWlWindow *winPtr)
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
 * Surfaces --
 *
 *	The children of the root window have a wl_surface and an image which
 *	Tk draws into. The roles of the surfaces (decorated toplevels or
 *	popups) are managed by tkWaylandWm.c.
 *
 *----------------------------------------------------------------------
 */

/*
 * The scale of a surface is the preferred fractional scale the compositor
 * gives, or its preferred integer scale, or the largest scale of the
 * outputs the surface is on, in this order. Fractional scales need the
 * viewporter.
 */

static void
SurfaceEnter(
    void *data,
    TCL_UNUSED(struct wl_surface *),
    struct wl_output *output)
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;
    TkWlOutput *outPtr;

    for (outPtr = tkWlDisplay->outputs; outPtr; outPtr = outPtr->nextPtr) {
	if (outPtr->output == output) {
	    break;
	}
    }
    if (outPtr && surfPtr->numOutputs < 8) {
	surfPtr->outputs[surfPtr->numOutputs++] = outPtr;
	TkWlUpdateScale(surfPtr);
    }
}

static void
SurfaceLeave(
    void *data,
    TCL_UNUSED(struct wl_surface *),
    struct wl_output *output)
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;
    int i;

    for (i = 0; i < surfPtr->numOutputs; i++) {
	if (surfPtr->outputs[i]->output == output) {
	    surfPtr->outputs[i] = surfPtr->outputs[--surfPtr->numOutputs];
	    TkWlUpdateScale(surfPtr);
	    break;
	}
    }
}

#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
static void
SurfacePreferredScale(
    void *data,
    TCL_UNUSED(struct wl_surface *),
    int32_t factor)
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;

    surfPtr->preferredScale = factor;
    TkWlUpdateScale(surfPtr);
}
#endif

static void
FractionalPreferredScale(
    void *data,
    TCL_UNUSED(struct wp_fractional_scale_v1 *),
    uint32_t scale)
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;

    surfPtr->fractionalScale = scale / 120.0;
    TkWlUpdateScale(surfPtr);
}

static const struct wp_fractional_scale_v1_listener fractionalListener = {
    .preferred_scale = FractionalPreferredScale
};

#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
static void
SurfacePreferredTransform(
    void *data,
    TCL_UNUSED(struct wl_surface *),
    TCL_UNUSED(uint32_t))
{
    (void)data;
}
#endif

static const struct wl_surface_listener surfaceListener = {
    .enter = SurfaceEnter,
    .leave = SurfaceLeave,
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
    .preferred_buffer_scale = SurfacePreferredScale,
    .preferred_buffer_transform = SurfacePreferredTransform
#endif
};

/*
 * Create and destroy the wl_surface of a surface, with its viewport.
 */

static void
CreateWlSurface(
    TkWlSurface *surfPtr)
{
    surfPtr->surface = wl_compositor_create_surface(tkWlDisplay->compositor);
    wl_surface_set_user_data(surfPtr->surface, surfPtr);
    wl_surface_add_listener(surfPtr->surface, &surfaceListener, surfPtr);
    if (tkWlDisplay->viewporter) {
	surfPtr->viewport = wp_viewporter_get_viewport(
		tkWlDisplay->viewporter, surfPtr->surface);
	if (tkWlDisplay->fractionalScaleManager) {
	    surfPtr->fractional =
		    wp_fractional_scale_manager_v1_get_fractional_scale(
		    tkWlDisplay->fractionalScaleManager, surfPtr->surface);
	    wp_fractional_scale_v1_add_listener(surfPtr->fractional,
		    &fractionalListener, surfPtr);
	}
    }
    surfPtr->numOutputs = 0;
    surfPtr->roleType = TKWL_ROLE_NONE;
}

static void
DestroyWlSurface(
    TkWlSurface *surfPtr)
{
    if (surfPtr->frameCallback) {
	wl_callback_destroy(surfPtr->frameCallback);
	surfPtr->frameCallback = NULL;
    }
    if (surfPtr->fractional) {
	wp_fractional_scale_v1_destroy(surfPtr->fractional);
	surfPtr->fractional = NULL;
    }
    if (surfPtr->viewport) {
	wp_viewport_destroy(surfPtr->viewport);
	surfPtr->viewport = NULL;
    }
    wl_surface_destroy(surfPtr->surface);
    surfPtr->surface = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlSurfaceSetRole --
 *
 *	Called before a surface is given a role. The role of a wl_surface
 *	cannot change (a toplevel cannot become a popup, even after its
 *	xdg_toplevel was destroyed), so the wl_surface is replaced when the
 *	surface gets a role of another type.
 *
 *----------------------------------------------------------------------
 */

void
TkWlSurfaceSetRole(
    TkWlSurface *surfPtr,
    int roleType)
{
    int i;

    if (surfPtr->roleType != TKWL_ROLE_NONE
	    && surfPtr->roleType != roleType) {
	DestroyWlSurface(surfPtr);
	CreateWlSurface(surfPtr);
	surfPtr->committed = false;
	surfPtr->configured = false;
	surfPtr->presented = false;
	for (i = 0; i < TKWL_NUM_BUFFERS; i++) {
	    surfPtr->buffers[i].busy = false;
	    pixman_region32_union_rect(&surfPtr->buffers[i].damage,
		    &surfPtr->buffers[i].damage, 0, 0,
		    (unsigned)surfPtr->width, (unsigned)surfPtr->height);
	}
    }
    surfPtr->roleType = roleType;
}

static TkWlSurface *
NewSurface(
    TkWlWindow *winPtr)
{
    TkWlSurface *surfPtr = (TkWlSurface *)Tcl_Alloc(sizeof(TkWlSurface));
    int i;

    memset(surfPtr, 0, sizeof(TkWlSurface));
    surfPtr->winPtr = winPtr;
    surfPtr->scale = 1.0;
    surfPtr->opacity = 1.0;
    pixman_region32_init(&surfPtr->damage);
    for (i = 0; i < TKWL_NUM_BUFFERS; i++) {
	pixman_region32_init(&surfPtr->buffers[i].damage);
    }
    CreateWlSurface(surfPtr);
    TkWlUpdateScale(surfPtr);
    surfPtr->nextPtr = tkWlDisplay->surfaces;
    tkWlDisplay->surfaces = surfPtr;
    winPtr->surface = surfPtr;
    return surfPtr;
}

TkWlSurface *
TkWlSurfaceFromWl(
    struct wl_surface *surface)
{
    TkWlSurface *surfPtr;

    for (surfPtr = tkWlDisplay->surfaces; surfPtr; surfPtr = surfPtr->nextPtr) {
	if (surfPtr->surface == surface) {
	    return surfPtr;
	}
    }
    return NULL;
}

static void
FreeBuffer(
    TkWlBuffer *bufPtr)
{
    if (bufPtr->buffer) {
	wl_buffer_destroy(bufPtr->buffer);
	munmap(bufPtr->data, bufPtr->size);
	bufPtr->buffer = NULL;
	bufPtr->data = NULL;
    }
    bufPtr->busy = false;
}

static void
DestroySurface(
    TkWlSurface *surfPtr)
{
    TkWlSurface **prevPtrPtr;
    int i;

    for (prevPtrPtr = &tkWlDisplay->surfaces; *prevPtrPtr;
	    prevPtrPtr = &(*prevPtrPtr)->nextPtr) {
	if (*prevPtrPtr == surfPtr) {
	    *prevPtrPtr = surfPtr->nextPtr;
	    break;
	}
    }
    if (tkWlDisplay->pointerSurface == surfPtr) {
	tkWlDisplay->pointerSurface = NULL;
    }
    if (tkWlDisplay->keyboardSurface == surfPtr) {
	tkWlDisplay->keyboardSurface = NULL;
    }
    if (surfPtr->presentPending) {
	Tcl_CancelIdleCall(PresentIdle, surfPtr);
    }
    TkWlWmUnmapSurface(surfPtr);
    for (i = 0; i < TKWL_NUM_BUFFERS; i++) {
	FreeBuffer(&surfPtr->buffers[i]);
	pixman_region32_fini(&surfPtr->buffers[i].damage);
    }
    DestroyWlSurface(surfPtr);
    if (surfPtr->backing) {
	cairo_surface_destroy(surfPtr->backing);
    }
    pixman_region32_fini(&surfPtr->damage);
    surfPtr->winPtr->surface = NULL;
    Tcl_Free(surfPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * ResizeBacking --
 *
 *	Make the backing image of a surface the size of its window, keeping
 *	what was drawn in it.
 *
 *----------------------------------------------------------------------
 */

/*
 * The size in pixels of a length in logical pixels, as in the fractional
 * scale protocol.
 */

static int
BufferSize(
    int length,
    double scale)
{
    int size = (int)lround(length * scale);

    return (size > 0) ? size : 1;
}

/*
 * Convert a box in surface coordinates to the pixels of the buffer which it
 * touches.
 */

static void
BufferBox(
    TkWlSurface *surfPtr,
    pixman_box32_t *boxPtr,
    int *xPtr, int *yPtr, int *widthPtr, int *heightPtr)
{
    double s = surfPtr->scale;
    int x1 = (int)floor(boxPtr->x1 * s), y1 = (int)floor(boxPtr->y1 * s);
    int x2 = (int)ceil(boxPtr->x2 * s), y2 = (int)ceil(boxPtr->y2 * s);

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > surfPtr->bufferWidth) x2 = surfPtr->bufferWidth;
    if (y2 > surfPtr->bufferHeight) y2 = surfPtr->bufferHeight;
    *xPtr = x1;
    *yPtr = y1;
    *widthPtr = (x2 > x1) ? x2 - x1 : 0;
    *heightPtr = (y2 > y1) ? y2 - y1 : 0;
}

bool
TkWlIntegralScale(
    TkWlSurface *surfPtr)
{
    return surfPtr->scale == floor(surfPtr->scale);
}

static void
ResizeBacking(
    TkWlSurface *surfPtr)
{
    TkWlWindow *winPtr = surfPtr->winPtr;
    int width = winPtr->width + 2 * winPtr->borderWidth;
    int height = winPtr->height + 2 * winPtr->borderWidth;
    cairo_surface_t *backing;

    if (width < 1) {
	width = 1;
    }
    if (height < 1) {
	height = 1;
    }
    if (surfPtr->backing && width == surfPtr->width
	    && height == surfPtr->height) {
	return;
    }
    surfPtr->bufferWidth = BufferSize(width, surfPtr->scale);
    surfPtr->bufferHeight = BufferSize(height, surfPtr->scale);
    backing = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
	    surfPtr->bufferWidth, surfPtr->bufferHeight);
    cairo_surface_set_device_scale(backing, surfPtr->scale, surfPtr->scale);
    if (surfPtr->backing) {
	cairo_t *cr = cairo_create(backing);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(cr, surfPtr->backing, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(surfPtr->backing);
    }
    surfPtr->backing = backing;
    surfPtr->width = width;
    surfPtr->height = height;
    pixman_region32_intersect_rect(&surfPtr->damage, &surfPtr->damage, 0, 0,
	    (unsigned)width, (unsigned)height);
}

/*
 * Forget what was drawn in a window and its descendants, so that they get
 * Expose events for all of their visible parts.
 */

static void
ForgetExposed(
    TkWlWindow *winPtr)
{
    TkWlWindow *childPtr;

    pixman_region32_clear(&winPtr->exposed);
    winPtr->exposedWidth = -1;
    for (childPtr = winPtr->firstChild; childPtr; childPtr = childPtr->above) {
	ForgetExposed(childPtr);
    }
}

void
TkWlUpdateScale(
    TkWlSurface *surfPtr)
{
    TkWlWindow *winPtr = surfPtr->winPtr;
    double scale = 1.0;
    int i;

    if (tkWlDisplay->forcedScale > 0.0) {
	scale = tkWlDisplay->forcedScale;
    } else if (surfPtr->fractionalScale > 0.0) {
	scale = surfPtr->fractionalScale;
    } else if (surfPtr->preferredScale > 0) {
	scale = surfPtr->preferredScale;
    } else {
	for (i = 0; i < surfPtr->numOutputs; i++) {
	    if (surfPtr->outputs[i]->scale > scale) {
		scale = surfPtr->outputs[i]->scale;
	    }
	}
    }
    if (surfPtr->viewport == NULL) {
	scale = ceil(scale);
    }
    if (scale == surfPtr->scale) {
	return;
    }

    /*
     * Everything has to be drawn again at the new scale.
     */

    surfPtr->scale = scale;
    if (surfPtr->backing) {
	cairo_surface_destroy(surfPtr->backing);
	surfPtr->backing = NULL;
	ResizeBacking(surfPtr);
	ForgetExposed(winPtr);
	TkWlTreeChanged(winPtr);
	TkWlProcessExposures(surfPtr);
    }
    if (tkWlDisplay->pointerSurface == surfPtr) {
	TkWlUpdateCursor();
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlAddDamage --
 *
 *	Record that a region of a surface was drawn into, and schedule its
 *	presentation.
 *
 *----------------------------------------------------------------------
 */

void
TkWlAddDamage(
    TkWlSurface *surfPtr,
    pixman_region32_t *rgn)
{
    int i;

    pixman_region32_union(&surfPtr->damage, &surfPtr->damage, rgn);
    for (i = 0; i < TKWL_NUM_BUFFERS; i++) {
	pixman_region32_union(&surfPtr->buffers[i].damage,
		&surfPtr->buffers[i].damage, rgn);
    }
    SchedulePresent(surfPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Presentation --
 *
 *	The damaged parts of the backing image are copied to a free wl_shm
 *	buffer, which is committed. Presenting is done at idle time, and
 *	deferred once more so that the redisplay of the widgets which handle
 *	the Expose events generated with the damage happen first. It is
 *	throttled with frame callbacks.
 *
 *----------------------------------------------------------------------
 */

static void
SchedulePresent(
    TkWlSurface *surfPtr)
{
    if (!surfPtr->presentPending) {
	surfPtr->presentPending = true;
	Tcl_DoWhenIdle(PresentIdle, surfPtr);
    }
}

static void
PresentIdle(
    void *clientData)
{
    TkWlSurface *surfPtr = (TkWlSurface *)clientData;

    if (!surfPtr->presentDeferred) {
	surfPtr->presentDeferred = true;
	Tcl_DoWhenIdle(PresentIdle, surfPtr);
	return;
    }
    surfPtr->presentDeferred = false;
    surfPtr->presentPending = false;
    Present(surfPtr);
}

static void
FrameDone(
    void *data,
    struct wl_callback *callback,
    TCL_UNUSED(uint32_t))
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;

    wl_callback_destroy(callback);
    surfPtr->frameCallback = NULL;
    surfPtr->presented = surfPtr->committed;
    if (pixman_region32_not_empty(&surfPtr->damage)) {
	SchedulePresent(surfPtr);
    }
}

static void
BufferRelease(
    void *data,
    TCL_UNUSED(struct wl_buffer *))
{
    ((TkWlBuffer *)data)->busy = false;
}

static const struct wl_buffer_listener bufferListener = {
    .release = BufferRelease
};

static bool
AllocBuffer(
    TkWlBuffer *bufPtr,
    int width, int height,
    uint32_t format)
{
    struct wl_shm_pool *pool;
    int fd, stride = width * 4;
    size_t size = (size_t)stride * height;
    void *data;

    fd = memfd_create("tk-wayland", MFD_CLOEXEC);
    if (fd < 0) {
	return false;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
	close(fd);
	return false;
    }
    data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
	close(fd);
	return false;
    }
    pool = wl_shm_create_pool(tkWlDisplay->shm, fd, (int32_t)size);
    bufPtr->buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
	    stride, format);
    bufPtr->format = format;
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(bufPtr->buffer, &bufferListener, bufPtr);
    bufPtr->data = data;
    bufPtr->size = size;
    bufPtr->width = width;
    bufPtr->height = height;
    bufPtr->stride = stride;
    bufPtr->busy = false;
    return true;
}

static void
Present(
    TkWlSurface *surfPtr)
{
    TkWlBuffer *bufPtr = NULL;
    int i, n, width, height;
    pixman_box32_t *boxes;
    pixman_image_t *src, *dst, *mask = NULL;
    uint32_t format = (surfPtr->opacity < 1.0) ? WL_SHM_FORMAT_ARGB8888
	    : WL_SHM_FORMAT_XRGB8888;

    TkWlProcessExposures(surfPtr);
    if (!surfPtr->configured || surfPtr->backing == NULL
	    || !surfPtr->winPtr->mapped) {
	return;
    }
    if (surfPtr->frameCallback) {
	/*
	 * FrameDone will present again.
	 */

	return;
    }
    if (!pixman_region32_not_empty(&surfPtr->damage) && surfPtr->committed) {
	return;
    }

    width = surfPtr->bufferWidth;
    height = surfPtr->bufferHeight;
    for (i = 0; i < TKWL_NUM_BUFFERS; i++) {
	if (!surfPtr->buffers[i].busy) {
	    bufPtr = &surfPtr->buffers[i];
	    break;
	}
    }
    if (bufPtr == NULL) {
	/*
	 * All buffers are used by the compositor. Try again later.
	 */

	SchedulePresent(surfPtr);
	return;
    }
    if (bufPtr->buffer == NULL || bufPtr->width != width
	    || bufPtr->height != height || bufPtr->format != format) {
	FreeBuffer(bufPtr);
	if (!AllocBuffer(bufPtr, width, height, format)) {
	    return;
	}
	pixman_region32_fini(&bufPtr->damage);
	pixman_region32_init_rect(&bufPtr->damage, 0, 0,
		(unsigned)surfPtr->width, (unsigned)surfPtr->height);
    }

    /*
     * Bring the buffer up to date with the backing image.
     */

    cairo_surface_flush(surfPtr->backing);
    src = pixman_image_create_bits(PIXMAN_x8r8g8b8, width, height,
	    (uint32_t *)cairo_image_surface_get_data(surfPtr->backing),
	    cairo_image_surface_get_stride(surfPtr->backing));
    dst = pixman_image_create_bits((format == WL_SHM_FORMAT_ARGB8888)
	    ? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8, width, height,
	    (uint32_t *)bufPtr->data, bufPtr->stride);
    if (format == WL_SHM_FORMAT_ARGB8888) {
	/*
	 * A translucent window: the pixels are multiplied by its opacity.
	 */

	pixman_color_t alpha = {0, 0, 0,
		(uint16_t)(surfPtr->opacity * 0xFFFF)};

	mask = pixman_image_create_solid_fill(&alpha);
    }
    boxes = pixman_region32_rectangles(&bufPtr->damage, &n);
    for (i = 0; i < n; i++) {
	int x, y, w, h;

	BufferBox(surfPtr, &boxes[i], &x, &y, &w, &h);
	pixman_image_composite32(PIXMAN_OP_SRC, src, mask, dst, x, y, 0, 0,
		x, y, w, h);
    }
    pixman_image_unref(src);
    pixman_image_unref(dst);
    if (mask) {
	pixman_image_unref(mask);
    }
    pixman_region32_clear(&bufPtr->damage);

    wl_surface_attach(surfPtr->surface, bufPtr->buffer, 0, 0);
    if (surfPtr->viewport) {
	/*
	 * The viewport scales the buffer to the size of the surface.
	 */

	wl_surface_set_buffer_scale(surfPtr->surface, 1);
	wp_viewport_set_destination(surfPtr->viewport, surfPtr->width,
		surfPtr->height);
    } else {
	wl_surface_set_buffer_scale(surfPtr->surface, (int)surfPtr->scale);
    }
    if (!surfPtr->committed) {
	wl_surface_damage_buffer(surfPtr->surface, 0, 0, width, height);
    } else {
	boxes = pixman_region32_rectangles(&surfPtr->damage, &n);
	for (i = 0; i < n; i++) {
	    int x, y, w, h;

	    BufferBox(surfPtr, &boxes[i], &x, &y, &w, &h);
	    wl_surface_damage_buffer(surfPtr->surface, x, y, w, h);
	}
    }
    pixman_region32_clear(&surfPtr->damage);
    surfPtr->frameCallback = wl_surface_frame(surfPtr->surface);
    wl_callback_add_listener(surfPtr->frameCallback, &frameListener,
	    surfPtr);
    bufPtr->busy = true;
    wl_surface_commit(surfPtr->surface);
    surfPtr->committed = true;
    TkWlWmSurfaceCommitted(surfPtr);
    wl_display_flush(tkWlDisplay->wlDisplay);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlSurfaceSetOpacity --
 *
 *	Set the opacity of a surface ([wm attributes -alpha]), which is
 *	applied when the backing image is copied to the buffers.
 *
 *----------------------------------------------------------------------
 */

void
TkWlSurfaceSetOpacity(
    TkWlSurface *surfPtr,
    double opacity)
{
    pixman_region32_t all;

    if (opacity < 0.0) {
	opacity = 0.0;
    } else if (opacity > 1.0) {
	opacity = 1.0;
    }
    if (opacity == surfPtr->opacity) {
	return;
    }
    surfPtr->opacity = opacity;
    pixman_region32_init_rect(&all, 0, 0, (unsigned)surfPtr->width,
	    (unsigned)surfPtr->height);
    TkWlAddDamage(surfPtr, &all);
    pixman_region32_fini(&all);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlSurfaceConfigured --
 *
 *	Called by tkWaylandWm.c when the compositor has configured the
 *	surface, so that buffers can be attached to it.
 *
 *----------------------------------------------------------------------
 */

void
TkWlSurfaceConfigured(
    TkWlSurface *surfPtr)
{
    if (!surfPtr->configured) {
	surfPtr->configured = true;
	surfPtr->committed = false;
    }
    SchedulePresent(surfPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlWindowAtPoint --
 *
 *	Find the deepest mapped descendant of a window which contains a
 *	point, given relative to the inside of the window.
 *
 *----------------------------------------------------------------------
 */

TkWlWindow *
TkWlWindowAtPoint(
    TkWlWindow *winPtr,
    int x, int y)
{
    TkWlWindow *childPtr;

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
 * XCreateWindow --
 *
 *----------------------------------------------------------------------
 */

static void
ChangeAttributes(
    TkWlWindow *winPtr,
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
    TkWlWindow *parentPtr = TkWlGetWindow(parent);
    TkWlWindow *winPtr;

    TkWlNextRequest(display);
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
	NewSurface(winPtr);
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
	TkWlQueueEvent(&event);
    }
    return winPtr->id;
}

/*
 *----------------------------------------------------------------------
 *
 * XDestroyWindow --
 *
 *	Destroy a window and its descendants.
 *
 *----------------------------------------------------------------------
 */

static void
FreeWindow(
    TkWlWindow *winPtr)
{
    TkWindow *tkwinPtr;

    while (winPtr->lastChild) {
	TkWlWindow *childPtr = winPtr->lastChild;

	UnlinkWindow(childPtr);
	FreeWindow(childPtr);
    }
    tkwinPtr = (TkWindow *)Tk_IdToWindow(winPtr->display, winPtr->id);
    if (tkwinPtr) {
	TkPointerDeadWindow(tkwinPtr);
    }
    if (winPtr->surface) {
	DestroySurface(winPtr->surface);
    }
    TkWlFreeProperties(winPtr);
    TkWlFreeId(winPtr->id);
    pixman_region32_fini(&winPtr->exposed);
    pixman_region32_fini(&winPtr->clip);
    Tcl_Free(winPtr);
}

int
XDestroyWindow(
    Display *display,
    Window w)
{
    TkWlWindow *winPtr = TkWlGetWindow(w);
    TkWlWindow *parentPtr;

    TkWlNextRequest(display);
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
	TkWlQueueEvent(&event);
    }
    UnlinkWindow(winPtr);
    FreeWindow(winPtr);
    clipGeneration++;
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XMapWindow, XUnmapWindow --
 *
 *----------------------------------------------------------------------
 */

int
XMapWindow(
    Display *display,
    Window w)
{
    TkWlWindow *winPtr = TkWlGetWindow(w);
    XEvent event;

    TkWlNextRequest(display);
    if (winPtr == NULL) {
	return BadWindow;
    }
    if (winPtr->mapped || winPtr->parent == NULL) {
	return Success;
    }
    winPtr->mapped = true;
    if (winPtr->surface && !winPtr->surface->iconic) {
	ResizeBacking(winPtr->surface);
	pixman_region32_clear(&winPtr->exposed);
	winPtr->exposedWidth = -1;
    }
    TkWlTreeChanged(winPtr);
    if (winPtr->surface) {
	TkWlWmMapSurface(winPtr->surface);
    }

    memset(&event, 0, sizeof(XEvent));
    event.xmap.type = MapNotify;
    event.xmap.window = w;
    event.xmap.override_redirect = winPtr->overrideRedirect;
    QueueStructureEvent(winPtr, &event);
    return Success;
}

int
XMapRaised(
    Display *display,
    Window w)
{
    XRaiseWindow(display, w);
    return XMapWindow(display, w);
}

int
XUnmapWindow(
    Display *display,
    Window w)
{
    TkWlWindow *winPtr = TkWlGetWindow(w);
    XEvent event;

    TkWlNextRequest(display);
    if (winPtr == NULL) {
	return BadWindow;
    }
    if (!winPtr->mapped) {
	return Success;
    }
    TkWlTreeChanged(winPtr);
    winPtr->mapped = false;
    if (winPtr->surface && !winPtr->surface->iconic) {
	TkWlSurface *surfPtr = winPtr->surface;

	/*
	 * Destroy the role of the surface, and unmap it.
	 */

	TkWlWmUnmapSurface(surfPtr);
	if (surfPtr->committed) {
	    wl_surface_attach(surfPtr->surface, NULL, 0, 0);
	    wl_surface_commit(surfPtr->surface);
	}

	/*
	 * An unmapped surface gets no frame callbacks: the next present must
	 * not wait for one.
	 */

	if (surfPtr->frameCallback) {
	    wl_callback_destroy(surfPtr->frameCallback);
	    surfPtr->frameCallback = NULL;
	}
	surfPtr->committed = false;
	surfPtr->configured = false;
	surfPtr->presented = false;
    }

    memset(&event, 0, sizeof(XEvent));
    event.xunmap.type = UnmapNotify;
    event.xunmap.window = w;
    event.xunmap.from_configure = False;
    QueueStructureEvent(winPtr, &event);
    return Success;
}

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
 * XConfigureWindow and friends --
 *
 *----------------------------------------------------------------------
 */

static void
Restack(
    TkWlWindow *winPtr,
    TkWlWindow *siblingPtr,
    int stackMode)
{
    TkWlWindow *parentPtr = winPtr->parent;

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

int
XConfigureWindow(
    Display *display,
    Window w,
    unsigned int valueMask,
    XWindowChanges *values)
{
    TkWlWindow *winPtr = TkWlGetWindow(w);

    TkWlNextRequest(display);
    if (winPtr == NULL) {
	return BadWindow;
    }
    if (winPtr->mapped) {
	TkWlTreeChanged(winPtr);
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
		? TkWlGetWindow(values->sibling) : NULL, values->stack_mode);
    }
    if (winPtr->surface && winPtr->mapped) {
	ResizeBacking(winPtr->surface);
	if (valueMask & (CWWidth | CWHeight)) {
	    TkWlWmSurfaceResized(winPtr->surface);
	}
    }
    if (winPtr->mapped) {
	TkWlTreeChanged(winPtr);
    } else {
	clipGeneration++;
    }
    QueueConfigureNotify(winPtr);
    return Success;
}

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

int
XRaiseWindow(
    Display *display,
    Window w)
{
    XWindowChanges values;
    TkWlWindow *winPtr = TkWlGetWindow(w);

    /*
     * A Wayland client cannot raise its toplevels; it can ask the compositor
     * to activate them.
     */

    if (winPtr && winPtr->surface && winPtr->mapped
	    && !winPtr->overrideRedirect) {
	TkWlWmActivate(winPtr->surface);
    }
    values.stack_mode = Above;
    return XConfigureWindow(display, w, CWStackMode, &values);
}

int
XLowerWindow(
    Display *display,
    Window w)
{
    XWindowChanges values;

    values.stack_mode = Below;
    return XConfigureWindow(display, w, CWStackMode, &values);
}

int
XReparentWindow(
    Display *display,
    Window w,
    Window parent,
    int x, int y)
{
    TkWlWindow *winPtr = TkWlGetWindow(w);
    TkWlWindow *parentPtr = TkWlGetWindow(parent);
    bool wasMapped;
    XEvent event;

    TkWlNextRequest(display);
    if (winPtr == NULL || parentPtr == NULL || winPtr->parent == NULL) {
	return BadWindow;
    }
    wasMapped = winPtr->mapped;
    if (wasMapped) {
	XUnmapWindow(display, w);
    }
    if (winPtr->surface && parentPtr->parent) {
	DestroySurface(winPtr->surface);
    }
    UnlinkWindow(winPtr);
    winPtr->x = x;
    winPtr->y = y;
    LinkWindow(winPtr, parentPtr, parentPtr->lastChild);
    if (parentPtr->parent == NULL && winPtr->surface == NULL) {
	NewSurface(winPtr);
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
	TkWlQueueEvent(&event);
    }
    if (parentPtr->eventMask & SubstructureNotifyMask) {
	event.xreparent.event = parent;
	TkWlQueueEvent(&event);
    }

    if (wasMapped) {
	XMapWindow(display, w);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * Window attributes --
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
    TkWlWindow *winPtr = TkWlGetWindow(w);

    TkWlNextRequest(display);
    if (winPtr == NULL) {
	return BadWindow;
    }
    ChangeAttributes(winPtr, valueMask, attrPtr);
    if (valueMask & CWCursor) {
	TkWlUpdateCursor();
    }
    return Success;
}

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

int
XSetWindowBorderPixmap(
    Display *display,
    TCL_UNUSED(Window),
    TCL_UNUSED(Pixmap))
{
    TkWlNextRequest(display);
    return Success;
}

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

int
XClearWindow(
    Display *display,
    Window w)
{
    TkWlWindow *winPtr = TkWlGetWindow(w);
    pixman_region32_t rgn;

    TkWlNextRequest(display);
    if (winPtr == NULL) {
	return BadWindow;
    }
    if (winPtr->parent && winPtr->parent->parent == NULL) {
	TkWlProcessExposures(winPtr->surface);
    } else if (TkWlTopWindow(winPtr) && TkWlTopWindow(winPtr)->surface) {
	TkWlProcessExposures(TkWlTopWindow(winPtr)->surface);
    }
    pixman_region32_init(&rgn);
    TkWlVisibleRegion(winPtr, false, &rgn);
    TkWlPaintBackground(winPtr, &rgn);
    pixman_region32_fini(&rgn);
    return Success;
}

int
XGetWindowAttributes(
    Display *display,
    Window w,
    XWindowAttributes *attrPtr)
{
    TkWlWindow *winPtr = TkWlGetWindow(w);

    TkWlNextRequest(display);
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
	    : TkWlIsViewable(winPtr) ? IsViewable : IsUnviewable;
    attrPtr->all_event_masks = winPtr->eventMask;
    attrPtr->your_event_mask = winPtr->eventMask;
    attrPtr->override_redirect = winPtr->overrideRedirect;
    attrPtr->screen = DefaultScreenOfDisplay(display);
    return 1;
}

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
    void *p = TkWlLookupId(d);

    TkWlNextRequest(display);
    *rootPtr = RootWindow(display, 0);
    if (p == NULL) {
	return 0;
    }
    if (*(int *)p == TKWL_WINDOW) {
	TkWlWindow *winPtr = (TkWlWindow *)p;

	*xPtr = winPtr->x;
	*yPtr = winPtr->y;
	*widthPtr = (unsigned)winPtr->width;
	*heightPtr = (unsigned)winPtr->height;
	*borderWidthPtr = (unsigned)winPtr->borderWidth;
	*depthPtr = 24;
    } else {
	TkWlPixmap *pixPtr = (TkWlPixmap *)p;

	*xPtr = *yPtr = 0;
	*widthPtr = (unsigned)pixPtr->width;
	*heightPtr = (unsigned)pixPtr->height;
	*borderWidthPtr = 0;
	*depthPtr = (unsigned)pixPtr->depth;
    }
    return 1;
}

Status
XQueryTree(
    Display *display,
    Window w,
    Window *rootPtr,
    Window *parentPtr,
    Window **childrenPtr,
    unsigned int *countPtr)
{
    TkWlWindow *winPtr = TkWlGetWindow(w), *childPtr;
    unsigned int n = 0;

    TkWlNextRequest(display);
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
 * XTranslateCoordinates --
 *
 *	Wayland clients do not know where their windows are on the screen.
 *	The root coordinates of a toplevel are those Tk thinks it has (the
 *	position of its wrapper window), which keeps the coordinates
 *	consistent within the application.
 *
 *----------------------------------------------------------------------
 */

static void
RootPosition(
    TkWlWindow *winPtr,
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

Bool
XTranslateCoordinates(
    Display *display,
    Window src,
    Window dst,
    int srcX, int srcY,
    int *dstXPtr, int *dstYPtr,
    Window *childPtr)
{
    TkWlWindow *srcPtr = TkWlGetWindow(src), *dstPtr = TkWlGetWindow(dst);
    int sx, sy, dx, dy;

    TkWlNextRequest(display);
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
	TkWlWindow *childWinPtr;

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
 *----------------------------------------------------------------------
 */

int
XFlush(
    TCL_UNUSED(Display *))
{
    if (tkWlDisplay) {
	wl_display_flush(tkWlDisplay->wlDisplay);
    }
    return Success;
}

int
XSync(
    Display *display,
    Bool discard)
{
    (void)discard;
    TkWlNextRequest(display);
    LastKnownRequestProcessed(display) = NextRequest(display) - 1;
    if (tkWlDisplay) {
	wl_display_flush(tkWlDisplay->wlDisplay);
    }
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
