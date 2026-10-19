/*
 * tkAndroidSurface.c --
 *
 *	The surfaces of the children of the root window on Android: their
 *	backing images, which Tk draws into, and their composition into the
 *	window of the activity, which is the screen of the emulated display.
 *
 *	The parts of the screen which changed (because Tk drew in a surface,
 *	or a surface was mapped, moved, restacked or unmapped) are recorded
 *	as damage, in root coordinates. At idle time the damaged part of the
 *	window is locked, filled with the background of the root window and
 *	the backing images of the surfaces which cover it, from the bottom of
 *	the stacking order to the top, and posted.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkAndroidInt.h"

/*
 * The color of the screen where no window is.
 */

#define ROOT_COLOR_RED		0x2020
#define ROOT_COLOR_GREEN	0x2020
#define ROOT_COLOR_BLUE		0x2020

static void		DamageBox(pixman_box32_t *boxPtr);
static void		NewBacking(TkXeSurface *surfPtr);
static void		Present(void);
static void		PresentIdle(void *clientData);
static void		SchedulePresent(void);
static void		UpdateScale(TkXeSurface *surfPtr);
static void		UpdateShown(void);

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
 *	backing image is created when its window is mapped.
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
    surfPtr->opacity = 1.0;
    surfPtr->shownIndex = -1;
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
 *	The part of the screen where the surface was shown is composited
 *	again.
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
    TkAndroidWmUnmapSurface(surfPtr);
    if (surfPtr->shown) {
	DamageBox(&surfPtr->shownBox);
    }
    if (surfPtr->backing) {
	cairo_surface_destroy(surfPtr->backing);
    }
    surfPtr->winPtr->surface = NULL;
    Tcl_Free(surfPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeResizeBacking --
 *
 *	Make the backing image of a surface the size of its window, keeping
 *	what was drawn in it. Called when the window is mapped or configured.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The screen is composited again, since the window may have moved or
 *	been restacked.
 *
 *----------------------------------------------------------------------
 */

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
 *	The backing image may be replaced.
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

	/*
	 * Only the part of the old image was kept: the rest must be drawn.
	 */

	TkXeLimitExposed(winPtr, surfPtr->width, surfPtr->height);
    }
    surfPtr->backing = backing;
    surfPtr->width = width;
    surfPtr->height = height;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeResizeBacking --
 *
 *	Make the backing image of a surface the size of its window, keeping
 *	what was drawn in it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The backing image may be replaced, and the screen is composited
 *	again.
 *
 *----------------------------------------------------------------------
 */

