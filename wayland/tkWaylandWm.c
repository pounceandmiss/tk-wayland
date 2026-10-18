/*
 * tkWaylandWm.c --
 *
 *	The window manager of the emulated X display. The "wm" command is
 *	implemented by the X11 code (tkUnixWm.c), which communicates with the
 *	window manager through ICCCM and EWMH properties and client messages.
 *	This file plays the part of that window manager: it gives the
 *	children of the root window which are not override-redirect the role
 *	of decorated xdg_toplevels (with libdecor), and translates the hints
 *	in their properties to xdg_toplevel requests, and the configure and
 *	close events of the compositor to X events. Override-redirect windows
 *	(menus, combobox dropdowns, tooltips) get the role of xdg_popups.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkWaylandInt.h"

static void		FrameConfigure(struct libdecor_frame *frame,
			    struct libdecor_configuration *configuration,
			    void *data);
static void		FrameClose(struct libdecor_frame *frame, void *data);
static void		FrameCommit(struct libdecor_frame *frame, void *data);
static void		FrameDismissPopup(struct libdecor_frame *frame,
			    const char *seatName, void *data);

static void		DestroyRole(TkWlSurface *surfPtr);
static void		MapPopup(TkWlSurface *surfPtr);

static struct libdecor_frame_interface frameInterface = {
    FrameConfigure,
    FrameClose,
    FrameCommit,
    FrameDismissPopup,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/*
 * The atoms of the properties and messages the window manager handles.
 */

static struct {
    Atom WM_PROTOCOLS, WM_DELETE_WINDOW, WM_CHANGE_STATE, UTF8_STRING,
	_NET_WM_NAME, _NET_WM_STATE, _NET_WM_STATE_MAXIMIZED_VERT,
	_NET_WM_STATE_MAXIMIZED_HORZ, _NET_WM_STATE_FULLSCREEN,
	_NET_WM_STATE_HIDDEN, _NET_ACTIVE_WINDOW;
} atoms;

static void
InitAtoms(
    Display *display)
{
    if (atoms.WM_PROTOCOLS != None) {
	return;
    }
#define ATOM(name) atoms.name = XInternAtom(display, #name, False)
    ATOM(WM_PROTOCOLS);
    ATOM(WM_DELETE_WINDOW);
    ATOM(WM_CHANGE_STATE);
    ATOM(UTF8_STRING);
    ATOM(_NET_WM_NAME);
    ATOM(_NET_WM_STATE);
    ATOM(_NET_WM_STATE_MAXIMIZED_VERT);
    ATOM(_NET_WM_STATE_MAXIMIZED_HORZ);
    ATOM(_NET_WM_STATE_FULLSCREEN);
    ATOM(_NET_WM_STATE_HIDDEN);
    ATOM(_NET_ACTIVE_WINDOW);
#undef ATOM
}

/*
 *----------------------------------------------------------------------
 *
 * Xlib functions which set the hints for the window manager --
 *
 *	As in Xlib, they only set properties.
 *
 *----------------------------------------------------------------------
 */

XSizeHints *
XAllocSizeHints(void)
{
    XSizeHints *hints = (XSizeHints *)Tcl_Alloc(sizeof(XSizeHints));

    memset(hints, 0, sizeof(XSizeHints));
    return hints;
}

XClassHint *
XAllocClassHint(void)
{
    XClassHint *hints = (XClassHint *)Tcl_Alloc(sizeof(XClassHint));

    memset(hints, 0, sizeof(XClassHint));
    return hints;
}

int
XStoreName(
    Display *display,
    Window w,
    const char *name)
{
    return XChangeProperty(display, w, XA_WM_NAME, XA_STRING, 8,
	    PropModeReplace, (const unsigned char *)name, (int)strlen(name));
}

/*
 * The layout of the WM_NORMAL_HINTS property, in longs.
 */

enum {
    SH_FLAGS, SH_X, SH_Y, SH_WIDTH, SH_HEIGHT, SH_MIN_WIDTH, SH_MIN_HEIGHT,
    SH_MAX_WIDTH, SH_MAX_HEIGHT, SH_WIDTH_INC, SH_HEIGHT_INC,
    SH_MIN_ASPECT_X, SH_MIN_ASPECT_Y, SH_MAX_ASPECT_X, SH_MAX_ASPECT_Y,
    SH_BASE_WIDTH, SH_BASE_HEIGHT, SH_WIN_GRAVITY, SH_NUM
};

