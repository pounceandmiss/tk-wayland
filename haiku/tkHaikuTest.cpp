/*
 * tkHaikuTest.cpp --
 *
 *	Commands of tktest for the tests of the Haiku port. They make input
 *	the way the user does: the pointer moves through the app_server, and
 *	buttons and keys are delivered by the thread of the window which gets
 *	them, as messages of the same form as those of the app_server, so that
 *	the tests go through all the code of the port real input goes through.
 *
 *	testmovemouse x y
 *		Move the pointer to the point of the root window.
 *	testpressbutton x y
 *		Move the pointer there, and click button 1 in the window
 *		which is there.
 *	testhaikukeys ?-async delay? window string
 *		Type the characters of the string (those which the keymap of
 *		the user has, and newline, tab and backspace) in the native
 *		window of a Tk window.
 *	testhaikumoves ?-async delay? x0 y0 x1 y1 count
 *		Move the pointer in count steps from one point to another.
 *
 *	The commands return when the input was delivered to the thread of the
 *	window (it is then queued for Tk), or with -async at once: a thread of
 *	their own then makes the input, waiting the delay (in milliseconds)
 *	between two keys or moves, while Tk may be busy.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include <Application.h>
#include <InterfaceDefs.h>
#include <Messenger.h>
#include <OS.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>
#include <WindowScreen.h>	/* set_mouse_position */

#include <stdlib.h>
#include <string.h>
#include <vector>

#include "tk.h"
#include "tkHaiku.h"

/*
 * Input made by a thread of its own (-async): messages for a window, or
 * moves of the pointer.
 */

typedef struct {
    BMessenger target;		/* The view the messages are for. */
    std::vector<BMessage *> messages;
    std::vector<BPoint> moves;	/* Points of the screen. */
    bigtime_t delay;		/* Between two of them, in microseconds. */
} Input;

static Tcl_ObjCmdProc2 TestmovemouseObjCmd;
static Tcl_ObjCmdProc2 TestpressbuttonObjCmd;
static Tcl_ObjCmdProc2 TesthaikukeysObjCmd;
static Tcl_ObjCmdProc2 TesthaikumovesObjCmd;

static bool		FindKey(const char *utf, int length, int32 *keyPtr,
			    uint32 *modifiersPtr, int32 *rawCharPtr);
static BMessenger	FindView(Tk_Window tkwin);
static int		GetAsync(Tcl_Interp *interp, Tcl_Size *objcPtr,
			    Tcl_Obj *const **objvPtr, bigtime_t *delayPtr,
			    bool *asyncPtr);
static BPoint		ToScreen(Tk_Window tkwin, double x, double y);
static status_t		InputThread(void *data);
static BMessage *	KeyMessage(uint32 what, int32 key, uint32 modifiers,
			    int32 rawChar, const char *bytes);
static void		MovePointer(BPoint point, bool wait);
static void		RunInput(Input *inputPtr, bool async);

/*
 *----------------------------------------------------------------------
 *
 * TkplatformtestInit --
 *
 *	Define the commands of the tests of the Haiku port.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	The commands are created.
 *
 *----------------------------------------------------------------------
 */

