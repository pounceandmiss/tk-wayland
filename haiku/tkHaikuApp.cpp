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
#include <Screen.h>
#include <View.h>
#include <Window.h>
#include <WindowScreen.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tkHaiku.h"

/*
 * The semaphore released when the application is running.
 */

static sem_id readySem = -1;

static void		Post(TkHaikuEvent *eventPtr, unsigned long window,
			    bigtime_t when);

/*
 *----------------------------------------------------------------------
 *
 * TkApp --
 *
 *	The application. It runs in a thread of its own, started by
 *	TkHaikuStartApp. It watches the clipboard, and turns the request to
 *	quit into an event for Tk.
 *
 *----------------------------------------------------------------------
 */

class TkApp : public BApplication {
public:
    TkApp(const char *signature) : BApplication(signature) {}

    virtual void ReadyToRun() {
	be_clipboard->StartWatching(BMessenger(this));
	release_sem(readySem);
    }

    virtual void MessageReceived(BMessage *message) {
	if (message->what == B_CLIPBOARD_CHANGED) {
	    TkHaikuEvent event;

	    Post(&event, 0, system_time());
	    event.type = TKHAIKU_CLIPBOARD;
	    TkHaikuPostEvent(&event);
	    return;
	}
	BApplication::MessageReceived(message);
    }

    virtual bool QuitRequested() {
	TkHaikuEvent event;

	Post(&event, 0, system_time());
	event.type = TKHAIKU_QUIT;
	TkHaikuPostEvent(&event);
	return false;
    }
};

/*
 *----------------------------------------------------------------------
 *
 * Post --
 *
 *	Initialize an event.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The event is cleared, and its window and time set.
 *
 *----------------------------------------------------------------------
 */

static void
Post(
    TkHaikuEvent *eventPtr,
    unsigned long window,
    bigtime_t when)		/* In microseconds. */
{
    memset(eventPtr, 0, sizeof(TkHaikuEvent));
    eventPtr->window = window;
    eventPtr->time = (long long)(when / 1000);
}

/*
 *----------------------------------------------------------------------
 *
 * TkView --
 *
 *	The view which fills a window: it draws the bitmap of the window, and
 *	posts the input it gets.
 *
 *----------------------------------------------------------------------
 */

