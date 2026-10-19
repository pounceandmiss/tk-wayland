/*
 * tkXemuInt.h --
 *
 *	Declarations of the X server emulation shared by the ports of Tk to
 *	systems without X (Wayland), which run the X11 code of Tk
 *	on an X server emulated inside the process:
 *
 *	- Every X window is a TkXeWindow in a window tree rooted at an
 *	  emulated root window. The children of the root window (the wrapper
 *	  windows of Tk's toplevels, and override-redirect windows) each get
 *	  a TkXeSurface, which has a backing image.
 *	- Drawing is done with Cairo into the backing image of the surface,
 *	  clipped to the visible region of the window, as the X server clips
 *	  to it.
 *	- Expose events are generated, and window backgrounds painted, when
 *	  the window tree changes, as the X server does.
 *	- Pixmaps are Cairo image surfaces.
 *	- Properties are stored; the hints of the window manager which
 *	  tkUnixWm.c sets in them are given to the window manager of the port.
 *
 *	Window and Pixmap ids are never reused, so that events and requests
 *	for destroyed windows are ignored, as in X.
 *
 *	A port (a backend) shows the backing images of the surfaces, gives
 *	Tk its input as X events, and is the window manager. Its header
 *	(tkWaylandInt.h), included at the end of this one,
 *	defines the structures of the surfaces and of the display, with at
 *	least the members which the emulation uses:
 *
 *	struct TkXeSurface:
 *	    TkXeWindow *winPtr		The window of the surface.
 *	    cairo_surface_t *backing	The backing image, whose device scale
 *					is the scale, or NULL.
 *	    double scale		Pixels per logical pixel.
 *	    bool exposePending		The window tree changed: Expose events
 *					have to be generated.
 *	    bool iconic			The window is iconified.
 *	    struct TkXeSurface *nextPtr	The next surface of the display.
 *
 *	struct TkXeDisplay:
 *	    Display *display		The emulated X display.
 *	    TkXeSurface *surfaces	The surfaces.
 *	    struct xkb_keymap *keymap	The keymap, or NULL.
 *
 *	the functions declared below as given by the port, the part of
 *	xkbcommon which the Xlib emulation uses, and TKXE_VENDOR, the vendor
 *	of the X server.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKXEMUINT
#define _TKXEMUINT

#ifndef _TKINT
#include "tkInt.h"
#endif
#include "tkUnixInt.h"

#include <stdbool.h>
#include <cairo.h>
#include <pixman.h>

/*
 * Kinds of drawables. Both structures start with the type.
 */

#define TKXE_PIXMAP	1
#define TKXE_WINDOW	2

typedef struct TkXePixmap {
    int type;			/* TKXE_PIXMAP. */
    Pixmap id;
    int width, height;
    int depth;			/* 1 (a bitmap, CAIRO_FORMAT_A8), 24
				 * (CAIRO_FORMAT_RGB24) or 32
				 * (CAIRO_FORMAT_ARGB32). */
    cairo_surface_t *surface;
} TkXePixmap;

/*
 * A property of a window. Only the windows of this process exist, so
 * properties are only stored.
 */

typedef struct TkXeProperty {
    Atom name;
    Atom type;
    int format;			/* 8, 16 or 32. */
    unsigned long nitems;
    unsigned char *data;	/* For format 32, an array of longs, as in
				 * Xlib. */
    struct TkXeProperty *nextPtr;
} TkXeProperty;

/*
 * The surfaces and the display are defined by the port.
 */

typedef struct TkXeSurface TkXeSurface;
typedef struct TkXeDisplay TkXeDisplay;

typedef struct TkXeWindow {
    int type;			/* TKXE_WINDOW. */
    Window id;
    Display *display;
    struct TkXeWindow *parent;	/* NULL for the root window. */
    struct TkXeWindow *firstChild;
				/* Bottom-most child in the stacking order. */
    struct TkXeWindow *lastChild;
				/* Top-most child in the stacking order. */
    struct TkXeWindow *below;	/* Next sibling below this one. */
    struct TkXeWindow *above;	/* Next sibling above this one. */
    int x, y;			/* Position of the outer corner relative to
				 * the inside of the parent. */
    int width, height;		/* Inside size. */
    int borderWidth;
    bool mapped;
    bool overrideRedirect;
    bool inputOnly;
    long eventMask;
    int bitGravity;
    int backgroundType;		/* One of the BG_* values below. */
    unsigned long backgroundPixel;
    Pixmap backgroundPixmap;
    unsigned long borderPixel;
    Cursor cursor;
    Colormap colormap;
    TkXeProperty *properties;
    TkXeSurface *surface;	/* The surface of this window, if it is a
				 * child of the root window. */
    /*
     * The state below is used to generate Expose events. The region is in
     * the coordinates of the surface.
     */

    pixman_region32_t exposed;	/* Part of the window that was visible
				 * when Expose events were last generated. */
    int exposedX, exposedY;	/* Position of the inside of the window in
				 * its surface at that time. */
    int exposedWidth, exposedHeight;
    int visibility;		/* VisibilityUnobscured etc., or -1 when the
				 * window is not viewable. */

    /*
     * The visible region is cached until the window tree changes.
     */

    unsigned long clipGeneration;
    bool clipInferiors;
    pixman_region32_t clip;
} TkXeWindow;

/*
 * The layout of the WM_NORMAL_HINTS property, in longs.
 */

