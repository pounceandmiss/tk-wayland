/*
 * tkWaylandSurface.c --
 *
 *	The surfaces of the children of the root window, which present the
 *	images Tk draws into: their wl_surfaces, scale, backing images and
 *	the wl_shm buffers they are copied to.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#define _GNU_SOURCE		/* For memfd_create. */
#include "tkWaylandInt.h"
#include <sys/mman.h>

static void		FrameDone(void *data, struct wl_callback *callback,
			    uint32_t time);
static void		PresentIdle(void *clientData);
static void		Present(TkXeSurface *surfPtr);
static void		SchedulePresent(TkXeSurface *surfPtr);

static const struct wl_callback_listener frameListener = {
    FrameDone
};

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
 *----------------------------------------------------------------------
 *
 * SurfaceEnter, SurfaceLeave --
 *
 *	The compositor tells which outputs a surface is on. The scale of a
 *	surface is the preferred fractional scale the compositor gives, or its
 *	preferred integer scale, or the largest scale of the outputs the
 *	surface is on, in this order. Fractional scales need the viewporter.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The outputs of the surface are recorded, and its scale updated.
 *
 *----------------------------------------------------------------------
 */

static void
SurfaceEnter(
    void *data,
    TCL_UNUSED(struct wl_surface *),
    struct wl_output *output)
{
    TkXeSurface *surfPtr = (TkXeSurface *)data;
    TkWlOutput *outPtr;

