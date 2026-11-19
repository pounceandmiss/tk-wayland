/*
 * tkHaikuApp.cpp --
 *
 *	The glue between the Be API and the Haiku port of Tk: the
 *	BApplication, and a BWindow with a BView for every child of the root
 *	window. This is the only file of the port in C++; Tk calls it through
 *	the C functions of tkHaiku.h, and it posts what happens to Tk as
 *	events (see tkHaiku.h for the threads).
 *
 *	The view of a window draws a BBitmap, into which Tk copies the parts
 *	of its backing image which changed. The bitmap is only touched with
 *	the window locked: by Tk when it copies, and by the window thread when
 *	it draws.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include <Application.h>
#include <Beep.h>
#include <Bitmap.h>
#include <Clipboard.h>
#include <Cursor.h>
#include <Deskbar.h>
#include <Font.h>
#include <Input.h>
#include <InterfaceDefs.h>
#include <PropertyInfo.h>
#include <Screen.h>
#include <String.h>
#include <View.h>
#include <Window.h>
#include <WindowScreen.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tkHaiku.h"

/*
 * The semaphore released when the application is running, or could not be
 * created (appStatus).
 */

static sem_id readySem = -1;
static status_t appStatus = B_OK;

static void		InitEvent(TkHaikuEvent *eventPtr,
			    TkHaikuEventType type, unsigned long window,
			    bigtime_t when);

/*
 *----------------------------------------------------------------------
 *
 * ClipboardWatcher --
 *
 *	A handler of the application, which watches the system clipboard and
 *	posts its changes.
 *
 *----------------------------------------------------------------------
 */

class ClipboardWatcher : public BHandler {
public:
    ClipboardWatcher() : BHandler("tk clipboard") {}

    /*
     * Called with the application locked.
     */

    void Start() {
	be_app->AddHandler(this);
	be_clipboard->StartWatching(BMessenger(this));
    }

    virtual void MessageReceived(BMessage *message) {
	if (message->what == B_CLIPBOARD_CHANGED) {
	    TkHaikuEvent event;

	    InitEvent(&event, TKHAIKU_CLIPBOARD, 0, system_time());
	    TkHaikuPostEvent(&event);
	    return;
	}
	BHandler::MessageReceived(message);
    }
};

/*
 *----------------------------------------------------------------------
 *
 * ScriptHandler --
 *
 *	The widgets of Tk for the scripting of Haiku: the property "Widget" of
 *	the application and of the windows resolves to a handler, which posts
 *	the requests to Tk (the window or application thread never waits for
 *	Tk). Tk answers them (tkHaikuScript.c, library/haiku.tcl) with
 *	TkHaikuScriptReply.
 *
 *----------------------------------------------------------------------
 */

#define TKHAIKU_WIDGETS_SUITE	"suite/x-vnd.tcl-tk-widgets"
#define TKHAIKU_WIDGET_SUITE	"suite/x-vnd.tcl-tk-widget"

/*
 * The property "Widget" of the application and of the windows, and the
 * properties of a widget.
 */

static property_info widgetsProperties[] = {
    {"Widget", {B_COUNT_PROPERTIES, 0}, {B_DIRECT_SPECIFIER, 0},
	"Returns the number of widgets of Tk.", 0, {B_INT32_TYPE}},
    {"Widget", {B_EXECUTE_PROPERTY, 0},
	{B_NAME_SPECIFIER, B_INDEX_SPECIFIER, B_REVERSE_INDEX_SPECIFIER, 0},
	"Invokes a button, by its path name, label or index.", 0, {}},
    {"Widget", {}, {B_NAME_SPECIFIER, B_INDEX_SPECIFIER,
	B_REVERSE_INDEX_SPECIFIER, 0},
	"Directs the scripting message to a widget of Tk.", 0, {}},
    {0}
};

