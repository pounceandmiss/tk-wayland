/*
 * tkXemuDraw.c --
 *
 *	The drawing part of the X server emulation: pixmaps, images, and the
 *	drawing functions, which draw with Cairo into the backing image of
 *	the surface of a window or into a pixmap, following the rules of X11
 *	for which pixels are drawn.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkXemuInt.h"
#include "tkXemuDraw.h"

/*
 *----------------------------------------------------------------------
 *
 * Pixmaps --
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * PixmapSize --
 *
 *	Compute the size in pixels of the image of a pixmap: only whole
 *	pixels, which can be drawn entirely, so that copies never use pixels
 *	which nothing was drawn into.
 *
 * Results:
 *	The number of pixels, at least 1.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PixmapSize(
    unsigned int length,
    double scale)
{
    int size = (int)floor(length * scale + 1e-6);

    return (size > 0) ? size : 1;
}

/*
 *----------------------------------------------------------------------
 *
 * XCreatePixmap --
 *
 *	Create a pixmap, as a Cairo image of depth 1 (A8), 24 or 32. Pixmaps
 *	of color have the scale of the surface of the window they are made
 *	for.
 *
 * Results:
 *	The XID of the pixmap.
 *
 * Side effects:
 *	Allocates the pixmap and its image.
 *
 *----------------------------------------------------------------------
 */

Pixmap
XCreatePixmap(
    Display *display,
    Drawable d,
    unsigned int width, unsigned int height,
    unsigned int depth)
{
    TkXePixmap *pixPtr = (TkXePixmap *)Tcl_Alloc(sizeof(TkXePixmap));
    cairo_format_t format = (depth == 1) ? CAIRO_FORMAT_A8
	    : (depth == 32) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
    TkXeSurface *surfPtr = TkXeGetSurface(d);
    double scale = 1.0;

    TkXeNextRequest(display);
    if (width == 0) {
	width = 1;
    }
    if (height == 0) {
	height = 1;
    }
    pixPtr->type = TKXE_PIXMAP;
    pixPtr->width = (int)width;
    pixPtr->height = (int)height;
    pixPtr->depth = (depth == 1 || depth == 32) ? (int)depth : 24;

    /*
     * Pixmaps of color are mostly used for double buffering: they have the
     * scale of the surface of the window they are made for, so that what is
     * drawn in them has the resolution of the surface.
     */

    if (surfPtr && depth != 1) {
	scale = surfPtr->scale;
    }
    pixPtr->surface = cairo_image_surface_create(format,
	    PixmapSize(width, scale), PixmapSize(height, scale));
    cairo_surface_set_device_scale(pixPtr->surface, scale, scale);
    pixPtr->id = TkXeNewId(pixPtr);
    return pixPtr->id;
}

/*
 *----------------------------------------------------------------------
 *
 * XFreePixmap --
 *
 *	Free a pixmap.
 *
 * Results:
 *	Success, or BadPixmap if the pixmap does not exist.
 *
 * Side effects:
 *	The pixmap and its image are freed, and its XID is released.
 *
 *----------------------------------------------------------------------
 */

