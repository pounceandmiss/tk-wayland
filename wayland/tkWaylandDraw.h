/*
 * tkWaylandDraw.h --
 *
 *	Declarations of the drawing contexts of the Wayland port, which are
 *	used by the drawing functions of the Xlib emulation and by the text
 *	drawing functions.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKWAYLANDDRAW
#define _TKWAYLANDDRAW

typedef struct TkWlDrawContext {
    cairo_t *cr;		/* Its user space is that of the drawable. */
    cairo_surface_t *target;	/* The image drawn into. */
    TkWlSurface *surfPtr;	/* The surface of the window, or NULL for a
				 * pixmap. */
    int depth;			/* Depth of the drawable. */
    int x, y;			/* Origin of the drawable in the target. */
    pixman_region32_t clip;	/* Where drawing can be done, in the
				 * coordinates of the target. */
    cairo_surface_t *mask;	/* Clip mask of the GC, or NULL. */
    int maskX, maskY;		/* Origin of the clip mask. */
    cairo_surface_t *stipple;	/* Stipple of the GC, or NULL. */
} TkWlDrawContext;

MODULE_SCOPE bool	TkWlBeginDraw(Drawable d, GC gc,
			    TkWlDrawContext *ctxPtr);
MODULE_SCOPE void	TkWlEndDraw(TkWlDrawContext *ctxPtr, int x, int y,
			    int width, int height);
MODULE_SCOPE void	TkWlSetSource(TkWlDrawContext *ctxPtr, GC gc,
			    unsigned long pixel);
MODULE_SCOPE void	TkWlFill(TkWlDrawContext *ctxPtr, GC gc);
MODULE_SCOPE void	TkWlStroke(TkWlDrawContext *ctxPtr, GC gc);

#endif /* _TKWAYLANDDRAW */