class TkView : public BView {
public:
    TkView(BRect frame, unsigned long xid)
	: BView(frame, "tk", B_FOLLOW_ALL_SIDES, B_WILL_DRAW),
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
	if (message->what == B_MOUSE_WHEEL_CHANGED) {
	    TkHaikuEvent event;
	    float dx = 0, dy = 0;
	    int32 modifiers = 0;
	    bigtime_t when = system_time();

	    message->FindFloat("be:wheel_delta_x", &dx);
	    message->FindFloat("be:wheel_delta_y", &dy);
	    message->FindInt32("modifiers", &modifiers);
	    message->FindInt64("when", &when);
	    Post(&event, fXid, when);
	    event.type = TKHAIKU_WHEEL;
	    event.u.wheel.dx = dx;
	    event.u.wheel.dy = dy;
	    event.u.wheel.modifiers = (uint32_t)modifiers;
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
    void PostPointer(int action, BPoint where, bool inside) {
	BMessage *message = Window()->CurrentMessage();
	TkHaikuEvent event;
	int32 buttons = 0, modifiers = 0, clicks = 1;
	bigtime_t when = system_time();

	if (message) {
	    message->FindInt32("buttons", &buttons);
	    message->FindInt32("modifiers", &modifiers);
	    message->FindInt32("clicks", &clicks);
	    message->FindInt64("when", &when);
	}
	if (message == NULL || !message->HasInt32("buttons")) {
	    BPoint point;
	    uint32 b = 0;

	    get_mouse(&point, &b);
	    buttons = (int32)b;
	}
	if (message == NULL || !message->HasInt32("modifiers")) {
	    modifiers = (int32)::modifiers();
	}
	where = ConvertToScreen(where);
	Post(&event, fXid, when);
	event.type = TKHAIKU_POINTER;
	event.u.pointer.action = action;
	event.u.pointer.x = where.x;
	event.u.pointer.y = where.y;
	event.u.pointer.buttons = (uint32_t)buttons;
	event.u.pointer.modifiers = (uint32_t)modifiers;
	event.u.pointer.clicks = clicks;
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
	  fXid(xid), fShown(false) {
	fView = new TkView(Bounds(), xid);
	AddChild(fView);
	fView->MakeFocus(true);
    }

    virtual bool QuitRequested() {
	PostSimple(TKHAIKU_CLOSE);
	return false;
    }

    virtual void WindowActivated(bool active) {
	TkHaikuEvent event;

	Post(&event, fXid, system_time());
	event.type = TKHAIKU_ACTIVATED;
	event.u.active = active;
	TkHaikuPostEvent(&event);
    }

    virtual void FrameMoved(BPoint where) {
	TkHaikuEvent event;

	Post(&event, fXid, system_time());
	event.type = TKHAIKU_MOVED;
	event.u.frame.x = (int)where.x;
	event.u.frame.y = (int)where.y;
	TkHaikuPostEvent(&event);
    }

    virtual void FrameResized(float width, float height) {
	TkHaikuEvent event;

	Post(&event, fXid, system_time());
	event.type = TKHAIKU_RESIZED;
	event.u.frame.width = (int)width + 1;
	event.u.frame.height = (int)height + 1;
	TkHaikuPostEvent(&event);
    }

    virtual void Minimize(bool minimize) {
	TkHaikuEvent event;

	BWindow::Minimize(minimize);
	Post(&event, fXid, system_time());
	event.type = TKHAIKU_MINIMIZED;
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
	    int32 modifiers = 0, old = 0;
	    bigtime_t when = system_time();

	    message->FindInt32("modifiers", &modifiers);
	    message->FindInt32("be:old_modifiers", &old);
	    message->FindInt64("when", &when);
	    Post(&event, fXid, when);
	    event.type = TKHAIKU_MODIFIERS;
	    event.u.modifiers.modifiers = (uint32_t)modifiers;
	    event.u.modifiers.old = (uint32_t)old;
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

private:
    void PostSimple(TkHaikuEventType type) {
	TkHaikuEvent event;

	Post(&event, fXid, system_time());
	event.type = type;
	TkHaikuPostEvent(&event);
    }

    void PostKey(BMessage *message) {
	TkHaikuEvent event;
	int32 key = 0, modifiers = 0, rawChar = 0, repeat = 0;
	const char *bytes = NULL;
	bigtime_t when = system_time();

	message->FindInt32("key", &key);
	message->FindInt32("modifiers", &modifiers);
	message->FindInt32("raw_char", &rawChar);
	message->FindInt32("be:key_repeat", &repeat);
	message->FindString("bytes", &bytes);
	message->FindInt64("when", &when);
	Post(&event, fXid, when);
	event.type = TKHAIKU_KEY;
	event.u.key.down = (message->what == B_KEY_DOWN
		|| message->what == B_UNMAPPED_KEY_DOWN);
	event.u.key.repeat = (repeat > 0);
	event.u.key.key = key;
	event.u.key.modifiers = (uint32_t)modifiers;
	event.u.key.rawChar = rawChar;
	if (bytes && message->what == B_KEY_DOWN) {
	    strncpy(event.u.key.bytes, bytes, sizeof(event.u.key.bytes) - 1);
	}
	TkHaikuPostEvent(&event);
    }

    unsigned long fXid;
    TkView *fView;
    bool fShown;
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
    TkApp *app = new TkApp((const char *)data);

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
 *	already has one (Tk loaded into a Haiku application).
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
    thread_id thread;

    if (be_app) {
	return true;
    }
    readySem = create_sem(0, "tk ready");
    thread = spawn_thread(AppThread, "tk application", B_NORMAL_PRIORITY,
	    (void *)signature);
    if (thread < 0 || resume_thread(thread) != B_OK) {
	return false;
    }
    return acquire_sem_etc(readySem, 1, B_RELATIVE_TIMEOUT, 10000000) == B_OK;
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
 * The other functions on windows --
 *
 *	Called by Tk; each locks the window.
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
	window->SetFeel(level == TKHAIKU_ABOVE ? B_FLOATING_ALL_WINDOW_FEEL
		: B_NORMAL_WINDOW_FEEL);
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
	if (window->IsMinimized() != minimize) {
	    window->BWindow::Minimize(minimize);
	}
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
 * TkHaikuKeymapChars --
 *
 *	The characters a key types with modifiers, in the keymap of the user.
 *
 * Results:
 *	The length of the characters, in UTF-8, stored in the buffer with a
 *	null byte, or 0.
 *
 * Side effects:
 *	The keymap is read the first time.
 *
 *----------------------------------------------------------------------
 */

int
TkHaikuKeymapChars(
    int key,			/* Key code of the Be API. */
    uint32_t modifiers,		/* B_SHIFT_KEY etc. */
    char *buffer,
    int size)
{
    static key_map *keys = NULL;
    static char *chars = NULL;
    const int32 *map;
    int32 offset;
    int length;

    if (keys == NULL) {
	get_key_map(&keys, &chars);
	if (keys == NULL || chars == NULL) {
	    return 0;
	}
    }
    if (key < 0 || key >= 128) {
	return 0;
    }
    bool shift = (modifiers & B_SHIFT_KEY) != 0;
    bool caps = (modifiers & B_CAPS_LOCK) != 0;

    if (modifiers & B_CONTROL_KEY) {
	map = keys->control_map;
    } else if (modifiers & B_OPTION_KEY) {
	map = (shift && caps) ? keys->option_caps_shift_map
		: caps ? keys->option_caps_map
		: shift ? keys->option_shift_map : keys->option_map;
    } else {
	map = (shift && caps) ? keys->caps_shift_map
		: caps ? keys->caps_map
		: shift ? keys->shift_map : keys->normal_map;
    }
    offset = map[key];
    length = (unsigned char)chars[offset];
    if (length <= 0 || length >= size) {
	return 0;
    }
    memcpy(buffer, chars + offset + 1, length);
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
