/*
 * tkKeyboard.c --
 *
 *	On-screen keyboards ("tk keyboard"). The ports which have one (Wayland,
 *	where it is shown by the compositor when text input is enabled)
 *	register a procedure with TkSetKeyboardProc, which shows or hides it,
 *	and tell with TkKeyboardChanged when it was shown or hidden, if they
 *	know.
 *
 *	In the automatic mode (the default), the keyboard follows the focus:
 *	it is shown when the focus is in a widget which shows an insertion
 *	cursor (which calls Tk_SetCaretPos) and can be edited, and hidden when
 *	the focus moves to another window, except a menu or a window of an
 *	override-redirect toplevel (a popup), which leave it as it is. In the
 *	manual mode, only "tk keyboard show" and "tk keyboard hide" move it.
 *	The type of a widget ("tk keyboard type") tells the keyboard what kind
 *	of text it takes, which may change its layout; "none" means that the
 *	keyboard is never shown for it.
 *
 *	The keyboard sends <<KeyboardShown>> and <<KeyboardHidden>> virtual
 *	events to the window with the focus when the port tells it was shown
 *	or hidden.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkInt.h"

/*
 * When the focus no longer wants the keyboard, it is hidden after this many
 * milliseconds if that is still the case: the focus may have moved to
 * another widget which takes text, which has not drawn its insertion cursor
 * yet.
 */

#define HIDE_DELAY	100

static const char *const typeNames[] = {
    "text", "number", "phone", "email", "url", "password", "none", NULL
};

/*
 * The procedure of the port, for the process.
 */

static TkKeyboardProc *keyboardProc = NULL;

typedef struct {
    bool initialized;
    Tcl_HashTable typeTable;	/* Types set with "tk keyboard type", keyed by
				 * window. */
    bool manual;		/* The mode is manual. */
    bool requested;		/* Tk asked for the keyboard to be shown. */
    int shown;			/* Whether the port said the keyboard is shown,
				 * or -1 if it did not. */
    Display *display;		/* The display of lastFocus. */
    Window lastFocus;		/* The window which had the focus, and */
    bool lastWanted;		/* whether it wanted the keyboard, when */
    int lastType;		/* the keyboard was last decided. */
    Window caretWindow;		/* The last window which set its insertion
				 * cursor while it had the focus: widgets also
				 * set it without the focus (texts, when they
				 * draw their insertion cursor). */
    Tcl_TimerToken hideTimer;	/* Hides the keyboard, or NULL. */
} ThreadSpecificData;
static Tcl_ThreadDataKey dataKey;

static int		ButtonProc(void *clientData, XEvent *eventPtr);
static void		CheckProc(void *clientData, int flags);
static void		Decide(ThreadSpecificData *tsdPtr, bool hideNow);
static bool		Editable(TkWindow *winPtr);
static int		GetType(TkWindow *winPtr);
static ThreadSpecificData *GetTSD(void);
static void		HideProc(void *clientData);
static bool		InPopup(TkWindow *winPtr);
static void		QueueKeyboardEvent(Tk_Window tkwin, bool shown);
static void		SetupProc(void *clientData, int flags);
static void		ShowKeyboard(ThreadSpecificData *tsdPtr,
			    TkWindow *winPtr, bool show, int type);
static void		TypeEventProc(void *clientData, XEvent *eventPtr);

/*
 *----------------------------------------------------------------------
 *
 * GetTSD --
 *
 *	The keyboard state of the thread.
 *
 * Results:
 *	The state, initialized the first time.
 *
 * Side effects:
 *	May initialize the table of types.
 *
 *----------------------------------------------------------------------
 */

