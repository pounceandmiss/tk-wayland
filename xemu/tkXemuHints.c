/*
 * tkXemuHints.c --
 *
 *	The Xlib functions with which Tk gives hints to the window manager.
 *	As in Xlib, they only set properties, which the window manager of
 *	the port (tkWaylandWm.c, tkAndroidWm.c) reads.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkXemuInt.h"

/*
 *----------------------------------------------------------------------
 *
 * Xlib functions which set the hints for the window manager --
 *
 *	As in Xlib, they only set properties.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * XAllocSizeHints --
 *
 *	Allocate an XSizeHints structure, as Xlib does.
 *
 * Results:
 *	The structure, cleared, which the caller frees with XFree.
 *
 * Side effects:
 *	Allocates memory.
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

/*
 *----------------------------------------------------------------------
 *
 * XAllocClassHint --
 *
 *	Allocate an XClassHint structure, as Xlib does.
 *
 * Results:
 *	The structure, cleared, which the caller frees with XFree.
 *
 * Side effects:
 *	Allocates memory.
 *
 *----------------------------------------------------------------------
 */

XClassHint *
XAllocClassHint(void)
{
    XClassHint *hints = (XClassHint *)Tcl_Alloc(sizeof(XClassHint));

    memset(hints, 0, sizeof(XClassHint));
    return hints;
}

/*
 *----------------------------------------------------------------------
 *
 * XStoreName --
 *
 *	Set the name of a window: its WM_NAME property.
 *
 * Results:
 *	The result of XChangeProperty.
 *
 * Side effects:
 *	Changes the property, which the window manager applies as the title of
 *	the toplevel.
 *
 *----------------------------------------------------------------------
 */

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
 *----------------------------------------------------------------------
 *
 * XSetWMNormalHints --
 *
 *	Set the size hints of a window: its WM_NORMAL_HINTS property, in the
 *	layout of the protocol.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Changes the property, which the window manager applies as the minimum
 *	and maximum sizes of the toplevel.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * XSetWMHints --
 *
 *	Set the WM_HINTS property of a window.
 *
 * Results:
 *	The result of XChangeProperty.
 *
 * Side effects:
 *	Changes the property, whose initial state the window manager uses when
 *	the window is mapped.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * XSetClassHint --
 *
 *	Set the name and class of a window: its WM_CLASS property, where they
 *	are separated by a null byte.
 *
 * Results:
 *	The result of XChangeProperty.
 *
 * Side effects:
 *	Changes the property, which the window manager applies as the app_id
 *	of the toplevel.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * XSetTransientForHint --
 *
 *	Set the WM_TRANSIENT_FOR property of a window.
 *
 * Results:
 *	The result of XChangeProperty.
 *
 * Side effects:
 *	Changes the property, which the window manager applies as the parent
 *	of the toplevel.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * XSetWMColormapWindows --
 *
 *	Set the colormap windows of a toplevel (its WM_COLORMAP_WINDOWS
 *	property). There is only one colormap, so the window manager has
 *	nothing to do with them.
 *
 * Results:
 *	1 (success), or 0 if the window does not exist.
 *
 * Side effects:
 *	The property is changed.
 *
 *----------------------------------------------------------------------
 */

