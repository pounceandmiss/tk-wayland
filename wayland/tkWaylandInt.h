/*
 * tkWaylandInt.h --
 *
 *	Declarations of the Wayland port, included by tkXemuInt.h when Tk is
 *	built for Wayland.
 *
 *	The Wayland port runs Tk on the emulated X server of ../xemu: the
 *	surface of every child of the root window owns a wl_surface, an
 *	xdg_toplevel (decorated by libdecor) or an xdg_popup, and its backing
 *	image, whose damaged parts are copied to a wl_shm buffer and committed
 *	at idle time (tkWaylandSurface.c). The hints of tkUnixWm.c are given
 *	to the compositor (tkWaylandWm.c); input events come from the seat
 *	(tkWaylandEvent.c, tkWaylandTextInput.c); the selections are exchanged
 *	with other clients (tkWaylandSelect.c).
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKWAYLANDINT
#define _TKWAYLANDINT

#include "tkXemuInt.h"

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <libdecor.h>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"

#define TKXE_VENDOR	"Wayland"

/*
 * A wl_shm buffer.
 */

typedef struct TkWlBuffer {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int width, height, stride;	/* In pixels of the buffer. */
    uint32_t format;		/* WL_SHM_FORMAT_XRGB8888, or ARGB8888 for
				 * translucent windows. */
    bool busy;			/* The compositor still uses it. */
    pixman_region32_t damage;	/* Part of the backing image which is more
				 * recent than this buffer, in surface
				 * coordinates. */
} TkWlBuffer;

#define TKWL_NUM_BUFFERS 3

/*
 * The number of fingers followed on a touchscreen.
 */

#define TKWL_MAX_TOUCHES 10

/*
 * The surface of a child of the root window.
 */

struct TkXeSurface {
    TkXeWindow *winPtr;		/* The X window of this surface. */
    struct wl_surface *surface;
    struct libdecor_frame *frame;
				/* The decorated xdg_toplevel, or NULL. */
    struct xdg_surface *xdgSurface;
				/* For popups (override-redirect windows) and
				 * undecorated toplevels. */
    struct xdg_popup *xdgPopup;
    struct xdg_toplevel *xdgToplevel;
    struct TkXeSurface *popupParent;
				/* The surface a popup is attached to. */
    bool popupGrab;		/* The popup grabs the input. */
    bool popupDismissed;	/* The compositor dismissed the popup. */
    bool popupWaiting;		/* The popup waits for the popup which
				 * grabs the input to be shown, to be
				 * attached to it. */
    bool configured;		/* The compositor has sent the first
				 * configure event. */
    bool iconic;		/* The window was iconified: the X window is
				 * unmapped, but the surface keeps its role,
				 * since Wayland has no iconic state. */
    double opacity;		/* From _NET_WM_WINDOW_OPACITY. */
    double cornerRadius;	/* Radius of the rounded corners, in logical
				 * pixels, or 0. */
    bool squareCorners;		/* Maximized, fullscreen or tiled: the
				 * corners are not rounded. */
    pixman_image_t *cornerMask;	/* Rounded rectangle of twice the size of a
				 * corner, with the opacity, whose corners are
				 * those of the buffers. */
    double cornerMaskRadius, cornerMaskOpacity;
				/* What cornerMask was made for. */
    int roleType;		/* TKWL_ROLE_*: the type of the role the
				 * wl_surface had, which cannot change. */
    bool rolePending;		/* The role is created at idle time. */
    bool minimizePending;	/* Minimize after the first commit. */
    struct wl_callback *minimizeSync;
				/* Until it is done, the keyboard may enter
				 * the surface because of events from before
				 * it was minimized. */
    bool resized;		/* The application resized the window, and
				 * has not committed the new size yet. */
    struct wl_callback *resizeSync;
				/* Until it is done, configures may have been
				 * sent before the compositor saw the size
				 * last committed by the application. */
    int floatingWidth, floatingHeight;
				/* Size before being maximized or made
				 * fullscreen, restored after. */
    int boundWidth, boundHeight;
				/* Size the compositor gave the window when
				 * maximized or fullscreen, which it must not
				 * exceed, or 0. */
    bool boundExact;		/* Maximized (even if also fullscreen): the
				 * window must have exactly that size. */
    int minWidth, minHeight;	/* Minimum size given to libdecor. */
    int maxWidth, maxHeight;	/* Maximum size given to libdecor, or 0. */
    int decorWidth, decorHeight;
				/* Minimum size the plugin of libdecor raised
				 * the minimum size to for its decorations,
				 * or 0. */
    int configWidth, configHeight;
				/* Size the compositor last gave the floating
				 * window, or 0. */
    int maximizeRequest;	/* MAXIMIZE_*: the maximization requested
				 * since the last configure. */
    bool frameHasContent;	/* The frame was committed with a size: the
				 * visibility of its decorations can be set,
				 * which libdecor before 0.2.3 crashes on
				 * before. */
    bool inConfigure;		/* The window is being resized by the
				 * compositor. */
    bool committed;		/* A buffer was attached. */
    bool presented;		/* A frame was shown since the role was
				 * given. */
    double scale;		/* Pixels of the buffer per logical pixel,
				 * which may be fractional when the viewport
				 * scales the buffer. */
    double fractionalScale;	/* Preferred fractional scale, or 0. */
    int preferredScale;		/* Preferred integer scale, or 0. */
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional;
    struct TkWlOutput *outputs[8];
				/* The outputs the surface is on. */
    int numOutputs;
    int width, height;		/* Size of the backing image, in surface
				 * coordinates. */
    int bufferWidth, bufferHeight;
				/* Size of the backing image, in pixels. */
    cairo_surface_t *backing;	/* The image Tk draws into, with the scale as
				 * device scale. */
    pixman_region32_t damage;	/* Damage not yet committed, in surface
				 * coordinates. */
    TkWlBuffer buffers[TKWL_NUM_BUFFERS];
    struct wl_callback *frameCallback;
    bool presentPending;	/* A present is scheduled at idle time. */
    bool presentDeferred;	/* The present was already deferred once. */
    bool exposePending;		/* The window tree changed; Expose events
				 * have to be generated. */
    struct TkXeSurface *nextPtr;
};

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