enum {
    SH_FLAGS, SH_X, SH_Y, SH_WIDTH, SH_HEIGHT, SH_MIN_WIDTH, SH_MIN_HEIGHT,
    SH_MAX_WIDTH, SH_MAX_HEIGHT, SH_WIDTH_INC, SH_HEIGHT_INC,
    SH_MIN_ASPECT_X, SH_MIN_ASPECT_Y, SH_MAX_ASPECT_X, SH_MAX_ASPECT_Y,
    SH_BASE_WIDTH, SH_BASE_HEIGHT, SH_WIN_GRAVITY, SH_NUM
};

#define BG_NONE		0
#define BG_PIXEL	1
#define BG_PIXMAP	2
#define BG_PARENT	3

/*
 * The display of the port. There is only one per process.
 */

MODULE_SCOPE TkXeDisplay *tkXeDisplay;

/*
 * Conversion between ids and structures.
 */

MODULE_SCOPE XID	TkXeNewId(void *ptr);
MODULE_SCOPE void	TkXeFreeId(XID id);
MODULE_SCOPE void *	TkXeLookupId(XID id);

/*
 *----------------------------------------------------------------------
 *
 * TkXeGetWindow, TkXeGetPixmap --
 *
 *	Find the window or the pixmap which has an id.
 *
 * Results:
 *	The window or the pixmap, or NULL if the id is not that of a window
 *	(or of a pixmap).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static inline TkXeWindow *
TkXeGetWindow(
    Window w)
{
    int *p = (int *)TkXeLookupId(w);

    return (p && *p == TKXE_WINDOW) ? (TkXeWindow *)p : NULL;
}

static inline TkXePixmap *
TkXeGetPixmap(
    Pixmap pixmap)
{
    int *p = (int *)TkXeLookupId(pixmap);

    return (p && *p == TKXE_PIXMAP) ? (TkXePixmap *)p : NULL;
}

/*
 * Functions of the emulation.
 */

/* tkXemuWindow.c */
MODULE_SCOPE TkXeWindow *TkXeCreateRootWindow(Display *display, int width,
			    int height);
MODULE_SCOPE void	TkXeTreeChanged(TkXeWindow *winPtr);
MODULE_SCOPE TkXeWindow *TkXeTopWindow(TkXeWindow *winPtr);
MODULE_SCOPE bool	TkXeIsViewable(TkXeWindow *winPtr);
MODULE_SCOPE void	TkXeWindowOrigin(TkXeWindow *winPtr, int *xPtr,
			    int *yPtr);
MODULE_SCOPE void	TkXeVisibleRegion(TkXeWindow *winPtr,
			    bool includeInferiors, pixman_region32_t *rgn);
MODULE_SCOPE void	TkXeProcessExposures(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkXeForgetExposed(TkXeWindow *winPtr);
MODULE_SCOPE void	TkXeLimitExposed(TkXeWindow *winPtr, int width,
			    int height);
MODULE_SCOPE TkXeWindow *TkXeWindowAtPoint(TkXeWindow *winPtr, int x, int y);
MODULE_SCOPE void	TkXeQueueEvent(XEvent *eventPtr);
MODULE_SCOPE TkXeProperty *TkXeGetProperty(TkXeWindow *winPtr, Atom name);
MODULE_SCOPE void	TkXeFreeProperties(TkXeWindow *winPtr);
MODULE_SCOPE TkXeSurface *TkXeGetSurface(Window w);

/* tkXemuDraw.c */
MODULE_SCOPE cairo_surface_t *TkXePixmapSurface(Pixmap pixmap);
MODULE_SCOPE void	TkXePaintBackground(TkXeWindow *winPtr,
			    pixman_region32_t *rgn);

/* tkXemuXlib.c */
MODULE_SCOPE Display *	TkXeOpenXDisplay(const char *name, int fd, int width,
			    int height, int mmWidth, int mmHeight);
MODULE_SCOPE void	TkXeSetScreenSize(Display *display, int width,
			    int height, int mmWidth, int mmHeight);
MODULE_SCOPE pixman_region32_t *TkXePixmanRegion(Region r);
MODULE_SCOPE unsigned long TkXeNextRequest(Display *display);
MODULE_SCOPE struct _FcPattern *TkXeXlfdParse(const char *name);

/*
 * Functions given by the port.
 */

/* Surfaces. */
MODULE_SCOPE TkXeSurface *TkXeNewSurface(TkXeWindow *winPtr);
MODULE_SCOPE void	TkXeDestroySurface(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkXeResizeBacking(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkXeSurfaceUnmapped(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkXeAddDamage(TkXeSurface *surfPtr,
			    pixman_region32_t *rgn);
MODULE_SCOPE bool	TkXeIntegralScale(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkXeFlush(void);

/* Input. */
MODULE_SCOPE Time	TkXeCurrentTime(void);
MODULE_SCOPE void	TkXeUpdateCursor(void);

/* The window manager. */
MODULE_SCOPE void	TkXeWmMapSurface(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkXeWmSurfaceResized(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkXeWmPropertyChanged(TkXeSurface *surfPtr,
			    Atom property);
MODULE_SCOPE bool	TkXeWmClientMessage(XEvent *eventPtr);
MODULE_SCOPE void	TkXeWmActivate(TkXeSurface *surfPtr);

/*
 * The declarations of the port.
 */

#if defined(TK_USE_WAYLAND)
#include "tkWaylandInt.h"
#endif

#endif /* _TKXEMUINT */