static property_info widgetProperties[] = {
    {"Path", {B_GET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
	"The path name of the widget.", 0, {B_STRING_TYPE}},
    {"Class", {B_GET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
	"The class of the widget.", 0, {B_STRING_TYPE}},
    {"Label", {B_GET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
	"The text of a button or label.", 0, {B_STRING_TYPE}},
    {"Text", {B_GET_PROPERTY, B_SET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
	"The text of an entry or text widget.", 0, {B_STRING_TYPE}},
    {"Value", {B_GET_PROPERTY, B_SET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
	"The value of a check or radio button, scale or spinbox.", 0,
	{B_INT32_TYPE, B_FLOAT_TYPE}},
    {"Enabled", {B_GET_PROPERTY, B_SET_PROPERTY, 0},
	{B_DIRECT_SPECIFIER, 0}, "Whether the widget is enabled.", 0,
	{B_BOOL_TYPE}},
    {"Focus", {B_GET_PROPERTY, B_SET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
	"Whether the widget has the focus.", 0, {B_BOOL_TYPE}},
    {"Frame", {B_GET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
	"The rectangle of the widget on the screen.", 0, {B_RECT_TYPE}},
    {0}
};

/*
 * Whether a scripting request is for the widgets of Tk, and the suites of
 * the property "Widget", which the application and the windows have, and of
 * the widgets.
 */

static bool
IsWidgetProperty(
    const char *property)
{
    return property && strcmp(property, "Widget") == 0;
}

static void
AddWidgetSuite(
    BMessage *data,
    bool widget)		/* The properties of a widget, or the
				 * property "Widget". */
{
    BPropertyInfo info(widget ? widgetProperties : widgetsProperties);

    data->AddString("suites", widget ? TKHAIKU_WIDGET_SUITE
	    : TKHAIKU_WIDGETS_SUITE);
    data->AddFlat("messages", &info);
}

class ScriptHandler : public BHandler {
public:
    ScriptHandler(unsigned long xid) : BHandler("tk widgets"), fXid(xid) {}

    virtual BHandler *ResolveSpecifier(BMessage *, int32, BMessage *,
	    int32, const char *) {
	return this;
    }

    virtual status_t GetSupportedSuites(BMessage *data) {
	AddWidgetSuite(data, true);
	return BHandler::GetSupportedSuites(data);
    }

    virtual void MessageReceived(BMessage *message) {
	switch (message->what) {
	case B_GET_PROPERTY:
	case B_SET_PROPERTY:
	case B_EXECUTE_PROPERTY:
	case B_COUNT_PROPERTIES: {
	    TkHaikuEvent event;

	    InitEvent(&event, TKHAIKU_SCRIPT, fXid, 0);
	    event.u.script = Looper()->DetachCurrentMessage();
	    TkHaikuPostEvent(&event);
	    return;
	}
	}
	BHandler::MessageReceived(message);
    }

private:
    unsigned long fXid;		/* The X window of the window, or 0 for
				 * the application. */
};

/*
 *----------------------------------------------------------------------
 *
 * TkApp --
 *
 *	The application, when Tk creates it: it runs in a thread of its own,
 *	started by TkHaikuStartApp, and turns the request to quit into an
 *	event for Tk.
 *
 *----------------------------------------------------------------------
 */

class TkApp : public BApplication {
public:
    TkApp(const char *signature, status_t *statusPtr)
	: BApplication(signature, statusPtr), fScript(NULL) {}

    virtual void ReadyToRun() {
	(new ClipboardWatcher())->Start();
	fScript = new ScriptHandler(0);
	AddHandler(fScript);
	release_sem(readySem);
    }

    virtual BHandler *ResolveSpecifier(BMessage *message, int32 index,
	    BMessage *specifier, int32 what, const char *property) {
	if (IsWidgetProperty(property) && fScript) {
	    return fScript;
	}
	return BApplication::ResolveSpecifier(message, index, specifier,
		what, property);
    }

    virtual status_t GetSupportedSuites(BMessage *data) {
	AddWidgetSuite(data, false);
	return BApplication::GetSupportedSuites(data);
    }

    virtual void MessageReceived(BMessage *message) {
	if (message->what == B_KEY_MAP_LOADED) {
	    TkHaikuEvent event;

	    InitEvent(&event, TKHAIKU_KEYMAP, 0, system_time());
	    TkHaikuPostEvent(&event);
	    return;
	}
	BApplication::MessageReceived(message);
    }

    virtual bool QuitRequested() {
	TkHaikuEvent event;

	InitEvent(&event, TKHAIKU_QUIT, 0, system_time());
	TkHaikuPostEvent(&event);
	return false;
    }

private:
    ScriptHandler *fScript;
};

/*
 *----------------------------------------------------------------------
 *
 * InitEvent --
 *
 *	Initialize an event, before it is posted.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The event is cleared, and its type, window and time set.
 *
 *----------------------------------------------------------------------
 */

static void
InitEvent(
    TkHaikuEvent *eventPtr,
    TkHaikuEventType type,
    unsigned long window,	/* The X window, or 0. */
    bigtime_t when)		/* In microseconds. */
{
    memset(eventPtr, 0, sizeof(TkHaikuEvent));
    eventPtr->type = type;
    eventPtr->window = window;
    eventPtr->time = (long long)(when / 1000);
}

/*
 *----------------------------------------------------------------------
 *
 * When --
 *
 *	The time of an event of the app_server: its "when". Messages sent by
 *	scripting have none, and are input made now. The app_server sends
 *	messages without one too, when windows change under the pointer (Tk
 *	makes its own crossing events then): they are older than anything,
 *	and dropped after a warp of the pointer (tkHaikuEvent.c).
 *
 * Results:
 *	The time, in microseconds since boot.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static bigtime_t
When(
    BMessage *message)		/* The message, or NULL. */
{
    if (message == NULL) {
	return system_time();
    }
    return message->GetInt64("when",
	    message->HasSpecifiers() ? system_time() : 0);
}

/*
 *----------------------------------------------------------------------
 *
 * TkView --
 *
 *	The view which fills a window: it draws the bitmap of the window, and
 *	posts the input it gets. It is named after the X window, as "tk
 *	0x<id>" (TKHAIKU_VIEW_NAME), which identifies it for the tests and
 *	for scripting.
 *
 *----------------------------------------------------------------------
 */

class TkView : public BView {
public:
    TkView(BRect frame, unsigned long xid)
	: BView(frame, BString().SetToFormat(TKHAIKU_VIEW_NAME,
		xid).String(),
		B_FOLLOW_ALL_SIDES, B_WILL_DRAW),
	  fXid(xid), fBitmap(NULL), fCaptured(false) {
	SetViewColor(B_TRANSPARENT_COLOR);
	SetHighColor(0xd9, 0xd9, 0xd9);
    }

    virtual ~TkView() {
	delete fBitmap;
    }

    virtual void Draw(BRect updateRect) {
	BRect bounds = Bounds();

	if (fBitmap) {
	    BRect area = updateRect & fBitmap->Bounds();

	    if (area.IsValid()) {
		DrawBitmap(fBitmap, area, area);
	    }
	    if (bounds.right > fBitmap->Bounds().right) {
		FillRect(BRect(fBitmap->Bounds().right + 1, bounds.top,
			bounds.right, bounds.bottom) & updateRect);
	    }
	    if (bounds.bottom > fBitmap->Bounds().bottom) {
		FillRect(BRect(bounds.left, fBitmap->Bounds().bottom + 1,
			bounds.right, bounds.bottom) & updateRect);
	    }
	} else {
	    FillRect(updateRect);
	}
    }

    virtual void MouseDown(BPoint where) {
	/*
	 * As with X, the pointer is grabbed while a button is pressed.
	 */

	SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
	PostPointer(TKHAIKU_DOWN, where, true);
    }

    virtual void MouseUp(BPoint where) {
	PostPointer(TKHAIKU_UP, where, Bounds().Contains(where));
    }

    virtual void MouseMoved(BPoint where, uint32 transit,
	    const BMessage *) {
	PostPointer((transit == B_EXITED_VIEW) ? TKHAIKU_EXIT : TKHAIKU_MOVE,
		where, transit == B_ENTERED_VIEW || transit == B_INSIDE_VIEW);
    }

    virtual void MessageReceived(BMessage *message) {
	if (message->HasSpecifiers() && ScriptedInput(message)) {
	    return;
	}
	if (message->what == B_MOUSE_WHEEL_CHANGED) {
	    TkHaikuEvent event;

	    InitEvent(&event, TKHAIKU_WHEEL, fXid, When(message));
	    event.u.wheel.dx = message->GetFloat("be:wheel_delta_x", 0);
	    event.u.wheel.dy = message->GetFloat("be:wheel_delta_y", 0);
	    event.u.wheel.modifiers = (uint32_t)message->GetInt32("modifiers",
		    (int32)::modifiers());
	    TkHaikuPostEvent(&event);
	    return;
	}
	BView::MessageReceived(message);
    }

    /*
     * Called by Tk with the window locked.
     */

    void Update(const unsigned char *data, int stride, int width,
	    int height, const int *rects, int numRects) {
	BRect bounds(0, 0, width - 1, height - 1);
	int full[4] = {0, 0, width, height};
	int i, y;

	if (fBitmap == NULL || fBitmap->Bounds() != bounds) {
	    delete fBitmap;
	    fBitmap = new BBitmap(bounds, B_RGB32);
	    if (!fBitmap->IsValid()) {
		delete fBitmap;
		fBitmap = NULL;
		return;
	    }
	    rects = full;
	    numRects = 1;
	}
	for (i = 0; i < numRects; i++) {
	    int x0 = rects[4 * i], y0 = rects[4 * i + 1];
	    int w = rects[4 * i + 2], h = rects[4 * i + 3];
	    unsigned char *bits = (unsigned char *)fBitmap->Bits();
	    int32 bpr = fBitmap->BytesPerRow();

	    if (x0 < 0) {
		w += x0;
		x0 = 0;
	    }
	    if (y0 < 0) {
		h += y0;
		y0 = 0;
	    }
	    if (x0 + w > width) {
		w = width - x0;
	    }
	    if (y0 + h > height) {
		h = height - y0;
	    }
	    if (w <= 0 || h <= 0) {
		continue;
	    }
	    for (y = y0; y < y0 + h; y++) {
		memcpy(bits + y * bpr + 4 * x0, data + y * stride + 4 * x0,
			4 * w);
	    }
	    Invalidate(BRect(x0, y0, x0 + w - 1, y0 + h - 1));
	}
    }

    void Capture(bool capture) {
	if (capture != fCaptured) {
	    fCaptured = capture;
	    SetEventMask(capture ? B_POINTER_EVENTS : 0,
		    capture ? B_NO_POINTER_HISTORY : 0);
	}
    }

private:
    /*
     * Messages of the mouse sent by scripting (as with hey, to the view
     * named after the X window), which BView takes for scripting requests
     * because of their specifiers, are taken as input. The keys need
     * nothing: the window posts them whatever their target.
     */

    bool ScriptedInput(BMessage *message) {
	BPoint where;

	switch (message->what) {
	case B_MOUSE_DOWN:
	case B_MOUSE_UP:
	case B_MOUSE_MOVED:
	    break;
	default:
	    return false;
	}
	if (message->FindPoint("be:view_where", &where) != B_OK
		&& message->FindPoint("where", &where) != B_OK) {
	    return false;
	}
	if (message->what == B_MOUSE_DOWN) {
	    MouseDown(where);
	} else if (message->what == B_MOUSE_UP) {
	    MouseUp(where);
	} else {
	    MouseMoved(where, Bounds().Contains(where) ? B_INSIDE_VIEW
		    : B_OUTSIDE_VIEW, NULL);
	}
	if (message->IsSourceWaiting()) {
	    BMessage reply(B_REPLY);

	    reply.AddInt32("error", B_OK);
	    message->SendReply(&reply);
	}
	return true;
    }

    void PostPointer(int action, BPoint where, bool inside) {
	BMessage *message = Window()->CurrentMessage();
	TkHaikuEvent event;
	int32 buttons, modifiers;
	bigtime_t when = When(message);

	if (message && message->HasInt32("buttons")) {
	    buttons = message->GetInt32("buttons", 0);
	} else {
	    BPoint point;
	    uint32 b = 0;

	    get_mouse(&point, &b);
	    buttons = (int32)b;
	}
	modifiers = message ? message->GetInt32("modifiers",
		(int32)::modifiers()) : (int32)::modifiers();
	where = ConvertToScreen(where);
	InitEvent(&event, TKHAIKU_POINTER, fXid, when);
	event.u.pointer.action = action;
	event.u.pointer.x = where.x;
	event.u.pointer.y = where.y;
	event.u.pointer.buttons = (uint32_t)buttons;
	event.u.pointer.modifiers = (uint32_t)modifiers;
	event.u.pointer.inside = inside;
	TkHaikuPostEvent(&event);
    }

    unsigned long fXid;
    BBitmap *fBitmap;
    bool fCaptured;
};

/*
 *----------------------------------------------------------------------
 *
 * TkWindow --
 *
 *	A window of Tk: the decorated window of a toplevel, or the borderless
 *	window of a menu, tooltip or other override-redirect window.
 *
 *----------------------------------------------------------------------
 */

class TkWindow : public BWindow {
public:
    TkWindow(BRect frame, unsigned long xid, int kind)
	: BWindow(frame, "", (kind == TKHAIKU_POPUP)
		? B_NO_BORDER_WINDOW_LOOK : B_TITLED_WINDOW_LOOK,
		(kind == TKHAIKU_POPUP)
		? B_FLOATING_ALL_WINDOW_FEEL : B_NORMAL_WINDOW_FEEL,
		(kind == TKHAIKU_POPUP) ? (B_AVOID_FOCUS | B_NOT_MOVABLE
		| B_NOT_CLOSABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
		| B_NOT_RESIZABLE | B_WILL_ACCEPT_FIRST_CLICK
		| B_NO_WORKSPACE_ACTIVATION) : B_WILL_ACCEPT_FIRST_CLICK),
	  fXid(xid), fShown(false), fPopup(kind == TKHAIKU_POPUP),
	  fLevel(TKHAIKU_NORMAL) {
	fView = new TkView(Bounds(), xid);
	AddChild(fView);
	fView->MakeFocus(true);
	fScript = new ScriptHandler(xid);
	AddHandler(fScript);
    }

    virtual BHandler *ResolveSpecifier(BMessage *message, int32 index,
	    BMessage *specifier, int32 what, const char *property) {
	if (IsWidgetProperty(property)) {
	    return fScript;
	}
	return BWindow::ResolveSpecifier(message, index, specifier, what,
		property);
    }

    virtual status_t GetSupportedSuites(BMessage *data) {
	AddWidgetSuite(data, false);
	return BWindow::GetSupportedSuites(data);
    }

    virtual bool QuitRequested() {
	PostSimple(TKHAIKU_CLOSE);
	return false;
    }

    virtual void WindowActivated(bool active) {
	TkHaikuEvent event;

	InitEvent(&event, TKHAIKU_ACTIVATED, fXid, system_time());
	event.u.active = active;
	TkHaikuPostEvent(&event);
    }

    virtual void FrameMoved(BPoint where) {
	TkHaikuEvent event;

	InitEvent(&event, TKHAIKU_MOVED, fXid, system_time());
	event.u.frame.x = (int)where.x;
	event.u.frame.y = (int)where.y;
	TkHaikuPostEvent(&event);
    }

    virtual void FrameResized(float width, float height) {
	TkHaikuEvent event;

	InitEvent(&event, TKHAIKU_RESIZED, fXid, system_time());
	event.u.frame.width = (int)width + 1;
	event.u.frame.height = (int)height + 1;
	TkHaikuPostEvent(&event);
    }

    virtual void Minimize(bool minimize) {
	TkHaikuEvent event;

	BWindow::Minimize(minimize);
	if (!minimize) {
	    ApplyLevel();
	}
	InitEvent(&event, TKHAIKU_MINIMIZED, fXid, system_time());
	event.u.minimized = minimize;
	TkHaikuPostEvent(&event);
    }

    virtual void Zoom(BPoint, float, float) {
	PostSimple(TKHAIKU_ZOOM);
    }

    virtual void ScreenChanged(BRect, color_space) {
	PostSimple(TKHAIKU_SCREEN);
    }

    /*
     * Keys go to Tk before the shortcuts of the window (Alt+Q, Alt+W, ...)
     * are looked for: its bindings decide.
     */

    virtual void DispatchMessage(BMessage *message, BHandler *handler) {
	switch (message->what) {
	case B_KEY_DOWN:
	case B_KEY_UP:
	case B_UNMAPPED_KEY_DOWN:
	case B_UNMAPPED_KEY_UP:
	    PostKey(message);
	    return;
	case B_MODIFIERS_CHANGED: {
	    TkHaikuEvent event;

	    InitEvent(&event, TKHAIKU_MODIFIERS, fXid, When(message));
	    event.u.modifiers = (uint32_t)message->GetInt32("modifiers",
		    (int32)::modifiers());
	    TkHaikuPostEvent(&event);
	    return;
	}
	}
	BWindow::DispatchMessage(message, handler);
    }

    void SetShown(bool show) {
	if (show != fShown) {
	    fShown = show;
	    if (show) {
		Show();
	    } else {
		Hide();
	    }
	}
    }

    TkView *View() {
	return fView;
    }

    /*
     * Haiku does not minimize floating windows: a toplevel above the others
     * (TKHAIKU_ABOVE) is a normal window while it is minimized.
     */

    void SetLevel(int level) {
	fLevel = level;
	if (!IsMinimized()) {
	    ApplyLevel();
	}
    }

    void MinimizeWindow(bool minimize) {
	if (IsMinimized() == minimize) {
	    return;
	}
	if (minimize && !fPopup) {
	    SetFeel(B_NORMAL_WINDOW_FEEL);
	}
	BWindow::Minimize(minimize);
	if (!minimize) {
	    ApplyLevel();
	}
    }

private:
    void ApplyLevel() {
	if (!fPopup) {
	    SetFeel((fLevel == TKHAIKU_ABOVE) ? B_FLOATING_ALL_WINDOW_FEEL
		    : B_NORMAL_WINDOW_FEEL);
	}
    }

    void PostSimple(TkHaikuEventType type) {
	TkHaikuEvent event;

	InitEvent(&event, type, fXid, system_time());
	TkHaikuPostEvent(&event);
    }

    void PostKey(BMessage *message) {
	TkHaikuEvent event;
	const char *bytes = message->GetString("bytes", NULL);

	InitEvent(&event, TKHAIKU_KEY, fXid, When(message));
	event.u.key.down = (message->what == B_KEY_DOWN
		|| message->what == B_UNMAPPED_KEY_DOWN);
	event.u.key.key = (int)message->GetInt32("key", 0);
	event.u.key.modifiers = (uint32_t)message->GetInt32("modifiers",
		(int32)::modifiers());
	if (bytes && message->what == B_KEY_DOWN) {
	    strncpy(event.u.key.bytes, bytes, sizeof(event.u.key.bytes) - 1);
	}
	TkHaikuPostEvent(&event);
    }

    unsigned long fXid;
    TkView *fView;
    ScriptHandler *fScript;
    bool fShown;
    bool fPopup;		/* Override-redirect: always floating. */
    int fLevel;			/* TKHAIKU_NORMAL or TKHAIKU_ABOVE. */
};

/*
 * The C functions of tkHaiku.h: the opaque TkHaikuWindow is a TkWindow.
 */

#define WIN(p) ((TkWindow *)(void *)(p))

/*
 *----------------------------------------------------------------------
 *
 * AppThread --
 *
 *	The thread of the application: create it, and run its looper.
 *
 * Results:
 *	0.
 *
 * Side effects:
 *	The application runs until the process exits.
 *
 *----------------------------------------------------------------------
 */

static int32
AppThread(
    void *data)			/* The signature. */
{
    status_t status = B_OK;
    TkApp *app = new TkApp((const char *)data, &status);

    if (status != B_OK) {
	delete app;
	appStatus = status;
	release_sem(readySem);
	return status;
    }
    app->Run();
    return 0;
}

extern "C" {

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuStartApp --
 *
 *	Start the application in a thread of its own, unless the process
 *	already has one (Tk loaded into a Haiku application), whose clipboard
 *	changes are then watched. It is only tried once: an application which
 *	failed to start, or is late (the app_server did not answer in ten
 *	seconds) is not started again.
 *
 * Results:
 *	Whether there is an application.
 *
 * Side effects:
 *	A thread is started, which connects to the app_server.
 *
 *----------------------------------------------------------------------
 */

bool
TkHaikuStartApp(
    const char *signature)
{
    static bool tried = false;
    thread_id thread;

    if (be_app && readySem < 0) {
	if (!tried && be_app->Lock()) {
	    (new ClipboardWatcher())->Start();
	    be_app->Unlock();
	}
	tried = true;
	return true;
    }
    if (tried) {
	return be_app != NULL && appStatus == B_OK;
    }
    tried = true;
    readySem = create_sem(0, "tk ready");
    thread = spawn_thread(AppThread, "tk application", B_NORMAL_PRIORITY,
	    (void *)signature);
    if (readySem < 0 || thread < 0 || resume_thread(thread) != B_OK) {
	return false;
    }
    return acquire_sem_etc(readySem, 1, B_RELATIVE_TIMEOUT, 10000000) == B_OK
	    && appStatus == B_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuScreenSize, TkHaikuWorkArea, TkHaikuGetMouse,
 * TkHaikuSetMousePosition, TkHaikuModifiers, TkHaikuSystemTime,
 * TkHaikuBeep, TkHaikuFontScale, TkHaikuFontFamilies --
 *
 *	The screen, the part of it which the Deskbar leaves to windows, the
 *	mouse, the keyboard, the time of the events (in milliseconds since
 *	boot), the beep, and the size (Haiku scales its interface with it)
 *	and families of the fonts the user chose.
 *
 * Results:
 *	As the names say; the functions which return several values store
 *	them.
 *
 * Side effects:
 *	TkHaikuSetMousePosition moves the pointer, TkHaikuBeep beeps.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuScreenSize(
    int *widthPtr, int *heightPtr,
    double *dpiPtr)
{
    BScreen screen;
    BRect frame = screen.Frame();

    *widthPtr = (int)frame.Width() + 1;
    *heightPtr = (int)frame.Height() + 1;
    *dpiPtr = 96.0;
}

void
TkHaikuWorkArea(
    int *xPtr, int *yPtr,
    int *widthPtr, int *heightPtr)
{
    BScreen screen;
    BRect frame = screen.Frame();
    BDeskbar deskbar;

    /*
     * Only a Deskbar across the top or the bottom of the screen takes room
     * from windows; in a corner, windows may go under it.
     */

    if (deskbar.IsRunning()) {
	BRect bar = deskbar.Frame();

	switch (deskbar.Location()) {
	case B_DESKBAR_TOP:
	    frame.top = bar.bottom + 1;
	    break;
	case B_DESKBAR_BOTTOM:
	    frame.bottom = bar.top - 1;
	    break;
	default:
	    break;
	}
    }
    *xPtr = (int)frame.left;
    *yPtr = (int)frame.top;
    *widthPtr = (int)frame.Width() + 1;
    *heightPtr = (int)frame.Height() + 1;
}

void
TkHaikuGetMouse(
    int *xPtr, int *yPtr,
    uint32_t *buttonsPtr)
{
    BPoint where;
    uint32 buttons = 0;

    get_mouse(&where, &buttons);
    *xPtr = (int)where.x;
    *yPtr = (int)where.y;
    *buttonsPtr = buttons;
}

void
TkHaikuSetMousePosition(
    int x, int y)
{
    set_mouse_position(x, y);
}

uint32_t
TkHaikuModifiers(void)
{
    return modifiers();
}

long long
TkHaikuSystemTime(void)
{
    return (long long)(system_time() / 1000);
}

void
TkHaikuBeep(void)
{
    beep();
}

double
TkHaikuFontScale(void)
{
    return be_plain_font->Size() / 12.0;
}

void
TkHaikuFontFamilies(
    char *plain,		/* Returns the family of the plain font. */
    char *fixed,		/* Returns the family of the fixed font. */
    int size)			/* Size of both buffers. */
{
    font_family family;
    font_style style;

    be_plain_font->GetFamilyAndStyle(&family, &style);
    snprintf(plain, size, "%s", family);
    be_fixed_font->GetFamilyAndStyle(&family, &style);
    snprintf(fixed, size, "%s", family);
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuWindowCreate, TkHaikuWindowDestroy --
 *
 *	Create a window, hidden, or destroy it.
 *
 * Results:
 *	TkHaikuWindowCreate returns the window.
 *
 * Side effects:
 *	A BWindow is created, with a thread of its own, or quits: its thread
 *	ends, and the BWindow is deleted.
 *
 *----------------------------------------------------------------------
 */

TkHaikuWindow *
TkHaikuWindowCreate(
    unsigned long xid,		/* The X window. */
    int kind,			/* TKHAIKU_TOPLEVEL or TKHAIKU_POPUP. */
    int x, int y,		/* Position of the content. */
    int width, int height)	/* Size of the content. */
{
    TkWindow *window = new TkWindow(BRect(x, y, x + width - 1,
	    y + height - 1), xid, kind);

    /*
     * The window is run hidden, so that its thread exists and the other
     * functions can lock it.
     */

    window->Hide();
    window->Show();
    return (TkHaikuWindow *)window;
}

void
TkHaikuWindowDestroy(
    TkHaikuWindow *winPtr)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	window->Quit();
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuWindowShow, TkHaikuWindowMoveResize, TkHaikuWindowSetTitle,
 * TkHaikuWindowSetSizeLimits, TkHaikuWindowSetDecorated,
 * TkHaikuWindowSetLevel, TkHaikuWindowMinimize, TkHaikuWindowActivate,
 * TkHaikuWindowIsActive, TkHaikuWindowSendBehind, TkHaikuWindowGetFrame,
 * TkHaikuWindowFrameExtents, TkHaikuWindowUpdate, TkHaikuWindowSetCursor,
 * TkHaikuWindowCapture --
 *
 *	Change or ask a window, as the names say. Called by Tk; each locks
 *	the window.
 *
 * Results:
 *	TkHaikuWindowIsActive returns whether the window is active;
 *	TkHaikuWindowGetFrame stores the position and size of its content,
 *	TkHaikuWindowFrameExtents the widths of its decorations (0 if the
 *	window cannot be locked).
 *
 * Side effects:
 *	The window changes. TkHaikuWindowUpdate copies parts of the image of
 *	Tk to the bitmap of the window, which draws them.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuWindowShow(
    TkHaikuWindow *winPtr,
    bool show)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	window->SetShown(show);
	window->Unlock();
    }
}

void
TkHaikuWindowMoveResize(
    TkHaikuWindow *winPtr,
    int x, int y,
    int width, int height)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	BRect frame = window->Frame();

	if (frame.left != x || frame.top != y) {
	    window->MoveTo(x, y);
	}
	if (frame.Width() != width - 1 || frame.Height() != height - 1) {
	    float minW, maxW, minH, maxH;

	    /*
	     * The size limits are for the user: as X window managers, the
	     * app_server gives the application the size it asks for.
	     */

	    window->GetSizeLimits(&minW, &maxW, &minH, &maxH);
	    if (width - 1 < minW || width - 1 > maxW || height - 1 < minH
		    || height - 1 > maxH) {
		window->SetSizeLimits(
			(width - 1 < minW) ? width - 1 : minW,
			(width - 1 > maxW) ? width - 1 : maxW,
			(height - 1 < minH) ? height - 1 : minH,
			(height - 1 > maxH) ? height - 1 : maxH);
	    }
	    window->ResizeTo(width - 1, height - 1);
	}
	window->Unlock();
    }
}

void
TkHaikuWindowSetTitle(
    TkHaikuWindow *winPtr,
    const char *title)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	window->SetTitle(title);
	window->Unlock();
    }
}

void
TkHaikuWindowSetSizeLimits(
    TkHaikuWindow *winPtr,
    int minWidth, int minHeight,
    int maxWidth, int maxHeight)	/* 0 for no maximum. */
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	uint32 flags = window->Flags()
		& ~(B_NOT_RESIZABLE | B_NOT_H_RESIZABLE | B_NOT_V_RESIZABLE
		| B_NOT_ZOOMABLE);
	float maxW = maxWidth > 0 ? maxWidth - 1 : 32767;
	float maxH = maxHeight > 0 ? maxHeight - 1 : 32767;

	window->SetSizeLimits(minWidth > 0 ? minWidth - 1 : 0, maxW,
		minHeight > 0 ? minHeight - 1 : 0, maxH);
	if (maxWidth > 0 && minWidth == maxWidth) {
	    flags |= B_NOT_H_RESIZABLE;
	}
	if (maxHeight > 0 && minHeight == maxHeight) {
	    flags |= B_NOT_V_RESIZABLE;
	}
	if ((flags & B_NOT_H_RESIZABLE) && (flags & B_NOT_V_RESIZABLE)) {
	    flags |= B_NOT_RESIZABLE | B_NOT_ZOOMABLE;
	}
	if (!(window->Flags() & B_AVOID_FOCUS)) {
	    window->SetFlags(flags);
	}
	window->Unlock();
    }
}

void
TkHaikuWindowSetDecorated(
    TkHaikuWindow *winPtr,
    bool decorated)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	window->SetLook(decorated ? B_TITLED_WINDOW_LOOK
		: B_NO_BORDER_WINDOW_LOOK);
	window->Unlock();
    }
}

