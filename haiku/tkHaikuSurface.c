/*
 * tkHaikuSurface.c --
 *
 *	The surfaces of the children of the root window on Haiku: their
 *	backing images, which Tk draws into, and their BWindows, which show
 *	them.
 *
 *	The parts of a backing image Tk draws into are recorded as damage,
 *	and copied to the bitmap of the BWindow at idle time, once Tk has
 *	drawn what it had to. A BWindow is shown when the window manager maps
 *	the window, after its image was first copied, so that it does not
 *	appear empty.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkHaikuInt.h"

static int		BufferSize(int length, double scale);
static void		NewBacking(TkXeSurface *surfPtr);
static void		PresentIdle(void *clientData);
static void		PresentSurface(TkXeSurface *surfPtr);
static void		SchedulePresent(void);

/*
 *----------------------------------------------------------------------
 *
 * TkXeNewSurface --
 *
 *	Create the surface of a child of the root window.
 *
 * Results:
 *	The new surface.
 *
 * Side effects:
 *	The surface is added to the list of surfaces of the display. Its
 *	backing image is created when its window is mapped, and its BWindow
 *	when the window manager maps it.
 *
 *----------------------------------------------------------------------
 */

TkXeSurface *
TkXeNewSurface(
    TkXeWindow *winPtr)		/* A child of the root window. */
{
    TkXeSurface *surfPtr = (TkXeSurface *)Tcl_Alloc(sizeof(TkXeSurface));

    memset(surfPtr, 0, sizeof(TkXeSurface));
    surfPtr->winPtr = winPtr;
    surfPtr->scale = tkXeDisplay->scale;
    surfPtr->stackIndex = -1;
    pixman_region32_init(&surfPtr->damage);
    surfPtr->nextPtr = tkXeDisplay->surfaces;
    tkXeDisplay->surfaces = surfPtr;
    winPtr->surface = surfPtr;
    return surfPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeDestroySurface --
 *
 *	Destroy the surface of a window which is destroyed or reparented.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The BWindow is destroyed.
 *
 *----------------------------------------------------------------------
 */

void
TkXeDestroySurface(
    TkXeSurface *surfPtr)	/* The surface to destroy. */
{
    TkXeSurface **prevPtrPtr;

    for (prevPtrPtr = &tkXeDisplay->surfaces; *prevPtrPtr;
	    prevPtrPtr = &(*prevPtrPtr)->nextPtr) {
	if (*prevPtrPtr == surfPtr) {
	    *prevPtrPtr = surfPtr->nextPtr;
	    break;
	}
    }
    TkHaikuPointerLeaveSurface(surfPtr);
    TkHaikuWmUnmapSurface(surfPtr);
    TkHaikuForgetSurface(surfPtr);
    TkHaikuPointerWindowChanged();
    if (tkXeDisplay->pointerSurface == surfPtr) {
	tkXeDisplay->pointerSurface = NULL;
    }
    if (surfPtr->window) {
	TkHaikuWindowDestroy(surfPtr->window);
    }
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
 * BufferSize --
 *
 *	The size in pixels of a length in logical pixels.
 *
 * Results:
 *	The size, at least 1.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
BufferSize(
    int length,			/* In logical pixels. */
    double scale)
{
    int size = (int)lround(length * scale);

    return (size > 0) ? size : 1;
}

/*
 *----------------------------------------------------------------------
 *
 * NewBacking --
 *
 *	Make the backing image of a surface the size of its window, keeping
 *	what was drawn in it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The backing image may be replaced; all of it is to be shown again.
 *
 *----------------------------------------------------------------------
 */

static void
NewBacking(
    TkXeSurface *surfPtr)	/* The surface whose window changed size. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
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
	    && height == surfPtr->height
	    && surfPtr->scale == tkXeDisplay->scale) {
	return;
    }
    surfPtr->scale = tkXeDisplay->scale;
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

	/*
	 * Only the part of the old image was kept: the rest must be drawn.
	 */

	TkXeLimitExposed(winPtr, surfPtr->width, surfPtr->height);
    }
    surfPtr->backing = backing;
    surfPtr->width = width;
    surfPtr->height = height;
    pixman_region32_union_rect(&surfPtr->damage, &surfPtr->damage, 0, 0,
	    (unsigned)width, (unsigned)height);
    SchedulePresent();
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeResizeBacking --
 *
 *	Called when the window of a surface is mapped or configured: make the
 *	backing image the size of the window, and the BWindow follow the
 *	window.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The backing image may be replaced; the BWindow may be moved, resized
 *	or restacked.
 *
 *----------------------------------------------------------------------
 */

void
TkXeResizeBacking(
    TkXeSurface *surfPtr)	/* The surface whose window changed. */
{
    NewBacking(surfPtr);
    TkHaikuWmConfigured(surfPtr);
    TkHaikuSyncFrame(surfPtr);
    TkHaikuWmRestack();
    TkHaikuPointerWindowChanged();
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuSyncFrame --
 *
 *	Move and resize the BWindow of a surface to its X window, unless the
 *	window manager is making the X window follow the BWindow.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The BWindow may move or change size.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuSyncFrame(
    TkXeSurface *surfPtr)	/* The surface. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    int x, y, width, height;

    if (surfPtr->window == NULL || surfPtr->inConfigure) {
	return;
    }
    TkHaikuWindowMoveResize(surfPtr->window, TkHaikuToScreen(winPtr->x),
	    TkHaikuToScreen(winPtr->y), surfPtr->bufferWidth,
	    surfPtr->bufferHeight);

    /*
     * The app_server does not make windows smaller than their decorations
     * allow: the X window keeps the size Tk asked for, and the rest of the
     * BWindow is filled.
     */

    TkHaikuWindowGetFrame(surfPtr->window, &x, &y, &width, &height);
    if (width != surfPtr->bufferWidth || height != surfPtr->bufferHeight) {
	surfPtr->clampedWidth = width;
	surfPtr->clampedHeight = height;
    } else {
	surfPtr->clampedWidth = surfPtr->clampedHeight = 0;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeSurfaceUnmapped --
 *
 *	Called when the window of a surface is unmapped.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The BWindow is hidden, unless the window was minimized (Haiku keeps
 *	minimized windows in the Deskbar); the window manager forgets the
 *	window.
 *
 *----------------------------------------------------------------------
 */

void
TkXeSurfaceUnmapped(
    TkXeSurface *surfPtr)	/* The surface whose window was unmapped. */
{
    TkHaikuPointerLeaveSurface(surfPtr);
    if (!surfPtr->iconic) {
	TkHaikuShowSurface(surfPtr, false);
    }
    TkHaikuWmUnmapSurface(surfPtr);
    TkHaikuPointerWindowChanged();
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuShowSurface --
 *
 *	Show or hide the BWindow of a surface. It is shown once its image
 *	has been copied to it, at idle time.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The BWindow is hidden, or will be shown.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuShowSurface(
    TkXeSurface *surfPtr,	/* The surface. */
    bool show)			/* Whether to show it. */
{
    if (surfPtr->window == NULL) {
	return;
    }
    if (show) {
	pixman_region32_union_rect(&surfPtr->damage, &surfPtr->damage, 0, 0,
		(unsigned)surfPtr->width, (unsigned)surfPtr->height);
	surfPtr->shown = true;
	SchedulePresent();
    } else if (surfPtr->shown) {
	surfPtr->shown = false;
	TkHaikuWindowShow(surfPtr->window, false);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeFlush --
 *
 *	There is no connection to flush: what Tk drew is shown at idle time.
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
TkXeFlush(void)
{
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeAddDamage --
 *
 *	Record that a region of a surface was drawn into.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The region is shown at idle time.
 *
 *----------------------------------------------------------------------
 */

void
TkXeAddDamage(
    TkXeSurface *surfPtr,	/* The surface drawn into. */
    pixman_region32_t *rgn)	/* The region drawn, in the coordinates of
				 * the surface. */
{
    pixman_region32_union(&surfPtr->damage, &surfPtr->damage, rgn);
    SchedulePresent();
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeIntegralScale --
 *
 *	Whether the scale of a surface is a whole number, so that moving its
 *	contents by whole logical pixels moves whole pixels.
 *
 * Results:
 *	True if it is.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

bool
TkXeIntegralScale(
    TkXeSurface *surfPtr)	/* The surface. */
{
    return surfPtr->scale == floor(surfPtr->scale);
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuSurfaceAt --
 *
 *	Find the top-most surface shown at a point of the screen, but one.
 *
 * Results:
 *	The surface, or NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

TkXeSurface *
TkHaikuSurfaceAt(
    int x, int y,		/* In root coordinates. */
    TkXeSurface *exceptPtr)	/* A surface going away, or NULL. */
{
    TkXeWindow *rootPtr, *childPtr;

    if (tkXeDisplay->display == NULL) {
	return NULL;
    }
    rootPtr = TkXeGetWindow(RootWindow(tkXeDisplay->display, 0));
    for (childPtr = rootPtr->lastChild; childPtr;
	    childPtr = childPtr->below) {
	if (childPtr->mapped && childPtr->surface
		&& childPtr->surface != exceptPtr
		&& !childPtr->surface->iconic && !childPtr->inputOnly
		&& x >= childPtr->x && y >= childPtr->y
		&& x < childPtr->x + childPtr->width
			+ 2 * childPtr->borderWidth
		&& y < childPtr->y + childPtr->height
			+ 2 * childPtr->borderWidth) {
	    return childPtr->surface;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * SchedulePresent, PresentIdle --
 *
 *	Show what was drawn at idle time.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An idle callback is scheduled.
 *
 *----------------------------------------------------------------------
 */

static void
SchedulePresent(void)
{
    if (!tkXeDisplay->presentPending) {
	tkXeDisplay->presentPending = true;
	Tcl_DoWhenIdle(PresentIdle, NULL);
    }
}

static void
PresentIdle(
    TCL_UNUSED(void *))
{
    tkXeDisplay->presentPending = false;
    TkHaikuPresent();
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuPresent --
 *
 *	Copy the damaged parts of the backing images of the surfaces to
 *	their BWindows, and show the BWindows waiting for it. The window
 *	tree changed, so the window under the pointer may have too.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The damage is cleared.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuPresent(void)
{
    TkXeSurface *surfPtr;

    for (surfPtr = tkXeDisplay->surfaces; surfPtr;
	    surfPtr = surfPtr->nextPtr) {
	if (surfPtr->exposePending) {
	    TkXeProcessExposures(surfPtr);
	}
	PresentSurface(surfPtr);
    }
    TkHaikuPointerWindowChanged();
}

/*
 *----------------------------------------------------------------------
 *
 * PresentSurface --
 *
 *	Copy the damaged part of the backing image of a surface to its
 *	BWindow, and show it if it is waiting for it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The damage of the surface is cleared.
 *
 *----------------------------------------------------------------------
 */

static void
PresentSurface(
    TkXeSurface *surfPtr)	/* The surface. */
{
    pixman_box32_t *boxes;
    int i, n, *rects;
    double s = surfPtr->scale;

    if (surfPtr->window == NULL || surfPtr->backing == NULL
	    || !surfPtr->winPtr->mapped || !surfPtr->shown) {
	return;
    }
    boxes = pixman_region32_rectangles(&surfPtr->damage, &n);
    if (n > 0) {
	rects = (int *)Tcl_Alloc(4 * n * sizeof(int));
	for (i = 0; i < n; i++) {
	    int x1 = (int)floor(boxes[i].x1 * s);
	    int y1 = (int)floor(boxes[i].y1 * s);
	    int x2 = (int)ceil(boxes[i].x2 * s);
	    int y2 = (int)ceil(boxes[i].y2 * s);

	    rects[4 * i] = x1;
	    rects[4 * i + 1] = y1;
	    rects[4 * i + 2] = x2 - x1;
	    rects[4 * i + 3] = y2 - y1;
	}
	cairo_surface_flush(surfPtr->backing);
	TkHaikuWindowUpdate(surfPtr->window,
		cairo_image_surface_get_data(surfPtr->backing),
		cairo_image_surface_get_stride(surfPtr->backing),
		surfPtr->bufferWidth, surfPtr->bufferHeight, rects, n);
	Tcl_Free(rects);
    }
    pixman_region32_clear(&surfPtr->damage);
    TkHaikuWindowShow(surfPtr->window, true);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
