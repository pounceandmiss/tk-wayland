/*
 * tkHaiku.h --
 *
 *	The interface between the glue of the Be API (tkHaikuApp.cpp, in C++)
 *	and the Haiku port of Tk (in C).
 *
 *	Threads: Tcl and Tk run in the thread which opened the display. The
 *	BApplication runs its looper in a thread of its own, and every BWindow
 *	in another. Those threads never wait for Tk: what happens to the
 *	application and its windows (input, moves and resizes by the user,
 *	activation, the clipboard) is posted as events, which are queued and
 *	read by Tk through a pipe (tkHaikuEvent.c), so that Tk may be busy
 *	for long (in a blocking read, say) while the windows keep drawing and
 *	the input is kept for it. Tk calls the functions below, which lock
 *	the window they change, and may wait for the threads of the Be API
 *	(for a lock, or the end of a window), which never wait for Tk: there
 *	is no deadlock.
 *
 *	Coordinates are those of the screen, in pixels. The images Tk draws
 *	are 32-bit xRGB, which is also the layout of B_RGB32.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKHAIKU
#define _TKHAIKU

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The kinds of events posted by the glue.
 */

typedef enum {
    TKHAIKU_POINTER,		/* The mouse moved, or a button was pressed
				 * or released. */
    TKHAIKU_WHEEL,		/* The mouse wheel turned. */
    TKHAIKU_KEY,		/* A key was pressed or released. */
    TKHAIKU_MODIFIERS,		/* Modifier keys were pressed or
				 * released. */
    TKHAIKU_ACTIVATED,		/* A window was activated or
				 * deactivated. */
    TKHAIKU_MOVED,		/* The user moved a window. */
    TKHAIKU_RESIZED,		/* The user resized a window. */
    TKHAIKU_CLOSE,		/* The close button of a window. */
    TKHAIKU_MINIMIZED,		/* A window was minimized or restored. */
    TKHAIKU_ZOOM,		/* The zoom button of a window. */
    TKHAIKU_SCREEN,		/* The screen changed. */
    TKHAIKU_CLIPBOARD,		/* The system clipboard changed. */
    TKHAIKU_KEYMAP,		/* The user chose another keymap. */
    TKHAIKU_SCRIPT,		/* A scripting request for the widgets
				 * of Tk (see TkHaikuGetScript). */
    TKHAIKU_QUIT		/* The user asked the application to quit
				 * (from the Deskbar). */
} TkHaikuEventType;

/*
 * Pointer actions (TKHAIKU_POINTER).
 */

#define TKHAIKU_MOVE		0
#define TKHAIKU_DOWN		1
#define TKHAIKU_UP		2
#define TKHAIKU_EXIT		3	/* The pointer left the window. */

/*
 * Buttons, as in the "buttons" of the messages of the Be API.
 */

#define TKHAIKU_PRIMARY_BUTTON		0x1	/* The left button. */
#define TKHAIKU_SECONDARY_BUTTON	0x2	/* The right button. */
#define TKHAIKU_TERTIARY_BUTTON		0x4	/* The middle button. */

/*
 * Modifiers, as the B_*_KEY bits of the Be API.
 */

#define TKHAIKU_SHIFT		0x00000001
#define TKHAIKU_COMMAND		0x00000002	/* Alt, by default. */
#define TKHAIKU_CONTROL		0x00000004
#define TKHAIKU_CAPS_LOCK	0x00000008
#define TKHAIKU_SCROLL_LOCK	0x00000010
#define TKHAIKU_NUM_LOCK	0x00000020
#define TKHAIKU_OPTION		0x00000040	/* The Windows key. */
#define TKHAIKU_MENU		0x00000080
#define TKHAIKU_LEFT_SHIFT	0x00000100
#define TKHAIKU_RIGHT_SHIFT	0x00000200
#define TKHAIKU_LEFT_COMMAND	0x00000400
#define TKHAIKU_RIGHT_COMMAND	0x00000800
#define TKHAIKU_LEFT_CONTROL	0x00001000
#define TKHAIKU_RIGHT_CONTROL	0x00002000
#define TKHAIKU_LEFT_OPTION	0x00004000
#define TKHAIKU_RIGHT_OPTION	0x00008000