static ThreadSpecificData *
GetTSD(void)
{
    ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    if (!tsdPtr->initialized) {
	tsdPtr->initialized = true;
	Tcl_InitHashTable(&tsdPtr->typeTable, TCL_ONE_WORD_KEYS);
	tsdPtr->shown = -1;
	tsdPtr->lastType = -1;
    }
    return tsdPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkSetKeyboardProc --
 *
 *	Called by a port which has an on-screen keyboard, to register the
 *	procedure which shows and hides it.
 *
 * Results:
 *	The procedure registered before, or NULL.
 *
 * Side effects:
 *	The keyboard follows the focus from now on, in the automatic mode.
 *
 *----------------------------------------------------------------------
 */

TkKeyboardProc *
TkSetKeyboardProc(
    TkKeyboardProc *proc)	/* The procedure, or NULL. */
{
    ThreadSpecificData *tsdPtr = GetTSD();
    TkKeyboardProc *oldProc = keyboardProc;

    keyboardProc = proc;
    if (proc && oldProc == NULL) {
	Tcl_CreateEventSource(SetupProc, CheckProc, NULL);
	Tk_CreateGenericHandler(ButtonProc, NULL);
    } else if (proc == NULL && oldProc) {
	Tcl_DeleteEventSource(SetupProc, CheckProc, NULL);
	Tk_DeleteGenericHandler(ButtonProc, NULL);
    }
    tsdPtr->requested = false;
    tsdPtr->shown = -1;
    tsdPtr->lastFocus = None;
    tsdPtr->lastType = -1;
    return oldProc;
}

/*
 *----------------------------------------------------------------------
 *
 * TkKeyboardChanged --
 *
 *	Called by the port when the keyboard was shown or hidden, by Tk or by
 *	the user.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	<<KeyboardShown>> or <<KeyboardHidden>> is sent to the window with the
 *	focus, or to tkwin.
 *
 *----------------------------------------------------------------------
 */

void
TkKeyboardChanged(
    Tk_Window tkwin,		/* A window of the display. */
    int shown)			/* Whether the keyboard is shown. */
{
    ThreadSpecificData *tsdPtr = GetTSD();
    TkDisplay *dispPtr;

    if (tsdPtr->shown == (shown != 0)) {
	return;
    }
    tsdPtr->shown = (shown != 0);
    if (!shown) {
	/*
	 * The user may have hidden it: it is shown again only when the focus
	 * moves, or when the window with the focus is clicked (ButtonProc).
	 */

	tsdPtr->requested = false;
    }
    dispPtr = tkwin ? ((TkWindow *)tkwin)->dispPtr : TkGetDisplayList();
    if (dispPtr && dispPtr->focusPtr
	    && !(dispPtr->focusPtr->flags & TK_ALREADY_DEAD)) {
	tkwin = (Tk_Window)dispPtr->focusPtr;
    }
    if (tkwin) {
	QueueKeyboardEvent(tkwin, shown != 0);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkKeyboardSetCaret --
 *
 *	Called by Tk_SetCaretPos: a window which sets its insertion cursor
 *	while it has the focus wants the keyboard (if it can be edited).
 *	Others are ignored: a text widget sets its insertion cursor when it
 *	draws it, also when it does not have the focus, which would take the
 *	keyboard from the window which has the focus.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The window is recorded, for Decide.
 *
 *----------------------------------------------------------------------
 */

void
TkKeyboardSetCaret(
    TkWindow *winPtr)		/* The window setting its insertion cursor. */
{
    if (winPtr == winPtr->dispPtr->focusPtr) {
	GetTSD()->caretWindow = winPtr->window;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * QueueKeyboardEvent --
 *
 *	Queue <<KeyboardShown>> or <<KeyboardHidden>> for a window.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is queued.
 *
 *----------------------------------------------------------------------
 */

static void
QueueKeyboardEvent(
    Tk_Window tkwin,		/* The window. */
    bool shown)			/* Which event. */
{
    union {
	XEvent general;
	XVirtualEvent virt;
    } event;

    if (Tk_WindowId(tkwin) == None) {
	return;
    }
    memset(&event, 0, sizeof(event));
    event.general.xany.type = VirtualEvent;
    event.general.xany.serial = NextRequest(Tk_Display(tkwin));
    event.general.xany.send_event = False;
    event.general.xany.window = Tk_WindowId(tkwin);
    event.general.xany.display = Tk_Display(tkwin);
    event.virt.name = Tk_GetUid(shown ? "KeyboardShown" : "KeyboardHidden");
    event.virt.root = RootWindow(Tk_Display(tkwin), Tk_ScreenNumber(tkwin));
    event.virt.time = TkpGetEventTime();
    event.virt.same_screen = True;
    Tk_QueueWindowEvent(&event.general, TCL_QUEUE_TAIL);
}

/*
 *----------------------------------------------------------------------
 *
 * ShowKeyboard --
 *
 *	Ask the port to show or hide the keyboard.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The keyboard is shown or hidden.
 *
 *----------------------------------------------------------------------
 */

static void
ShowKeyboard(
    ThreadSpecificData *tsdPtr,	/* The keyboard state. */
    TkWindow *winPtr,		/* The window it is for, or NULL. */
    bool show,			/* Show or hide. */
    int type)			/* TK_KEYBOARD_*. */
{
    if (tsdPtr->hideTimer) {
	Tcl_DeleteTimerHandler(tsdPtr->hideTimer);
	tsdPtr->hideTimer = NULL;
    }
    tsdPtr->requested = show;
    if (keyboardProc) {
	keyboardProc((Tk_Window)winPtr, show, type);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * InPopup, Editable, GetType --
 *
 *	What the window with the focus is: in a menu or a popup, whose focus
 *	leaves the keyboard as it is; a widget which can be edited (in the
 *	normal state, or neither disabled nor readonly for ttk widgets; other
 *	widgets which show an insertion cursor are taken to be); and the type
 *	of its text.
 *
 * Results:
 *	As above.
 *
 * Side effects:
 *	Editable and GetType evaluate scripts in the interpreter of the
 *	window.
 *
 *----------------------------------------------------------------------
 */

static bool
InPopup(
    TkWindow *winPtr)		/* The window with the focus. */
{
    TkWindow *topPtr;

    if (winPtr->classUid && !strcmp(winPtr->classUid, "Menu")) {
	return true;
    }
    for (topPtr = winPtr; topPtr && !(topPtr->flags & TK_TOP_HIERARCHY);
	    topPtr = topPtr->parentPtr) {
	/* Empty loop body. */
    }
    return topPtr && topPtr->atts.override_redirect;
}

static bool
Editable(
    TkWindow *winPtr)		/* A window which shows an insertion
				 * cursor. */
{
    Tcl_Interp *interp;
    Tcl_InterpState state;
    Tcl_Obj *scriptPtr;
    int editable = 1;

    if (winPtr->mainPtr == NULL || winPtr->pathName == NULL) {
	return false;
    }
    interp = winPtr->mainPtr->interp;
    state = Tcl_SaveInterpState(interp, TCL_OK);
    scriptPtr = Tcl_ObjPrintf(
	    "if {![catch {%s instate {!disabled !readonly}} r]} {"
	    "    set r"
	    "} elseif {![catch {%s cget -state} s]} {"
	    "    expr {$s eq \"normal\"}"
	    "} else {"
	    "    expr 1"
	    "}", winPtr->pathName, winPtr->pathName);
    Tcl_IncrRefCount(scriptPtr);
    if (Tcl_EvalObjEx(interp, scriptPtr, TCL_EVAL_GLOBAL) != TCL_OK
	    || Tcl_GetBooleanFromObj(NULL, Tcl_GetObjResult(interp),
		&editable) != TCL_OK) {
	editable = 1;
    }
    Tcl_DecrRefCount(scriptPtr);
    Tcl_RestoreInterpState(interp, state);
    return editable != 0;
}

static int
GetType(
    TkWindow *winPtr)		/* The window. */
{
    ThreadSpecificData *tsdPtr = GetTSD();
    Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&tsdPtr->typeTable, winPtr);
    int type = TK_KEYBOARD_TEXT;

    if (hPtr) {
	return PTR2INT(Tcl_GetHashValue(hPtr));
    }

    /*
     * Entries which hide their text take passwords.
     */

    if (winPtr->classUid && winPtr->mainPtr
	    && (!strcmp(winPtr->classUid, "Entry")
		|| !strcmp(winPtr->classUid, "TEntry"))) {
	Tcl_Interp *interp = winPtr->mainPtr->interp;
	Tcl_InterpState state = Tcl_SaveInterpState(interp, TCL_OK);
	Tcl_Obj *scriptPtr = Tcl_ObjPrintf("%s cget -show",
		winPtr->pathName);

	Tcl_IncrRefCount(scriptPtr);
	if (Tcl_EvalObjEx(interp, scriptPtr, TCL_EVAL_GLOBAL) == TCL_OK
		&& Tcl_GetString(Tcl_GetObjResult(interp))[0] != '\0') {
	    type = TK_KEYBOARD_PASSWORD;
	}
	Tcl_DecrRefCount(scriptPtr);
	Tcl_RestoreInterpState(interp, state);
    }
    return type;
}

/*
 *----------------------------------------------------------------------
 *
 * Decide --
 *
 *	In the automatic mode, show or hide the keyboard for the window with
 *	the focus, if it or whether it wants the keyboard changed since the
 *	keyboard was last decided.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The keyboard may be shown, or hidden (at once if hideNow is true, or
 *	else after HIDE_DELAY, if the focus does not want it then either).
 *
 *----------------------------------------------------------------------
 */

static void
Decide(
    ThreadSpecificData *tsdPtr,	/* The keyboard state. */
    bool hideNow)		/* Hide at once. */
{
    TkDisplay *dispPtr = TkGetDisplayList();
    TkWindow *focusPtr;
    bool wanted;
    int type;

    if (keyboardProc == NULL || tsdPtr->manual || dispPtr == NULL) {
	return;
    }
    focusPtr = dispPtr->focusPtr;
    if (focusPtr == NULL || (focusPtr->flags & TK_ALREADY_DEAD)
	    || focusPtr->window == None || InPopup(focusPtr)) {
	return;
    }
    wanted = (focusPtr->window == tsdPtr->caretWindow);
    if (focusPtr->window == tsdPtr->lastFocus && wanted == tsdPtr->lastWanted
	    && tsdPtr->display == focusPtr->display) {
	return;
    }
    type = GetType(focusPtr);
    wanted = wanted && type != TK_KEYBOARD_NONE && Editable(focusPtr);
    tsdPtr->display = focusPtr->display;
    tsdPtr->lastFocus = focusPtr->window;
    tsdPtr->lastWanted = (focusPtr->window == tsdPtr->caretWindow);
    if (wanted) {
	if (tsdPtr->hideTimer) {
	    Tcl_DeleteTimerHandler(tsdPtr->hideTimer);
	    tsdPtr->hideTimer = NULL;
	}
	if (!tsdPtr->requested || type != tsdPtr->lastType) {
	    ShowKeyboard(tsdPtr, focusPtr, true, type);
	}
	tsdPtr->lastType = type;
    } else if (tsdPtr->requested) {
	if (hideNow) {
	    ShowKeyboard(tsdPtr, focusPtr, false, type);
	} else if (tsdPtr->hideTimer == NULL) {
	    tsdPtr->hideTimer = Tcl_CreateTimerHandler(HIDE_DELAY, HideProc,
		    NULL);
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * HideProc --
 *
 *	Hide the keyboard if the focus still does not want it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The keyboard may be hidden.
 *
 *----------------------------------------------------------------------
 */

static void
HideProc(
    TCL_UNUSED(void *))
{
    ThreadSpecificData *tsdPtr = GetTSD();

    tsdPtr->hideTimer = NULL;
    tsdPtr->lastFocus = None;
    Decide(tsdPtr, true);
}

/*
 *----------------------------------------------------------------------
 *
 * SetupProc, CheckProc --
 *
 *	The keyboard is decided before Tcl waits for events, when the focus
 *	and the insertion cursors have been updated.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	See Decide.
 *
 *----------------------------------------------------------------------
 */

static void
SetupProc(
    TCL_UNUSED(void *),
    int flags)			/* The events Tcl waits for. */
{
    if (flags & TCL_WINDOW_EVENTS) {
	Decide(GetTSD(), false);
    }
}

static void
CheckProc(
    TCL_UNUSED(void *),
    TCL_UNUSED(int))
{
}

/*
 *----------------------------------------------------------------------
 *
 * ButtonProc --
 *
 *	A click in the window which has the focus decides the keyboard again,
 *	as if the focus had moved there: the keyboard which the user hid
 *	comes back when the text being edited is touched again.
 *
 * Results:
 *	0, so that the event is handled as usual.
 *
 * Side effects:
 *	The keyboard is decided before Tcl next waits for events.
 *
 *----------------------------------------------------------------------
 */

static int
ButtonProc(
    TCL_UNUSED(void *),
    XEvent *eventPtr)		/* Any event. */
{
    TkWindow *winPtr;

    if (eventPtr->type == ButtonPress && eventPtr->xbutton.button == Button1
	    && (winPtr = (TkWindow *)Tk_IdToWindow(eventPtr->xany.display,
		eventPtr->xany.window)) != NULL
	    && winPtr == winPtr->dispPtr->focusPtr) {
	GetTSD()->lastFocus = None;
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TypeEventProc --
 *
 *	Forget the type of a window when it is destroyed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The entry of the window is deleted.
 *
 *----------------------------------------------------------------------
 */

static void
TypeEventProc(
    void *clientData,		/* The window. */
    XEvent *eventPtr)		/* A StructureNotify event. */
{
    ThreadSpecificData *tsdPtr = GetTSD();
    Tcl_HashEntry *hPtr;

    if (eventPtr->type != DestroyNotify) {
	return;
    }
    hPtr = Tcl_FindHashEntry(&tsdPtr->typeTable, clientData);
    if (hPtr) {
	Tcl_DeleteHashEntry(hPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkKeyboardObjCmd --
 *
 *	Implements "tk keyboard":
 *	    tk keyboard ?-displayof window?
 *	    tk keyboard show|hide ?-displayof window?
 *	    tk keyboard mode ?-displayof window? ?auto|manual?
 *	    tk keyboard type window ?type?
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	The keyboard may be shown or hidden, the mode or a type set.
 *
 *----------------------------------------------------------------------
 */

int
TkKeyboardObjCmd(
    void *clientData,		/* The main window. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,		/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    static const char *const optionNames[] = {
	"hide", "mode", "show", "type", NULL
    };
    enum options {KBD_HIDE, KBD_MODE, KBD_SHOW, KBD_TYPE};
    static const char *const modeNames[] = {"auto", "manual", NULL};
    ThreadSpecificData *tsdPtr = GetTSD();
    Tk_Window tkwin = (Tk_Window)clientData;
    Tcl_HashEntry *hPtr;
    Tcl_Size skip;
    int option, index, isNew;
    bool shown;

    if (objc == 1 || (objc > 1
	    && !strcmp(Tcl_GetString(objv[1]), "-displayof"))) {
	skip = TkGetDisplayOf(interp, objc - 1, objv + 1, &tkwin);
	if (skip < 0) {
	    return TCL_ERROR;
	}
	if (objc != 1 + skip) {
	    Tcl_WrongNumArgs(interp, 1, objv, "?-displayof window?");
	    return TCL_ERROR;
	}
	shown = (tsdPtr->shown >= 0) ? tsdPtr->shown != 0 : tsdPtr->requested;
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		shown ? "shown" : "hidden", -1));
	return TCL_OK;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], optionNames, "option", 0,
	    &option) != TCL_OK) {
	return TCL_ERROR;
    }

    switch ((enum options)option) {
    case KBD_SHOW:
    case KBD_HIDE: {
	TkDisplay *dispPtr;
	TkWindow *winPtr;

	skip = TkGetDisplayOf(interp, objc - 2, objv + 2, &tkwin);
	if (skip < 0) {
	    return TCL_ERROR;
	}
	if (objc != 2 + skip) {
	    Tcl_WrongNumArgs(interp, 2, objv, "?-displayof window?");
	    return TCL_ERROR;
	}

	/*
	 * The keyboard stays so until the focus moves.
	 */

	dispPtr = ((TkWindow *)tkwin)->dispPtr;
	winPtr = dispPtr->focusPtr ? dispPtr->focusPtr : (TkWindow *)tkwin;
	ShowKeyboard(tsdPtr, winPtr, option == KBD_SHOW, GetType(winPtr));
	if (dispPtr->focusPtr) {
	    tsdPtr->display = dispPtr->display;
	    tsdPtr->lastFocus = dispPtr->focusPtr->window;
	    tsdPtr->lastWanted =
		    (dispPtr->focusPtr->window == tsdPtr->caretWindow);
	}
	return TCL_OK;
    }
    case KBD_MODE:
	skip = TkGetDisplayOf(interp, objc - 2, objv + 2, &tkwin);
	if (skip < 0) {
	    return TCL_ERROR;
	}
	if (objc > 3 + skip) {
	    Tcl_WrongNumArgs(interp, 2, objv, "?-displayof window? ?mode?");
	    return TCL_ERROR;
	}
	if (objc == 3 + skip) {
	    if (Tcl_GetIndexFromObj(interp, objv[2 + skip], modeNames, "mode",
		    0, &index) != TCL_OK) {
		return TCL_ERROR;
	    }
	    tsdPtr->manual = (index == 1);
	    tsdPtr->lastFocus = None;
	}
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		modeNames[tsdPtr->manual ? 1 : 0], -1));
	return TCL_OK;
    case KBD_TYPE:
	if (objc < 3 || objc > 4) {
	    Tcl_WrongNumArgs(interp, 2, objv, "window ?type?");
	    return TCL_ERROR;
	}
	tkwin = Tk_NameToWindow(interp, Tcl_GetString(objv[2]), tkwin);
	if (tkwin == NULL) {
	    return TCL_ERROR;
	}
	if (objc == 4) {
	    if (Tcl_GetIndexFromObj(interp, objv[3], typeNames, "type", 0,
		    &index) != TCL_OK) {
		return TCL_ERROR;
	    }
	    hPtr = Tcl_CreateHashEntry(&tsdPtr->typeTable, tkwin, &isNew);
	    if (isNew) {
		Tk_CreateEventHandler(tkwin, StructureNotifyMask,
			TypeEventProc, tkwin);
	    }
	    Tcl_SetHashValue(hPtr, INT2PTR(index));
	    tsdPtr->lastFocus = None;
	}
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		typeNames[GetType((TkWindow *)tkwin)], -1));
	return TCL_OK;
    }
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
