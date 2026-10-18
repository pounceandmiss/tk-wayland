/*
 * tkWaylandInt.h --
 *
 *	Declarations that are shared among the Wayland-specific parts of Tk
 *	but aren't used by the rest of Tk.
 *
 *	The Wayland port emulates an X server inside the process:
 *
 *	- Every X window is a TkWlWindow in a window tree rooted at an
 *	  emulated root window. The children of the root window (the wrapper
 *	  windows of Tk's toplevels, and override-redirect windows) each get
 *	  a TkWlSurface, which owns a wl_surface and a backing image.
 *	- Drawing is done with Cairo into the backing image of the surface,
 *	  clipped to the visible region of the window, as the X server
 *	  clips to it. Damaged parts of the backing image are copied to a
 *	  wl_shm buffer and committed at idle time.
 *	- Expose events are generated, and window backgrounds painted, when
 *	  the window tree changes, as the X server does.
 *	- Pixmaps are Cairo image surfaces.
 *
 *	Window and Pixmap ids are never reused, so that events and requests
 *	for destroyed windows are ignored, as in X.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKWAYLANDINT
#define _TKWAYLANDINT

#ifndef _TKINT
#include "tkInt.h"
#endif
#include "tkUnixInt.h"

#include <stdbool.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <libdecor.h>
#include <cairo.h>
#include <pixman.h>
#include "xdg-shell-client-protocol.h"

/*
 * Kinds of drawables. Both structures start with the type.
 */

#define TKWL_PIXMAP	1
#define TKWL_WINDOW	2

typedef struct TkWlPixmap {
    int type;			/* TKWL_PIXMAP. */
    Pixmap id;
    int width, height;
    int depth;			/* 1 (a bitmap, CAIRO_FORMAT_A8), 24
				 * (CAIRO_FORMAT_RGB24) or 32
				 * (CAIRO_FORMAT_ARGB32). */
    cairo_surface_t *surface;
} TkWlPixmap;

/*
 * A property of a window. Only the windows of this process exist, so
 * properties are only stored.
 */

typedef struct TkWlProperty {
    Atom name;
    Atom type;
    int format;			/* 8, 16 or 32. */
    unsigned long nitems;
    unsigned char *data;	/* For format 32, an array of longs, as in
				 * Xlib. */
    struct TkWlProperty *nextPtr;
} TkWlProperty;

struct TkWlSurface;

