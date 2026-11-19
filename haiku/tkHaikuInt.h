/*
 * tkHaikuInt.h --
 *
 *	Declarations of the Haiku port, included by tkXemuInt.h when Tk is
 *	built for Haiku.
 *
 *	The Haiku port runs Tk on the emulated X server of ../xemu, as the
 *	Wayland and Android ports do. Every child of the root window (the
 *	wrapper of a toplevel, or an override-redirect window) has a BWindow
 *	(tkHaikuApp.cpp, through tkHaiku.h), whose view shows the backing
 *	image of the surface (tkHaikuSurface.c). Haiku places windows where
 *	applications ask, so root coordinates are those of the screen, as
 *	with X11; the app_server draws the decorations around the windows,
 *	which the window manager (tkHaikuWm.c) reports in _NET_FRAME_EXTENTS,
 *	as X window managers which do not reparent do. Input comes from the
 *	threads of the windows as events (tkHaikuEvent.c).
 *
 *	Logical pixels have 96 dpi times the scale of the fonts the user
 *	chose; the images are drawn at the scale, as the device scale of
 *	Cairo, and root coordinates are screen pixels divided by it.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKHAIKUINT
#define _TKHAIKUINT

#include "tkXemuInt.h"
#include "tkHaiku.h"

#define TKXE_VENDOR	"Haiku"

/*
 * Haiku has no xkbcommon: the names of keysyms are those of tkXemuKeysym.c,
 * and there is no keymap (keys are translated with the keymap of Haiku, by
 * tkHaikuKey.c).
 */

#include "tkXemuXkb.h"

/*
 * The surface of a child of the root window: the image Tk draws into, its
 * BWindow, and the window manager state of the window.
 */

struct TkXeSurface {
    TkXeWindow *winPtr;		/* The X window of this surface. */
    bool iconic;		/* The window is minimized. */
    double scale;		/* Pixels of the backing image per logical
				 * pixel. */
    int width, height;		/* Size of the backing image, in logical
				 * pixels. */
    int bufferWidth, bufferHeight;
				/* Size of the backing image, in pixels. */
    cairo_surface_t *backing;	/* The image Tk draws into, with the scale
				 * as device scale. */
    bool exposePending;		/* The window tree changed; Expose events
				 * have to be generated. */
    pixman_region32_t damage;	/* Part of the image to show again, in
				 * logical pixels. */
    TkHaikuWindow *window;	/* The BWindow, or NULL while the window was
				 * never mapped. */
    bool shown;			/* The BWindow is shown. */
    bool popup;			/* The BWindow is that of an
				 * override-redirect window. */

    /*
     * Window manager state (tkHaikuWm.c).
     */

    bool inConfigure;		/* The window manager is moving or resizing
				 * the X window. */
    int wmX, wmY;		/* Where the window manager last put the X
				 * window: a move elsewhere is a request of
				 * the client. */
    int clampedWidth, clampedHeight;
				/* The size the app_server gave the BWindow
				 * instead of the one Tk asked for (it has a
				 * minimum size), in pixels, or 0. */
    bool placed;		/* The window manager placed the window. */
    bool maximized, fullscreen;	/* From _NET_WM_STATE. */
    int savedX, savedY;		/* Geometry before it was maximized or */
    int savedWidth, savedHeight;/* made fullscreen. */
    int level;			/* TKHAIKU_NORMAL or TKHAIKU_ABOVE. */
    int stackIndex;		/* Position among the mapped children of the
				 * root window, when the BWindow was last
				 * restacked. */
    struct TkXeSurface *nextPtr;
};

/*
 * The display. There is only one per process.
 */

struct TkXeDisplay {
    Display *display;		/* The emulated X display. */
    TkXeSurface *surfaces;
    struct xkb_keymap *keymap;	/* Always NULL. */
    int eventFd;		/* Read end of the pipe of the events the
				 * glue posts. */
    double scale;		/* Pixels per logical pixel. */
    bool presentPending;	/* A present is scheduled at idle time. */

    /*
     * Input state.
     */

