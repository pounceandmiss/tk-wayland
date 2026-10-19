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

#define _GNU_SOURCE		/* For memfd_create. */
#include "tkWaylandInt.h"
#include <sys/mman.h>

static void		FrameConfigure(struct libdecor_frame *frame,
			    struct libdecor_configuration *configuration,
			    void *data);
static void		FrameClose(struct libdecor_frame *frame, void *data);
static void		FrameCommit(struct libdecor_frame *frame, void *data);
static void		FrameDismissPopup(struct libdecor_frame *frame,
			    const char *seatName, void *data);

static void		DestroyRole(TkWlSurface *surfPtr);
static void		MapToplevel(void *clientData);
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
	_NET_WM_STATE_HIDDEN, _NET_ACTIVE_WINDOW, _NET_WM_WINDOW_OPACITY,
	_NET_WM_ICON;
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
    ATOM(_NET_WM_WINDOW_OPACITY);
    ATOM(_NET_WM_ICON);
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
    TkWlWindow *winPtr = TkWlGetWindow(w);

    /*
     * Raising a toplevel above the others ("raise") asks the compositor to
     * activate it.
     */

    if (winPtr && winPtr->surface && winPtr->mapped
	    && (mask & CWStackMode) && !(mask & CWSibling)
	    && changes->stack_mode == Above) {
	TkWlWmActivate(winPtr->surface);
    }
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

static void
ResizeSyncDone(
    void *data,
    struct wl_callback *callback,
    TCL_UNUSED(uint32_t))
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;

    wl_callback_destroy(callback);
    surfPtr->resizeSync = NULL;
}

static const struct wl_callback_listener resizeSyncListener = {
    .done = ResizeSyncDone
};

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

/*
 * Wayland messages are limited to 4096 bytes, and a longer string closes the
 * connection: titles and app_ids are truncated (on a character boundary).
 */

#define MAX_STRING 1024

static char *
TruncatedString(
    const char *string,
    Tcl_Size length)		/* Length in bytes, or -1. */
{
    Tcl_Size len = (length < 0) ? (Tcl_Size)strlen(string) : length;
    char *result;

    if (len > MAX_STRING) {
	len = MAX_STRING;
	while (len > 0 && (string[len] & 0xC0) == 0x80) {
	    len--;
	}
    }
    result = (char *)Tcl_Alloc(len + 1);
    memcpy(result, string, len);
    result[len] = '\0';
    return result;
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
	char *title = TruncatedString((const char *)propPtr->data,
		(Tcl_Size)strlen((const char *)propPtr->data));

	libdecor_frame_set_title(surfPtr->frame, title);
	Tcl_Free(title);
    }
}