void
XSetWMNormalHints(
    Display *display,
    Window w,
    XSizeHints *hints)
{
    long data[SH_NUM];

    data[SH_FLAGS] = hints->flags;
    data[SH_X] = hints->x;
    data[SH_Y] = hints->y;
    data[SH_WIDTH] = hints->width;
    data[SH_HEIGHT] = hints->height;
    data[SH_MIN_WIDTH] = hints->min_width;
    data[SH_MIN_HEIGHT] = hints->min_height;
    data[SH_MAX_WIDTH] = hints->max_width;
    data[SH_MAX_HEIGHT] = hints->max_height;
    data[SH_WIDTH_INC] = hints->width_inc;
    data[SH_HEIGHT_INC] = hints->height_inc;
    data[SH_MIN_ASPECT_X] = hints->min_aspect.x;
    data[SH_MIN_ASPECT_Y] = hints->min_aspect.y;
    data[SH_MAX_ASPECT_X] = hints->max_aspect.x;
    data[SH_MAX_ASPECT_Y] = hints->max_aspect.y;
    data[SH_BASE_WIDTH] = hints->base_width;
    data[SH_BASE_HEIGHT] = hints->base_height;
    data[SH_WIN_GRAVITY] = hints->win_gravity;
    XChangeProperty(display, w, XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS, 32,
	    PropModeReplace, (unsigned char *)data, SH_NUM);
}

int
XSetWMHints(
    Display *display,
    Window w,
    XWMHints *hints)
{
    long data[9];

    data[0] = hints->flags;
    data[1] = hints->input;
    data[2] = hints->initial_state;
    data[3] = (long)hints->icon_pixmap;
    data[4] = (long)hints->icon_window;
    data[5] = hints->icon_x;
    data[6] = hints->icon_y;
    data[7] = (long)hints->icon_mask;
    data[8] = (long)hints->window_group;
    return XChangeProperty(display, w, XA_WM_HINTS, XA_WM_HINTS, 32,
	    PropModeReplace, (unsigned char *)data, 9);
}

int
XSetClassHint(
    Display *display,
    Window w,
    XClassHint *hints)
{
    size_t nameLen = hints->res_name ? strlen(hints->res_name) : 0;
    size_t classLen = hints->res_class ? strlen(hints->res_class) : 0;
    char *data = (char *)Tcl_Alloc(nameLen + classLen + 2);
    int result;

    memcpy(data, hints->res_name ? hints->res_name : "", nameLen + 1);
    memcpy(data + nameLen + 1, hints->res_class ? hints->res_class : "",
	    classLen + 1);
    result = XChangeProperty(display, w, XA_WM_CLASS, XA_STRING, 8,
	    PropModeReplace, (unsigned char *)data,
	    (int)(nameLen + classLen + 2));
    Tcl_Free(data);
    return result;
}

int
XSetTransientForHint(
    Display *display,
    Window w,
    Window container)
{
    long data = (long)container;

    return XChangeProperty(display, w, XA_WM_TRANSIENT_FOR, XA_WINDOW, 32,
	    PropModeReplace, (unsigned char *)&data, 1);
}

Status
XSetWMColormapWindows(
    TCL_UNUSED(Display *),
    TCL_UNUSED(Window),
    TCL_UNUSED(Window *),
    TCL_UNUSED(int))
{
    return 1;
}

Status
XReconfigureWMWindow(
    Display *display,
    Window w,
    TCL_UNUSED(int),
    unsigned int mask,
    XWindowChanges *changes)
{
    return XConfigureWindow(display, w, mask, changes) == Success;
}

Status
XIconifyWindow(
    Display *display,
    Window w,
    int screen)
{
    XEvent event;

    InitAtoms(display);
    memset(&event, 0, sizeof(XEvent));
    event.xclient.type = ClientMessage;
    event.xclient.window = w;
    event.xclient.message_type = atoms.WM_CHANGE_STATE;
    event.xclient.format = 32;
    event.xclient.data.l[0] = IconicState;
    return XSendEvent(display, RootWindow(display, screen), False,
	    SubstructureRedirectMask | SubstructureNotifyMask, &event);
}