void
TkHaikuWindowSetLevel(
    TkHaikuWindow *winPtr,
    int level)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	window->SetLevel(level);
	window->Unlock();
    }
}

void
TkHaikuWindowMinimize(
    TkHaikuWindow *winPtr,
    bool minimize)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	window->MinimizeWindow(minimize);
	window->Unlock();
    }
}

void
TkHaikuWindowActivate(
    TkHaikuWindow *winPtr)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	window->Activate(true);
	window->Unlock();
    }
}

bool
TkHaikuWindowIsActive(
    TkHaikuWindow *winPtr)
{
    TkWindow *window = WIN(winPtr);
    bool active = false;

    if (window->Lock()) {
	active = window->IsActive();
	window->Unlock();
    }
    return active;
}

void
TkHaikuWindowSendBehind(
    TkHaikuWindow *winPtr,
    TkHaikuWindow *otherPtr)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	window->SendBehind(otherPtr ? WIN(otherPtr) : NULL);
	window->Unlock();
    }
}

void
TkHaikuWindowGetFrame(
    TkHaikuWindow *winPtr,
    int *xPtr, int *yPtr,
    int *widthPtr, int *heightPtr)
{
    TkWindow *window = WIN(winPtr);

    *xPtr = *yPtr = *widthPtr = *heightPtr = 0;
    if (window->Lock()) {
	BRect frame = window->Frame();

	*xPtr = (int)frame.left;
	*yPtr = (int)frame.top;
	*widthPtr = (int)frame.Width() + 1;
	*heightPtr = (int)frame.Height() + 1;
	window->Unlock();
    }
}

