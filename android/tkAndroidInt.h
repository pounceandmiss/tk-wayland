/*
 * tkAndroidInt.h --
 *
 *	Declarations of the Android port, included by tkXemuInt.h when Tk is
 *	built for Android.
 *
 *	The Android port runs Tk on the emulated X server of ../xemu, as the
 *	Wayland port does: the window tree, drawing with Cairo, fonts and
 *	properties. What differs is where the images Tk draws are shown, and
 *	where the input comes from. An activity has a
 *	single window (an ANativeWindow), which is the screen of the
 *	emulated display: the children of the root window (the wrappers of
 *	the toplevels, menus and other override-redirect windows) are placed
 *	on it by the window manager of the port (tkAndroidWm.c), and their
 *	backing images are composited into it (tkAndroidSurface.c). Root
 *	coordinates are therefore real, as with X11. Input events of the
 *	activity are given to Tk by tkAndroidEvent.c.
 *
 *	Logical pixels are density-independent pixels (1/160 inch); the
 *	images are drawn at the density of the screen, with the scale as the
 *	device scale of Cairo.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKANDROIDINT
#define _TKANDROIDINT

#include "tkXemuInt.h"
#include "tkAndroid.h"

#define TKXE_VENDOR	"Android"

/*
 * Android has no xkbcommon: the names of keysyms are those of tkXemuKeysym.c,
 * and there is no keymap.
 */

#include "tkXemuXkb.h"

/*
 * The surface of a child of the root window: the image Tk draws into, and
 * the window manager state of the window.
 */

struct TkXeSurface {
    TkXeWindow *winPtr;		/* The X window of this surface. */
    bool iconic;		/* Always false: Android has no iconic
				 * windows. */
    double opacity;		/* From _NET_WM_WINDOW_OPACITY. */
    double scale;		/* Pixels of the backing image per logical
				 * pixel. */
    int width, height;		/* Size of the backing image, in logical
				 * pixels. */
    int bufferWidth, bufferHeight;
				/* Size of the backing image, in pixels. */
    cairo_surface_t *backing;	/* The image Tk draws into, with the scale as
				 * device scale. */
    bool exposePending;		/* The window tree changed; Expose events
				 * have to be generated. */

    /*
     * Where the surface was last shown on the screen, so that the parts of
     * the screen which change when it moves, is restacked or unmapped are
     * composited again.
     */

    bool shown;
    pixman_box32_t shownBox;	/* In root coordinates. */
    int shownIndex;		/* Position in the stacking order. */

    /*
     * Window manager state (tkAndroidWm.c).
     */

    int placement;		/* TKANDROID_PLACE_*. */
    bool inConfigure;		/* The window manager is resizing it. */
    struct TkXeSurface *nextPtr;
};

/*
 * How the window manager places a toplevel.
 */

#define TKANDROID_PLACE_NONE		0	/* Override-redirect. */
#define TKANDROID_PLACE_MAXIMIZED	1	/* Fills the content area. */
#define TKANDROID_PLACE_FULLSCREEN	2	/* Fills the window. */
#define TKANDROID_PLACE_DIALOG		3	/* Centered in the content
						 * area. */

/*
 * The display: the window of the activity, and the input state. There is
 * only one per process.
 */

struct TkXeDisplay {
    Display *display;		/* The emulated X display. */
    TkXeSurface *surfaces;
    struct xkb_keymap *keymap;	/* Always NULL. */
    int eventFd;		/* Read end of the pipe of the events the
				 * glue posts. */

    /*
     * The window of the activity.
     */

    ANativeWindow *window;	/* NULL while the activity has none. */
    int windowWidth, windowHeight;
				/* Its size, in pixels. */
    int content[4];		/* The part of the window not covered by
				 * system bars and the soft keyboard (left,
				 * top, right, bottom), in pixels. */
    double scale;		/* Pixels per logical pixel. */
    double forcedScale;		/* Scale set with TK_ANDROID_SCALE, or 0. */
    int density;		/* Of the screen, in dots per inch. */
    bool visible;		/* The activity is visible (resumed). */
    bool hasFocus;		/* The window of the activity has the
				 * focus. */
    pixman_region32_t damage;	/* Part of the screen to composite again, in
				 * root coordinates. */
    bool presentPending;	/* A present is scheduled at idle time. */
    bool presentDeferred;	/* The present was already deferred once. */

    /*
     * Input state.
     */

    double pointerX, pointerY;	/* In root coordinates. */
    bool pointerInside;		/* The pointer (a mouse, or a finger touching
				 * the screen) is in the window. */
    unsigned int buttonState;	/* Button1Mask etc. */
    unsigned int modState;	/* ShiftMask etc. */
    Time lastTime;		/* Time of the last input event. */
};

/*
 * Functions shared between the files of the Android port.
 */

/* tkAndroidSurface.c */
MODULE_SCOPE void	TkAndroidSetWindow(ANativeWindow *window);
MODULE_SCOPE void	TkAndroidUpdateScale(void);
MODULE_SCOPE void	TkAndroidDamageScreen(int x, int y, int width,
			    int height);
MODULE_SCOPE TkXeSurface *TkAndroidSurfaceAt(int x, int y);
MODULE_SCOPE void	TkAndroidSurfaceSetOpacity(TkXeSurface *surfPtr,
			    double opacity);

/* tkAndroidWm.c */
MODULE_SCOPE void	TkAndroidWmWorkArea(int *xPtr, int *yPtr,
			    int *widthPtr, int *heightPtr);
MODULE_SCOPE void	TkAndroidWmRelayout(void);
MODULE_SCOPE void	TkAndroidWmBack(void);
MODULE_SCOPE void	TkAndroidWmUnmapSurface(TkXeSurface *surfPtr);

/* tkAndroidEvent.c */
MODULE_SCOPE void	TkAndroidSetKeyboardSurface(TkXeSurface *surfPtr);
MODULE_SCOPE TkXeSurface *TkAndroidKeyboardSurface(void);
MODULE_SCOPE void	TkAndroidQueueKey(KeySym keysym);
MODULE_SCOPE JNIEnv *	TkAndroidGetEnv(void);

/* tkAndroidSelect.c */
MODULE_SCOPE void	TkAndroidClipboardCheck(void);

/* tkAndroidKey.c */
MODULE_SCOPE KeySym	TkAndroidKeySym(int keyCode, int metaState,
			    int unicode);
MODULE_SCOPE unsigned int TkAndroidModState(int metaState);
MODULE_SCOPE KeyCode	TkAndroidKeyCode(KeySym keysym);

#endif /* _TKANDROIDINT */