/*
 *----------------------------------------------------------------------
 *
 * Reading the hints --
 *
 *----------------------------------------------------------------------
 */

static long *
GetLongs(
    TkWlWindow *winPtr,
    Atom property,
    unsigned long minItems)
{
    TkWlProperty *propPtr = TkWlGetProperty(winPtr, property);

    if (propPtr == NULL || propPtr->format != 32
	    || propPtr->nitems < minItems) {
	return NULL;
    }
    return (long *)(void *)propPtr->data;
}

static void
ApplyTitle(
    TkWlSurface *surfPtr)
{
    TkWlProperty *propPtr = TkWlGetProperty(surfPtr->winPtr,
	    atoms._NET_WM_NAME);

    if (propPtr == NULL || propPtr->format != 8) {
	propPtr = TkWlGetProperty(surfPtr->winPtr, XA_WM_NAME);
    }
    if (propPtr && propPtr->format == 8) {
	libdecor_frame_set_title(surfPtr->frame, (const char *)propPtr->data);
    }
}

static void
ApplyClass(
    TkWlSurface *surfPtr)
{
    TkWlProperty *propPtr = TkWlGetProperty(surfPtr->winPtr, XA_WM_CLASS);

    if (propPtr && propPtr->format == 8) {
	const char *name = (const char *)propPtr->data;
	size_t nameLen = strlen(name);

	/*
	 * The app_id is the class, which is the name of the application.
	 */

	if (nameLen + 1 < propPtr->nitems && name[nameLen + 1]) {
	    libdecor_frame_set_app_id(surfPtr->frame, name + nameLen + 1);
	} else {
	    libdecor_frame_set_app_id(surfPtr->frame, name);
	}
    }
}

static void
ApplySizeHints(
    TkWlSurface *surfPtr)
{
    long *hints = GetLongs(surfPtr->winPtr, XA_WM_NORMAL_HINTS, SH_NUM);
    int minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;

    if (hints) {
	if (hints[SH_FLAGS] & PMinSize) {
	    minWidth = (int)hints[SH_MIN_WIDTH];
	    minHeight = (int)hints[SH_MIN_HEIGHT];
	}
	if (hints[SH_FLAGS] & PMaxSize) {
	    maxWidth = (int)hints[SH_MAX_WIDTH];
	    maxHeight = (int)hints[SH_MAX_HEIGHT];
	}
    }
    libdecor_frame_set_min_content_size(surfPtr->frame, minWidth, minHeight);
    libdecor_frame_set_max_content_size(surfPtr->frame, maxWidth, maxHeight);
    if (maxWidth > 0 && maxWidth == minWidth && maxHeight == minHeight) {
	libdecor_frame_unset_capabilities(surfPtr->frame,
		LIBDECOR_ACTION_RESIZE | LIBDECOR_ACTION_FULLSCREEN);
    } else {
	libdecor_frame_set_capabilities(surfPtr->frame,
		LIBDECOR_ACTION_RESIZE | LIBDECOR_ACTION_FULLSCREEN);
    }
}

static void
ApplyTransient(
    TkWlSurface *surfPtr)
{
    long *container = GetLongs(surfPtr->winPtr, XA_WM_TRANSIENT_FOR, 1);
    TkWlSurface *parentPtr = container
	    ? TkWlGetSurface((Window)container[0]) : NULL;

    libdecor_frame_set_parent(surfPtr->frame,
	    (parentPtr && parentPtr->frame) ? parentPtr->frame : NULL);
}

/*
 * Apply the states in _NET_WM_STATE.
 */