void
TkHaikuWindowFrameExtents(
    TkHaikuWindow *winPtr,
    int *leftPtr, int *topPtr,
    int *rightPtr, int *bottomPtr)
{
    TkWindow *window = WIN(winPtr);

    *leftPtr = *topPtr = *rightPtr = *bottomPtr = 0;
    if (window->Lock()) {
	BRect frame = window->Frame();
	BRect decorator = window->DecoratorFrame();

	*leftPtr = (int)(frame.left - decorator.left);
	*topPtr = (int)(frame.top - decorator.top);
	*rightPtr = (int)(decorator.right - frame.right);
	*bottomPtr = (int)(decorator.bottom - frame.bottom);
	window->Unlock();
    }
}

void
TkHaikuWindowUpdate(
    TkHaikuWindow *winPtr,
    const unsigned char *data,	/* The image, xRGB. */
    int stride,			/* Bytes per row of the image. */
    int width, int height,	/* Its size. */
    const int *rects,		/* The parts of it which changed: x, y,
				 * width and height of each. */
    int numRects)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	window->View()->Update(data, stride, width, height, rects, numRects);
	window->Unlock();
    }
}

void
TkHaikuWindowSetCursor(
    TkHaikuWindow *winPtr,
    int cursorId,		/* A BCursorID, or 0 for an image. */
    const uint32_t *argb,	/* The image: premultiplied ARGB. */
    int width, int height,
    int xHot, int yHot)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	if (cursorId > 0) {
	    BCursor cursor((BCursorID)cursorId);

	    window->View()->SetViewCursor(&cursor, true);
	} else if (argb && width > 0 && height > 0) {
	    BBitmap bitmap(BRect(0, 0, width - 1, height - 1), B_RGBA32);
	    unsigned char *bits = (unsigned char *)bitmap.Bits();
	    int x, y;

	    for (y = 0; y < height; y++) {
		uint32 *row = (uint32 *)(void *)(bits
			+ y * bitmap.BytesPerRow());

		for (x = 0; x < width; x++) {
		    uint32_t p = argb[y * width + x];
		    uint32_t a = p >> 24;

		    /*
		     * B_RGBA32 is not premultiplied.
		     */

		    if (a > 0 && a < 255) {
			uint32_t r = ((p >> 16) & 0xFF) * 255 / a;
			uint32_t g = ((p >> 8) & 0xFF) * 255 / a;
			uint32_t b = (p & 0xFF) * 255 / a;

			p = (a << 24) | (r << 16) | (g << 8) | b;
		    }
		    row[x] = p;
		}
	    }
	    BCursor cursor(&bitmap, BPoint(xHot, yHot));

	    window->View()->SetViewCursor(&cursor, true);
	}
	window->Unlock();
    }
}

