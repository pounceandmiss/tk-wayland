/*
 * tkXemuDraw.h --
 *
 *	Declarations of the drawing contexts of the X server emulation, which
 *	are used by the drawing functions of the Xlib emulation and by the
 *	text drawing functions.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKXEMUDRAW
#define _TKXEMUDRAW

typedef struct TkXeDrawContext {
    cairo_t *cr;		/* Its user space is that of the drawable. */
    cairo_surface_t *target;	/* The image drawn into. */
    TkXeSurface *surfPtr;	/* The surface of the window, or NULL for a
				 * pixmap. */
    int depth;			/* Depth of the drawable. */
    int x, y;			/* Origin of the drawable in the target. */
    pixman_region32_t clip;	/* Where drawing can be done, in the
				 * coordinates of the target. */
    cairo_surface_t *mask;	/* Clip mask of the GC, or NULL. */
    int maskX, maskY;		/* Origin of the clip mask. */
    cairo_surface_t *stipple;	/* Stipple of the GC, or NULL. */
} TkXeDrawContext;

MODULE_SCOPE bool	TkXeBeginDraw(Drawable d, GC gc,
			    TkXeDrawContext *ctxPtr);
MODULE_SCOPE void	TkXeEndDraw(TkXeDrawContext *ctxPtr, int x, int y,
			    int width, int height);
MODULE_SCOPE void	TkXeSetSource(TkXeDrawContext *ctxPtr, GC gc,
			    unsigned long pixel);
MODULE_SCOPE void	TkXeFill(TkXeDrawContext *ctxPtr, GC gc);
MODULE_SCOPE void	TkXeStroke(TkXeDrawContext *ctxPtr, GC gc);

#endif /* _TKXEMUDRAW */