extern "C" int
TkplatformtestInit(
    Tcl_Interp *interp)		/* Interpreter to add the commands to. */
{
    Tcl_CreateObjCommand2(interp, "testmovemouse", TestmovemouseObjCmd,
	    NULL, NULL);
    Tcl_CreateObjCommand2(interp, "testpressbutton", TestpressbuttonObjCmd,
	    NULL, NULL);
    Tcl_CreateObjCommand2(interp, "testhaikukeys", TesthaikukeysObjCmd,
	    NULL, NULL);
    Tcl_CreateObjCommand2(interp, "testhaikumoves", TesthaikumovesObjCmd,
	    NULL, NULL);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ToScreen --
 *
 *	Convert a point of the root window to the screen, whose pixels may be
 *	smaller (the port scales Tk with the font size of the user).
 *
 * Results:
 *	The point of the screen.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static BPoint
ToScreen(
    Tk_Window tkwin,		/* A window of the display. */
    double x, double y)		/* In root coordinates. */
{
    BScreen screen;
    double scale = (screen.Frame().Width() + 1)
	    / WidthOfScreen(Tk_Screen(tkwin));

    return BPoint((float)(x * scale), (float)(y * scale));
}

/*
 *----------------------------------------------------------------------
 *
 * FindView --
 *
 *	Find the view of the BWindow which shows a Tk window: that of the
 *	child of the root window which contains it.
 *
 * Results:
 *	A messenger for the view, invalid if the window has no view (it was
 *	never mapped).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static BMessenger
FindView(
    Tk_Window tkwin)		/* The Tk window. */
{
    Display *display = Tk_Display(tkwin);
    Window w = Tk_WindowId(tkwin);
    char name[32];
    int i;

    while (w != None) {
	Window root, parent, *children = NULL;
	unsigned int count;

	if (!XQueryTree(display, w, &root, &parent, &children, &count)) {
	    return BMessenger();
	}
	if (children) {
	    XFree(children);
	}
	if (parent == root) {
	    break;
	}
	w = parent;
    }
    if (w == None) {
	return BMessenger();
    }
    snprintf(name, sizeof(name), TKHAIKU_VIEW_NAME, (unsigned long)w);
    for (i = 0; i < be_app->CountWindows(); i++) {
	BWindow *window = be_app->WindowAt(i);
	BMessenger messenger;

	if (window == NULL || !window->Lock()) {
	    continue;
	}
	BView *view = window->FindView(name);
	if (view) {
	    messenger = BMessenger(view, window);
	}
	window->Unlock();
	if (messenger.IsValid()) {
	    return messenger;
	}
    }
    return BMessenger();
}

/*
 *----------------------------------------------------------------------
 *
 * MovePointer --
 *
 *	Move the pointer, as the user does.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The pointer moves; with wait, this returns when the app_server has
 *	it there (or after a second).
 *
 *----------------------------------------------------------------------
 */

static void
MovePointer(
    BPoint point,		/* Where, on the screen. */
    bool wait)			/* Whether to wait for the move. */
{
    bigtime_t limit = system_time() + 1000000;

    set_mouse_position((int32)point.x, (int32)point.y);
    while (wait && system_time() < limit) {
	BPoint where;
	uint32 buttons;

	if (get_mouse(&where, &buttons) == B_OK
		&& (int32)where.x == (int32)point.x
		&& (int32)where.y == (int32)point.y) {
	    break;
	}
	snooze(5000);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * RunInput, InputThread --
 *
 *	Make input: send the messages to the view, move the pointer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The input is made, in a thread of its own if async (which owns and
 *	frees the input then). Synchronous messages return when the window
 *	handled them.
 *
 *----------------------------------------------------------------------
 */

static void
RunInput(
    Input *inputPtr,		/* The input, allocated with new. */
    bool async)			/* Whether to make it in a thread. */
{
    if (async) {
	thread_id thread = spawn_thread(InputThread, "tktest input",
		B_NORMAL_PRIORITY, inputPtr);

	if (thread >= 0 && resume_thread(thread) == B_OK) {
	    return;
	}
    }
    InputThread(inputPtr);
}

static status_t
InputThread(
    void *data)			/* The input. */
{
    Input *inputPtr = (Input *)data;
    bool first = true;

    for (BMessage *message : inputPtr->messages) {
	BMessage reply;

	if (!first && inputPtr->delay > 0) {
	    snooze(inputPtr->delay);
	}
	first = false;
	inputPtr->target.SendMessage(message, &reply);
	delete message;
    }
    for (size_t i = 0; i < inputPtr->moves.size(); i++) {
	if (!first && inputPtr->delay > 0) {
	    snooze(inputPtr->delay);
	}
	first = false;
	MovePointer(inputPtr->moves[i], i + 1 == inputPtr->moves.size());
    }
    delete inputPtr;
    return B_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * GetAsync --
 *
 *	Parse the option -async delay at the start of the arguments.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	The option is removed from the arguments.
 *
 *----------------------------------------------------------------------
 */

static int
GetAsync(
    Tcl_Interp *interp,
    Tcl_Size *objcPtr,		/* The arguments, after the name of the */
    Tcl_Obj *const **objvPtr,	/* command. */
    bigtime_t *delayPtr,	/* Returns the delay, in microseconds. */
    bool *asyncPtr)		/* Returns whether -async was given. */
{
    int delay;

    *asyncPtr = false;
    *delayPtr = 0;
    if (*objcPtr < 2 || strcmp(Tcl_GetString((*objvPtr)[0]), "-async")) {
	return TCL_OK;
    }
    if (Tcl_GetIntFromObj(interp, (*objvPtr)[1], &delay) != TCL_OK) {
	return TCL_ERROR;
    }
    *asyncPtr = true;
    *delayPtr = (bigtime_t)delay * 1000;
    *objcPtr -= 2;
    *objvPtr += 2;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TestmovemouseObjCmd, TesthaikumovesObjCmd --
 *
 *	testmovemouse x y
 *	testhaikumoves ?-async delay? x0 y0 x1 y1 count
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	The pointer moves.
 *
 *----------------------------------------------------------------------
 */

static int
TestmovemouseObjCmd(
    void *clientData,		/* The main window. */
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    double x, y;

    (void)clientData;
    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "x y");
	return TCL_ERROR;
    }
    if (Tcl_GetDoubleFromObj(interp, objv[1], &x) != TCL_OK
	    || Tcl_GetDoubleFromObj(interp, objv[2], &y) != TCL_OK) {
	return TCL_ERROR;
    }
    MovePointer(ToScreen(Tk_MainWindow(interp), x, y), true);
    return TCL_OK;
}

static int
TesthaikumovesObjCmd(
    void *clientData,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    Tk_Window tkwin = Tk_MainWindow(interp);
    Tcl_Obj *const *argv = objv + 1;
    Tcl_Size argc = objc - 1;
    double coords[4];
    bigtime_t delay;
    bool async;
    int count, i;

    (void)clientData;
    if (GetAsync(interp, &argc, &argv, &delay, &async) != TCL_OK) {
	return TCL_ERROR;
    }
    if (argc != 5) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"?-async delay? x0 y0 x1 y1 count");
	return TCL_ERROR;
    }
    for (i = 0; i < 4; i++) {
	if (Tcl_GetDoubleFromObj(interp, argv[i], &coords[i]) != TCL_OK) {
	    return TCL_ERROR;
	}
    }
    if (Tcl_GetIntFromObj(interp, argv[4], &count) != TCL_OK) {
	return TCL_ERROR;
    }
    if (count < 1) {
	count = 1;
    }

    Input *inputPtr = new Input;

    inputPtr->delay = delay;
    for (i = 1; i <= count; i++) {
	inputPtr->moves.push_back(ToScreen(tkwin,
		coords[0] + (coords[2] - coords[0]) * i / count,
		coords[1] + (coords[3] - coords[1]) * i / count));
    }
    RunInput(inputPtr, async);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TestpressbuttonObjCmd --
 *
 *	testpressbutton x y
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	The pointer moves to the point, and button 1 is pressed and released
 *	in the window there, if Tk has one there.
 *
 *----------------------------------------------------------------------
 */

static int
TestpressbuttonObjCmd(
    void *clientData,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    Tk_Window tkwin;
    BMessenger target;
    BPoint where;
    int x, y;
    static const uint32 whats[2] = {B_MOUSE_DOWN, B_MOUSE_UP};

    (void)clientData;
    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "x y");
	return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[1], &x) != TCL_OK
	    || Tcl_GetIntFromObj(interp, objv[2], &y) != TCL_OK) {
	return TCL_ERROR;
    }
    where = ToScreen(Tk_MainWindow(interp), x, y);
    MovePointer(where, true);
    tkwin = Tk_CoordsToWindow(x, y, Tk_MainWindow(interp));
    if (tkwin == NULL || !(target = FindView(tkwin)).IsValid()) {
	return TCL_OK;
    }

    Input *inputPtr = new Input;

    inputPtr->target = target;
    inputPtr->delay = 0;
    for (int i = 0; i < 2; i++) {
	BMessage *message = new BMessage(whats[i]);
	BPoint viewWhere = where;
	BLooper *looper;
	BView *view = (BView *)target.Target(&looper);

	if (view && looper->Lock()) {
	    viewWhere = view->ConvertFromScreen(where);
	    looper->Unlock();
	}
	message->AddInt64("when", system_time());
	message->AddPoint("screen_where", where);
	message->AddPoint("where", viewWhere);
	message->AddPoint("be:view_where", viewWhere);
	message->AddInt32("buttons", (whats[i] == B_MOUSE_DOWN)
		? B_PRIMARY_MOUSE_BUTTON : 0);
	message->AddInt32("modifiers", (int32)modifiers());
	message->AddInt32("clicks", 1);
	inputPtr->messages.push_back(message);
    }
    RunInput(inputPtr, false);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FindKey --
 *
 *	Find the key which types a character in the keymap of the user,
 *	without modifiers or with Shift.
 *
 * Results:
 *	Whether there is one; the key code, the modifiers and the character
 *	of the key without modifiers are stored.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static bool