void
TkXeResizeBacking(
    TkXeSurface *surfPtr)	/* The surface whose window changed. */
{
    NewBacking(surfPtr);
    SchedulePresent();
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
 *	The window manager forgets the window, and the screen is composited
 *	again without it.
 *
 *----------------------------------------------------------------------
 */

void
TkXeSurfaceUnmapped(
    TkXeSurface *surfPtr)	/* The surface whose window was unmapped. */
{
    TkAndroidWmUnmapSurface(surfPtr);
    SchedulePresent();
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeFlush --
 *
 *	There is no connection to flush on Android.
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
 *	The region is composited again at idle time.
 *
 *----------------------------------------------------------------------
 */

void
TkXeAddDamage(
    TkXeSurface *surfPtr,	/* The surface drawn into. */
    pixman_region32_t *rgn)	/* The region drawn, in the coordinates of the
				 * surface. */
{
    pixman_region32_t screen;

    pixman_region32_init(&screen);
    pixman_region32_copy(&screen, rgn);
    pixman_region32_translate(&screen, surfPtr->winPtr->x,
	    surfPtr->winPtr->y);
    pixman_region32_union(&tkXeDisplay->damage, &tkXeDisplay->damage,
	    &screen);
    pixman_region32_fini(&screen);
    SchedulePresent();
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidDamageScreen --
 *
 *	Record that a part of the screen must be composited again.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The rectangle is composited again at idle time.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidDamageScreen(
    int x, int y,		/* The rectangle, in root coordinates. */
    int width, int height)
{
    if (width > 0 && height > 0) {
	pixman_region32_union_rect(&tkXeDisplay->damage, &tkXeDisplay->damage,
		x, y, (unsigned)width, (unsigned)height);
    }
    SchedulePresent();
}

/*
 *----------------------------------------------------------------------
 *
 * DamageBox --
 *
 *	Record that a box of the screen must be composited again.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	See TkAndroidDamageScreen.
 *
 *----------------------------------------------------------------------
 */

static void
DamageBox(
    pixman_box32_t *boxPtr)	/* In root coordinates. */
{
    TkAndroidDamageScreen(boxPtr->x1, boxPtr->y1, boxPtr->x2 - boxPtr->x1,
	    boxPtr->y2 - boxPtr->y1);
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidSurfaceSetOpacity --
 *
 *	Set the opacity of a surface, from _NET_WM_WINDOW_OPACITY: it is
 *	applied when the surface is composited.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The part of the screen the surface is on is composited again.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidSurfaceSetOpacity(
    TkXeSurface *surfPtr,	/* The surface. */
    double opacity)		/* From 0.0 (transparent) to 1.0. */
{
    if (opacity < 0.0) {
	opacity = 0.0;
    } else if (opacity > 1.0) {
	opacity = 1.0;
    }
    if (opacity == surfPtr->opacity) {
	return;
    }
    surfPtr->opacity = opacity;
    if (surfPtr->shown) {
	DamageBox(&surfPtr->shownBox);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeIntegralScale --
 *
 *	Tell whether the scale of a surface is a whole number.
 *
 * Results:
 *	True if the scale is integral, false otherwise.
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
 * UpdateScale --
 *
 *	Give a surface the scale of the display.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If the scale changed, the backing image is made again, and the
 *	windows of the surface get Expose events for all of their visible
 *	parts.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateScale(
    TkXeSurface *surfPtr)	/* The surface. */
{
    TkXeWindow *winPtr = surfPtr->winPtr;

    if (surfPtr->scale == tkXeDisplay->scale) {
	return;
    }
    surfPtr->scale = tkXeDisplay->scale;
    if (surfPtr->backing) {
	cairo_surface_destroy(surfPtr->backing);
	surfPtr->backing = NULL;
	NewBacking(surfPtr);
	TkXeForgetExposed(winPtr);
	TkXeTreeChanged(winPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidUpdateScale --
 *
 *	Compute the scale and the size of the screen from the window of the
 *	activity and the density of the screen, and apply them.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The size of the screen and of the root window change, the surfaces
 *	get the new scale, and the window manager places the toplevels
 *	again.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidUpdateScale(void)
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    TkXeSurface *surfPtr;
    double scale;

    if (dispPtr->forcedScale > 0.0) {
	scale = dispPtr->forcedScale;
    } else if (dispPtr->density > 0) {
	scale = dispPtr->density / 160.0;
    } else {
	scale = 1.0;
    }
    if (scale < 0.5) {
	scale = 0.5;
    }
    dispPtr->scale = scale;
    if (dispPtr->display && dispPtr->windowWidth > 0) {
	TkXeSetScreenSize(dispPtr->display,
		(int)floor(dispPtr->windowWidth / scale),
		(int)floor(dispPtr->windowHeight / scale), 0, 0);
    }
    for (surfPtr = dispPtr->surfaces; surfPtr; surfPtr = surfPtr->nextPtr) {
	UpdateScale(surfPtr);
    }
    if (dispPtr->display) {
	TkAndroidWmRelayout();
	TkAndroidDamageScreen(0, 0, ScreenOfDisplay(dispPtr->display, 0)->width,
		ScreenOfDisplay(dispPtr->display, 0)->height);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidSetWindow --
 *
 *	Set the window of the activity, which the surfaces are shown in, or
 *	forget it (NULL) when it is destroyed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The reference to the previous window, which the glue acquired, is
 *	released. The whole screen is composited into a new window.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidSetWindow(
    ANativeWindow *window)	/* The window, or NULL. */
{
    TkXeDisplay *dispPtr = tkXeDisplay;

    if (dispPtr->window && dispPtr->window != window) {
	ANativeWindow_release(dispPtr->window);
    }
    dispPtr->window = window;
    if (window == NULL) {
	return;
    }
    ANativeWindow_setBuffersGeometry(window, 0, 0,
	    WINDOW_FORMAT_RGBX_8888);
    dispPtr->windowWidth = ANativeWindow_getWidth(window);
    dispPtr->windowHeight = ANativeWindow_getHeight(window);
    if (dispPtr->content[2] <= dispPtr->content[0]) {
	dispPtr->content[0] = dispPtr->content[1] = 0;
	dispPtr->content[2] = dispPtr->windowWidth;
	dispPtr->content[3] = dispPtr->windowHeight;
    }
    TkAndroidUpdateScale();
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidSurfaceAt --
 *
 *	Find the top-most surface shown at a point of the screen.
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
TkAndroidSurfaceAt(
    int x, int y)		/* In root coordinates. */
{
    TkXeWindow *rootPtr, *childPtr;

    if (tkXeDisplay->display == NULL) {
	return NULL;
    }
    rootPtr = TkXeGetWindow(RootWindow(tkXeDisplay->display, 0));
    for (childPtr = rootPtr->lastChild; childPtr;
	    childPtr = childPtr->below) {
	int w = childPtr->width + 2 * childPtr->borderWidth;
	int h = childPtr->height + 2 * childPtr->borderWidth;

	if (childPtr->mapped && !childPtr->inputOnly && childPtr->surface
		&& x >= childPtr->x && y >= childPtr->y
		&& x < childPtr->x + w && y < childPtr->y + h) {
	    return childPtr->surface;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Presentation --
 *
 *	The screen is composited at idle time, after a second idle callback,
 *	so that the widgets which redisplay because of the Expose events
 *	generated with the damage draw first, as in the Wayland port.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * SchedulePresent --
 *
 *	Schedule the composition of the screen at idle time.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An idle callback may be scheduled.
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

/*
 *----------------------------------------------------------------------
 *
 * PresentIdle --
 *
 *	Composite the screen, the second time it is called, so that the
 *	widgets which redraw at idle time have drawn.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The screen is composited.
 *
 *----------------------------------------------------------------------
 */

static void
PresentIdle(
    TCL_UNUSED(void *))
{
    if (!tkXeDisplay->presentDeferred) {
	tkXeDisplay->presentDeferred = true;
	Tcl_DoWhenIdle(PresentIdle, NULL);
	return;
    }
    tkXeDisplay->presentDeferred = false;
    tkXeDisplay->presentPending = false;
    Present();
}

/*
 *----------------------------------------------------------------------
 *
 * UpdateShown --
 *
 *	Damage the parts of the screen where surfaces were shown and where
 *	they are now, for those which were mapped, unmapped, moved, resized
 *	or restacked since the last composition.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The damage of the display grows.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateShown(void)
{
    TkXeWindow *rootPtr, *childPtr;
    int index = 0;

    rootPtr = TkXeGetWindow(RootWindow(tkXeDisplay->display, 0));
    for (childPtr = rootPtr->firstChild; childPtr;
	    childPtr = childPtr->above) {
	TkXeSurface *surfPtr = childPtr->surface;
	pixman_box32_t box;
	bool shown;

	if (surfPtr == NULL) {
	    continue;
	}
	shown = childPtr->mapped && !childPtr->inputOnly
		&& surfPtr->backing != NULL;
	box.x1 = childPtr->x;
	box.y1 = childPtr->y;
	box.x2 = childPtr->x + surfPtr->width;
	box.y2 = childPtr->y + surfPtr->height;
	if (shown != surfPtr->shown
		|| (shown && (surfPtr->shownIndex != index
		    || memcmp(&box, &surfPtr->shownBox, sizeof(box))))) {
	    if (surfPtr->shown) {
		DamageBox(&surfPtr->shownBox);
	    }
	    if (shown) {
		DamageBox(&box);
	    }
	}
	surfPtr->shown = shown;
	surfPtr->shownBox = box;
	surfPtr->shownIndex = shown ? index : -1;
	if (shown) {
	    index++;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * BufferFormat --
 *
 *	The pixman format of the pixels of a window buffer.
 *
 * Results:
 *	The format.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static pixman_format_code_t
BufferFormat(
    int32_t format)		/* AHARDWAREBUFFER_FORMAT_*. */
{
    switch (format) {
    case WINDOW_FORMAT_RGBA_8888:
	return PIXMAN_a8b8g8r8;
    case WINDOW_FORMAT_RGB_565:
	return PIXMAN_r5g6b5;
    default:
	return PIXMAN_x8b8g8r8;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Present --
 *
 *	Composite the damaged part of the screen into the window of the
 *	activity.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The window is drawn, and the damage cleared.
 *
 *----------------------------------------------------------------------
 */

static void
Present(void)
{
    TkXeDisplay *dispPtr = tkXeDisplay;
    TkXeSurface *surfPtr;
    TkXeWindow *rootPtr, *childPtr;
    pixman_box32_t *extents;
    pixman_image_t *dst;
    pixman_color_t rootColor = {
	ROOT_COLOR_RED, ROOT_COLOR_GREEN, ROOT_COLOR_BLUE, 0xFFFF
    };
    ANativeWindow_Buffer buffer;
    ARect dirty;
    double s = dispPtr->scale;

    if (dispPtr->display == NULL) {
	return;
    }
    for (surfPtr = dispPtr->surfaces; surfPtr; surfPtr = surfPtr->nextPtr) {
	TkXeProcessExposures(surfPtr);
    }
    UpdateShown();
    if (dispPtr->window == NULL
	    || !pixman_region32_not_empty(&dispPtr->damage)) {
	return;
    }

    /*
     * Lock the pixels which the damage touches. The window may give a
     * larger rectangle, which is all composited.
     */

    extents = pixman_region32_extents(&dispPtr->damage);
    dirty.left = (int32_t)floor(extents->x1 * s);
    dirty.top = (int32_t)floor(extents->y1 * s);
    dirty.right = (int32_t)ceil(extents->x2 * s);
    dirty.bottom = (int32_t)ceil(extents->y2 * s);
    if (dirty.left < 0) {
	dirty.left = 0;
    }
    if (dirty.top < 0) {
	dirty.top = 0;
    }
    if (dirty.right > dispPtr->windowWidth) {
	dirty.right = dispPtr->windowWidth;
    }
    if (dirty.bottom > dispPtr->windowHeight) {
	dirty.bottom = dispPtr->windowHeight;
    }
    pixman_region32_clear(&dispPtr->damage);
    if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) {
	return;
    }
    if (ANativeWindow_lock(dispPtr->window, &buffer, &dirty) != 0) {
	return;
    }
    if (dirty.right > buffer.width) {
	dirty.right = buffer.width;
    }
    if (dirty.bottom > buffer.height) {
	dirty.bottom = buffer.height;
    }
    dst = pixman_image_create_bits(BufferFormat(buffer.format), buffer.width,
	    buffer.height, (uint32_t *)buffer.bits,
	    buffer.stride * ((buffer.format == WINDOW_FORMAT_RGB_565) ? 2 : 4));

    /*
     * The background, then the surfaces from the bottom up.
     */

    {
	pixman_box32_t box = {dirty.left, dirty.top, dirty.right, dirty.bottom};

	pixman_image_fill_boxes(PIXMAN_OP_SRC, dst, &rootColor, 1, &box);
    }
    rootPtr = TkXeGetWindow(RootWindow(dispPtr->display, 0));
    for (childPtr = rootPtr->firstChild; childPtr;
	    childPtr = childPtr->above) {
	pixman_image_t *src, *mask = NULL;
	int ox, oy, x1, y1, x2, y2;

	surfPtr = childPtr->surface;
	if (surfPtr == NULL || !surfPtr->shown) {
	    continue;
	}
	ox = (int)lround(childPtr->x * s);
	oy = (int)lround(childPtr->y * s);
	x1 = (ox > dirty.left) ? ox : dirty.left;
	y1 = (oy > dirty.top) ? oy : dirty.top;
	x2 = ox + surfPtr->bufferWidth;
	y2 = oy + surfPtr->bufferHeight;
	if (x2 > dirty.right) {
	    x2 = dirty.right;
	}
	if (y2 > dirty.bottom) {
	    y2 = dirty.bottom;
	}
	if (x2 <= x1 || y2 <= y1) {
	    continue;
	}
	cairo_surface_flush(surfPtr->backing);
	src = pixman_image_create_bits(PIXMAN_x8r8g8b8, surfPtr->bufferWidth,
		surfPtr->bufferHeight,
		(uint32_t *)cairo_image_surface_get_data(surfPtr->backing),
		cairo_image_surface_get_stride(surfPtr->backing));
	if (surfPtr->opacity < 1.0) {
	    pixman_color_t alpha = {0, 0, 0,
		    (uint16_t)(surfPtr->opacity * 0xFFFF)};

	    mask = pixman_image_create_solid_fill(&alpha);
	}
	pixman_image_composite32(mask ? PIXMAN_OP_OVER : PIXMAN_OP_SRC, src,
		mask, dst, x1 - ox, y1 - oy, 0, 0, x1, y1, x2 - x1, y2 - y1);
	pixman_image_unref(src);
	if (mask) {
	    pixman_image_unref(mask);
	}
    }
    pixman_image_unref(dst);
    ANativeWindow_unlockAndPost(dispPtr->window);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