static void
ApplyClass(
    TkWlSurface *surfPtr)
{
    TkWlProperty *propPtr = TkWlGetProperty(surfPtr->winPtr, XA_WM_CLASS);
    const char *explicitId = TkWlCmdsAppId(surfPtr);

    if (explicitId) {
	char *appId = TruncatedString(explicitId, -1);

	libdecor_frame_set_app_id(surfPtr->frame, appId);
	Tcl_Free(appId);
    } else if (propPtr && propPtr->format == 8) {
	const char *name = (const char *)propPtr->data;
	size_t nameLen = strlen(name);

	/*
	 * The app_id is the class, which is the name of the application.
	 */

	char *appId = TruncatedString(
		(nameLen + 1 < propPtr->nitems && name[nameLen + 1])
		? name + nameLen + 1 : name, -1);

	libdecor_frame_set_app_id(surfPtr->frame, appId);
	Tcl_Free(appId);
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
 * Set the icon of a toplevel from the images in _NET_WM_ICON ([wm
 * iconphoto]): each image is an array of longs, the width, the height and
 * the pixels, 0xAARRGGBB. Icons must be square, so images are centered in
 * squares, and their pixels premultiplied, as Wayland expects.
 */

static struct wl_buffer *
IconBuffer(
    const long *pixels,
    int width, int height,
    void **dataPtr,
    size_t *sizePtr)
{
    int side = (width > height) ? width : height, x, y, fd;
    int x0 = (side - width) / 2, y0 = (side - height) / 2;
    size_t size = (size_t)side * side * 4;
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    uint32_t *data;

    fd = memfd_create("tk-icon", MFD_CLOEXEC);
    if (fd < 0) {
	return NULL;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
	close(fd);
	return NULL;
    }
    data = (uint32_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    fd, 0);
    if (data == MAP_FAILED) {
	close(fd);
	return NULL;
    }
    memset(data, 0, size);
    for (y = 0; y < height; y++) {
	for (x = 0; x < width; x++) {
	    uint32_t p = (uint32_t)pixels[y * width + x];
	    uint32_t a = p >> 24;

	    data[(y + y0) * side + x + x0] = (a << 24)
		    | ((((p >> 16) & 0xFF) * a / 255) << 16)
		    | ((((p >> 8) & 0xFF) * a / 255) << 8)
		    | ((p & 0xFF) * a / 255);
	}
    }
    pool = wl_shm_create_pool(tkWlDisplay->shm, fd, (int32_t)size);
    buffer = wl_shm_pool_create_buffer(pool, 0, side, side, side * 4,
	    WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    *dataPtr = data;
    *sizePtr = size;
    return buffer;
}

static void
ApplyIcon(
    TkWlSurface *surfPtr)
{
    TkWlProperty *propPtr = TkWlGetProperty(surfPtr->winPtr,
	    atoms._NET_WM_ICON);
    struct xdg_toplevel *toplevel;
    struct xdg_toplevel_icon_v1 *icon;
    struct {
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	int side;
    } buffers[16];
    int numBuffers = 0, i;
    unsigned long n = 0;
    const long *data;

    if (tkWlDisplay->iconManager == NULL || surfPtr->frame == NULL
	    || (toplevel = libdecor_frame_get_xdg_toplevel(surfPtr->frame))
		== NULL) {
	return;
    }
    if (propPtr == NULL || propPtr->format != 32) {
	xdg_toplevel_icon_manager_v1_set_icon(tkWlDisplay->iconManager,
		toplevel, NULL);
	return;
    }
    data = (const long *)(void *)propPtr->data;
    icon = xdg_toplevel_icon_manager_v1_create_icon(tkWlDisplay->iconManager);
    while (n + 2 <= propPtr->nitems && numBuffers < 16) {
	int width = (int)data[n], height = (int)data[n + 1];
	int side = (width > height) ? width : height;
	bool duplicate = false;

	if (width <= 0 || height <= 0
		|| n + 2 + (unsigned long)width * height > propPtr->nitems) {
	    break;
	}
	for (i = 0; i < numBuffers; i++) {
	    duplicate |= (buffers[i].side == side);
	}
	if (!duplicate) {
	    buffers[numBuffers].buffer = IconBuffer(data + n + 2, width,
		    height, &buffers[numBuffers].data,
		    &buffers[numBuffers].size);
	    if (buffers[numBuffers].buffer) {
		buffers[numBuffers].side = side;
		xdg_toplevel_icon_v1_add_buffer(icon,
			buffers[numBuffers].buffer, 1);
		numBuffers++;
	    }
	}
	n += 2 + (unsigned long)width * height;
    }
    xdg_toplevel_icon_manager_v1_set_icon(tkWlDisplay->iconManager, toplevel,
	    numBuffers ? icon : NULL);

    /*
     * The toplevel keeps its icon: the icon and its buffers are no longer
     * needed.
     */

    xdg_toplevel_icon_v1_destroy(icon);
    for (i = 0; i < numBuffers; i++) {
	wl_buffer_destroy(buffers[i].buffer);
	munmap(buffers[i].data, buffers[i].size);
    }
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

static void
MinimizeSyncDone(
    void *data,
    struct wl_callback *callback,
    TCL_UNUSED(uint32_t))
{
    TkWlSurface *surfPtr = (TkWlSurface *)data;

    wl_callback_destroy(callback);
    surfPtr->minimizeSync = NULL;
}

static const struct wl_callback_listener minimizeSyncListener = {
    .done = MinimizeSyncDone
};

/*
 * Minimize a toplevel. Compositors (Weston) do not expect toplevels to be
 * minimized before they are shown, so this waits for the first buffer.
 */

static void
Minimize(
    TkWlSurface *surfPtr)
{
    if (!surfPtr->committed) {
	surfPtr->minimizePending = true;
	return;
    }

    /*
     * X window managers unmap iconified windows, but Wayland surfaces stay
     * mapped when they are minimized: the X window is unmapped, and the
     * surface keeps its role.
     */

    libdecor_frame_set_minimized(surfPtr->frame);
    surfPtr->iconic = true;
    if (surfPtr->minimizeSync) {
	wl_callback_destroy(surfPtr->minimizeSync);
    }
    surfPtr->minimizeSync = wl_display_sync(tkWlDisplay->wlDisplay);
    wl_callback_add_listener(surfPtr->minimizeSync, &minimizeSyncListener,
	    surfPtr);
    XUnmapWindow(surfPtr->winPtr->display, surfPtr->winPtr->id);
}

void
TkWlWmSurfaceCommitted(
    TkWlSurface *surfPtr)
{
    if (surfPtr->minimizePending && surfPtr->frame) {
	surfPtr->minimizePending = false;
	Minimize(surfPtr);
    }

    /*
     * The compositor has seen the size the application gave the window when
     * it answers a sync request sent after the commit.
     */

    if (surfPtr->resized) {
	surfPtr->resized = false;
	if (surfPtr->resizeSync) {
	    wl_callback_destroy(surfPtr->resizeSync);
	}
	surfPtr->resizeSync = wl_display_sync(tkWlDisplay->wlDisplay);
	wl_callback_add_listener(surfPtr->resizeSync, &resizeSyncListener,
		surfPtr);
    }
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

    InitAtoms(winPtr->display);
    if (surfPtr->iconic) {
	/*
	 * Deiconified: the surface kept its role.
	 */

	surfPtr->iconic = false;
	TkWlWmActivate(surfPtr);
	return;
    }
    if (surfPtr->frame || surfPtr->xdgSurface) {
	return;
    }
    if (winPtr->overrideRedirect || tkWlDisplay->decor == NULL) {
	MapPopup(surfPtr);
	return;
    }

    /*
     * The toplevel is created at idle time, so that windows which are
     * mapped and unmapped or destroyed at once never get one. (Weston 15
     * crashes when a toplevel is destroyed before it is configured.)
     */

    if (!surfPtr->rolePending) {
	surfPtr->rolePending = true;
	Tcl_DoWhenIdle(MapToplevel, surfPtr);
    }
}

static void
MapToplevel(
    void *clientData)
{
    TkWlSurface *surfPtr = (TkWlSurface *)clientData;
    TkWlWindow *winPtr = surfPtr->winPtr;
    long *hints;

    surfPtr->rolePending = false;
    if (!winPtr->mapped || surfPtr->frame) {
	return;
    }
    TkWlSurfaceSetRole(surfPtr, TKWL_ROLE_TOPLEVEL);
    surfPtr->configured = false;
    surfPtr->committed = false;
    surfPtr->frame = libdecor_decorate(tkWlDisplay->decor, surfPtr->surface,
	    &frameInterface, surfPtr);
    ApplyTitle(surfPtr);
    ApplyClass(surfPtr);
    ApplySizeHints(surfPtr);
    ApplyTransient(surfPtr);
    ApplyNetWmState(surfPtr);
    TkWlCmdsSurfaceMapped(surfPtr);
    libdecor_frame_map(surfPtr->frame);
    ApplyIcon(surfPtr);

    /*
     * The first window of an application started by a launcher which gave
     * it an activation token is activated with it.
     */

    if (tkWlDisplay->activation && getenv("XDG_ACTIVATION_TOKEN")) {
	xdg_activation_v1_activate(tkWlDisplay->activation,
		getenv("XDG_ACTIVATION_TOKEN"), surfPtr->surface);
	unsetenv("XDG_ACTIVATION_TOKEN");
    }
    hints = GetLongs(winPtr, XA_WM_HINTS, 9);
    if (hints && (hints[0] & StateHint) && hints[2] == IconicState) {
	Minimize(surfPtr);
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
    surfPtr->minimizePending = false;
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
    surfPtr->resized = true;
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
    InitAtoms(surfPtr->winPtr->display);
    if (property == atoms._NET_WM_WINDOW_OPACITY) {
	long *opacity = GetLongs(surfPtr->winPtr, property, 1);

	TkWlSurfaceSetOpacity(surfPtr, opacity
		? (unsigned long)opacity[0] / (double)0xFFFFFFFFul : 1.0);
	return;
    }
    if (surfPtr->frame == NULL) {
	return;
    }
    if (property == XA_WM_NAME || property == atoms._NET_WM_NAME) {
	ApplyTitle(surfPtr);
    } else if (property == XA_WM_CLASS) {
	ApplyClass(surfPtr);
    } else if (property == XA_WM_NORMAL_HINTS) {
	ApplySizeHints(surfPtr);
    } else if (property == XA_WM_TRANSIENT_FOR) {
	ApplyTransient(surfPtr);
    } else if (property == atoms._NET_WM_ICON) {
	ApplyIcon(surfPtr);
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
	    Minimize(surfPtr);
	}
    } else if (eventPtr->xclient.message_type == atoms._NET_ACTIVE_WINDOW) {
	TkWlWmActivate(surfPtr);
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
 * TkWlWmActivate --
 *
 *	Ask the compositor to activate (raise and focus) a toplevel, with an
 *	activation token for the input the application last received. The
 *	compositor decides whether it does.
 *
 *----------------------------------------------------------------------
 */

static void
TokenDone(
    void *data,
    struct xdg_activation_token_v1 *token,
    const char *tokenString)
{
    TkWlSurface *surfPtr = TkWlGetSurface((Window)(uintptr_t)data);

    if (surfPtr && surfPtr->winPtr->mapped
	    && (surfPtr->frame || surfPtr->xdgToplevel)) {
	xdg_activation_v1_activate(tkWlDisplay->activation, tokenString,
		surfPtr->surface);
	wl_display_flush(tkWlDisplay->wlDisplay);
    }
    xdg_activation_token_v1_destroy(token);
}

static const struct xdg_activation_token_v1_listener tokenListener = {
    .done = TokenDone
};

void
TkWlWmActivate(
    TkWlSurface *surfPtr)
{
    struct xdg_activation_token_v1 *token;

    if (tkWlDisplay->activation == NULL || surfPtr == NULL
	    || surfPtr == tkWlDisplay->keyboardSurface) {
	return;
    }
    token = xdg_activation_v1_get_activation_token(tkWlDisplay->activation);
    xdg_activation_token_v1_add_listener(token, &tokenListener,
	    (void *)(uintptr_t)surfPtr->winPtr->id);
    if (tkWlDisplay->seat && tkWlDisplay->lastSerial) {
	xdg_activation_token_v1_set_serial(token, tkWlDisplay->lastSerial,
		tkWlDisplay->seat);
    }
    if (tkWlDisplay->keyboardSurface) {
	xdg_activation_token_v1_set_surface(token,
		tkWlDisplay->keyboardSurface->surface);
    }
    xdg_activation_token_v1_commit(token);
    wl_display_flush(tkWlDisplay->wlDisplay);
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
    .configure = XdgSurfaceConfigure
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

static const struct xdg_popup_listener popupListener = {
    .configure = PopupConfigure,
    .popup_done = PopupDone
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

static const struct xdg_toplevel_listener toplevelListener = {
    .configure = ToplevelConfigure,
    .close = ToplevelClose
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
 * Whether a surface can be the parent of a popup: it must have a role, have
 * been shown (Weston 15 crashes positioning popups of surfaces it has not
 * shown yet), and not be the popup or one of its descendants.
 */

static bool
CanBeParent(
    TkWlSurface *parentPtr,
    TkWlSurface *surfPtr)
{
    TkWlSurface *p;

    if (parentPtr == NULL || !parentPtr->winPtr->mapped
	    || !parentPtr->presented || RoleXdgSurface(parentPtr) == NULL) {
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
    TkWlSurfaceSetRole(surfPtr,
	    parentPtr ? TKWL_ROLE_POPUP : TKWL_ROLE_TOPLEVEL);
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
	if (!tkWlDisplay->weston) {
	    xdg_positioner_set_constraint_adjustment(positioner,
		    XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
		    | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y
		    | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
		    | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
	}
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

    if (surfPtr->rolePending) {
	Tcl_CancelIdleCall(MapToplevel, surfPtr);
	surfPtr->rolePending = false;
    }
    for (p = tkWlDisplay->surfaces; p; p = p->nextPtr) {
	if (p->popupParent == surfPtr) {
	    DestroyRole(p);
	    if (p->committed) {
		wl_surface_attach(p->surface, NULL, 0, 0);
		wl_surface_commit(p->surface);
		p->committed = false;
	    }
	    if (p->frameCallback) {
		wl_callback_destroy(p->frameCallback);
		p->frameCallback = NULL;
	    }
	    p->configured = false;
	    p->presented = false;
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
    TkWlCmdsSurfaceUnmapped(surfPtr);
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

    libdecor_configuration_get_window_state(configuration, &windowState);
    if (!libdecor_configuration_get_content_size(configuration, frame,
	    &width, &height) || width <= 0 || height <= 0) {
	width = height = 0;
    }

    /*
     * A floating window keeps the size the application gave it, until the
     * compositor has seen it: compositors give the size they know in every
     * configure, and those sent before (when the window is activated, for
     * instance) would undo the resize.
     */

    if ((surfPtr->resized || surfPtr->resizeSync)
	    && !(windowState & (LIBDECOR_WINDOW_STATE_MAXIMIZED
		| LIBDECOR_WINDOW_STATE_FULLSCREEN
		| LIBDECOR_WINDOW_STATE_TILED_LEFT
		| LIBDECOR_WINDOW_STATE_TILED_RIGHT
		| LIBDECOR_WINDOW_STATE_TILED_TOP
		| LIBDECOR_WINDOW_STATE_TILED_BOTTOM))) {
	width = height = 0;
    }
    if (width == 0) {
	width = winPtr->width;
	height = winPtr->height;

	/*
	 * The compositor lets the window choose its size when it is no
	 * longer maximized or fullscreen: it gets back the size it had, as
	 * window managers of X11 do.
	 */

	if (!(windowState & (LIBDECOR_WINDOW_STATE_MAXIMIZED
		| LIBDECOR_WINDOW_STATE_FULLSCREEN))
		&& surfPtr->floatingWidth > 0) {
	    width = surfPtr->floatingWidth;
	    height = surfPtr->floatingHeight;
	}
    }
    if (windowState & (LIBDECOR_WINDOW_STATE_MAXIMIZED
	    | LIBDECOR_WINDOW_STATE_FULLSCREEN)) {
	if (surfPtr->floatingWidth == 0) {
	    surfPtr->floatingWidth = winPtr->width;
	    surfPtr->floatingHeight = winPtr->height;
	}
    } else {
	surfPtr->floatingWidth = surfPtr->floatingHeight = 0;
    }

    /*
     * Whether the compositor decorates the window is set again when the
     * decoration mode is known: libdecor ignores it before.
     */

    if (!surfPtr->configured) {
	TkWlCmdsSurfaceMapped(surfPtr);
    }
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

    /*
     * The state is set before the size, so that it is known when the
     * window gets its new size.
     */

    SetNetWmState(surfPtr,
	    (windowState & LIBDECOR_WINDOW_STATE_MAXIMIZED) != 0,
	    (windowState & LIBDECOR_WINDOW_STATE_FULLSCREEN) != 0);
    TkWlSurfaceSetCorners(surfPtr, surfPtr->cornerRadius,
	    (windowState & (LIBDECOR_WINDOW_STATE_MAXIMIZED
		| LIBDECOR_WINDOW_STATE_FULLSCREEN
		| LIBDECOR_WINDOW_STATE_TILED_LEFT
		| LIBDECOR_WINDOW_STATE_TILED_RIGHT
		| LIBDECOR_WINDOW_STATE_TILED_TOP
		| LIBDECOR_WINDOW_STATE_TILED_BOTTOM)) != 0);
    ResizeWindow(surfPtr, width, height);
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
