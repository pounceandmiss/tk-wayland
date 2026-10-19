/*
 * tkWaylandTextInput.c --
 *
 *	Input methods, with the text input protocol (text-input-unstable-v3).
 *
 *	Text input is enabled while the focus of Tk is in a widget which
 *	shows an insertion cursor (which calls Tk_SetCaretPos), and the
 *	position of that cursor is given to the compositor, for the windows
 *	of the input method. The text being composed is shown with the
 *	virtual events <<TkStartIMEMarkedText>>, <<TkEndIMEMarkedText>> and
 *	<<TkClearIMEMarkedText>>, as on Aqua, and committed text is delivered
 *	as a KeyPress event carrying it, as with XIM on X11.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkWaylandInt.h"
#include "text-input-unstable-v3-client-protocol.h"

static struct zwp_text_input_manager_v3 *textInputManager = NULL;
static struct zwp_text_input_v3 *textInput = NULL;

static struct wl_surface *enteredSurface = NULL;
				/* The surface the text input is in. */
static bool enabled = false;
static int caretX = -1, caretY = -1, caretHeight = -1;
				/* Last cursor rectangle sent. */

/*
 * The state sent by the input method until the "done" event.
 */

static char *pendingPreedit = NULL;
static char *pendingCommit = NULL;

/*
 * The window which shows text being composed, or None.
 */

static Window composeWindow = None;

/*
 *----------------------------------------------------------------------
 *
 * TextInputEnter, TextInputLeave --
 *
 *	The keyboard focus of the seat enters or leaves a surface: text input
 *	is enabled again by TkWlTextInputUpdate if the focus of Tk is in a
 *	widget with an insertion cursor.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The surface is recorded. Leaving it disables text input.
 *
 *----------------------------------------------------------------------
 */

static void
TextInputEnter(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zwp_text_input_v3 *),
    struct wl_surface *surface)
{
    enteredSurface = surface;
    enabled = false;
    TkWlTextInputUpdate();
}

static void
TextInputLeave(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zwp_text_input_v3 *),
    struct wl_surface *surface)
{
    if (enteredSurface == surface) {
	enteredSurface = NULL;
    }
    if (enabled) {
	zwp_text_input_v3_disable(textInput);
	zwp_text_input_v3_commit(textInput);
	enabled = false;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SetPending --
 *
 *	Replace a string received from the input method, kept until its
 *	"done" event.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The previous string is freed, and *ptrPtr is set to a copy of text,
 *	or NULL if it is empty.
 *
 *----------------------------------------------------------------------
 */

static void
SetPending(
    char **ptrPtr,
    const char *text)
{
    if (*ptrPtr) {
	Tcl_Free(*ptrPtr);
	*ptrPtr = NULL;
    }
    if (text && *text) {
	*ptrPtr = (char *)Tcl_Alloc(strlen(text) + 1);
	strcpy(*ptrPtr, text);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TextInputPreeditString, TextInputCommitString --
 *
 *	The input method sends the text being composed, or the text to
 *	insert.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The text is kept until the "done" event.
 *
 *----------------------------------------------------------------------
 */

static void
TextInputPreeditString(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zwp_text_input_v3 *),
    const char *text,
    TCL_UNUSED(int32_t),	/* cursorBegin */
    TCL_UNUSED(int32_t))	/* cursorEnd */
{
    SetPending(&pendingPreedit, text);
}

static void
TextInputCommitString(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zwp_text_input_v3 *),
    const char *text)
{
    SetPending(&pendingCommit, text);
}

/*
 *----------------------------------------------------------------------
 *
 * TextInputDeleteSurroundingText --
 *
 *	The input method asks to delete text around the insertion cursor.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
TextInputDeleteSurroundingText(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zwp_text_input_v3 *),
    TCL_UNUSED(uint32_t),
    TCL_UNUSED(uint32_t))
{
    /*
     * The surrounding text is not given to the input method, so it does not
     * ask to delete it.
     */
}

/*
 *----------------------------------------------------------------------
 *
 * QueueText --
 *
 *	Insert text in the focus window, with a KeyPress event which carries
 *	it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A KeyPress event is queued.
 *
 *----------------------------------------------------------------------
 */

static void
QueueText(
    TkWindow *winPtr,
    const char *text)
{
    union {
	XEvent x;
	TkKeyEvent k;
    } event;
    Tcl_Size len = (Tcl_Size)strlen(text);

    memset(&event, 0, sizeof(event));
    event.x.xkey.type = KeyPress;
    event.x.xkey.display = winPtr->display;
    event.x.xkey.window = winPtr->window;
    event.x.xkey.root = RootWindow(winPtr->display, winPtr->screenNum);
    event.x.xkey.time = TkWlCurrentTime();
    event.x.xkey.state = tkWlDisplay->modState
	    & ~(ControlMask | Mod1Mask | Mod4Mask);
    event.x.xkey.keycode = 0;
    event.x.xkey.same_screen = True;
    event.k.keysym = NoSymbol;
    event.k.charValuePtr = (char *)Tcl_Alloc(len + 1);
    memcpy(event.k.charValuePtr, text, len + 1);
    event.k.charValueLen = len;
    TkWlQueueEvent(&event.x);
}

/*
 *----------------------------------------------------------------------
 *
 * TextInputDone --
 *
 *	The input method has sent all of a change: the committed text is
 *	inserted, and the text being composed shown.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	KeyPress events and the IME marked text virtual events are queued
 *	for the focus window, and the pending strings are freed.
 *
 *----------------------------------------------------------------------
 */

static void
TextInputDone(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zwp_text_input_v3 *),
    TCL_UNUSED(uint32_t))
{
    TkDisplay *dispPtr = TkGetDisplayList();
    TkWindow *focusPtr = dispPtr ? dispPtr->focusPtr : NULL;
    Tk_Window composeWin = (dispPtr && composeWindow != None)
	    ? Tk_IdToWindow(dispPtr->display, composeWindow) : NULL;

    /*
     * Replace the text being composed with the committed text, then with
     * the new text being composed.
     */

    if (composeWin) {
	Tk_SendVirtualEvent(composeWin, "TkClearIMEMarkedText", NULL);
    }
    composeWindow = None;
    if (focusPtr && pendingCommit) {
	QueueText(focusPtr, pendingCommit);
    }
    if (focusPtr && pendingPreedit) {
	Tk_SendVirtualEvent((Tk_Window)focusPtr, "TkStartIMEMarkedText",
		NULL);
	QueueText(focusPtr, pendingPreedit);
	Tk_SendVirtualEvent((Tk_Window)focusPtr, "TkEndIMEMarkedText", NULL);
	composeWindow = focusPtr->window;
    }
    SetPending(&pendingCommit, NULL);
    SetPending(&pendingPreedit, NULL);
}

