/*
 * tkWaylandTextInput.c --
 *
 *	Input methods, with the text input protocol (text-input-unstable-v3).
 *
 *	Text input is enabled while Tk wants the on-screen keyboard
 *	(tkKeyboard.c: when the focus is in a widget which shows an insertion
 *	cursor and can be edited, or with "tk keyboard show"), with the type
 *	of the text as content type, and the position of the insertion
 *	cursor is given to the compositor, for the windows of the input
 *	method. Compositors show their on-screen keyboard while text input is
 *	enabled. The text being composed is shown with the
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
static Window enabledWindow = None;
				/* The window text input is enabled for. */
static bool keyboardWanted = false;
				/* Tk wants the keyboard. */
static int keyboardType = TK_KEYBOARD_TEXT;
				/* The type of its text. */
static int sentType = -1;	/* The type sent with the content type. */
static int caretX = -1, caretY = -1, caretHeight = -1;
				/* Last cursor rectangle sent. */
static Window caretWindow = None;
static int caretWinX, caretWinY, caretWinHeight;
				/* The insertion cursor of the focus window:
				 * text widgets also set the caret of the
				 * display when they do not have the focus. */

/*
 * The state sent by the input method until the "done" event.
 */

static void		KeyboardProc(Tk_Window tkwin, int show, int type);
static void		SetContentType(int type);

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
    enabledWindow = None;
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
	enabledWindow = None;
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
    event.x.xkey.time = TkXeCurrentTime();
    event.x.xkey.state = tkXeDisplay->modState
	    & ~(ControlMask | Mod1Mask | Mod4Mask);
    event.x.xkey.keycode = 0;
    event.x.xkey.same_screen = True;
    event.k.keysym = NoSymbol;
    event.k.charValuePtr = (char *)Tcl_Alloc(len + 1);
    memcpy(event.k.charValuePtr, text, len + 1);
    event.k.charValueLen = len;
    TkXeQueueEvent(&event.x);
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
    if (textInputManager && tkXeDisplay->seat && textInput == NULL) {
	textInput = zwp_text_input_manager_v3_get_text_input(textInputManager,
		tkXeDisplay->seat);
	zwp_text_input_v3_add_listener(textInput, &textInputListener, NULL);
	TkSetKeyboardProc(KeyboardProc);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * KeyboardProc --
 *
 *	Called by tkKeyboard.c to show or hide the on-screen keyboard: text
 *	input is enabled or disabled.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Requests are sent to the compositor.
 *
 *----------------------------------------------------------------------
 */

static void
KeyboardProc(
    TCL_UNUSED(Tk_Window),
    int show,			/* Whether to show the keyboard. */
    int type)			/* TK_KEYBOARD_*. */
{
    keyboardWanted = (show != 0);
    keyboardType = type;
    TkWlTextInputUpdate();
    TkXeFlush();
}

/*
 *----------------------------------------------------------------------
 *
 * SetContentType --
 *
 *	Tell the compositor the type of the text, as the content type.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A request is sent.
 *
 *----------------------------------------------------------------------
 */

static void
SetContentType(
    int type)			/* TK_KEYBOARD_*. */
{
    uint32_t hint = ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE;
    uint32_t purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;

    switch (type) {
    case TK_KEYBOARD_NUMBER:
	purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NUMBER;
	break;
    case TK_KEYBOARD_PHONE:
	purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PHONE;
	break;
    case TK_KEYBOARD_EMAIL:
	purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_EMAIL;
	break;
    case TK_KEYBOARD_URL:
	purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_URL;
	break;
    case TK_KEYBOARD_PASSWORD:
	purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
	hint = ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT
		| ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA;
	break;
    }
    zwp_text_input_v3_set_content_type(textInput, hint, purpose);
    sentType = type;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlTextInputSetCaret --
 *
 *	Record the position of the insertion cursor of a window, if it has
 *	the focus: called by Tk_SetCaretPos.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The position is sent to the compositor by TkWlTextInputUpdate.
 *
 *----------------------------------------------------------------------
 */

void
TkWlTextInputSetCaret(
    TkWindow *winPtr,		/* The window setting its insertion cursor. */
    int x, int y,		/* Its position in the window. */
    int height)			/* Its height. */
{
    if (winPtr != winPtr->dispPtr->focusPtr) {
	return;
    }
    caretWindow = winPtr->window;
    caretWinX = x;
    caretWinY = y;
    caretWinHeight = height;
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
 *	a window which lost the focus is abandoned. Text input is enabled
 *	again, without being disabled, when the focus moves to another window
 *	which wants it, so that the input method starts over in another text
 *	field without hiding an on-screen keyboard.
 *
 *----------------------------------------------------------------------
 */

void
TkWlTextInputUpdate(void)
{
    TkDisplay *dispPtr = TkGetDisplayList();
    TkWindow *focusPtr = dispPtr ? dispPtr->focusPtr : NULL;
    TkXeWindow *wlWinPtr = NULL;
    TkXeWindow *topPtr = NULL;
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
    if (keyboardWanted && focusPtr && focusPtr->window != None
	    && !(focusPtr->flags & TK_ALREADY_DEAD)) {
	wlWinPtr = TkXeGetWindow(focusPtr->window);
	topPtr = wlWinPtr ? TkXeTopWindow(wlWinPtr) : NULL;
    }
    want = topPtr && topPtr->surface
	    && topPtr->surface->surface == enteredSurface;

    if (!want) {
	if (enabled) {
	    zwp_text_input_v3_disable(textInput);
	    zwp_text_input_v3_commit(textInput);
	    enabled = false;
	    enabledWindow = None;
	}
	return;
    }
    if (!enabled || enabledWindow != focusPtr->window) {
	enabled = true;
	enabledWindow = focusPtr->window;
	zwp_text_input_v3_enable(textInput);
	SetContentType(keyboardType);
	caretX = caretY = caretHeight = -1;
	zwp_text_input_v3_commit(textInput);
    } else if (sentType != keyboardType) {
	SetContentType(keyboardType);
	zwp_text_input_v3_commit(textInput);
    }
    if (focusPtr->window == caretWindow) {
	int x, y;

	TkXeWindowOrigin(wlWinPtr, &x, &y);
	x += caretWinX;
	y += caretWinY;
	if (x != caretX || y != caretY || caretWinHeight != caretHeight) {
	    zwp_text_input_v3_set_cursor_rectangle(textInput, x, y, 1,
		    caretWinHeight);
	    caretX = x;
	    caretY = y;
	    caretHeight = caretWinHeight;
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