FindKey(
    const char *utf,		/* The character, in UTF-8. */
    int length,			/* Its length in bytes. */
    int32 *keyPtr,		/* Returns the key code. */
    uint32 *modifiersPtr,	/* Returns B_SHIFT_KEY or 0. */
    int32 *rawCharPtr)		/* Returns the character without
				 * modifiers. */
{
    static const struct {
	char c;
	int32 key;
    } controls[] = {
	{'\n', 0x47}, {'\t', 0x26}, {'\b', 0x1e}, {0, 0}
    };
    key_map *keys = NULL;
    char *chars = NULL;
    bool found = false;
    int i, key;

    for (i = 0; controls[i].c; i++) {
	if (length == 1 && utf[0] == controls[i].c) {
	    *keyPtr = controls[i].key;
	    *modifiersPtr = 0;
	    *rawCharPtr = (unsigned char)utf[0];
	    return true;
	}
    }
    get_key_map(&keys, &chars);
    if (keys == NULL || chars == NULL) {
	free(keys);
	free(chars);
	return false;
    }
    for (int shift = 0; shift < 2 && !found; shift++) {
	const int32 *map = shift ? keys->shift_map : keys->normal_map;

	for (key = 1; key < 128 && !found; key++) {
	    const char *entry = chars + map[key];

	    if (map[key] && entry[0] == length
		    && memcmp(entry + 1, utf, length) == 0) {
		const char *raw = chars + keys->normal_map[key];

		*keyPtr = key;
		*modifiersPtr = shift ? (B_SHIFT_KEY | B_LEFT_SHIFT_KEY) : 0;
		*rawCharPtr = (raw[0] == 1) ? (unsigned char)raw[1] : 0;
		found = true;
	    }
	}
    }
    free(keys);
    free(chars);
    return found;
}