Status
XSetWMColormapWindows(
    Display *display,
    Window w,
    Window *windows,
    int count)
{
    long *data = (long *)Tcl_Alloc((count > 0 ? count : 1) * sizeof(long));
    int i, result;

    for (i = 0; i < count; i++) {
	data[i] = (long)windows[i];
    }
    result = XChangeProperty(display, w,
	    XInternAtom(display, "WM_COLORMAP_WINDOWS", False), XA_WINDOW, 32,
	    PropModeReplace, (unsigned char *)data, count);
    Tcl_Free(data);
    return result == Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XGetWMColormapWindows --
 *
 *	Get the colormap windows of a toplevel, from its WM_COLORMAP_WINDOWS
 *	property.
 *
 * Results:
 *	1 with the windows in *windowsPtr (to be freed with XFree) and their
 *	number in *countPtr, or 0 if the window has no such property.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Status
XGetWMColormapWindows(
    Display *display,
    Window w,
    Window **windowsPtr,
    int *countPtr)
{
    Atom type;
    int format;
    unsigned long i, count, after;
    unsigned char *data = NULL;
    Window *windows;

    *windowsPtr = NULL;
    *countPtr = 0;
    if (XGetWindowProperty(display, w,
	    XInternAtom(display, "WM_COLORMAP_WINDOWS", False), 0, 0x7FFFFFFF,
	    False, XA_WINDOW, &type, &format, &count, &after, &data)
	    != Success || type != XA_WINDOW || format != 32) {
	XFree(data);
	return 0;
    }
    windows = (Window *)Tcl_Alloc((count > 0 ? count : 1) * sizeof(Window));
    for (i = 0; i < count; i++) {
	windows[i] = (Window)((long *)data)[i];
    }
    XFree(data);
    *windowsPtr = windows;
    *countPtr = (int)count;
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * XSetCommand, XSetIconName, XSetWMClientMachine --
 *
 *	Set the WM_COMMAND, WM_ICON_NAME and WM_CLIENT_MACHINE properties of
 *	a toplevel. The window manager of the port does not use them.
 *
 * Results:
 *	As XChangeProperty; XSetWMClientMachine returns nothing.
 *
 * Side effects:
 *	The property is changed.
 *
 *----------------------------------------------------------------------
 */

int
XSetCommand(
    Display *display,
    Window w,
    char **argv,
    int argc)
{
    Tcl_DString ds;
    int i, result;

    /*
     * The arguments, each with its terminating null.
     */

    Tcl_DStringInit(&ds);
    for (i = 0; i < argc; i++) {
	Tcl_DStringAppend(&ds, argv[i], -1);
	Tcl_DStringSetLength(&ds, Tcl_DStringLength(&ds) + 1);
    }
    result = XChangeProperty(display, w, XA_WM_COMMAND, XA_STRING, 8,
	    PropModeReplace, (unsigned char *)Tcl_DStringValue(&ds),
	    (int)Tcl_DStringLength(&ds));
    Tcl_DStringFree(&ds);
    return result;
}

int
XSetIconName(
    Display *display,
    Window w,
    const char *name)
{
    return XChangeProperty(display, w, XA_WM_ICON_NAME, XA_STRING, 8,
	    PropModeReplace, (const unsigned char *)name, (int)strlen(name));
}

void
XSetWMClientMachine(
    Display *display,
    Window w,
    XTextProperty *textPtr)
{
    XChangeProperty(display, w, XA_WM_CLIENT_MACHINE, textPtr->encoding,
	    textPtr->format, PropModeReplace, textPtr->value,
	    (int)textPtr->nitems);
}

/*
 *----------------------------------------------------------------------
 *
 * XReconfigureWMWindow --
 *
 *	Reconfigure a toplevel, which X window managers are asked to do.
 *	Raising a toplevel above the others ("raise") asks the window manager
 *	to activate it.
 *
 * Results:
 *	True if the window was reconfigured.
 *
 * Side effects:
 *	Configures the window, and may request its activation.
 *
 *----------------------------------------------------------------------
 */

Status
XReconfigureWMWindow(
    Display *display,
    Window w,
    TCL_UNUSED(int),
    unsigned int mask,
    XWindowChanges *changes)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);

    /*
     * Raising a toplevel above the others ("raise") asks the window manager
     * to activate it.
     */

    if (winPtr && winPtr->surface && winPtr->mapped
	    && (mask & CWStackMode) && !(mask & CWSibling)
	    && changes->stack_mode == Above) {
	TkXeWmActivate(winPtr->surface);
    }
    return XConfigureWindow(display, w, mask, changes) == Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XIconifyWindow --
 *
 *	Ask the window manager to iconify a toplevel, with a WM_CHANGE_STATE
 *	client message to the root window, as Xlib does.
 *
 * Results:
 *	The result of XSendEvent.
 *
 * Side effects:
 *	The window manager minimizes the toplevel (see TkXeWmClientMessage).
 *
 *----------------------------------------------------------------------
 */

Status
XIconifyWindow(
    Display *display,
    Window w,
    int screen)
{
    XEvent event;

    memset(&event, 0, sizeof(XEvent));
    event.xclient.type = ClientMessage;
    event.xclient.window = w;
    event.xclient.message_type = XInternAtom(display, "WM_CHANGE_STATE",
	    False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = IconicState;
    return XSendEvent(display, RootWindow(display, screen), False,
	    SubstructureRedirectMask | SubstructureNotifyMask, &event);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
