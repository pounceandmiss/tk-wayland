/*
 * tkAndroid.h --
 *
 *	The interface between the glue of the NativeActivity (tkAndroidMain.c)
 *	and the Android port of Tk. The callbacks of the activity run in the
 *	main thread of the application, and Tcl and Tk in a thread of their
 *	own: the glue posts what happens to the activity (its window, its
 *	input, its configuration) as events, which Tk reads in its event
 *	loop.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKANDROID
#define _TKANDROID

#include <android/native_activity.h>
#include <android/native_window.h>

/*
 * The kinds of events.
 */

typedef enum {
    TKANDROID_WINDOW_CREATED,	/* The activity has a window. */
    TKANDROID_WINDOW_DESTROYED,	/* The window is going away: it must no
				 * longer be drawn into. */
    TKANDROID_WINDOW_RESIZED,	/* The window changed size. */
    TKANDROID_REDRAW,		/* The window must be drawn again. */
    TKANDROID_CONTENT_RECT,	/* The part of the window not covered by the
				 * system bars and the soft keyboard. */
    TKANDROID_FOCUS,		/* The window gained or lost the focus. */
    TKANDROID_CONFIG,		/* The configuration (density) changed. */
    TKANDROID_PAUSE,		/* The activity is no longer visible. */
    TKANDROID_RESUME,		/* The activity is visible again. */
    TKANDROID_POINTER,		/* A touch or mouse event. */
    TKANDROID_SCROLL,		/* A mouse wheel event. */
    TKANDROID_KEY,		/* A key event. */
    TKANDROID_TEXT,		/* Text the soft keyboard committed. */
    TKANDROID_DELETE,		/* The soft keyboard deletes text. */
    TKANDROID_KEYBOARD,		/* The soft keyboard was shown or hidden. */
    TKANDROID_BACK		/* The back button or gesture. */
} TkAndroidEventType;

/*
 * Actions of pointer events.
 */

#define TKANDROID_DOWN		0
#define TKANDROID_UP		1
#define TKANDROID_MOVE		2
#define TKANDROID_CANCEL	3
#define TKANDROID_HOVER		4
#define TKANDROID_LEAVE		5

typedef struct TkAndroidEvent {
    TkAndroidEventType type;
    long long time;		/* Time of input events, in milliseconds of
				 * the monotonic clock. */
    union {
	ANativeWindow *window;	/* TKANDROID_WINDOW_CREATED and _DESTROYED.
				 * The glue acquires a reference to the
				 * window, which the receiver releases. */
	struct {
	    int left, top, right, bottom;
	} rect;			/* TKANDROID_CONTENT_RECT, in pixels. */
	int hasFocus;		/* TKANDROID_FOCUS. */
	int keyboardShown;	/* TKANDROID_KEYBOARD. */
	int density;		/* TKANDROID_CONFIG, in dots per inch. */
	struct {
	    int action;		/* TKANDROID_DOWN etc. */
	    int id;		/* Identifies the finger. */
	    float x, y;		/* Position in the window, in pixels. */
	    int buttons;	/* Pressed mouse buttons (AMOTION_EVENT_BUTTON_*
				 * bits), or 0 for touches. */
	    int isMouse;	/* Whether the pointer is a mouse. */
	} pointer;		/* TKANDROID_POINTER. */
	struct {
	    float x, y;		/* Position, in pixels. */
	    float dx, dy;	/* Scrolled amount, in notches. */
	} scroll;		/* TKANDROID_SCROLL. */
	struct {
	    int down;		/* Pressed or released. */
	    int keyCode;	/* AKEYCODE_*. */
	    int metaState;	/* AMETA_*. */
	    int unicode;	/* The character the key produces with the
				 * meta state, or 0. */
	} key;			/* TKANDROID_KEY. */
	struct {
	    unsigned short *chars;
				/* UTF-16, allocated with malloc and freed
				 * by the receiver. */
	    int length;		/* Number of UTF-16 units. */
	} text;			/* TKANDROID_TEXT. */
	struct {
	    int before, after;	/* Characters to delete before and after
				 * the insertion cursor. */
	} del;			/* TKANDROID_DELETE. */
    } u;
} TkAndroidEvent;

/*
 * The activity, set by the glue before Tcl starts.
 */

extern ANativeActivity *tkAndroidActivity;

/*
 * Functions of the port called by the glue, from any thread.
 */

extern void		TkAndroidPostEvent(const TkAndroidEvent *eventPtr);
extern void		TkAndroidPostEventAndWait(
			    const TkAndroidEvent *eventPtr, int timeout);

#endif /* _TKANDROID */