/*
 *----------------------------------------------------------------------
 *
 * KeyMessage --
 *
 *	A message of the app_server for a key or modifiers.
 *
 * Results:
 *	The message, allocated with new.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static BMessage *
KeyMessage(
    uint32 what,		/* B_KEY_DOWN, B_KEY_UP or
				 * B_MODIFIERS_CHANGED. */
    int32 key,			/* The key code. */
    uint32 modifiers,		/* The modifiers, after the change for
				 * B_MODIFIERS_CHANGED. */
    int32 rawChar,		/* The character without modifiers. */
    const char *bytes)		/* The characters typed. */
{
    BMessage *message = new BMessage(what);

    message->AddInt64("when", system_time());
    message->AddInt32("modifiers", (int32)modifiers);
    if (what == B_MODIFIERS_CHANGED) {
	message->AddInt32("be:old_modifiers",
		(int32)(modifiers ^ (B_SHIFT_KEY | B_LEFT_SHIFT_KEY)));
	return message;
    }
    message->AddInt32("key", key);
    message->AddInt32("raw_char", rawChar);
    message->AddInt8("byte", (int8)bytes[0]);
    message->AddString("bytes", bytes);
    return message;
}

/*
 *----------------------------------------------------------------------
 *
 * TesthaikukeysObjCmd --
 *
 *	testhaikukeys ?-async delay? window string
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	The keys of the characters are pressed and released in the native
 *	window of the Tk window, with Shift where needed.
 *
 *----------------------------------------------------------------------
 */

static int
TesthaikukeysObjCmd(
    void *clientData,
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    Tcl_Obj *const *argv = objv + 1;
    Tcl_Size argc = objc - 1, length;
    Tk_Window tkwin;
    BMessenger target;
    bigtime_t delay;
    bool async;
    const char *string, *p;

    (void)clientData;
    if (GetAsync(interp, &argc, &argv, &delay, &async) != TCL_OK) {
	return TCL_ERROR;
    }
    if (argc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "?-async delay? window string");
	return TCL_ERROR;
    }
    tkwin = Tk_NameToWindow(interp, Tcl_GetString(argv[0]),
	    Tk_MainWindow(interp));
    if (tkwin == NULL) {
	return TCL_ERROR;
    }
    target = FindView(tkwin);
    if (!target.IsValid()) {
	Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		"window \"%s\" has no native window", Tk_PathName(tkwin)));
	return TCL_ERROR;
    }
    string = Tcl_GetStringFromObj(argv[1], &length);

    Input *inputPtr = new Input;

    inputPtr->target = target;
    inputPtr->delay = delay;
    for (p = string; p < string + length; ) {
	int n = Tcl_UtfNext(p) - p;
	char bytes[8];
	int32 key, rawChar;
	uint32 modifiers;

	if (!FindKey(p, n, &key, &modifiers, &rawChar) || n >= 8) {
	    for (BMessage *message : inputPtr->messages) {
		delete message;
	    }
	    delete inputPtr;
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "no key types \"%.*s\"", n, p));
	    return TCL_ERROR;
	}
	memcpy(bytes, p, n);
	bytes[n] = '\0';
	if (modifiers) {
	    inputPtr->messages.push_back(KeyMessage(B_MODIFIERS_CHANGED, 0,
		    modifiers, 0, ""));
	}
	inputPtr->messages.push_back(KeyMessage(B_KEY_DOWN, key, modifiers,
		rawChar, bytes));
	inputPtr->messages.push_back(KeyMessage(B_KEY_UP, key, modifiers,
		rawChar, bytes));
	if (modifiers) {
	    inputPtr->messages.push_back(KeyMessage(B_MODIFIERS_CHANGED, 0,
		    0, 0, ""));
	}
	p += n;
    }
    RunInput(inputPtr, async);
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c++
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