typedef struct TkWlWindow {
    int type;			/* TKWL_WINDOW. */
    Window id;
    Display *display;
    struct TkWlWindow *parent;	/* NULL for the root window. */
    struct TkWlWindow *firstChild;
				/* Bottom-most child in the stacking order. */
    struct TkWlWindow *lastChild;
				/* Top-most child in the stacking order. */
    struct TkWlWindow *below;	/* Next sibling below this one. */
    struct TkWlWindow *above;	/* Next sibling above this one. */
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
    TkWlProperty *properties;
    struct TkWlSurface *surface;
				/* The surface of this window, if it is a
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
} TkWlWindow;

#define BG_NONE		0
#define BG_PIXEL	1
#define BG_PIXMAP	2
#define BG_PARENT	3

/*
 * A wl_shm buffer.
 */

typedef struct TkWlBuffer {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int width, height, stride;	/* In pixels of the buffer. */
    bool busy;			/* The compositor still uses it. */
    pixman_region32_t damage;	/* Part of the backing image which is more
				 * recent than this buffer, in surface
				 * coordinates. */
} TkWlBuffer;

#define TKWL_NUM_BUFFERS 3

/*
 * The surface of a child of the root window.
 */

typedef struct TkWlSurface {
    TkWlWindow *winPtr;		/* The X window of this surface. */
    struct wl_surface *surface;
    struct libdecor_frame *frame;
				/* The decorated xdg_toplevel, or NULL. */
    struct xdg_surface *xdgSurface;
				/* For popups (override-redirect windows) and
				 * undecorated toplevels. */
    struct xdg_popup *xdgPopup;
    struct xdg_toplevel *xdgToplevel;
    struct TkWlSurface *popupParent;
				/* The surface a popup is attached to. */
    bool configured;		/* The compositor has sent the first
				 * configure event. */
    bool iconic;		/* The window was iconified: the X window is
				 * unmapped, but the surface keeps its role,
				 * since Wayland has no iconic state. */
    bool inConfigure;		/* The window is being resized by the
				 * compositor. */
    bool committed;		/* A buffer was attached. */
    int scale;			/* Buffer scale. */
    int width, height;		/* Size of the backing image, in surface
				 * coordinates. */
    cairo_surface_t *backing;	/* The image Tk draws into, with the buffer
				 * scale as device scale. */
    pixman_region32_t damage;	/* Damage not yet committed, in surface
				 * coordinates. */
    TkWlBuffer buffers[TKWL_NUM_BUFFERS];
    struct wl_callback *frameCallback;
    bool presentPending;	/* A present is scheduled at idle time. */
    bool presentDeferred;	/* The present was already deferred once. */
    bool exposePending;		/* The window tree changed; Expose events
				 * have to be generated. */
    struct TkWlSurface *nextPtr;
} TkWlSurface;

/*
 * An output (monitor).
 */

typedef struct TkWlOutput {
    struct wl_output *output;
    uint32_t name;		/* Registry name. */
    int scale;
    int width, height;		/* Current mode, in pixels. */
    int mmWidth, mmHeight;
    struct TkWlOutput *nextPtr;
} TkWlOutput;

/*
 * The connection to the compositor. There is only one per process.
 */

typedef struct TkWlDisplay {
    Display *display;		/* The emulated X display. */
    struct wl_display *wlDisplay;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wmBase;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct libdecor *decor;
    TkWlOutput *outputs;
    TkWlSurface *surfaces;
    TkWlWindow *root;

    /*
     * Pointer state.
     */

    TkWlSurface *pointerSurface;
				/* Surface with the pointer, or NULL. */
    uint32_t pointerSerial;	/* Serial of the last pointer enter. */
    double pointerX, pointerY;	/* Position in the surface. */
    unsigned int buttonState;	/* Button1Mask etc. */
    struct wl_cursor_theme *cursorTheme;
    struct wl_surface *cursorSurface;
    Cursor cursor;		/* Current cursor. */
    double scrollX, scrollY;	/* Accumulated smooth scrolling. */

    /*
     * Keyboard state.
     */

    TkWlSurface *keyboardSurface;
				/* Surface with the keyboard focus. */
    struct xkb_context *xkbContext;
    struct xkb_keymap *keymap;
    struct xkb_state *xkbState;
    struct xkb_compose_table *composeTable;
    struct xkb_compose_state *composeState;
    unsigned int modState;	/* ShiftMask etc. */
    int repeatRate, repeatDelay;
    uint32_t repeatKey;
    Tcl_TimerToken repeatTimer;

    uint32_t lastSerial;	/* Serial of the last input event. */
    uint32_t lastTime;		/* Time of the last input event. */
} TkWlDisplay;

MODULE_SCOPE TkWlDisplay *tkWlDisplay;

/*
 * Conversion between ids and structures.
 */

MODULE_SCOPE XID	TkWlNewId(void *ptr);
MODULE_SCOPE void	TkWlFreeId(XID id);
MODULE_SCOPE void *	TkWlLookupId(XID id);

static inline TkWlWindow *
TkWlGetWindow(
    Window w)
{
    int *p = (int *)TkWlLookupId(w);

    return (p && *p == TKWL_WINDOW) ? (TkWlWindow *)p : NULL;
}

static inline TkWlPixmap *
TkWlGetPixmap(
    Pixmap pixmap)
{
    int *p = (int *)TkWlLookupId(pixmap);

    return (p && *p == TKWL_PIXMAP) ? (TkWlPixmap *)p : NULL;
}

/*
 * Functions shared between the files of the port.
 */

/* tkWaylandWindow.c */
MODULE_SCOPE TkWlWindow *TkWlCreateRootWindow(Display *display, int width,
			    int height);
MODULE_SCOPE void	TkWlTreeChanged(TkWlWindow *winPtr);
MODULE_SCOPE TkWlWindow *TkWlTopWindow(TkWlWindow *winPtr);
MODULE_SCOPE bool	TkWlIsViewable(TkWlWindow *winPtr);
MODULE_SCOPE void	TkWlWindowOrigin(TkWlWindow *winPtr, int *xPtr,
			    int *yPtr);
MODULE_SCOPE void	TkWlVisibleRegion(TkWlWindow *winPtr,
			    bool includeInferiors, pixman_region32_t *rgn);
MODULE_SCOPE void	TkWlProcessExposures(TkWlSurface *surfPtr);
MODULE_SCOPE void	TkWlAddDamage(TkWlSurface *surfPtr,
			    pixman_region32_t *rgn);
MODULE_SCOPE TkWlWindow *TkWlWindowAtPoint(TkWlWindow *winPtr, int x, int y);
MODULE_SCOPE TkWlSurface *TkWlSurfaceFromWl(struct wl_surface *surface);
MODULE_SCOPE void	TkWlQueueEvent(XEvent *eventPtr);
MODULE_SCOPE void	TkWlSurfaceConfigured(TkWlSurface *surfPtr);
MODULE_SCOPE TkWlProperty *TkWlGetProperty(TkWlWindow *winPtr, Atom name);
MODULE_SCOPE void	TkWlFreeProperties(TkWlWindow *winPtr);
MODULE_SCOPE TkWlSurface *TkWlGetSurface(Window w);
MODULE_SCOPE void	TkWlSurfaceSetScale(TkWlSurface *surfPtr, int scale);

/* tkWaylandDraw.c */
MODULE_SCOPE cairo_surface_t *TkWlPixmapSurface(Pixmap pixmap);
MODULE_SCOPE void	TkWlPaintBackground(TkWlWindow *winPtr,
			    pixman_region32_t *rgn);

/* tkWaylandXlib.c */
MODULE_SCOPE Display *	TkWlOpenXDisplay(const char *name, int fd, int width,
			    int height, int mmWidth, int mmHeight);
MODULE_SCOPE void	TkWlSetScreenSize(Display *display, int width,
			    int height, int mmWidth, int mmHeight);
MODULE_SCOPE pixman_region32_t *TkWlPixmanRegion(Region r);
MODULE_SCOPE unsigned long TkWlNextRequest(Display *display);

/* tkWaylandEvent.c */
MODULE_SCOPE Time	TkWlCurrentTime(void);
MODULE_SCOPE void	TkWlUpdateCursor(void);

/* tkWaylandWm.c */
MODULE_SCOPE void	TkWlSurfaceSetFrame(TkWlSurface *surfPtr,
			    struct libdecor_frame *frame);
MODULE_SCOPE void	TkWlWmMapSurface(TkWlSurface *surfPtr);
MODULE_SCOPE void	TkWlWmUnmapSurface(TkWlSurface *surfPtr);
MODULE_SCOPE void	TkWlWmSurfaceResized(TkWlSurface *surfPtr);
MODULE_SCOPE void	TkWlWmPropertyChanged(TkWlSurface *surfPtr,
			    Atom property);
MODULE_SCOPE bool	TkWlWmClientMessage(XEvent *eventPtr);

#endif /* _TKWAYLANDINT */