typedef struct TkHaikuEvent {
    TkHaikuEventType type;
    unsigned long window;	/* The X window of the BWindow the event is
				 * for, or 0. */
    long long time;		/* Time of the event, in milliseconds since
				 * boot (system_time). */
    union {
	struct {
	    int action;		/* TKHAIKU_MOVE etc. */
	    float x, y;		/* Position on the screen. */
	    uint32_t buttons;	/* Pressed buttons
				 * (TKHAIKU_PRIMARY_BUTTON etc.). */
	    uint32_t modifiers;	/* Modifier keys (B_SHIFT_KEY etc.). */
	    bool inside;	/* The pointer is in the window. */
	} pointer;		/* TKHAIKU_POINTER. */
	struct {
	    float dx, dy;	/* Turned amount, in notches; positive is
				 * down and right. */
	    uint32_t modifiers;
	} wheel;		/* TKHAIKU_WHEEL. */
	struct {
	    bool down;		/* Pressed or released. */
	    int key;		/* The key code of the Be API (its position
				 * on the keyboard). */
	    uint32_t modifiers;	/* B_SHIFT_KEY etc. */
	    char bytes[8];	/* The characters typed, in UTF-8. */
	} key;			/* TKHAIKU_KEY. */
	uint32_t modifiers;	/* TKHAIKU_MODIFIERS: the new modifiers. */
	void *script;		/* TKHAIKU_SCRIPT: the request, to be
				 * answered with TkHaikuScriptReply. */
	bool active;		/* TKHAIKU_ACTIVATED. */
	bool minimized;		/* TKHAIKU_MINIMIZED. */
	struct {
	    int x, y;		/* TKHAIKU_MOVED: the new position of the
				 * content of the window. */
	    int width, height;	/* TKHAIKU_RESIZED: its new size. */
	} frame;
    } u;
} TkHaikuEvent;

/*
 * The modifier keys of the keymap, in the order of TkHaikuModifierKeys.
 */

#define TKHAIKU_KEY_LEFT_SHIFT		0
#define TKHAIKU_KEY_RIGHT_SHIFT		1
#define TKHAIKU_KEY_LEFT_COMMAND	2
#define TKHAIKU_KEY_RIGHT_COMMAND	3
#define TKHAIKU_KEY_LEFT_CONTROL	4
#define TKHAIKU_KEY_RIGHT_CONTROL	5
#define TKHAIKU_KEY_LEFT_OPTION		6
#define TKHAIKU_KEY_RIGHT_OPTION	7

/*
 * Kinds of windows.
 */

#define TKHAIKU_TOPLEVEL	0	/* Decorated, takes the focus. */
#define TKHAIKU_POPUP		1	/* Override-redirect: no
					 * decorations, never takes the focus,
					 * above the other windows. */

/*
 * Levels (window feels).
 */

#define TKHAIKU_NORMAL		0
#define TKHAIKU_ABOVE		1	/* Above the windows of all
					 * applications (topmost). */

typedef struct TkHaikuWindow TkHaikuWindow;

/*
 * A scripting request of Haiku (as hey sends them) for the property
 * "Widget" of the application or of a window (the window of the event), as
 * TkHaikuGetScript reads it. The strings are freed by TkHaikuScriptReply.
 */

typedef struct TkHaikuScript {
    const char *verb;		/* "get", "set", "do" or "count". */
    char *property;		/* The property of the widget (Text, ...),
				 * or "" for do and count. */
    const char *form;		/* How the widget is given: "name",
				 * "index", "reverse" (an index from the
				 * end, 1 for the last), or "" for
				 * count. */
    char *widget;		/* Its name or index. */
    char *value;		/* The value to set, or "". */
} TkHaikuScript;

/*
 * The name of the view of a window, from its X window (see TkView).
 */

#define TKHAIKU_VIEW_NAME	"tk 0x%lx"