    for (outPtr = tkXeDisplay->outputs; outPtr; outPtr = outPtr->nextPtr) {
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
    TkXeSurface *surfPtr = (TkXeSurface *)data;
    int i;

    for (i = 0; i < surfPtr->numOutputs; i++) {
	if (surfPtr->outputs[i]->output == output) {
	    surfPtr->outputs[i] = surfPtr->outputs[--surfPtr->numOutputs];
	    TkWlUpdateScale(surfPtr);
	    break;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SurfacePreferredScale --
 *
 *	The compositor gives the integer scale it prefers for a surface.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The scale of the surface is updated.
 *
 *----------------------------------------------------------------------
 */

#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
static void
SurfacePreferredScale(
    void *data,
    TCL_UNUSED(struct wl_surface *),
    int32_t factor)
{
    TkXeSurface *surfPtr = (TkXeSurface *)data;

    surfPtr->preferredScale = factor;
    TkWlUpdateScale(surfPtr);
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * FractionalPreferredScale --
 *
 *	The compositor gives the fractional scale it prefers for a surface, in
 *	120ths.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The scale of the surface is updated.
 *
 *----------------------------------------------------------------------
 */

static void
FractionalPreferredScale(
    void *data,
    TCL_UNUSED(struct wp_fractional_scale_v1 *),
    uint32_t scale)
{
    TkXeSurface *surfPtr = (TkXeSurface *)data;

    surfPtr->fractionalScale = scale / 120.0;
    TkWlUpdateScale(surfPtr);
}

static const struct wp_fractional_scale_v1_listener fractionalListener = {
    FractionalPreferredScale
};

/*
 *----------------------------------------------------------------------
 *
 * SurfacePreferredTransform --
 *
 *	The compositor gives the transform it prefers for the buffers of a
 *	surface, which is not used.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

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

/*
 * The events of version 6 of wl_surface are in the listener only with the
 * headers which have them.
 */

static const struct wl_surface_listener surfaceListener = {
    SurfaceEnter,
    SurfaceLeave,
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
    SurfacePreferredScale,
    SurfacePreferredTransform
#endif
};

/*
 *----------------------------------------------------------------------
 *
 * CreateWlSurface, DestroyWlSurface --
 *
 *	Create the wl_surface of a surface, with its viewport and its
 *	fractional scale object, or destroy them.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Objects of the compositor are created, or destroyed with the pending
 *	callbacks of the surface.
 *
 *----------------------------------------------------------------------
 */

static void
CreateWlSurface(
    TkXeSurface *surfPtr)
{
    surfPtr->surface = wl_compositor_create_surface(tkXeDisplay->compositor);
    wl_surface_set_user_data(surfPtr->surface, surfPtr);
    wl_surface_add_listener(surfPtr->surface, &surfaceListener, surfPtr);
    if (tkXeDisplay->viewporter) {
	surfPtr->viewport = wp_viewporter_get_viewport(
		tkXeDisplay->viewporter, surfPtr->surface);
	if (tkXeDisplay->fractionalScaleManager) {
	    surfPtr->fractional =
		    wp_fractional_scale_manager_v1_get_fractional_scale(
		    tkXeDisplay->fractionalScaleManager, surfPtr->surface);
	    wp_fractional_scale_v1_add_listener(surfPtr->fractional,
		    &fractionalListener, surfPtr);
	}
    }
    surfPtr->numOutputs = 0;
    surfPtr->roleType = TKWL_ROLE_NONE;
}

static void
DestroyWlSurface(
    TkXeSurface *surfPtr)
{
    if (surfPtr->frameCallback) {
	wl_callback_destroy(surfPtr->frameCallback);
	surfPtr->frameCallback = NULL;
    }
    if (surfPtr->resizeSync) {
	wl_callback_destroy(surfPtr->resizeSync);
	surfPtr->resizeSync = NULL;
    }
    if (surfPtr->minimizeSync) {
	wl_callback_destroy(surfPtr->minimizeSync);
	surfPtr->minimizeSync = NULL;
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
 * Results:
 *	None.
 *
 * Side effects:
 *	The wl_surface may be replaced, in which case the surface has to be
 *	configured and drawn entirely again.
 *
 *----------------------------------------------------------------------
 */

void
TkWlSurfaceSetRole(
    TkXeSurface *surfPtr,
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
 *	Memory is allocated, a wl_surface is created, and the surface is
 *	linked in the list of the display.
 *
 *----------------------------------------------------------------------
 */

TkXeSurface *
TkXeNewSurface(
    TkXeWindow *winPtr)
{
    TkXeSurface *surfPtr = (TkXeSurface *)Tcl_Alloc(sizeof(TkXeSurface));
    int i;

    memset(surfPtr, 0, sizeof(TkXeSurface));
    surfPtr->winPtr = winPtr;
    surfPtr->scale = 1.0;
    surfPtr->opacity = 1.0;
    pixman_region32_init(&surfPtr->damage);
    for (i = 0; i < TKWL_NUM_BUFFERS; i++) {
	pixman_region32_init(&surfPtr->buffers[i].damage);
    }
    CreateWlSurface(surfPtr);
    TkWlUpdateScale(surfPtr);
    surfPtr->nextPtr = tkXeDisplay->surfaces;
    tkXeDisplay->surfaces = surfPtr;
    winPtr->surface = surfPtr;
    return surfPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlSurfaceFromWl --
 *
 *	Find the surface which has a wl_surface.
 *
 * Results:
 *	The surface, or NULL if the wl_surface is not one of Tk.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

TkXeSurface *
TkWlSurfaceFromWl(
    struct wl_surface *surface)
{
    TkXeSurface *surfPtr;

    for (surfPtr = tkXeDisplay->surfaces; surfPtr; surfPtr = surfPtr->nextPtr) {
	if (surfPtr->surface == surface) {
	    return surfPtr;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeBuffer --
 *
 *	Free the wl_shm buffer of a buffer structure.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The buffer is destroyed and its memory unmapped.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * TkXeDestroySurface --
 *
 *	Destroy the surface of a window.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The role, the buffers, the wl_surface and the backing image of the
 *	surface are destroyed, and the surface is freed.
 *
 *----------------------------------------------------------------------
 */

void
TkXeDestroySurface(
    TkXeSurface *surfPtr)
{
    TkXeSurface **prevPtrPtr;
    int i;

    for (prevPtrPtr = &tkXeDisplay->surfaces; *prevPtrPtr;
	    prevPtrPtr = &(*prevPtrPtr)->nextPtr) {
	if (*prevPtrPtr == surfPtr) {
	    *prevPtrPtr = surfPtr->nextPtr;
	    break;
	}
    }
    if (tkXeDisplay->pointerSurface == surfPtr) {
	tkXeDisplay->pointerSurface = NULL;
    }
    if (tkXeDisplay->keyboardSurface == surfPtr) {
	tkXeDisplay->keyboardSurface = NULL;
    }
    for (i = 0; i < TKWL_MAX_TOUCHES; i++) {
	if (tkXeDisplay->touches[i].surfPtr == surfPtr) {
	    tkXeDisplay->touches[i].surfPtr = NULL;
	}
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
    if (surfPtr->cornerMask) {
	pixman_image_unref(surfPtr->cornerMask);
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
 *	Compute the size in pixels of a length in logical pixels, as in the
 *	fractional scale protocol.
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
    int length,
    double scale)
{
    int size = (int)lround(length * scale);

    return (size > 0) ? size : 1;
}

/*
 *----------------------------------------------------------------------
 *
 * BufferBox --
 *
 *	Convert a box in surface coordinates to the pixels of the buffer which
 *	it touches.
 *
 * Results:
 *	The box is stored in *xPtr, *yPtr, *widthPtr and *heightPtr, clipped
 *	to the buffer.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
BufferBox(
    TkXeSurface *surfPtr,
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
    TkXeSurface *surfPtr)
{
    return surfPtr->scale == floor(surfPtr->scale);
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
 *	The backing image may be replaced, and the damage of the surface is
 *	clipped to its new size.
 *
 *----------------------------------------------------------------------
 */

void
TkXeResizeBacking(
    TkXeSurface *surfPtr)
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
    pixman_region32_intersect_rect(&surfPtr->damage, &surfPtr->damage, 0, 0,
	    (unsigned)width, (unsigned)height);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlUpdateScale --
 *
 *	Compute the scale of a surface again, after the compositor told
 *	something which may change it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	When the scale changes, the backing image is replaced, everything in
 *	the surface is drawn again and the cursor is updated.
 *
 *----------------------------------------------------------------------
 */

void
TkWlUpdateScale(
    TkXeSurface *surfPtr)
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    double scale = 1.0;
    int i;

    if (tkXeDisplay->forcedScale > 0.0) {
	scale = tkXeDisplay->forcedScale;
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
	TkXeResizeBacking(surfPtr);
	TkXeForgetExposed(winPtr);
	TkXeTreeChanged(winPtr);
	TkXeProcessExposures(surfPtr);
    }
    if (tkXeDisplay->pointerSurface == surfPtr) {
	TkXeUpdateCursor();
    }
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
 *	The region is added to the damage of the surface and of its buffers,
 *	and the presentation of the surface is scheduled.
 *
 *----------------------------------------------------------------------
 */

void
TkXeAddDamage(
    TkXeSurface *surfPtr,
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
 * Rounded corners --
 *
 *	Windows without decorations can have rounded corners: their buffers
 *	are translucent, with the corners copied from the backing image
 *	through an antialiased mask. The corners still take input, where
 *	users grab windows to resize them.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * CornerRadius --
 *
 *	Compute the radius of the corners of a surface as it is now: none when
 *	they are square, and at most half the size of the window.
 *
 * Results:
 *	The radius in logical pixels, or 0.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static double
CornerRadius(
    TkXeSurface *surfPtr)
{
    double radius = surfPtr->squareCorners ? 0.0 : surfPtr->cornerRadius;

    /*
     * The corners cannot be larger than half the window.
     */

    if (radius > surfPtr->width / 2.0) {
	radius = surfPtr->width / 2.0;
    }
    if (radius > surfPtr->height / 2.0) {
	radius = surfPtr->height / 2.0;
    }
    return (radius > 0.0) ? radius : 0.0;
}

/*
 *----------------------------------------------------------------------
 *
 * RoundCorners --
 *
 *	Copy the corners of the backing image to the buffer through the mask:
 *	the corners are copied whole every time, so that the mask is applied
 *	to the backing image once whatever was damaged.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The corners of the buffer are written; the mask is made again when the
 *	radius or the opacity changed.
 *
 *----------------------------------------------------------------------
 */

static void
RoundCorners(
    TkXeSurface *surfPtr,
    pixman_image_t *src,
    pixman_image_t *dst,
    double radius)
{
    double r = radius * surfPtr->scale;
    int size = (int)ceil(r), i;
    int width = surfPtr->bufferWidth, height = surfPtr->bufferHeight;

    if (surfPtr->cornerMask == NULL || surfPtr->cornerMaskRadius != r
	    || surfPtr->cornerMaskOpacity != surfPtr->opacity) {
	cairo_surface_t *maskSurf;
	cairo_t *cr;

	if (surfPtr->cornerMask) {
	    pixman_image_unref(surfPtr->cornerMask);
	}
	surfPtr->cornerMask = pixman_image_create_bits(PIXMAN_a8, 2 * size,
		2 * size, NULL, 0);
	maskSurf = cairo_image_surface_create_for_data(
		(unsigned char *)pixman_image_get_data(surfPtr->cornerMask),
		CAIRO_FORMAT_A8, 2 * size, 2 * size,
		pixman_image_get_stride(surfPtr->cornerMask));
	cr = cairo_create(maskSurf);
	cairo_set_source_rgba(cr, 0, 0, 0, surfPtr->opacity);
	cairo_new_sub_path(cr);
	cairo_arc(cr, r, r, r, M_PI, 1.5 * M_PI);
	cairo_arc(cr, 2 * size - r, r, r, 1.5 * M_PI, 2 * M_PI);
	cairo_arc(cr, 2 * size - r, 2 * size - r, r, 0, 0.5 * M_PI);
	cairo_arc(cr, r, 2 * size - r, r, 0.5 * M_PI, M_PI);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_destroy(cr);
	cairo_surface_flush(maskSurf);
	cairo_surface_destroy(maskSurf);
	surfPtr->cornerMaskRadius = r;
	surfPtr->cornerMaskOpacity = surfPtr->opacity;
    }
    for (i = 0; i < 4; i++) {
	int right = i & 1, bottom = i >> 1;
	int x = right ? width - size : 0, y = bottom ? height - size : 0;

	pixman_image_composite32(PIXMAN_OP_SRC, src, surfPtr->cornerMask,
		dst, x, y, right * size, bottom * size, x, y, size, size);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlSurfaceSetCorners --
 *
 *	Set the radius of the corners of a surface, and whether they are
 *	square for now (when the window is maximized, for instance).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The whole surface is presented again if something changed.
 *
 *----------------------------------------------------------------------
 */

void
TkWlSurfaceSetCorners(
    TkXeSurface *surfPtr,
    double radius,
    bool square)
{
    pixman_region32_t all;

    if (radius < 0.0) {
	radius = 0.0;
    }
    if (radius == surfPtr->cornerRadius && square == surfPtr->squareCorners) {
	return;
    }
    surfPtr->cornerRadius = radius;
    surfPtr->squareCorners = square;
    pixman_region32_init_rect(&all, 0, 0, (unsigned)surfPtr->width,
	    (unsigned)surfPtr->height);
    TkXeAddDamage(surfPtr, &all);
    pixman_region32_fini(&all);
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

/*
 *----------------------------------------------------------------------
 *
 * SchedulePresent --
 *
 *	Schedule the presentation of a surface at idle time.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An idle handler is created, unless one is pending.
 *
 *----------------------------------------------------------------------
 */

static void
SchedulePresent(
    TkXeSurface *surfPtr)
{
    if (!surfPtr->presentPending) {
	surfPtr->presentPending = true;
	Tcl_DoWhenIdle(PresentIdle, surfPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PresentIdle --
 *
 *	Idle handler which presents a surface, the second time it is called.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	See Present.
 *
 *----------------------------------------------------------------------
 */

static void
PresentIdle(
    void *clientData)
{
    TkXeSurface *surfPtr = (TkXeSurface *)clientData;

    if (!surfPtr->presentDeferred) {
	surfPtr->presentDeferred = true;
	Tcl_DoWhenIdle(PresentIdle, surfPtr);
	return;
    }
    surfPtr->presentDeferred = false;
    surfPtr->presentPending = false;
    Present(surfPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * FrameDone --
 *
 *	The compositor is ready for a new frame of a surface.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The surface is presented again if it was drawn into meanwhile.
 *
 *----------------------------------------------------------------------
 */

static void
FrameDone(
    void *data,
    struct wl_callback *callback,
    TCL_UNUSED(uint32_t))
{
    TkXeSurface *surfPtr = (TkXeSurface *)data;

    wl_callback_destroy(callback);
    surfPtr->frameCallback = NULL;
    if (surfPtr->committed && !surfPtr->presented) {
	surfPtr->presented = true;
	TkWlWmSurfacePresented(surfPtr);
    }
    surfPtr->presented = surfPtr->committed;
    if (pixman_region32_not_empty(&surfPtr->damage)) {
	SchedulePresent(surfPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * BufferRelease --
 *
 *	The compositor no longer uses a buffer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The buffer can be used again.
 *
 *----------------------------------------------------------------------
 */

static void
BufferRelease(
    void *data,
    TCL_UNUSED(struct wl_buffer *))
{
    ((TkWlBuffer *)data)->busy = false;
}

static const struct wl_buffer_listener bufferListener = {
    BufferRelease
};

/*
 *----------------------------------------------------------------------
 *
 * AllocBuffer --
 *
 *	Allocate a wl_shm buffer of the given size and format.
 *
 * Results:
 *	True if the buffer was allocated, false otherwise.
 *
 * Side effects:
 *	Shared memory is allocated and mapped, and a wl_buffer is created.
 *
 *----------------------------------------------------------------------
 */

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
    pool = wl_shm_create_pool(tkXeDisplay->shm, fd, (int32_t)size);
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

/*
 *----------------------------------------------------------------------
 *
 * Present --
 *
 *	Present a surface: bring a free buffer up to date with the backing
 *	image, and commit it with its damage.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The pending Expose events of the surface are generated first. The
 *	buffer is attached and committed, a frame callback is requested, and
 *	the presentation is retried later when no buffer is free.
 *
 *----------------------------------------------------------------------
 */

static void
Present(
    TkXeSurface *surfPtr)
{
    TkWlBuffer *bufPtr = NULL;
    int i, n, width, height;
    pixman_box32_t *boxes;
    pixman_image_t *src, *dst, *mask = NULL;
    double radius = CornerRadius(surfPtr);
    uint32_t format = (surfPtr->opacity < 1.0 || radius > 0.0)
	    ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;

    TkXeProcessExposures(surfPtr);
    if (!surfPtr->configured || surfPtr->backing == NULL
	    || (!surfPtr->winPtr->mapped && !surfPtr->minimizePending)) {
	/*
	 * A toplevel iconified before its first buffer is committed with it
	 * anyway, to be minimized (see Minimize in tkWaylandWm.c).
	 */

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
    if (surfPtr->opacity < 1.0) {
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
    if (radius > 0.0) {
	RoundCorners(surfPtr, src, dst, radius);
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
    wl_display_flush(tkXeDisplay->wlDisplay);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlSurfaceSetOpacity --
 *
 *	Set the opacity of a surface ([wm attributes -alpha]), which is
 *	applied when the backing image is copied to the buffers.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The whole surface is presented again if the opacity changed.
 *
 *----------------------------------------------------------------------
 */

void
TkWlSurfaceSetOpacity(
    TkXeSurface *surfPtr,
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
    TkXeAddDamage(surfPtr, &all);
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
 * Results:
 *	None.
 *
 * Side effects:
 *	The surface is presented.
 *
 *----------------------------------------------------------------------
 */

void
TkWlSurfaceConfigured(
    TkXeSurface *surfPtr)
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
 * TkXeSurfaceUnmapped --
 *
 *	Called when the window of a surface is unmapped (and not iconified):
 *	destroy the role of the surface, and unmap it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The surface no longer has a role nor a buffer.
 *
 *----------------------------------------------------------------------
 */

void
TkXeSurfaceUnmapped(
    TkXeSurface *surfPtr)	/* The surface whose window was unmapped. */
{
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

/*
 *----------------------------------------------------------------------
 *
 * TkXeFlush --
 *
 *	Send the requests made so far to the compositor.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The connection is flushed.
 *
 *----------------------------------------------------------------------
 */

void
TkXeFlush(void)
{
    if (tkXeDisplay) {
	wl_display_flush(tkXeDisplay->wlDisplay);
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