/*
 * Version 2 of the protocol added events, which are not used, since version
 * 1 is bound: they are in the listener only with headers which have them.
 */

static const struct zwp_text_input_v3_listener textInputListener = {
    TextInputEnter,
    TextInputLeave,
    TextInputPreeditString,
    TextInputCommitString,
    TextInputDeleteSurroundingText,
    TextInputDone,
#ifdef ZWP_TEXT_INPUT_V3_ACTION_SINCE_VERSION
    NULL,			/* action */
    NULL,			/* language */
    NULL			/* preedit_hint */
#endif
};

/*
 *----------------------------------------------------------------------
 *
 * TkWlTextInputBind --
 *
 *	Bind the text input manager, if the global announced by the registry
 *	is it.
 *
 * Results:
 *	True if the global was the text input manager, false otherwise.
 *
 * Side effects:
 *	The manager is bound.
 *
 *----------------------------------------------------------------------
 */

bool
TkWlTextInputBind(
    struct wl_registry *registry,
    uint32_t name,
    const char *interface)
{
    if (!strcmp(interface, zwp_text_input_manager_v3_interface.name)) {
	textInputManager = (struct zwp_text_input_manager_v3 *)
		wl_registry_bind(registry, name,
		&zwp_text_input_manager_v3_interface, 1);
	return true;
    }
    return false;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlTextInputInit --
 *
 *	Create the text input of the seat, once the seat and the manager are
 *	known.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The text input is created, with its listener.
 *
 *----------------------------------------------------------------------
 */

void
TkWlTextInputInit(void)
{
    if (textInputManager && tkWlDisplay->seat && textInput == NULL) {
	textInput = zwp_text_input_manager_v3_get_text_input(textInputManager,
		tkWlDisplay->seat);
	zwp_text_input_v3_add_listener(textInput, &textInputListener, NULL);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlTextInputUpdate --
 *
 *	Enable or disable text input, and send the position of the insertion
 *	cursor. Called before Tcl waits for events.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Requests are sent to the compositor when the state of text input or
 *	the position of the insertion cursor changed; text being composed in
 *	a window which lost the focus is abandoned.
 *
 *----------------------------------------------------------------------
 */

void
TkWlTextInputUpdate(void)
{
    TkDisplay *dispPtr = TkGetDisplayList();
    TkWindow *focusPtr = dispPtr ? dispPtr->focusPtr : NULL;
    TkWindow *caretPtr = dispPtr ? dispPtr->caret.winPtr : NULL;
    TkWlWindow *wlWinPtr = NULL;
    TkWlWindow *topPtr = NULL;
    bool want;

    if (textInput == NULL || enteredSurface == NULL) {
	return;
    }
    if (composeWindow != None
	    && (focusPtr == NULL || focusPtr->window != composeWindow)) {
	/*
	 * The focus moved: what was being composed is abandoned.
	 */

	composeWindow = None;
    }
    if (focusPtr && focusPtr == caretPtr && focusPtr->window != None
	    && !(focusPtr->flags & TK_ALREADY_DEAD)) {
	wlWinPtr = TkWlGetWindow(focusPtr->window);
	topPtr = wlWinPtr ? TkWlTopWindow(wlWinPtr) : NULL;
    }
    want = topPtr && topPtr->surface
	    && topPtr->surface->surface == enteredSurface;

    if (want != enabled) {
	enabled = want;
	if (!want) {
	    zwp_text_input_v3_disable(textInput);
	    zwp_text_input_v3_commit(textInput);
	    return;
	}
	zwp_text_input_v3_enable(textInput);
	zwp_text_input_v3_set_content_type(textInput,
		ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
		ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
	caretX = caretY = caretHeight = -1;
    }
    if (enabled) {
	int x, y;

	TkWlWindowOrigin(wlWinPtr, &x, &y);
	x += dispPtr->caret.x;
	y += dispPtr->caret.y;
	if (x != caretX || y != caretY
		|| dispPtr->caret.height != caretHeight) {
	    zwp_text_input_v3_set_cursor_rectangle(textInput, x, y, 1,
		    dispPtr->caret.height);
	    caretX = x;
	    caretY = y;
	    caretHeight = dispPtr->caret.height;
	    zwp_text_input_v3_commit(textInput);
	}
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