struct TkXeDisplay {
    Display *display;		/* The emulated X display. */
    struct wl_display *wlDisplay;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wmBase;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractionalScaleManager;
    struct xdg_activation_v1 *activation;
    struct xdg_toplevel_icon_manager_v1 *iconManager;
    bool weston;		/* The compositor is Weston, whose version 15
				 * crashes positioning popups which may slide
				 * or flip. */
    double forcedScale;		/* Scale set with TK_WAYLAND_SCALE, for
				 * testing, or 0. */
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_touch *touch;
    struct libdecor *decor;
    TkWlOutput *outputs;
    TkXeSurface *surfaces;
    TkXeWindow *root;

    /*
     * Pointer state.
     */

    TkXeSurface *pointerSurface;
				/* Surface with the pointer, or NULL. */
    struct {
	int32_t id;		/* Id of the touch point, or -1. */
	TkXeSurface *surfPtr;	/* The surface it touched, or NULL. */
    } touches[TKWL_MAX_TOUCHES];
				/* The fingers on the screen. */
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

    TkXeSurface *keyboardSurface;
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
};

/*
 * Functions shared between the files of the Wayland port.
 */

/* tkWaylandSurface.c */
MODULE_SCOPE TkXeSurface *TkWlSurfaceFromWl(struct wl_surface *surface);
MODULE_SCOPE void	TkWlSurfaceSetRole(TkXeSurface *surfPtr,
			    int roleType);
MODULE_SCOPE void	TkWlSurfaceConfigured(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkWlUpdateScale(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkWlSurfaceSetCorners(TkXeSurface *surfPtr,
			    double radius, bool square);
MODULE_SCOPE void	TkWlSurfaceSetOpacity(TkXeSurface *surfPtr,
			    double opacity);

#define TKWL_ROLE_NONE		0
#define TKWL_ROLE_TOPLEVEL	1
#define TKWL_ROLE_POPUP		2

/*
 * The maximizations requested from the compositor (maximizeRequest).
 */

#define MAXIMIZE_NONE		0
#define MAXIMIZE_SET		1
#define MAXIMIZE_UNSET		2

/* tkWaylandWm.c */
MODULE_SCOPE void	TkWlWmUnmapSurface(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkWlWmSurfaceCommitted(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkWlWmSurfacePresented(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkWlSurfaceSetFrame(TkXeSurface *surfPtr,
			    struct libdecor_frame *frame);

/* tkWaylandSelect.c */
MODULE_SCOPE bool	TkWlSelectionBind(struct wl_registry *registry,
			    uint32_t name, const char *interface,
			    uint32_t version);
MODULE_SCOPE void	TkWlSelectionInit(void);

/* tkWaylandTextInput.c */
MODULE_SCOPE bool	TkWlTextInputBind(struct wl_registry *registry,
			    uint32_t name, const char *interface);
MODULE_SCOPE void	TkWlTextInputInit(void);
MODULE_SCOPE void	TkWlTextInputUpdate(void);
MODULE_SCOPE void	TkWlTextInputSetCaret(TkWindow *winPtr, int x, int y,
			    int height);

/*
 * The ::tk::wayland commands (tkWaylandCmds.c).
 */

MODULE_SCOPE bool	TkWlCmdsBind(struct wl_registry *registry,
			    uint32_t name, const char *interface);
MODULE_SCOPE const char *TkWlCmdsAppId(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkWlCmdsSurfaceMapped(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkWlCmdsSurfaceUnmapped(TkXeSurface *surfPtr);

#endif /* _TKWAYLANDINT */