    double pointerX, pointerY;	/* In root coordinates. */
    bool pointerInside;		/* The pointer is in a window of Tk. */
    TkXeSurface *pointerSurface;/* The surface under the pointer, or
				 * NULL. */
    unsigned int buttonState;	/* Button1Mask etc. */
    unsigned int modState;	/* ShiftMask etc. */
    Time lastTime;		/* Time of the last input event. */

    /*
     * The pointer moves where it is warped later, and events from before
     * come after the warp: they are dropped, and the position of the
     * warp is the position of the pointer until it is there.
     */

    long long warpTime;		/* When the pointer was last warped. */
    bool warpPending;		/* It may not be there yet. */
    int warpX, warpY;		/* Where it was warped, in root
				 * coordinates. */
};

/*
 * Conversions between logical pixels (root coordinates) and screen pixels.
 */

static inline int
TkHaikuToScreen(
    double length)
{
    return (int)lround(length * tkXeDisplay->scale);
}

static inline double
TkHaikuFromScreen(
    double length)
{
    return length / tkXeDisplay->scale;
}

/*
 * The X state of the buttons of the mouse: the middle button is Button2,
 * the right one Button3.
 */

static inline unsigned int
TkHaikuButtonState(
    uint32_t buttons)		/* TKHAIKU_PRIMARY_BUTTON etc. */
{
    return ((buttons & TKHAIKU_PRIMARY_BUTTON) ? Button1Mask : 0)
	    | ((buttons & TKHAIKU_TERTIARY_BUTTON) ? Button2Mask : 0)
	    | ((buttons & TKHAIKU_SECONDARY_BUTTON) ? Button3Mask : 0);
}

/*
 * Functions shared between the files of the Haiku port.
 */

/* tkHaikuSurface.c */
MODULE_SCOPE void	TkHaikuShowSurface(TkXeSurface *surfPtr, bool show);
MODULE_SCOPE void	TkHaikuPresent(void);
MODULE_SCOPE void	TkHaikuSyncFrame(TkXeSurface *surfPtr);
MODULE_SCOPE TkXeSurface *TkHaikuSurfaceAt(int x, int y,
			    TkXeSurface *exceptPtr);
MODULE_SCOPE void	TkHaikuDestroyWindow(TkXeSurface *surfPtr);

/* tkHaikuWm.c */
MODULE_SCOPE void	TkHaikuWmUnmapSurface(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkHaikuWmConfigured(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkHaikuWmEvent(TkXeSurface *surfPtr,
			    TkHaikuEvent *eventPtr);
MODULE_SCOPE void	TkHaikuWmScreenChanged(void);
MODULE_SCOPE void	TkHaikuWmQuit(void);
MODULE_SCOPE void	TkHaikuWmRestack(void);

/* tkHaikuEvent.c */
MODULE_SCOPE void	TkHaikuSetKeyboardSurface(TkXeSurface *surfPtr);
MODULE_SCOPE TkXeSurface *TkHaikuKeyboardSurface(void);
MODULE_SCOPE void	TkHaikuUpdateScreen(void);
MODULE_SCOPE void	TkHaikuUpdateCapture(void);
MODULE_SCOPE void	TkHaikuForgetSurface(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkHaikuForgetWindow(TkXeSurface *surfPtr);
MODULE_SCOPE void	TkHaikuPointerWindowChanged(bool mayEnter);
MODULE_SCOPE void	TkHaikuPointerLeaveSurface(TkXeSurface *surfPtr);

/* tkHaikuCursor.c */
MODULE_SCOPE void	TkHaikuForgetCursorWindow(TkHaikuWindow *window);

/* tkHaikuScript.c */
MODULE_SCOPE void	TkHaikuHandleScript(TkHaikuEvent *eventPtr);

/* tkHaikuSelect.c */
MODULE_SCOPE void	TkHaikuClipboardChanged(void);

/* tkHaikuKey.c */
MODULE_SCOPE KeySym	TkHaikuKeySym(int key, uint32_t modifiers,
			    const char *bytes);
MODULE_SCOPE unsigned int TkHaikuModState(uint32_t modifiers);
MODULE_SCOPE KeyCode	TkHaikuKeyCode(KeySym keysym, bool *shiftPtr);

#endif /* _TKHAIKUINT */