/*
 * Called by the glue, from any thread (tkHaikuEvent.c).
 */

extern void		TkHaikuPostEvent(const TkHaikuEvent *eventPtr);

/*
 * The glue, called by Tk (tkHaikuApp.cpp).
 */

extern bool		TkHaikuStartApp(const char *signature);
extern void		TkHaikuScreenSize(int *widthPtr, int *heightPtr,
			    double *dpiPtr);
extern void		TkHaikuWorkArea(int *xPtr, int *yPtr, int *widthPtr,
			    int *heightPtr);
extern void		TkHaikuGetMouse(int *xPtr, int *yPtr,
			    uint32_t *buttonsPtr);
extern void		TkHaikuSetMousePosition(int x, int y);
extern uint32_t		TkHaikuModifiers(void);
extern long long	TkHaikuSystemTime(void);
extern void		TkHaikuBeep(void);
extern double		TkHaikuFontScale(void);
extern void		TkHaikuFontFamilies(char *plain, char *fixed,
			    int size);

extern TkHaikuWindow *	TkHaikuWindowCreate(unsigned long window,
			    int kind, int x, int y, int width, int height);
extern void		TkHaikuWindowDestroy(TkHaikuWindow *winPtr);
extern void		TkHaikuWindowShow(TkHaikuWindow *winPtr, bool show);
extern void		TkHaikuWindowMoveResize(TkHaikuWindow *winPtr,
			    int x, int y, int width, int height);
extern void		TkHaikuWindowSetTitle(TkHaikuWindow *winPtr,
			    const char *title);
extern void		TkHaikuWindowSetSizeLimits(TkHaikuWindow *winPtr,
			    int minWidth, int minHeight, int maxWidth,
			    int maxHeight);
extern void		TkHaikuWindowSetDecorated(TkHaikuWindow *winPtr,
			    bool decorated);
extern void		TkHaikuWindowSetLevel(TkHaikuWindow *winPtr,
			    int level);
extern void		TkHaikuWindowMinimize(TkHaikuWindow *winPtr,
			    bool minimize);
extern void		TkHaikuWindowActivate(TkHaikuWindow *winPtr);
extern bool		TkHaikuWindowIsActive(TkHaikuWindow *winPtr);
extern void		TkHaikuWindowSendBehind(TkHaikuWindow *winPtr,
			    TkHaikuWindow *otherPtr);
extern void		TkHaikuWindowGetFrame(TkHaikuWindow *winPtr,
			    int *xPtr, int *yPtr, int *widthPtr,
			    int *heightPtr);
extern void		TkHaikuWindowFrameExtents(TkHaikuWindow *winPtr,
			    int *leftPtr, int *topPtr, int *rightPtr,
			    int *bottomPtr);
extern void		TkHaikuWindowUpdate(TkHaikuWindow *winPtr,
			    const unsigned char *data, int stride, int width,
			    int height, const int *rects, int numRects);
extern void		TkHaikuWindowSetCursor(TkHaikuWindow *winPtr,
			    int cursorId, const uint32_t *argb, int width,
			    int height, int xHot, int yHot);
extern void		TkHaikuWindowCapture(TkHaikuWindow *winPtr,
			    bool capture);

extern bool		TkHaikuClipboardGet(char **textPtr,
			    size_t *lengthPtr);
extern void		TkHaikuClipboardSet(const char *text, size_t length);
extern void		TkHaikuClipboardClear(void);
extern uint32_t		TkHaikuClipboardCount(void);
extern int		TkHaikuKeymapChars(int key, uint32_t modifiers,
			    char *buffer, int size);
extern void		TkHaikuModifierKeys(int keys[8]);
extern bool		TkHaikuGetScript(void *request,
			    TkHaikuScript *scriptPtr);
extern void		TkHaikuScriptReply(void *request,
			    TkHaikuScript *scriptPtr, const char *error,
			    const char *type, const char *value);
extern void		TkHaikuKeymapChanged(void);

#ifdef __cplusplus
}
#endif

#endif /* _TKHAIKU */