void
TkHaikuWindowCapture(
    TkHaikuWindow *winPtr,
    bool capture)
{
    TkWindow *window = WIN(winPtr);

    if (window->Lock()) {
	window->View()->Capture(capture);
	window->Unlock();
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuClipboardGet, TkHaikuClipboardSet, TkHaikuClipboardClear,
 * TkHaikuClipboardCount --
 *
 *	The text of the system clipboard, emptying it, and the number of
 *	times it was changed (by any application).
 *
 * Results:
 *	TkHaikuClipboardGet returns whether the clipboard has text, and
 *	stores it; TkHaikuClipboardCount returns the count.
 *
 * Side effects:
 *	TkHaikuClipboardSet and TkHaikuClipboardClear change the clipboard of
 *	the system.
 *
 *----------------------------------------------------------------------
 */

bool
TkHaikuClipboardGet(
    char **textPtr,		/* Returns the text, allocated with
				 * malloc. */
    size_t *lengthPtr)		/* Returns its length. */
{
    const char *text = NULL;
    ssize_t length = 0;
    bool result = false;

    if (be_clipboard->Lock()) {
	BMessage *clip = be_clipboard->Data();

	if (clip && clip->FindData("text/plain", B_MIME_TYPE,
		(const void **)&text, &length) == B_OK) {
	    *textPtr = (char *)malloc(length + 1);
	    memcpy(*textPtr, text, length);
	    (*textPtr)[length] = '\0';
	    *lengthPtr = (size_t)length;
	    result = true;
	}
	be_clipboard->Unlock();
    }
    return result;
}

void
TkHaikuClipboardSet(
    const char *text,
    size_t length)
{
    if (be_clipboard->Lock()) {
	BMessage *clip;

	be_clipboard->Clear();
	clip = be_clipboard->Data();
	if (clip) {
	    clip->AddData("text/plain", B_MIME_TYPE, text, (ssize_t)length);
	    be_clipboard->Commit();
	}
	be_clipboard->Unlock();
    }
}

void
TkHaikuClipboardClear(void)
{
    if (be_clipboard->Lock()) {
	be_clipboard->Clear();
	be_clipboard->Commit();
	be_clipboard->Unlock();
    }
}

uint32_t
TkHaikuClipboardCount(void)
{
    return be_clipboard->SystemCount();
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuGetScript, TkHaikuScriptReply, ScriptValue --
 *
 *	Read a scripting request for the widgets of Tk, and answer it, from
 *	the thread of Tk.
 *
 * Results:
 *	TkHaikuGetScript returns whether the request is one for Tk, and fills
 *	in the TkHaikuScript.
 *
 * Side effects:
 *	TkHaikuScriptReply sends the reply (the result, of the type Tk gives:
 *	bool, int32, float, rect or string; or the error), and deletes the
 *	request and the strings of the TkHaikuScript.
 *
 *----------------------------------------------------------------------
 */

static char *
ScriptValue(
    BMessage *message,
    const char *name)		/* The field. */
{
    type_code type;
    BString value;

    if (message->GetInfo(name, &type) != B_OK) {
	return strdup("");
    }
    switch (type) {
    case B_STRING_TYPE:
	value = message->GetString(name, "");
	break;
    case B_BOOL_TYPE:
	value = message->GetBool(name, false) ? "1" : "0";
	break;
    case B_INT8_TYPE:
	value << (int32)message->GetInt8(name, 0);
	break;
    case B_INT16_TYPE:
	value << (int32)message->GetInt16(name, 0);
	break;
    case B_INT32_TYPE:
	value << message->GetInt32(name, 0);
	break;
    case B_INT64_TYPE:
	value << message->GetInt64(name, 0);
	break;
    case B_FLOAT_TYPE:
	value << message->GetFloat(name, 0);
	break;
    case B_DOUBLE_TYPE:
	value << message->GetDouble(name, 0);
	break;
    }
    return strdup(value.String());
}

bool
TkHaikuGetScript(
    void *request,		/* The request, a BMessage. */
    TkHaikuScript *scriptPtr)	/* Returns what it asks. */
{
    BMessage *message = (BMessage *)request;
    BMessage specifier;
    int32 i;

    memset(scriptPtr, 0, sizeof(TkHaikuScript));
    switch (message->what) {
    case B_GET_PROPERTY:
	scriptPtr->verb = "get";
	break;
    case B_SET_PROPERTY:
	scriptPtr->verb = "set";
	break;
    case B_EXECUTE_PROPERTY:
	scriptPtr->verb = "do";
	break;
    case B_COUNT_PROPERTIES:
	scriptPtr->verb = "count";
	break;
    default:
	return false;
    }
    scriptPtr->form = "";
    for (i = 0; message->FindMessage("specifiers", i, &specifier) == B_OK;
	    i++) {
	const char *property = specifier.GetString("property", "");
	BString widget;

	if (!IsWidgetProperty(property)) {
	    if (strcmp(property, "Window") != 0 && !scriptPtr->property) {
		scriptPtr->property = strdup(property);
	    }
	    continue;
	}
	switch (specifier.what) {
	case B_NAME_SPECIFIER:
	    scriptPtr->form = "name";
	    widget = specifier.GetString("name", "");
	    break;
	case B_INDEX_SPECIFIER:
	    scriptPtr->form = "index";
	    widget << specifier.GetInt32("index", 0);
	    break;
	case B_REVERSE_INDEX_SPECIFIER:
	    scriptPtr->form = "reverse";
	    widget << specifier.GetInt32("index", 0);
	    break;
	}
	free(scriptPtr->widget);
	scriptPtr->widget = strdup(widget.String());
    }
    if (!scriptPtr->property) {
	scriptPtr->property = strdup("");
    }
    if (!scriptPtr->widget) {
	scriptPtr->widget = strdup("");
    }
    scriptPtr->value = ScriptValue(message, "data");
    return true;
}

void
TkHaikuScriptReply(
    void *request,		/* The request, a BMessage. */
    TkHaikuScript *scriptPtr,	/* What TkHaikuGetScript filled in. */
    const char *error,		/* The error, or NULL. */
    const char *type,		/* The type of the result, or NULL. */
    const char *value)		/* The result. */
{
    BMessage *message = (BMessage *)request;
    BMessage reply(error ? (uint32)B_MESSAGE_NOT_UNDERSTOOD
	    : (uint32)B_REPLY);

    if (error) {
	reply.AddString("message", error);
	reply.AddInt32("error", B_BAD_VALUE);
    } else {
	if (type == NULL || strcmp(type, "none") == 0) {
	    /* No result. */
	} else if (strcmp(type, "bool") == 0) {
	    reply.AddBool("result", atoi(value) != 0);
	} else if (strcmp(type, "int32") == 0) {
	    reply.AddInt32("result", (int32)strtol(value, NULL, 10));
	} else if (strcmp(type, "float") == 0) {
	    reply.AddFloat("result", strtof(value, NULL));
	} else if (strcmp(type, "rect") == 0) {
	    float l = 0, t = 0, r = 0, b = 0;

	    sscanf(value, "%f %f %f %f", &l, &t, &r, &b);
	    reply.AddRect("result", BRect(l, t, r, b));
	} else {
	    reply.AddString("result", value);
	}
	reply.AddInt32("error", B_OK);
    }
    message->SendReply(&reply);
    delete message;
    if (scriptPtr) {
	free(scriptPtr->property);
	free(scriptPtr->widget);
	free(scriptPtr->value);
	memset(scriptPtr, 0, sizeof(TkHaikuScript));
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuKeymapChars, TkHaikuModifierKeys, TkHaikuKeymapChanged,
 * GetKeymap --
 *
 *	The keymap of the user, read when first needed, and again after
 *	TkHaikuKeymapChanged: the characters a key types with modifiers, and
 *	the keys which are the modifiers (the user may swap them).
 *
 * Results:
 *	TkHaikuKeymapChars: the length of the characters, in UTF-8, stored in
 *	the buffer with a null byte, or 0. TkHaikuModifierKeys stores the key
 *	codes of the modifiers (TKHAIKU_KEY_LEFT_SHIFT etc.), 0 where there
 *	is none.
 *
 * Side effects:
 *	The keymap may be read.
 *
 *----------------------------------------------------------------------
 */

static key_map *keymap = NULL;	/* Called by Tk only. */
static char *keymapChars = NULL;

static bool
GetKeymap(void)
{
    if (keymap == NULL) {
	get_key_map(&keymap, &keymapChars);
	if (keymap == NULL || keymapChars == NULL) {
	    free(keymap);
	    free(keymapChars);
	    keymap = NULL;
	    keymapChars = NULL;
	    return false;
	}
    }
    return true;
}

void
TkHaikuKeymapChanged(void)
{
    free(keymap);
    free(keymapChars);
    keymap = NULL;
    keymapChars = NULL;
}

void
TkHaikuModifierKeys(
    int keys[8])		/* Returns the key codes. */
{
    if (!GetKeymap()) {
	memset(keys, 0, 8 * sizeof(int));
	return;
    }
    keys[TKHAIKU_KEY_LEFT_SHIFT] = (int)keymap->left_shift_key;
    keys[TKHAIKU_KEY_RIGHT_SHIFT] = (int)keymap->right_shift_key;
    keys[TKHAIKU_KEY_LEFT_COMMAND] = (int)keymap->left_command_key;
    keys[TKHAIKU_KEY_RIGHT_COMMAND] = (int)keymap->right_command_key;
    keys[TKHAIKU_KEY_LEFT_CONTROL] = (int)keymap->left_control_key;
    keys[TKHAIKU_KEY_RIGHT_CONTROL] = (int)keymap->right_control_key;
    keys[TKHAIKU_KEY_LEFT_OPTION] = (int)keymap->left_option_key;
    keys[TKHAIKU_KEY_RIGHT_OPTION] = (int)keymap->right_option_key;
}

int
TkHaikuKeymapChars(
    int key,			/* Key code of the Be API. */
    uint32_t modifiers,		/* B_SHIFT_KEY, B_CAPS_LOCK and
				 * B_OPTION_KEY are used. */
    char *buffer,
    int size)
{
    bool shift = (modifiers & B_SHIFT_KEY) != 0;
    bool caps = (modifiers & B_CAPS_LOCK) != 0;
    const int32 *map;
    int length;

    if (key < 0 || key >= 128 || !GetKeymap()) {
	return 0;
    }
    if (modifiers & B_OPTION_KEY) {
	map = (shift && caps) ? keymap->option_caps_shift_map
		: caps ? keymap->option_caps_map
		: shift ? keymap->option_shift_map : keymap->option_map;
    } else {
	map = (shift && caps) ? keymap->caps_shift_map
		: caps ? keymap->caps_map
		: shift ? keymap->shift_map : keymap->normal_map;
    }
    length = (unsigned char)keymapChars[map[key]];
    if (length <= 0 || length >= size) {
	return 0;
    }
    memcpy(buffer, keymapChars + map[key] + 1, length);
    buffer[length] = '\0';
    return length;
}

} /* extern "C" */

/*
 * Local Variables:
 * mode: c++
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