static void
ApplyNetWmState(
    TkWlSurface *surfPtr)
{
    TkWlProperty *propPtr = TkWlGetProperty(surfPtr->winPtr,
	    atoms._NET_WM_STATE);
    bool maximized = false, fullscreen = false;
    unsigned long i;

    if (propPtr && propPtr->format == 32) {
	long *states = (long *)(void *)propPtr->data;

	for (i = 0; i < propPtr->nitems; i++) {
	    if ((Atom)states[i] == atoms._NET_WM_STATE_MAXIMIZED_VERT
		    || (Atom)states[i] == atoms._NET_WM_STATE_MAXIMIZED_HORZ) {
		maximized = true;
	    } else if ((Atom)states[i] == atoms._NET_WM_STATE_FULLSCREEN) {
		fullscreen = true;
	    }
	}
    }
    if (fullscreen) {
	libdecor_frame_set_fullscreen(surfPtr->frame, NULL);
    } else if (maximized) {
	libdecor_frame_set_maximized(surfPtr->frame);
    }
}

/*
 * Set _NET_WM_STATE to the state of the frame, as a window manager does,
 * which tells tkUnixWm.c with a PropertyNotify event.
 */

static void
SetNetWmState(
    TkWlSurface *surfPtr,
    bool maximized,
    bool fullscreen)
{
    TkWlWindow *winPtr = surfPtr->winPtr;
    long states[3];
    int n = 0;
    TkWlProperty *propPtr = TkWlGetProperty(winPtr, atoms._NET_WM_STATE);

    if (maximized) {
	states[n++] = (long)atoms._NET_WM_STATE_MAXIMIZED_VERT;
	states[n++] = (long)atoms._NET_WM_STATE_MAXIMIZED_HORZ;
    }
    if (fullscreen) {
	states[n++] = (long)atoms._NET_WM_STATE_FULLSCREEN;
    }
    if (propPtr && propPtr->format == 32 && propPtr->nitems == (unsigned)n
	    && !memcmp(propPtr->data, states, n * sizeof(long))) {
	return;
    }
    XChangeProperty(winPtr->display, winPtr->id, atoms._NET_WM_STATE,
	    XA_ATOM, 32, PropModeReplace, (unsigned char *)states, n);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlWmMapSurface --
 *
 *	Called when a child of the root window is mapped: give it the role of
 *	a decorated toplevel, unless it is override-redirect.
 *
 *----------------------------------------------------------------------
 */

void
TkWlWmMapSurface(
    TkWlSurface *surfPtr)
{
    TkWlWindow *winPtr = surfPtr->winPtr;
    long *hints;

    InitAtoms(winPtr->display);
    if (surfPtr->iconic) {
	/*
	 * Deiconified: the surface kept its role.
	 */

	surfPtr->iconic = false;
	return;
    }
    if (surfPtr->frame || surfPtr->xdgSurface) {
	return;
    }
    if (winPtr->overrideRedirect || tkWlDisplay->decor == NULL) {
	MapPopup(surfPtr);
	return;
    }
    surfPtr->configured = false;
    surfPtr->committed = false;
    surfPtr->frame = libdecor_decorate(tkWlDisplay->decor, surfPtr->surface,
	    &frameInterface, surfPtr);
    ApplyTitle(surfPtr);
    ApplyClass(surfPtr);
    ApplySizeHints(surfPtr);
    ApplyTransient(surfPtr);
    ApplyNetWmState(surfPtr);
    libdecor_frame_map(surfPtr->frame);
    hints = GetLongs(winPtr, XA_WM_HINTS, 9);
    if (hints && (hints[0] & StateHint) && hints[2] == IconicState) {
	libdecor_frame_set_minimized(surfPtr->frame);
    }
    wl_display_flush(tkWlDisplay->wlDisplay);
}

void
TkWlWmUnmapSurface(
    TkWlSurface *surfPtr)
{
    DestroyRole(surfPtr);
}

void
TkWlSurfaceSetFrame(
    TkWlSurface *surfPtr,
    struct libdecor_frame *frame)
{
    if (surfPtr->frame) {
	libdecor_frame_unref(surfPtr->frame);
    }
    surfPtr->frame = frame;
    surfPtr->iconic = false;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlWmSurfaceResized --
 *
 *	Called when Tk resizes a child of the root window: the new size of
 *	the content has to be given to libdecor.
 *
 *----------------------------------------------------------------------
 */

/*
 * Decorations can have a minimum size (the header bar of the GTK plugin of
 * libdecor), which the window cannot be smaller than.
 */

static void
ClampSize(
    TkWlSurface *surfPtr,
    int *widthPtr, int *heightPtr)
{
    int minWidth = 0, minHeight = 0;

    libdecor_frame_get_min_content_size(surfPtr->frame, &minWidth,
	    &minHeight);
    if (*widthPtr < minWidth) {
	*widthPtr = minWidth;
    }
    if (*heightPtr < minHeight) {
	*heightPtr = minHeight;
    }
}

/*
 * Resize the window as the window manager, which tells Tk with a
 * ConfigureNotify event.
 */

static void
ResizeWindow(
    TkWlSurface *surfPtr,
    int width, int height)
{
    TkWlWindow *winPtr = surfPtr->winPtr;

    if (width != winPtr->width || height != winPtr->height) {
	surfPtr->inConfigure = true;
	XResizeWindow(winPtr->display, winPtr->id, (unsigned)width,
		(unsigned)height);
	surfPtr->inConfigure = false;
    }
}

void
TkWlWmSurfaceResized(
    TkWlSurface *surfPtr)
{
    struct libdecor_state *state;
    int width = surfPtr->winPtr->width, height = surfPtr->winPtr->height;

    if (surfPtr->frame == NULL || !surfPtr->configured
	    || surfPtr->inConfigure) {
	return;
    }
    ClampSize(surfPtr, &width, &height);
    state = libdecor_state_new(width, height);
    libdecor_frame_commit(surfPtr->frame, state, NULL);
    libdecor_state_free(state);
    ResizeWindow(surfPtr, width, height);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlWmPropertyChanged --
 *
 *	Called when a property of a child of the root window changes.
 *
 *----------------------------------------------------------------------
 */

void
TkWlWmPropertyChanged(
    TkWlSurface *surfPtr,
    Atom property)
{
    if (surfPtr->frame == NULL) {
	return;
    }
    InitAtoms(surfPtr->winPtr->display);
    if (property == XA_WM_NAME || property == atoms._NET_WM_NAME) {
	ApplyTitle(surfPtr);
    } else if (property == XA_WM_CLASS) {
	ApplyClass(surfPtr);
    } else if (property == XA_WM_NORMAL_HINTS) {
	ApplySizeHints(surfPtr);
    } else if (property == XA_WM_TRANSIENT_FOR) {
	ApplyTransient(surfPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlWmClientMessage --
 *
 *	Handle a client message sent to the root window, which is for the
 *	window manager.
 *
 * Results:
 *	True if the message was for the window manager.
 *
 *----------------------------------------------------------------------
 */

bool
TkWlWmClientMessage(
    XEvent *eventPtr)
{
    TkWlWindow *winPtr;
    TkWlSurface *surfPtr;

    if (eventPtr->type != ClientMessage) {
	return false;
    }
    InitAtoms(eventPtr->xany.display);
    winPtr = TkWlGetWindow(eventPtr->xclient.window);
    if (winPtr == NULL || (surfPtr = winPtr->surface) == NULL) {
	return true;
    }
    if (eventPtr->xclient.message_type == atoms.WM_CHANGE_STATE
	    && eventPtr->xclient.data.l[0] == IconicState) {
	/*
	 * Iconify: X window managers unmap iconified windows, but Wayland
	 * surfaces stay mapped when they are minimized.
	 */

	if (surfPtr->frame && winPtr->mapped) {
	    libdecor_frame_set_minimized(surfPtr->frame);
	    surfPtr->iconic = true;
	    XUnmapWindow(winPtr->display, winPtr->id);
	}
    } else if (eventPtr->xclient.message_type == atoms._NET_WM_STATE) {
	long action = eventPtr->xclient.data.l[0];
	int i;

	for (i = 1; i <= 2; i++) {
	    Atom state = (Atom)eventPtr->xclient.data.l[i];
	    bool on = (action == 1);

	    if (surfPtr->frame == NULL) {
		break;
	    }
	    if (action == 2) {
		TkWlProperty *propPtr = TkWlGetProperty(winPtr,
			atoms._NET_WM_STATE);
		unsigned long j;

		on = true;
		for (j = 0; propPtr && j < propPtr->nitems; j++) {
		    if (((long *)(void *)propPtr->data)[j] == (long)state) {
			on = false;
		    }
		}
	    }
	    if (state == atoms._NET_WM_STATE_FULLSCREEN) {
		if (on) {
		    libdecor_frame_set_fullscreen(surfPtr->frame, NULL);
		} else {
		    libdecor_frame_unset_fullscreen(surfPtr->frame);
		}
	    } else if (state == atoms._NET_WM_STATE_MAXIMIZED_VERT
		    || state == atoms._NET_WM_STATE_MAXIMIZED_HORZ) {
		if (on) {
		    libdecor_frame_set_maximized(surfPtr->frame);
		} else {
		    libdecor_frame_unset_maximized(surfPtr->frame);
		}
	    }
	}
    }
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * Popups --
 *
 *	Override-redirect windows are positioned by Tk relative to other
 *	windows of the application, so they are popups of the surface which
 *	received the input that posted them. Their position relative to that
 *	surface is the one Tk computed, and the compositor may slide or flip
 *	them to keep them on the screen. Menus and combobox dropdowns grab
 *	the input, which makes the compositor dismiss them when the user
 *	clicks elsewhere.
 *
 *----------------------------------------------------------------------
 */

static void
XdgSurfaceConfigure(
    void *data,
    struct xdg_surface *xdgSurface,
    uint32_t serial)
{
    xdg_surface_ack_configure(xdgSurface, serial);
    TkWlSurfaceConfigured((TkWlSurface *)data);
}

static const struct xdg_surface_listener xdgSurfaceListener = {
    XdgSurfaceConfigure
};

static void
PopupConfigure(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct xdg_popup *),
    TCL_UNUSED(int32_t),
    TCL_UNUSED(int32_t),
    TCL_UNUSED(int32_t),
    TCL_UNUSED(int32_t))
{
}

/*
 * The compositor dismissed a popup, because the user clicked outside of
 * it. Tell Tk as the X server would: the click goes to the window which
 * has the grab.
 */

static void
PopupDone(
    void *data,
    TCL_UNUSED(struct xdg_popup *))
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;
    unsigned int state = tkWlDisplay->modState;

    if (!surfPtr->winPtr->mapped || TkpGetCapture() == NULL) {
	return;
    }
    Tk_UpdatePointer(NULL, -10000, -10000, state | Button1Mask);
    Tk_UpdatePointer(NULL, -10000, -10000, state);
}

static void
PopupRepositioned(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct xdg_popup *),
    TCL_UNUSED(uint32_t))
{
}

static const struct xdg_popup_listener popupListener = {
    PopupConfigure,
    PopupDone,
    PopupRepositioned
};

/*
 * Undecorated toplevels, for override-redirect windows which have no
 * surface to be a popup of.
 */

static void
ToplevelConfigure(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct xdg_toplevel *),
    TCL_UNUSED(int32_t),
    TCL_UNUSED(int32_t),
    TCL_UNUSED(struct wl_array *))
{
}

static void
ToplevelClose(
    void *data,
    TCL_UNUSED(struct xdg_toplevel *))
{
    FrameClose(NULL, data);
}

static void
ToplevelConfigureBounds(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct xdg_toplevel *),
    TCL_UNUSED(int32_t),
    TCL_UNUSED(int32_t))
{
}

static void
ToplevelWmCapabilities(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct xdg_toplevel *),
    TCL_UNUSED(struct wl_array *))
{
}

static const struct xdg_toplevel_listener toplevelListener = {
    ToplevelConfigure,
    ToplevelClose,
    ToplevelConfigureBounds,
    ToplevelWmCapabilities
};

static struct xdg_surface *
RoleXdgSurface(
    TkWlSurface *surfPtr)
{
    if (surfPtr->frame) {
	return libdecor_frame_get_xdg_surface(surfPtr->frame);
    }
    return surfPtr->xdgSurface;
}

/*
 * Whether a surface can be the parent of a popup: it must have a role, and
 * not be the popup or one of its descendants.
 */

static bool
CanBeParent(
    TkWlSurface *parentPtr,
    TkWlSurface *surfPtr)
{
    TkWlSurface *p;

    if (parentPtr == NULL || !parentPtr->winPtr->mapped
	    || !parentPtr->configured || RoleXdgSurface(parentPtr) == NULL) {
	return false;
    }
    for (p = parentPtr; p; p = p->popupParent) {
	if (p == surfPtr) {
	    return false;
	}
    }
    return true;
}

static TkWlSurface *
FindPopupParent(
    TkWlSurface *surfPtr)
{
    TkWlSurface *p;

    if (CanBeParent(tkWlDisplay->pointerSurface, surfPtr)) {
	return tkWlDisplay->pointerSurface;
    }
    if (CanBeParent(tkWlDisplay->keyboardSurface, surfPtr)) {
	return tkWlDisplay->keyboardSurface;
    }
    for (p = tkWlDisplay->surfaces; p; p = p->nextPtr) {
	if (CanBeParent(p, surfPtr)) {
	    return p;
	}
    }
    return NULL;
}

/*
 * Whether a popup grabs the input: the Tk window in it is a menu or a
 * combobox dropdown.
 */

static bool
PopupGrabs(
    TkWlWindow *winPtr)
{
    TkWlWindow *childPtr;

    for (childPtr = winPtr; childPtr; childPtr = childPtr->firstChild) {
	Tk_Window tkwin = Tk_IdToWindow(winPtr->display, childPtr->id);

	if (tkwin && !(((TkWindow *)tkwin)->flags & TK_WRAPPER)) {
	    const char *className = Tk_Class(tkwin);

	    return className && (!strcmp(className, "Menu")
		    || !strcmp(className, "ComboboxPopdown"));
	}
    }
    return false;
}

static void
MapPopup(
    TkWlSurface *surfPtr)
{
    TkWlWindow *winPtr = surfPtr->winPtr;
    TkWlSurface *parentPtr = FindPopupParent(surfPtr);
    int width = winPtr->width + 2 * winPtr->borderWidth;
    int height = winPtr->height + 2 * winPtr->borderWidth;

    if (tkWlDisplay->wmBase == NULL) {
	return;
    }
    surfPtr->configured = false;
    surfPtr->committed = false;
    surfPtr->xdgSurface = xdg_wm_base_get_xdg_surface(tkWlDisplay->wmBase,
	    surfPtr->surface);
    xdg_surface_add_listener(surfPtr->xdgSurface, &xdgSurfaceListener,
	    surfPtr);
    if (parentPtr) {
	struct xdg_positioner *positioner =
		xdg_wm_base_create_positioner(tkWlDisplay->wmBase);
	TkWlWindow *parentWinPtr = parentPtr->winPtr;
	int x = winPtr->x - parentWinPtr->x;
	int y = winPtr->y - parentWinPtr->y;
	int anchorX = x, anchorY = y;

	/*
	 * The anchor must be within the parent; the rest of the distance is
	 * an offset.
	 */

	if (anchorX < 0) {
	    anchorX = 0;
	} else if (anchorX > parentWinPtr->width - 1) {
	    anchorX = parentWinPtr->width - 1;
	}
	if (anchorY < 0) {
	    anchorY = 0;
	} else if (anchorY > parentWinPtr->height - 1) {
	    anchorY = parentWinPtr->height - 1;
	}
	xdg_positioner_set_size(positioner, width > 0 ? width : 1,
		height > 0 ? height : 1);
	xdg_positioner_set_anchor_rect(positioner, anchorX, anchorY, 1, 1);
	xdg_positioner_set_offset(positioner, x - anchorX, y - anchorY);
	xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
	xdg_positioner_set_gravity(positioner,
		XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(positioner,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
		| XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y
		| XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
		| XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
	surfPtr->xdgPopup = xdg_surface_get_popup(surfPtr->xdgSurface,
		RoleXdgSurface(parentPtr), positioner);
	xdg_positioner_destroy(positioner);
	xdg_popup_add_listener(surfPtr->xdgPopup, &popupListener, surfPtr);
	surfPtr->popupParent = parentPtr;
	if (PopupGrabs(winPtr) && tkWlDisplay->seat) {
	    xdg_popup_grab(surfPtr->xdgPopup, tkWlDisplay->seat,
		    tkWlDisplay->lastSerial);
	}
    } else {
	surfPtr->xdgToplevel = xdg_surface_get_toplevel(surfPtr->xdgSurface);
	xdg_toplevel_add_listener(surfPtr->xdgToplevel, &toplevelListener,
		surfPtr);
    }
    wl_surface_commit(surfPtr->surface);
    wl_display_flush(tkWlDisplay->wlDisplay);
}

/*
 * Destroy the role of a surface, after those of the popups attached to it.
 */

static void
DestroyRole(
    TkWlSurface *surfPtr)
{
    TkWlSurface *p;

    for (p = tkWlDisplay->surfaces; p; p = p->nextPtr) {
	if (p->popupParent == surfPtr) {
	    DestroyRole(p);
	    if (p->committed) {
		wl_surface_attach(p->surface, NULL, 0, 0);
		wl_surface_commit(p->surface);
		p->committed = false;
	    }
	    p->configured = false;
	}
    }
    if (surfPtr->xdgPopup) {
	xdg_popup_destroy(surfPtr->xdgPopup);
	surfPtr->xdgPopup = NULL;
    }
    if (surfPtr->xdgToplevel) {
	xdg_toplevel_destroy(surfPtr->xdgToplevel);
	surfPtr->xdgToplevel = NULL;
    }
    if (surfPtr->xdgSurface) {
	xdg_surface_destroy(surfPtr->xdgSurface);
	surfPtr->xdgSurface = NULL;
    }
    surfPtr->popupParent = NULL;
    TkWlSurfaceSetFrame(surfPtr, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * The callbacks of the frames --
 *
 *----------------------------------------------------------------------
 */

static void
FrameConfigure(
    struct libdecor_frame *frame,
    struct libdecor_configuration *configuration,
    void *data)
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;
    TkWlWindow *winPtr = surfPtr->winPtr;
    enum libdecor_window_state windowState = LIBDECOR_WINDOW_STATE_NONE;
    struct libdecor_state *state;
    int width, height;

    if (!libdecor_configuration_get_content_size(configuration, frame,
	    &width, &height) || width <= 0 || height <= 0) {
	width = winPtr->width;
	height = winPtr->height;
    }
    libdecor_configuration_get_window_state(configuration, &windowState);
    ClampSize(surfPtr, &width, &height);
    state = libdecor_state_new(width, height);
    libdecor_frame_commit(frame, state, configuration);
    libdecor_state_free(state);

    /*
     * Drawing the decorations can raise their minimum size.
     */

    {
	int clampedWidth = width, clampedHeight = height;

	ClampSize(surfPtr, &clampedWidth, &clampedHeight);
	if (clampedWidth != width || clampedHeight != height) {
	    width = clampedWidth;
	    height = clampedHeight;
	    state = libdecor_state_new(width, height);
	    libdecor_frame_commit(frame, state, NULL);
	    libdecor_state_free(state);
	}
    }
    ResizeWindow(surfPtr, width, height);
    SetNetWmState(surfPtr,
	    (windowState & LIBDECOR_WINDOW_STATE_MAXIMIZED) != 0,
	    (windowState & LIBDECOR_WINDOW_STATE_FULLSCREEN) != 0);
    TkWlSurfaceConfigured(surfPtr);
}

static void
FrameClose(
    TCL_UNUSED(struct libdecor_frame *),
    void *data)
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;
    TkWlWindow *winPtr = surfPtr->winPtr;
    XEvent event;

    memset(&event, 0, sizeof(XEvent));
    event.xclient.type = ClientMessage;
    event.xclient.display = winPtr->display;
    event.xclient.window = winPtr->id;
    event.xclient.message_type = atoms.WM_PROTOCOLS;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long)atoms.WM_DELETE_WINDOW;
    event.xclient.data.l[1] = (long)TkWlCurrentTime();
    TkWlQueueEvent(&event);
}

static void
FrameCommit(
    TCL_UNUSED(struct libdecor_frame *),
    void *data)
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;

    if (surfPtr->committed) {
	wl_surface_commit(surfPtr->surface);
    }
}

static void
FrameDismissPopup(
    TCL_UNUSED(struct libdecor_frame *),
    TCL_UNUSED(const char *),
    TCL_UNUSED(void *))
{
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