int
XFreePixmap(
    Display *display,
    Pixmap pixmap)
{
    TkXePixmap *pixPtr = TkXeGetPixmap(pixmap);

    TkXeNextRequest(display);
    if (pixPtr == NULL) {
	return BadPixmap;
    }
    TkXeFreeId(pixmap);
    cairo_surface_destroy(pixPtr->surface);
    Tcl_Free(pixPtr);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXePixmapSurface --
 *
 *	Get the Cairo image of a pixmap.
 *
 * Results:
 *	The image, or NULL if the XID is not that of a pixmap.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

cairo_surface_t *
TkXePixmapSurface(
    Pixmap pixmap)
{
    TkXePixmap *pixPtr = TkXeGetPixmap(pixmap);

    return pixPtr ? pixPtr->surface : NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * SetSourcePixel --
 *
 *	Set the source of a Cairo context to the color of a pixel: for depth 1
 *	the alpha of the A8 images of bitmaps, otherwise 0xRRGGBB.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The source of the context is changed.
 *
 *----------------------------------------------------------------------
 */

static void
SetSourcePixel(
    cairo_t *cr,
    int depth,
    unsigned long pixel)
{
    if (depth == 1) {
	cairo_set_source_rgba(cr, 0, 0, 0, (pixel & 1) ? 1.0 : 0.0);
    } else {
	cairo_set_source_rgb(cr, ((pixel >> 16) & 0xFF) / 255.0,
		((pixel >> 8) & 0xFF) / 255.0, (pixel & 0xFF) / 255.0);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeBeginDraw --
 *
 *	Prepare a Cairo context to draw in a drawable: the user space is that
 *	of the drawable, and the context is clipped to the visible region of
 *	the window and to the clip region of the GC. If the GC has a clip
 *	mask, drawing is done in a group which is masked by TkXeEndDraw.
 *
 * Results:
 *	True if drawing can be done, false if nothing can be drawn (the
 *	drawable does not exist or is not visible, or the clip is empty).
 *
 * Side effects:
 *	Pending exposures of the surface of a window are processed. When the
 *	result is true, *ctxPtr holds a new Cairo context and a clip region,
 *	which TkXeEndDraw frees.
 *
 *----------------------------------------------------------------------
 */

bool
TkXeBeginDraw(
    Drawable d,
    GC gc,
    TkXeDrawContext *ctxPtr)
{
    void *p = TkXeLookupId(d);
    TkpClipMask *clipPtr = gc ? (TkpClipMask *)gc->clip_mask : NULL;
    int i, n;
    pixman_box32_t *boxes;

    memset(ctxPtr, 0, sizeof(TkXeDrawContext));
    pixman_region32_init(&ctxPtr->clip);
    if (p == NULL) {
	return false;
    }
    if (*(int *)p == TKXE_WINDOW) {
	TkXeWindow *winPtr = (TkXeWindow *)p, *topPtr = TkXeTopWindow(winPtr);

	if (topPtr == NULL || topPtr->surface == NULL
		|| !TkXeIsViewable(winPtr)) {
	    return false;
	}
	ctxPtr->surfPtr = topPtr->surface;
	TkXeProcessExposures(ctxPtr->surfPtr);
	if (ctxPtr->surfPtr->backing == NULL) {
	    return false;
	}
	ctxPtr->target = ctxPtr->surfPtr->backing;
	ctxPtr->depth = 24;
	TkXeWindowOrigin(winPtr, &ctxPtr->x, &ctxPtr->y);
	TkXeVisibleRegion(winPtr,
		gc && gc->subwindow_mode == IncludeInferiors, &ctxPtr->clip);
    } else {
	TkXePixmap *pixPtr = (TkXePixmap *)p;

	ctxPtr->target = pixPtr->surface;
	ctxPtr->depth = pixPtr->depth;
	pixman_region32_fini(&ctxPtr->clip);
	pixman_region32_init_rect(&ctxPtr->clip, 0, 0,
		(unsigned)pixPtr->width, (unsigned)pixPtr->height);
    }

    if (clipPtr && clipPtr->type == TKP_CLIP_REGION) {
	pixman_region32_t rgn;

	pixman_region32_init(&rgn);
	pixman_region32_copy(&rgn, TkXePixmanRegion(clipPtr->value.region));
	pixman_region32_translate(&rgn, ctxPtr->x + gc->clip_x_origin,
		ctxPtr->y + gc->clip_y_origin);
	pixman_region32_intersect(&ctxPtr->clip, &ctxPtr->clip, &rgn);
	pixman_region32_fini(&rgn);
    }
    if (!pixman_region32_not_empty(&ctxPtr->clip)) {
	pixman_region32_fini(&ctxPtr->clip);
	return false;
    }

    ctxPtr->cr = cairo_create(ctxPtr->target);

    /*
     * Clip without antialiasing: with a fractional scale, the edges of the
     * clip fall between pixels, which must be drawn entirely or not.
     */

    cairo_set_antialias(ctxPtr->cr, CAIRO_ANTIALIAS_NONE);
    boxes = pixman_region32_rectangles(&ctxPtr->clip, &n);
    for (i = 0; i < n; i++) {
	cairo_rectangle(ctxPtr->cr, boxes[i].x1, boxes[i].y1,
		boxes[i].x2 - boxes[i].x1, boxes[i].y2 - boxes[i].y1);
    }
    cairo_clip(ctxPtr->cr);
    cairo_translate(ctxPtr->cr, ctxPtr->x, ctxPtr->y);
    cairo_set_antialias(ctxPtr->cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_operator(ctxPtr->cr, (ctxPtr->depth == 1)
	    ? CAIRO_OPERATOR_SOURCE : CAIRO_OPERATOR_OVER);

    if (gc) {
	switch (gc->function) {
	case GXxor:
	case GXinvert:
	case GXequiv:
	    cairo_set_operator(ctxPtr->cr, CAIRO_OPERATOR_DIFFERENCE);
	    break;
	case GXclear:
	    cairo_set_operator(ctxPtr->cr, CAIRO_OPERATOR_CLEAR);
	    break;
	default:
	    break;
	}
	if (clipPtr && clipPtr->type == TKP_CLIP_PIXMAP
		&& TkXePixmapSurface(clipPtr->value.pixmap)) {
	    ctxPtr->mask = TkXePixmapSurface(clipPtr->value.pixmap);
	    ctxPtr->maskX = gc->clip_x_origin;
	    ctxPtr->maskY = gc->clip_y_origin;
	    cairo_push_group(ctxPtr->cr);
	}
	TkXeSetSource(ctxPtr, gc, gc->foreground);
    }
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeSetSource --
 *
 *	Set the source of a drawing context from a pixel and the fill style of
 *	a GC.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The source of the Cairo context is changed, and the stipple of the
 *	drawing context is set from the GC.
 *
 *----------------------------------------------------------------------
 */

void
TkXeSetSource(
    TkXeDrawContext *ctxPtr,
    GC gc,
    unsigned long pixel)
{
    cairo_t *cr = ctxPtr->cr;

    if (gc->function == GXinvert) {
	pixel = 0xFFFFFF;
    } else if (gc->function == GXxor || gc->function == GXequiv) {
	pixel ^= (gc->function == GXequiv) ? 0xFFFFFF : 0;
    }
    ctxPtr->stipple = NULL;
    if (gc->fill_style == FillTiled && TkXePixmapSurface(gc->tile)) {
	cairo_pattern_t *pattern = cairo_pattern_create_for_surface(
		TkXePixmapSurface(gc->tile));
	cairo_matrix_t matrix;

	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
	cairo_matrix_init_translate(&matrix, -gc->ts_x_origin,
		-gc->ts_y_origin);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_set_source(cr, pattern);
	cairo_pattern_destroy(pattern);
	return;
    }
    SetSourcePixel(cr, ctxPtr->depth, pixel);
    if ((gc->fill_style == FillStippled || gc->fill_style == FillOpaqueStippled)
	    && TkXePixmapSurface(gc->stipple)) {
	ctxPtr->stipple = TkXePixmapSurface(gc->stipple);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * StippleMask --
 *
 *	Paint the source of a drawing context through the stipple of the GC,
 *	over the background of the GC for FillOpaqueStippled.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Draws in the target of the context.
 *
 *----------------------------------------------------------------------
 */

static void
StippleMask(
    TkXeDrawContext *ctxPtr,
    GC gc)
{
    cairo_t *cr = ctxPtr->cr;
    cairo_pattern_t *pattern = cairo_pattern_create_for_surface(
	    ctxPtr->stipple);
    cairo_matrix_t matrix;

    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
    cairo_matrix_init_translate(&matrix, -gc->ts_x_origin, -gc->ts_y_origin);
    cairo_pattern_set_matrix(pattern, &matrix);
    if (gc->fill_style == FillOpaqueStippled) {
	cairo_pattern_t *fg = cairo_get_source(cr);

	cairo_pattern_reference(fg);
	SetSourcePixel(cr, ctxPtr->depth, gc->background);
	cairo_paint(cr);
	cairo_set_source(cr, fg);
	cairo_pattern_destroy(fg);
    }
    cairo_mask(cr, pattern);
    cairo_pattern_destroy(pattern);
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeFill --
 *
 *	Fill the current path of a drawing context, applying the stipple of
 *	the GC.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Draws in the target of the context; the path is consumed.
 *
 *----------------------------------------------------------------------
 */

void
TkXeFill(
    TkXeDrawContext *ctxPtr,
    GC gc)
{
    cairo_t *cr = ctxPtr->cr;

    if (ctxPtr->stipple == NULL) {
	cairo_fill(cr);
	return;
    }
    cairo_save(cr);
    cairo_clip(cr);
    StippleMask(ctxPtr, gc);
    cairo_restore(cr);
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeStroke --
 *
 *	Stroke the current path of a drawing context, applying the stipple of
 *	the GC: a stippled stroke is turned into a mask, through which the
 *	stipple is painted.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Draws in the target of the context; the path is consumed.
 *
 *----------------------------------------------------------------------
 */

void
TkXeStroke(
    TkXeDrawContext *ctxPtr,
    GC gc)
{
    cairo_t *cr = ctxPtr->cr;

    if (ctxPtr->stipple == NULL) {
	cairo_stroke(cr);
	return;
    }

    /*
     * Turn the stroke into a fill, so that it can be stippled.
     */

    cairo_save(cr);
    {
	cairo_surface_t *maskSurface;
	cairo_t *maskCr;
	double x1, y1, x2, y2;
	cairo_path_t *path = cairo_copy_path(cr);

	cairo_stroke_extents(cr, &x1, &y1, &x2, &y2);
	cairo_new_path(cr);
	maskSurface = cairo_surface_create_similar(cairo_get_target(cr),
		CAIRO_CONTENT_ALPHA, (int)ceil(x2 - x1) + 2,
		(int)ceil(y2 - y1) + 2);
	maskCr = cairo_create(maskSurface);
	cairo_set_antialias(maskCr, CAIRO_ANTIALIAS_NONE);
	cairo_translate(maskCr, -floor(x1) + 1, -floor(y1) + 1);
	cairo_append_path(maskCr, path);
	cairo_set_line_width(maskCr, cairo_get_line_width(cr));
	cairo_set_line_cap(maskCr, cairo_get_line_cap(cr));
	cairo_set_line_join(maskCr, cairo_get_line_join(cr));
	cairo_stroke(maskCr);
	cairo_destroy(maskCr);
	cairo_path_destroy(path);
	cairo_rectangle(cr, floor(x1) - 1, floor(y1) - 1,
		ceil(x2 - x1) + 2, ceil(y2 - y1) + 2);
	cairo_clip(cr);
	cairo_push_group(cr);
	StippleMask(ctxPtr, gc);
	cairo_pop_group_to_source(cr);
	cairo_mask_surface(cr, maskSurface, floor(x1) - 1, floor(y1) - 1);
	cairo_surface_destroy(maskSurface);
    }
    cairo_restore(cr);
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeEndDraw --
 *
 *	Finish drawing: apply the clip mask of the GC, and record the damage,
 *	which is the given rectangle (in the coordinates of the drawable)
 *	clipped to the visible region.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The Cairo context and the clip region of the drawing context are
 *	freed, and the damage is added to the surface, which will be
 *	presented.
 *
 *----------------------------------------------------------------------
 */

void
TkXeEndDraw(
    TkXeDrawContext *ctxPtr,
    int x, int y, int width, int height)
{
    if (ctxPtr->cr == NULL) {
	return;
    }
    if (ctxPtr->mask) {
	cairo_pop_group_to_source(ctxPtr->cr);
	cairo_mask_surface(ctxPtr->cr, ctxPtr->mask, ctxPtr->maskX,
		ctxPtr->maskY);
    }
    cairo_destroy(ctxPtr->cr);
    ctxPtr->cr = NULL;
    if (ctxPtr->surfPtr && width > 0 && height > 0) {
	pixman_region32_intersect_rect(&ctxPtr->clip, &ctxPtr->clip,
		x + ctxPtr->x, y + ctxPtr->y, (unsigned)width,
		(unsigned)height);
	TkXeAddDamage(ctxPtr->surfPtr, &ctxPtr->clip);
    }
    pixman_region32_fini(&ctxPtr->clip);
}

/*
 *----------------------------------------------------------------------
 *
 * Lines --
 *
 *	X11 draws lines through the centers of pixels; lines of width 0 or 1
 *	include their end points unless the cap style is CapNotLast.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * SetLineAttributes --
 *
 *	Set the line width, cap and join styles and dashes of a Cairo context
 *	from a GC. X11 draws lines through the centers of pixels; lines of
 *	width 0 or 1 include their end points unless the cap style is
 *	CapNotLast.
 *
 * Results:
 *	The offset to add to coordinates so that lines of odd width go through
 *	the centers of pixels: 0.5 or 0.
 *
 * Side effects:
 *	The line attributes of the context are changed.
 *
 *----------------------------------------------------------------------
 */

static double
SetLineAttributes(
    cairo_t *cr,
    GC gc)
{
    int width = gc->line_width;
    const char *dashes = &gc->dashes;

    cairo_set_line_width(cr, (width > 0) ? width : 1);
    if (width <= 1) {
	cairo_set_line_cap(cr, (gc->cap_style == CapNotLast)
		? CAIRO_LINE_CAP_BUTT : CAIRO_LINE_CAP_SQUARE);
    } else {
	cairo_set_line_cap(cr, (gc->cap_style == CapRound)
		? CAIRO_LINE_CAP_ROUND : (gc->cap_style == CapProjecting)
		? CAIRO_LINE_CAP_SQUARE : CAIRO_LINE_CAP_BUTT);
    }
    cairo_set_line_join(cr, (gc->join_style == JoinRound)
	    ? CAIRO_LINE_JOIN_ROUND : (gc->join_style == JoinBevel)
	    ? CAIRO_LINE_JOIN_BEVEL : CAIRO_LINE_JOIN_MITER);
    if (gc->line_style != LineSolid && dashes[0] != 0) {
	double d[16];
	int n = 0;

	while (n < 16 && dashes[n]) {
	    d[n] = (unsigned char)dashes[n];
	    n++;
	}
	cairo_set_dash(cr, d, n, gc->dash_offset);
    }

    /*
     * Lines of odd width go through the centers of pixels.
     */

    return ((width <= 1) || (width & 1)) ? 0.5 : 0.0;
}

/*
 *----------------------------------------------------------------------
 *
 * LineBounds --
 *
 *	Compute the bounding box of a polyline, including the width of its
 *	lines.
 *
 * Results:
 *	1 if there are points, 0 otherwise.
 *
 * Side effects:
 *	Fills *boxPtr.
 *
 *----------------------------------------------------------------------
 */

static int
LineBounds(
    XPoint *points,
    int n,
    int lineWidth,
    XRectangle *boxPtr)
{
    int i, x1, y1, x2, y2, pad = (lineWidth > 1) ? lineWidth : 1;

    if (n < 1) {
	return 0;
    }
    x1 = x2 = points[0].x;
    y1 = y2 = points[0].y;
    for (i = 1; i < n; i++) {
	if (points[i].x < x1) x1 = points[i].x;
	if (points[i].x > x2) x2 = points[i].x;
	if (points[i].y < y1) y1 = points[i].y;
	if (points[i].y > y2) y2 = points[i].y;
    }
    boxPtr->x = (short)(x1 - pad);
    boxPtr->y = (short)(y1 - pad);
    boxPtr->width = (unsigned short)(x2 - x1 + 2 * pad + 1);
    boxPtr->height = (unsigned short)(y2 - y1 + 2 * pad + 1);
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * XDrawLines --
 *
 *	Draw connected lines through points, given in absolute coordinates or
 *	(CoordModePrevious) relative to the previous point.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XDrawLines(
    Display *display,
    Drawable d,
    GC gc,
    XPoint *points,
    int npoints,
    int mode)
{
    TkXeDrawContext ctx;
    XPoint *absPoints = points;
    XRectangle box;
    double offset;
    int i;

    TkXeNextRequest(display);
    if (npoints < 1 || !TkXeBeginDraw(d, gc, &ctx)) {
	return Success;
    }
    if (mode == CoordModePrevious) {
	absPoints = (XPoint *)Tcl_Alloc(npoints * sizeof(XPoint));
	absPoints[0] = points[0];
	for (i = 1; i < npoints; i++) {
	    absPoints[i].x = absPoints[i - 1].x + points[i].x;
	    absPoints[i].y = absPoints[i - 1].y + points[i].y;
	}
    }
    offset = SetLineAttributes(ctx.cr, gc);
    cairo_move_to(ctx.cr, absPoints[0].x + offset, absPoints[0].y + offset);
    for (i = 1; i < npoints; i++) {
	cairo_line_to(ctx.cr, absPoints[i].x + offset,
		absPoints[i].y + offset);
    }
    if (npoints == 1) {
	cairo_line_to(ctx.cr, absPoints[0].x + offset,
		absPoints[0].y + offset);
    }
    TkXeStroke(&ctx, gc);
    LineBounds(absPoints, npoints, gc->line_width, &box);
    if (absPoints != points) {
	Tcl_Free(absPoints);
    }
    TkXeEndDraw(&ctx, box.x, box.y, box.width, box.height);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XDrawSegments --
 *
 *	Draw unconnected line segments.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XDrawSegments(
    Display *display,
    Drawable d,
    GC gc,
    XSegment *segments,
    int nsegments)
{
    int i;

    for (i = 0; i < nsegments; i++) {
	XPoint points[2];

	points[0].x = segments[i].x1;
	points[0].y = segments[i].y1;
	points[1].x = segments[i].x2;
	points[1].y = segments[i].y2;
	XDrawLines(display, d, gc, points, 2, CoordModeOrigin);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XDrawRectangle --
 *
 *	Draw the outline of a rectangle, which covers width + 1 by height + 1
 *	pixels, as in X11.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XDrawRectangle(
    Display *display,
    Drawable d,
    GC gc,
    int x, int y,
    unsigned int width, unsigned int height)
{
    TkXeDrawContext ctx;
    double offset;
    int pad = (gc->line_width > 1) ? gc->line_width : 1;

    TkXeNextRequest(display);
    if (!TkXeBeginDraw(d, gc, &ctx)) {
	return Success;
    }
    offset = SetLineAttributes(ctx.cr, gc);
    cairo_set_line_cap(ctx.cr, CAIRO_LINE_CAP_SQUARE);
    if (width == 0 || height == 0) {
	cairo_move_to(ctx.cr, x + offset, y + offset);
	cairo_line_to(ctx.cr, x + width + offset, y + height + offset);
    } else {
	cairo_rectangle(ctx.cr, x + offset, y + offset, width, height);
    }
    TkXeStroke(&ctx, gc);
    TkXeEndDraw(&ctx, x - pad, y - pad, (int)width + 2 * pad + 1,
	    (int)height + 2 * pad + 1);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XDrawRectangles --
 *
 *	Draw the outlines of rectangles.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XDrawRectangles(
    Display *display,
    Drawable d,
    GC gc,
    XRectangle *rects,
    int nrects)
{
    int i;

    for (i = 0; i < nrects; i++) {
	XDrawRectangle(display, d, gc, rects[i].x, rects[i].y,
		rects[i].width, rects[i].height);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XFillRectangles --
 *
 *	Fill rectangles.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XFillRectangles(
    Display *display,
    Drawable d,
    GC gc,
    XRectangle *rects,
    int nrects)
{
    TkXeDrawContext ctx;
    int i, x1, y1, x2, y2;

    TkXeNextRequest(display);
    if (nrects < 1 || !TkXeBeginDraw(d, gc, &ctx)) {
	return Success;
    }
    x1 = y1 = INT_MAX;
    x2 = y2 = INT_MIN;
    for (i = 0; i < nrects; i++) {
	cairo_rectangle(ctx.cr, rects[i].x, rects[i].y, rects[i].width,
		rects[i].height);
	if (rects[i].x < x1) {
	    x1 = rects[i].x;
	}
	if (rects[i].y < y1) {
	    y1 = rects[i].y;
	}
	if (rects[i].x + rects[i].width > x2) {
	    x2 = rects[i].x + rects[i].width;
	}
	if (rects[i].y + rects[i].height > y2) {
	    y2 = rects[i].y + rects[i].height;
	}
    }
    TkXeFill(&ctx, gc);
    TkXeEndDraw(&ctx, x1, y1, x2 - x1, y2 - y1);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * Arcs --
 *
 *	Angles are in 64ths of a degree, counterclockwise from three o'clock.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * ArcPath --
 *
 *	Add an elliptical arc to the path of a Cairo context. Angles are in
 *	64ths of a degree, counterclockwise from three o'clock.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The path of the context is extended; nothing is added if the ellipse
 *	is empty.
 *
 *----------------------------------------------------------------------
 */

static void
ArcPath(
    cairo_t *cr,
    double x, double y,
    double width, double height,
    int angle1, int angle2)
{
    double a1 = -angle1 / 64.0 * M_PI / 180.0;
    double a2 = -(angle1 + angle2) / 64.0 * M_PI / 180.0;

    if (width <= 0 || height <= 0) {
	return;
    }
    cairo_save(cr);
    cairo_translate(cr, x + width / 2.0, y + height / 2.0);
    cairo_scale(cr, width / 2.0, height / 2.0);
    if (angle2 >= 0) {
	cairo_arc_negative(cr, 0, 0, 1, a1, a2);
    } else {
	cairo_arc(cr, 0, 0, 1, a1, a2);
    }
    cairo_restore(cr);
}

/*
 *----------------------------------------------------------------------
 *
 * XDrawArc --
 *
 *	Draw an elliptical arc.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XDrawArc(
    Display *display,
    Drawable d,
    GC gc,
    int x, int y,
    unsigned int width, unsigned int height,
    int angle1, int angle2)
{
    TkXeDrawContext ctx;
    double offset;
    int pad = (gc->line_width > 1) ? gc->line_width : 1;

    TkXeNextRequest(display);
    if (!TkXeBeginDraw(d, gc, &ctx)) {
	return Success;
    }
    offset = SetLineAttributes(ctx.cr, gc);
    cairo_new_path(ctx.cr);
    ArcPath(ctx.cr, x + offset, y + offset, width, height, angle1, angle2);
    TkXeStroke(&ctx, gc);
    TkXeEndDraw(&ctx, x - pad, y - pad, (int)width + 2 * pad + 1,
	    (int)height + 2 * pad + 1);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XDrawArcs --
 *
 *	Draw elliptical arcs.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XDrawArcs(
    Display *display,
    Drawable d,
    GC gc,
    XArc *arcs,
    int narcs)
{
    int i;

    for (i = 0; i < narcs; i++) {
	XDrawArc(display, d, gc, arcs[i].x, arcs[i].y, arcs[i].width,
		arcs[i].height, arcs[i].angle1, arcs[i].angle2);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XFillArc --
 *
 *	Fill an elliptical arc, as a pie slice or a chord according to the arc
 *	mode of the GC.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XFillArc(
    Display *display,
    Drawable d,
    GC gc,
    int x, int y,
    unsigned int width, unsigned int height,
    int angle1, int angle2)
{
    TkXeDrawContext ctx;

    TkXeNextRequest(display);
    if (!TkXeBeginDraw(d, gc, &ctx)) {
	return Success;
    }
    cairo_new_path(ctx.cr);
    if (gc->arc_mode == ArcPieSlice
	    && (angle2 < 360 * 64 && angle2 > -360 * 64)) {
	cairo_move_to(ctx.cr, x + width / 2.0, y + height / 2.0);
    }
    ArcPath(ctx.cr, x, y, width, height, angle1, angle2);
    cairo_close_path(ctx.cr);
    TkXeFill(&ctx, gc);
    TkXeEndDraw(&ctx, x, y, (int)width + 1, (int)height + 1);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XFillArcs --
 *
 *	Fill elliptical arcs.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XFillArcs(
    Display *display,
    Drawable d,
    GC gc,
    XArc *arcs,
    int narcs)
{
    int i;

    for (i = 0; i < narcs; i++) {
	XFillArc(display, d, gc, arcs[i].x, arcs[i].y, arcs[i].width,
		arcs[i].height, arcs[i].angle1, arcs[i].angle2);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XFillPolygon --
 *
 *	Fill a polygon, with the fill rule of the GC.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XFillPolygon(
    Display *display,
    Drawable d,
    GC gc,
    XPoint *points,
    int npoints,
    TCL_UNUSED(int),		/* shape */
    int mode)
{
    TkXeDrawContext ctx;
    XRectangle box;
    int i, x, y;

    TkXeNextRequest(display);
    if (npoints < 2 || !TkXeBeginDraw(d, gc, &ctx)) {
	return Success;
    }
    cairo_set_fill_rule(ctx.cr, (gc->fill_rule == WindingRule)
	    ? CAIRO_FILL_RULE_WINDING : CAIRO_FILL_RULE_EVEN_ODD);
    x = points[0].x;
    y = points[0].y;
    cairo_move_to(ctx.cr, x, y);
    box.x = box.width = (short)x;
    box.y = box.height = (short)y;
    for (i = 1; i < npoints; i++) {
	if (mode == CoordModePrevious) {
	    x += points[i].x;
	    y += points[i].y;
	} else {
	    x = points[i].x;
	    y = points[i].y;
	}
	cairo_line_to(ctx.cr, x, y);
	if (x < box.x) box.x = (short)x;
	if (y < box.y) box.y = (short)y;
	if (x > (short)box.width) box.width = (unsigned short)x;
	if (y > (short)box.height) box.height = (unsigned short)y;
    }
    cairo_close_path(ctx.cr);
    TkXeFill(&ctx, gc);
    TkXeEndDraw(&ctx, box.x, box.y, (short)box.width - box.x + 1,
	    (short)box.height - box.y + 1);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXePaintBackground --
 *
 *	Paint a region of a window (in surface coordinates) with its
 *	background, as the X server does when the region is exposed or
 *	cleared.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Draws in the backing image of the surface, and adds the region to its
 *	damage.
 *
 *----------------------------------------------------------------------
 */

void
TkXePaintBackground(
    TkXeWindow *winPtr,
    pixman_region32_t *rgn)
{
    TkXeWindow *topPtr = TkXeTopWindow(winPtr), *bgPtr = winPtr;
    TkXeSurface *surfPtr;
    cairo_t *cr;
    pixman_box32_t *boxes;
    int i, n, x, y;

    while (bgPtr->backgroundType == BG_PARENT && bgPtr->parent
	    && bgPtr->parent->parent) {
	bgPtr = bgPtr->parent;
    }
    if (topPtr == NULL || (surfPtr = topPtr->surface) == NULL
	    || surfPtr->backing == NULL || bgPtr->backgroundType == BG_NONE
	    || bgPtr->backgroundType == BG_PARENT
	    || !pixman_region32_not_empty(rgn)) {
	return;
    }
    cr = cairo_create(surfPtr->backing);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    boxes = pixman_region32_rectangles(rgn, &n);
    for (i = 0; i < n; i++) {
	cairo_rectangle(cr, boxes[i].x1, boxes[i].y1,
		boxes[i].x2 - boxes[i].x1, boxes[i].y2 - boxes[i].y1);
    }
    cairo_clip(cr);
    if (bgPtr->backgroundType == BG_PIXMAP
	    && TkXePixmapSurface(bgPtr->backgroundPixmap)) {
	cairo_pattern_t *pattern = cairo_pattern_create_for_surface(
		TkXePixmapSurface(bgPtr->backgroundPixmap));
	cairo_matrix_t matrix;

	TkXeWindowOrigin(bgPtr, &x, &y);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
	cairo_matrix_init_translate(&matrix, -x, -y);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_set_source(cr, pattern);
	cairo_pattern_destroy(pattern);
    } else {
	SetSourcePixel(cr, 24, bgPtr->backgroundPixel);
    }
    cairo_paint(cr);
    cairo_destroy(cr);
    TkXeAddDamage(surfPtr, rgn);
}

/*
 *----------------------------------------------------------------------
 *
 * SourceSurface --
 *
 *	Find the Cairo image which holds the pixels of a drawable, to copy
 *	from it.
 *
 * Results:
 *	The image of the pixmap or the backing image of the surface of the
 *	window, or NULL. *xPtr and *yPtr are set to the position of the
 *	drawable in the image, and *winPtrPtr to the window, or NULL for a
 *	pixmap.
 *
 * Side effects:
 *	Pending exposures of the surface of a window are processed.
 *
 *----------------------------------------------------------------------
 */

static cairo_surface_t *
SourceSurface(
    Drawable src,
    int *xPtr, int *yPtr,
    TkXeWindow **winPtrPtr)
{
    void *p = TkXeLookupId(src);

    *xPtr = *yPtr = 0;
    *winPtrPtr = NULL;
    if (p == NULL) {
	return NULL;
    }
    if (*(int *)p == TKXE_PIXMAP) {
	return ((TkXePixmap *)p)->surface;
    } else {
	TkXeWindow *winPtr = (TkXeWindow *)p, *topPtr = TkXeTopWindow(winPtr);

	if (topPtr == NULL || topPtr->surface == NULL) {
	    return NULL;
	}
	TkXeProcessExposures(topPtr->surface);
	*winPtrPtr = winPtr;
	TkXeWindowOrigin(winPtr, xPtr, yPtr);
	return topPtr->surface->backing;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * QueueGraphicsExposures --
 *
 *	Queue the events which tell the application what a copy from a
 *	drawable could not do: GraphicsExpose events for the parts of the
 *	destination whose source was not visible (the whole destination if
 *	allLost is true), or else a NoExpose event.
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
QueueGraphicsExposures(
    Display *display,
    Drawable dst,
    TkXeWindow *srcWinPtr,
    int srcX, int srcY,
    unsigned int width, unsigned int height,
    int dstX, int dstY,
    int majorCode,
    bool allLost)		/* The whole area is exposed. */
{
    XEvent event;
    pixman_region32_t lost;
    int n = 0;

    memset(&event, 0, sizeof(XEvent));
    event.xany.display = display;
    pixman_region32_init(&lost);
    if (allLost) {
	pixman_region32_union_rect(&lost, &lost, dstX, dstY, width, height);
    } else if (srcWinPtr) {
	pixman_region32_t visible;
	int x, y;

	pixman_region32_init(&visible);
	TkXeVisibleRegion(srcWinPtr, true, &visible);
	TkXeWindowOrigin(srcWinPtr, &x, &y);
	pixman_region32_translate(&visible, -x, -y);
	pixman_region32_init_rect(&lost, srcX, srcY, width, height);
	pixman_region32_subtract(&lost, &lost, &visible);
	pixman_region32_translate(&lost, dstX - srcX, dstY - srcY);
	pixman_region32_fini(&visible);
    }
    if (pixman_region32_not_empty(&lost)) {
	pixman_box32_t *boxes = pixman_region32_rectangles(&lost, &n);
	int i;

	for (i = 0; i < n; i++) {
	    event.xgraphicsexpose.type = GraphicsExpose;
	    event.xgraphicsexpose.drawable = dst;
	    event.xgraphicsexpose.x = boxes[i].x1;
	    event.xgraphicsexpose.y = boxes[i].y1;
	    event.xgraphicsexpose.width = boxes[i].x2 - boxes[i].x1;
	    event.xgraphicsexpose.height = boxes[i].y2 - boxes[i].y1;
	    event.xgraphicsexpose.count = n - i - 1;
	    event.xgraphicsexpose.major_code = majorCode;
	    TkXeQueueEvent(&event);
	}
    } else {
	event.xnoexpose.type = NoExpose;
	event.xnoexpose.drawable = dst;
	event.xnoexpose.major_code = majorCode;
	TkXeQueueEvent(&event);
    }
    pixman_region32_fini(&lost);
}

/*
 *----------------------------------------------------------------------
 *
 * XCopyArea --
 *
 *	Copy an area between drawables. If the source is a window and parts of
 *	the area are not visible, GraphicsExpose events are generated for
 *	them, otherwise a NoExpose event, if the GC asks for them. Scrolling
 *	at a fractional scale is reported as an exposure of the destination
 *	instead.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the destination, and may queue GraphicsExpose or NoExpose
 *	events.
 *
 *----------------------------------------------------------------------
 */

int
XCopyArea(
    Display *display,
    Drawable src,
    Drawable dst,
    GC gc,
    int srcX, int srcY,
    unsigned int width, unsigned int height,
    int dstX, int dstY)
{
    TkXeDrawContext ctx;
    TkXeWindow *srcWinPtr;
    cairo_surface_t *srcSurface;
    int sx, sy;

    TkXeNextRequest(display);
    srcSurface = SourceSurface(src, &sx, &sy, &srcWinPtr);
    if (srcSurface && TkXeBeginDraw(dst, gc, &ctx)) {
	cairo_surface_t *copy = NULL;

	cairo_set_operator(ctx.cr, (gc->function == GXxor)
		? CAIRO_OPERATOR_DIFFERENCE : CAIRO_OPERATOR_SOURCE);
	if (srcSurface == ctx.target && gc->graphics_exposures
		&& ctx.surfPtr && !TkXeIntegralScale(ctx.surfPtr)) {
	    /*
	     * Scrolling at a fractional scale would move the pixels by a
	     * fraction of a pixel: report the destination as exposed
	     * instead, so that it is redrawn.
	     */

	    cairo_destroy(ctx.cr);
	    ctx.cr = NULL;
	    pixman_region32_fini(&ctx.clip);
	    QueueGraphicsExposures(display, dst, NULL, srcX, srcY, width,
		    height, dstX, dstY, 62 /* X_CopyArea */, true);
	    return Success;
	} else if (srcSurface == ctx.target) {
	    /*
	     * Copy within the same image (scrolling): go through a copy of
	     * the area, since the areas may overlap.
	     */

	    cairo_t *cr;
	    double scale = 1.0;

	    cairo_surface_get_device_scale(srcSurface, &scale, &scale);
	    copy = cairo_surface_create_similar_image(srcSurface,
		    cairo_image_surface_get_format(srcSurface),
		    (int)(width * scale), (int)(height * scale));
	    cairo_surface_set_device_scale(copy, scale, scale);
	    cr = cairo_create(copy);
	    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	    cairo_set_source_surface(cr, srcSurface, -(sx + srcX),
		    -(sy + srcY));
	    cairo_paint(cr);
	    cairo_destroy(cr);
	    cairo_set_source_surface(ctx.cr, copy, dstX, dstY);
	} else {
	    double srcScale, dstScale, dummy;

	    cairo_set_source_surface(ctx.cr, srcSurface, dstX - srcX - sx,
		    dstY - srcY - sy);
	    cairo_surface_get_device_scale(srcSurface, &srcScale, &dummy);
	    cairo_surface_get_device_scale(ctx.target, &dstScale, &dummy);
	    if (srcScale == dstScale) {
		/*
		 * A copy of pixels: with a fractional scale the position may
		 * fall between pixels, which must not blur them.
		 */

		cairo_pattern_set_filter(cairo_get_source(ctx.cr),
			CAIRO_FILTER_NEAREST);
	    }
	    cairo_pattern_set_extend(cairo_get_source(ctx.cr),
		    CAIRO_EXTEND_PAD);
	}
	cairo_rectangle(ctx.cr, dstX, dstY, width, height);
	cairo_fill(ctx.cr);
	TkXeEndDraw(&ctx, dstX, dstY, (int)width, (int)height);
	if (copy) {
	    cairo_surface_destroy(copy);
	}
    }
    if (gc->graphics_exposures) {
	QueueGraphicsExposures(display, dst, srcWinPtr, srcX, srcY, width,
		height, dstX, dstY, 62 /* X_CopyArea */, false);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * PlaneMask --
 *
 *	Make an A8 mask of the pixels of an image in which a plane is set.
 *
 * Results:
 *	The mask (a new reference to the image itself if it is already A8),
 *	which the caller destroys.
 *
 * Side effects:
 *	May allocate an image.
 *
 *----------------------------------------------------------------------
 */

static cairo_surface_t *
PlaneMask(
    cairo_surface_t *srcSurface,
    unsigned long plane)
{
    int width, height, x, y, srcStride, dstStride;
    unsigned char *srcData, *dstData;
    cairo_surface_t *mask;

    if (cairo_image_surface_get_format(srcSurface) == CAIRO_FORMAT_A8) {
	return cairo_surface_reference(srcSurface);
    }
    cairo_surface_flush(srcSurface);
    width = cairo_image_surface_get_width(srcSurface);
    height = cairo_image_surface_get_height(srcSurface);
    mask = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    srcData = cairo_image_surface_get_data(srcSurface);
    dstData = cairo_image_surface_get_data(mask);
    srcStride = cairo_image_surface_get_stride(srcSurface);
    dstStride = cairo_image_surface_get_stride(mask);
    for (y = 0; y < height; y++) {
	uint32_t *s = (uint32_t *)(srcData + y * srcStride);

	for (x = 0; x < width; x++) {
	    dstData[y * dstStride + x] = (s[x] & plane & 0xFFFFFF) ? 0xFF : 0;
	}
    }
    cairo_surface_mark_dirty(mask);
    return mask;
}

/*
 *----------------------------------------------------------------------
 *
 * XCopyPlane --
 *
 *	Copy a bitmap: the pixels where the plane is set are drawn with the
 *	foreground of the GC, the others with its background.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the destination, and may queue GraphicsExpose or NoExpose
 *	events.
 *
 *----------------------------------------------------------------------
 */

int
XCopyPlane(
    Display *display,
    Drawable src,
    Drawable dst,
    GC gc,
    int srcX, int srcY,
    unsigned int width, unsigned int height,
    int dstX, int dstY,
    unsigned long plane)
{
    TkXeDrawContext ctx;
    TkXeWindow *srcWinPtr;
    cairo_surface_t *srcSurface, *mask;
    int sx, sy;

    TkXeNextRequest(display);
    srcSurface = SourceSurface(src, &sx, &sy, &srcWinPtr);
    if (srcSurface && TkXeBeginDraw(dst, gc, &ctx)) {
	cairo_save(ctx.cr);
	cairo_rectangle(ctx.cr, dstX, dstY, width, height);
	cairo_clip(ctx.cr);
	mask = PlaneMask(srcSurface, plane);
	if (ctx.depth == 1) {
	    /*
	     * Copying a bitmap into a bitmap.
	     */

	    cairo_set_operator(ctx.cr, CAIRO_OPERATOR_SOURCE);
	    SetSourcePixel(ctx.cr, 1, gc->background);
	    cairo_paint(ctx.cr);
	    SetSourcePixel(ctx.cr, 1, gc->foreground);
	    cairo_mask_surface(ctx.cr, mask, dstX - srcX - sx,
		    dstY - srcY - sy);
	} else {
	    SetSourcePixel(ctx.cr, 24, gc->background);
	    cairo_paint(ctx.cr);
	    SetSourcePixel(ctx.cr, 24, gc->foreground);
	    cairo_mask_surface(ctx.cr, mask, dstX - srcX - sx,
		    dstY - srcY - sy);
	}
	cairo_surface_destroy(mask);
	cairo_restore(ctx.cr);
	TkXeEndDraw(&ctx, dstX, dstY, (int)width, (int)height);
    }
    if (gc->graphics_exposures) {
	QueueGraphicsExposures(display, dst, srcWinPtr, srcX, srcY, width,
		height, dstX, dstY, 63 /* X_CopyPlane */, false);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * Images --
 *
 *	ZPixmap images have 32 bits per pixel, 0xRRGGBB, in the byte order of
 *	the host, which is also the format of Cairo. XYBitmap and XYPixmap
 *	images of depth 1 have one bit per pixel.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * ImageGetPixel --
 *
 *	Get a pixel of an image (the get_pixel function of XImages).
 *
 * Results:
 *	The value of the pixel.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static unsigned long
ImageGetPixel(
    XImage *image,
    int x, int y)
{
    unsigned char *row = (unsigned char *)image->data
	    + y * image->bytes_per_line;

    if (image->bits_per_pixel == 1) {
	int bit = x + image->xoffset;
	unsigned char byte = row[bit / 8];

	return (image->bitmap_bit_order == LSBFirst)
		? (byte >> (bit % 8)) & 1 : (byte >> (7 - bit % 8)) & 1;
    } else if (image->bits_per_pixel == 32) {
	uint32_t *p = (uint32_t *)(void *)(row + 4 * x);

	return *p;
    } else if (image->bits_per_pixel == 24) {
	unsigned char *p = row + 3 * x;

	return (image->byte_order == LSBFirst)
		? (unsigned long)p[0] | ((unsigned long)p[1] << 8)
		    | ((unsigned long)p[2] << 16)
		: (unsigned long)p[2] | ((unsigned long)p[1] << 8)
		    | ((unsigned long)p[0] << 16);
    } else if (image->bits_per_pixel == 8) {
	return row[x];
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * ImagePutPixel --
 *
 *	Set a pixel of an image (the put_pixel function of XImages).
 *
 * Results:
 *	0.
 *
 * Side effects:
 *	The pixel in the data of the image is changed.
 *
 *----------------------------------------------------------------------
 */

static int
ImagePutPixel(
    XImage *image,
    int x, int y,
    unsigned long pixel)
{
    unsigned char *row = (unsigned char *)image->data
	    + y * image->bytes_per_line;

    if (image->bits_per_pixel == 1) {
	int bit = x + image->xoffset;
	unsigned char mask = (image->bitmap_bit_order == LSBFirst)
		? (unsigned char)(1 << (bit % 8))
		: (unsigned char)(0x80 >> (bit % 8));

	if (pixel & 1) {
	    row[bit / 8] |= mask;
	} else {
	    row[bit / 8] &= (unsigned char)~mask;
	}
    } else if (image->bits_per_pixel == 32) {
	uint32_t *p = (uint32_t *)(void *)(row + 4 * x);

	*p = (uint32_t)pixel;
    } else if (image->bits_per_pixel == 24) {
	unsigned char *p = row + 3 * x;

	p[0] = (unsigned char)pixel;
	p[1] = (unsigned char)(pixel >> 8);
	p[2] = (unsigned char)(pixel >> 16);
    } else if (image->bits_per_pixel == 8) {
	row[x] = (unsigned char)pixel;
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * DestroyImage --
 *
 *	Free an image and its data (the destroy_image function of XImages).
 *
 * Results:
 *	0.
 *
 * Side effects:
 *	The image and its data are freed.
 *
 *----------------------------------------------------------------------
 */

static int
DestroyImage(
    XImage *image)
{
    if (image) {
	if (image->data) {
	    Tcl_Free(image->data);
	}
	Tcl_Free(image);
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * SubImage --
 *
 *	Copy a part of an image (the sub_image function of XImages).
 *
 * Results:
 *	The new image.
 *
 * Side effects:
 *	Allocates the image and its data.
 *
 *----------------------------------------------------------------------
 */

static XImage *
SubImage(
    XImage *image,
    int x, int y,
    unsigned int width, unsigned int height)
{
    XImage *sub = XCreateImage(NULL, NULL, (unsigned)image->depth,
	    image->format, 0, NULL, width, height, image->bitmap_pad, 0);
    unsigned int i, j;

    sub->data = (char *)Tcl_Alloc((size_t)sub->bytes_per_line * height + 1);
    memset(sub->data, 0, (size_t)sub->bytes_per_line * height);
    for (j = 0; j < height; j++) {
	for (i = 0; i < width; i++) {
	    ImagePutPixel(sub, (int)i, (int)j,
		    ImageGetPixel(image, x + (int)i, y + (int)j));
	}
    }
    return sub;
}

/*
 *----------------------------------------------------------------------
 *
 * AddPixel --
 *
 *	Add a value to every pixel of an image (the add_pixel function of
 *	XImages).
 *
 * Results:
 *	0.
 *
 * Side effects:
 *	The pixels of the image are changed.
 *
 *----------------------------------------------------------------------
 */

static int
AddPixel(
    XImage *image,
    long value)
{
    int x, y;

    for (y = 0; y < image->height; y++) {
	for (x = 0; x < image->width; x++) {
	    ImagePutPixel(image, x, y, ImageGetPixel(image, x, y) + value);
	}
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * _XInitImageFuncPtrs --
 *
 *	Set the functions of an image to those of this file.
 *
 * Results:
 *	0.
 *
 * Side effects:
 *	The function pointers of the image are set.
 *
 *----------------------------------------------------------------------
 */

int
_XInitImageFuncPtrs(
    XImage *image)
{
    image->f.create_image = NULL;
    image->f.destroy_image = DestroyImage;
    image->f.get_pixel = ImageGetPixel;
    image->f.put_pixel = ImagePutPixel;
    image->f.sub_image = SubImage;
    image->f.add_pixel = AddPixel;
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * XCreateImage --
 *
 *	Create the structure of an image of the formats described above:
 *	ZPixmap images of depth more than 1 have 8 or 32 bits per pixel, the
 *	others 1 bit.
 *
 * Results:
 *	The new image, which XDestroyImage frees with its data.
 *
 * Side effects:
 *	Allocates the structure.
 *
 *----------------------------------------------------------------------
 */

XImage *
XCreateImage(
    Display *display,
    TCL_UNUSED(Visual *),
    unsigned int depth,
    int format,
    int offset,
    char *data,
    unsigned int width, unsigned int height,
    int bitmapPad,
    int bytesPerLine)
{
    XImage *image = (XImage *)Tcl_Alloc(sizeof(XImage));

    if (display) {
	TkXeNextRequest(display);
    }
    memset(image, 0, sizeof(XImage));
    image->width = (int)width;
    image->height = (int)height;
    image->xoffset = offset;
    image->format = format;
    image->data = data;
    image->depth = (int)depth;
    image->byte_order = LSBFirst;
    image->bitmap_unit = 8;
    image->bitmap_bit_order = LSBFirst;
    image->bitmap_pad = bitmapPad ? bitmapPad : 32;
    if (format == ZPixmap && depth > 1) {
	image->bits_per_pixel = (depth <= 8) ? 8 : 32;
    } else {
	image->bits_per_pixel = 1;
    }
    image->bytes_per_line = bytesPerLine ? bytesPerLine
	    : (int)((((width + offset) * image->bits_per_pixel
		    + image->bitmap_pad - 1) / image->bitmap_pad)
		    * (image->bitmap_pad / 8));
    image->red_mask = 0xFF0000;
    image->green_mask = 0x00FF00;
    image->blue_mask = 0x0000FF;
    _XInitImageFuncPtrs(image);
    return image;
}

/*
 *----------------------------------------------------------------------
 *
 * ImageToSurface --
 *
 *	Convert a part of an image to a Cairo image: ARGB32 for RGBA images,
 *	A8 for images of depth 1, RGB24 otherwise.
 *
 * Results:
 *	The new Cairo image, which the caller destroys.
 *
 * Side effects:
 *	Allocates the Cairo image.
 *
 *----------------------------------------------------------------------
 */

static cairo_surface_t *
ImageToSurface(
    XImage *image,
    int srcX, int srcY,
    int width, int height,
    bool rgba)
{
    cairo_surface_t *surface;
    unsigned char *data;
    int x, y, stride;
    bool bitmap = (image->bits_per_pixel == 1);

    surface = cairo_image_surface_create(bitmap ? CAIRO_FORMAT_A8
	    : rgba ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, width, height);
    data = cairo_image_surface_get_data(surface);
    stride = cairo_image_surface_get_stride(surface);
    for (y = 0; y < height; y++) {
	unsigned char *row = data + y * stride;

	if (bitmap) {
	    for (x = 0; x < width; x++) {
		row[x] = ImageGetPixel(image, srcX + x, srcY + y) ? 0xFF : 0;
	    }
	} else if (rgba) {
	    /*
	     * The bytes are R, G, B, A; Cairo wants premultiplied ARGB.
	     */

	    unsigned char *src = (unsigned char *)image->data
		    + (srcY + y) * image->bytes_per_line + 4 * srcX;
	    uint32_t *dst = (uint32_t *)(void *)row;

	    for (x = 0; x < width; x++, src += 4) {
		unsigned a = src[3];

		dst[x] = (a << 24) | (((src[0] * a + 127) / 255) << 16)
			| (((src[1] * a + 127) / 255) << 8)
			| ((src[2] * a + 127) / 255);
	    }
	} else {
	    uint32_t *dst = (uint32_t *)(void *)row;

	    for (x = 0; x < width; x++) {
		dst[x] = (uint32_t)ImageGetPixel(image, srcX + x, srcY + y)
			& 0xFFFFFF;
	    }
	}
    }
    cairo_surface_mark_dirty(surface);
    return surface;
}

/*
 *----------------------------------------------------------------------
 *
 * XPutImage --
 *
 *	Draw an image in a drawable. A bitmap is drawn with the foreground and
 *	background of the GC.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
XPutImage(
    Display *display,
    Drawable d,
    GC gc,
    XImage *image,
    int srcX, int srcY,
    int dstX, int dstY,
    unsigned int width, unsigned int height)
{
    TkXeDrawContext ctx;
    cairo_surface_t *surface;

    TkXeNextRequest(display);
    if (width == 0 || height == 0 || !TkXeBeginDraw(d, gc, &ctx)) {
	return Success;
    }
    surface = ImageToSurface(image, srcX, srcY, (int)width, (int)height,
	    false);
    cairo_rectangle(ctx.cr, dstX, dstY, width, height);
    cairo_clip(ctx.cr);
    if (image->bits_per_pixel == 1 && ctx.depth != 1) {
	/*
	 * A bitmap is drawn with the foreground and background of the GC.
	 */

	SetSourcePixel(ctx.cr, ctx.depth, gc->background);
	cairo_paint(ctx.cr);
	SetSourcePixel(ctx.cr, ctx.depth, gc->foreground);
	cairo_mask_surface(ctx.cr, surface, dstX, dstY);
    } else {
	cairo_set_operator(ctx.cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(ctx.cr, surface, dstX, dstY);
	cairo_paint(ctx.cr);
    }
    cairo_surface_destroy(surface);
    TkXeEndDraw(&ctx, dstX, dstY, (int)width, (int)height);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * TkpPutRGBAImage --
 *
 *	Draw an image whose pixels are R, G, B and A bytes, composited with
 *	its alpha channel over the drawable.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	Draws in the drawable, and adds what was drawn in a window to the
 *	damage of its surface.
 *
 *----------------------------------------------------------------------
 */

int
TkpPutRGBAImage(
    Display *display,
    Drawable d,
    GC gc,
    XImage *image,
    int srcX, int srcY,
    int dstX, int dstY,
    unsigned int width, unsigned int height)
{
    TkXeDrawContext ctx;
    cairo_surface_t *surface;

    TkXeNextRequest(display);
    if (width == 0 || height == 0 || !TkXeBeginDraw(d, gc, &ctx)) {
	return Success;
    }
    surface = ImageToSurface(image, srcX, srcY, (int)width, (int)height,
	    true);
    cairo_set_operator(ctx.cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_surface(ctx.cr, surface, dstX, dstY);
    cairo_rectangle(ctx.cr, dstX, dstY, width, height);
    cairo_fill(ctx.cr);
    cairo_surface_destroy(surface);
    TkXeEndDraw(&ctx, dstX, dstY, (int)width, (int)height);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XGetImage --
 *
 *	Read the pixels of an area of a drawable, at the scale of the user
 *	space of the drawable.
 *
 * Results:
 *	A new image: XYPixmap of depth 1 for bitmaps or when asked, otherwise
 *	ZPixmap of depth 24; NULL if the drawable does not exist or the area
 *	is empty.
 *
 * Side effects:
 *	Pending exposures of the surface of a window are processed; allocates
 *	the image.
 *
 *----------------------------------------------------------------------
 */

XImage *
XGetImage(
    Display *display,
    Drawable d,
    int x, int y,
    unsigned int width, unsigned int height,
    TCL_UNUSED(unsigned long),	/* planeMask */
    int format)
{
    TkXeWindow *winPtr;
    cairo_surface_t *surface, *copy;
    XImage *image;
    cairo_t *cr;
    int ox, oy, depth;
    unsigned char *data;
    int stride;
    unsigned int i;

    TkXeNextRequest(display);
    surface = SourceSurface(d, &ox, &oy, &winPtr);
    if (surface == NULL || width == 0 || height == 0) {
	return NULL;
    }
    depth = (cairo_image_surface_get_format(surface) == CAIRO_FORMAT_A8)
	    ? 1 : 24;

    /*
     * Read the pixels at the scale of the drawable's user space.
     */

    copy = cairo_image_surface_create(
	    (depth == 1) ? CAIRO_FORMAT_A8 : CAIRO_FORMAT_RGB24,
	    (int)width, (int)height);
    cr = cairo_create(copy);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, surface, -(ox + x), -(oy + y));
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(copy);
    data = cairo_image_surface_get_data(copy);
    stride = cairo_image_surface_get_stride(copy);

    if (depth == 1 || format != ZPixmap) {
	image = XCreateImage(display, NULL, 1, XYPixmap, 0, NULL, width,
		height, 32, 0);
	image->data = (char *)Tcl_Alloc(
		(size_t)image->bytes_per_line * height + 1);
	memset(image->data, 0, (size_t)image->bytes_per_line * height);
	for (i = 0; i < height; i++) {
	    unsigned int j;

	    for (j = 0; j < width; j++) {
		unsigned long pixel = (depth == 1)
			? data[i * stride + j] >= 0x80
			: (((uint32_t *)(void *)(data + i * stride))[j]
			    & 0xFFFFFF) != 0;

		ImagePutPixel(image, (int)j, (int)i, pixel);
	    }
	}
    } else {
	image = XCreateImage(display, NULL, 24, ZPixmap, 0, NULL, width,
		height, 32, 0);
	image->data = (char *)Tcl_Alloc(
		(size_t)image->bytes_per_line * height + 1);
	for (i = 0; i < height; i++) {
	    memcpy(image->data + i * image->bytes_per_line, data + i * stride,
		    4 * width);
	}
    }
    cairo_surface_destroy(copy);
    return image;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
