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

static void		DestroyRole(TkXeSurface *surfPtr);
static void		MapToplevel(void *clientData);
static void		MapPopup(TkXeSurface *surfPtr);

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

/*
 *----------------------------------------------------------------------
 *
 * InitAtoms --
 *
 *	Intern the atoms of the properties and messages the window manager
 *	handles, the first time it needs them.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Fills the atoms structure.
 *
 *----------------------------------------------------------------------
 */

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
 * ResizeSyncDone --
 *
 *	Called when the compositor answers the sync request sent after the
 *	commit of a size the application gave a window: from then on, the
 *	configures of the compositor take that size into account.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees the callback, and clears the resizeSync field of the surface.
 *
 *----------------------------------------------------------------------
 */

static void
ResizeSyncDone(
    void *data,
    struct wl_callback *callback,
    TCL_UNUSED(uint32_t))
{
    TkXeSurface *surfPtr = (TkXeSurface *)data;

    wl_callback_destroy(callback);
    surfPtr->resizeSync = NULL;
}

static const struct wl_callback_listener resizeSyncListener = {
    ResizeSyncDone
};

/*
 *----------------------------------------------------------------------
 *
 * Reading the hints --
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * GetLongs --
 *
 *	Get the data of a property of format 32 which has at least a number of
 *	items.
 *
 * Results:
 *	The longs of the property, or NULL if it does not exist or is too
 *	short.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static long *
GetLongs(
    TkXeWindow *winPtr,
    Atom property,
    unsigned long minItems)
{
    TkXeProperty *propPtr = TkXeGetProperty(winPtr, property);

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

/*
 *----------------------------------------------------------------------
 *
 * TruncatedString --
 *
 *	Copy a string, truncated to MAX_STRING bytes on a character boundary:
 *	Wayland messages are limited to 4096 bytes, and a longer string closes
 *	the connection.
 *
 * Results:
 *	The copy, which the caller frees with Tcl_Free.
 *
 * Side effects:
 *	Allocates memory.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * ApplyTitle --
 *
 *	Set the title of the frame of a toplevel from its _NET_WM_NAME
 *	property, or else its WM_NAME property.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The title of the frame changes.
 *
 *----------------------------------------------------------------------
 */

static void
ApplyTitle(
    TkXeSurface *surfPtr)
{
    TkXeProperty *propPtr = TkXeGetProperty(surfPtr->winPtr,
	    atoms._NET_WM_NAME);

    if (propPtr == NULL || propPtr->format != 8) {
	propPtr = TkXeGetProperty(surfPtr->winPtr, XA_WM_NAME);
    }
    if (propPtr && propPtr->format == 8) {
	char *title = TruncatedString((const char *)propPtr->data,
		(Tcl_Size)strlen((const char *)propPtr->data));

	libdecor_frame_set_title(surfPtr->frame, title);
	Tcl_Free(title);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ApplyClass --
 *
 *	Set the app_id of the frame of a toplevel: the one set with
 *	::tk::wayland::appid, or else the class in its WM_CLASS property,
 *	which is the name of the application.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The app_id of the frame changes.
 *
 *----------------------------------------------------------------------
 */

static void
ApplyClass(
    TkXeSurface *surfPtr)
{
    TkXeProperty *propPtr = TkXeGetProperty(surfPtr->winPtr, XA_WM_CLASS);
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

/*
 *----------------------------------------------------------------------
 *
 * ApplySizeHints --
 *
 *	Set the minimum and maximum sizes of the frame of a toplevel from its
 *	WM_NORMAL_HINTS property. A toplevel of fixed size cannot be resized
 *	or made fullscreen by the user.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Changes the size limits and the capabilities of the frame.
 *
 *----------------------------------------------------------------------
 */

static void
ApplySizeHints(
    TkXeSurface *surfPtr)
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

    /*
     * libdecor fails for good when the minimum size is larger than the
     * maximum size, which window managers of X11 accept: the minimum size
     * wins, as in the geometry management of Tk.
     */

    if (maxWidth > 0 && maxWidth < minWidth) {
	maxWidth = minWidth;
    }
    if (maxHeight > 0 && maxHeight < minHeight) {
	maxHeight = minHeight;
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

/*
 *----------------------------------------------------------------------
 *
 * ApplyTransient --
 *
 *	Set the parent of the frame of a toplevel from its WM_TRANSIENT_FOR
 *	property.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The parent of the frame changes.
 *
 *----------------------------------------------------------------------
 */

static void
ApplyTransient(
    TkXeSurface *surfPtr)
{
    long *container = GetLongs(surfPtr->winPtr, XA_WM_TRANSIENT_FOR, 1);
    TkXeSurface *parentPtr = container
	    ? TkXeGetSurface((Window)container[0]) : NULL;
    struct xdg_toplevel *toplevel;

    if (parentPtr && parentPtr->frame) {
	libdecor_frame_set_parent(surfPtr->frame, parentPtr->frame);
	return;
    }

    /*
     * libdecor before 0.2.3 crashes when the parent is NULL: the parent is
     * unset on the xdg_toplevel itself.
     */

    toplevel = libdecor_frame_get_xdg_toplevel(surfPtr->frame);
    if (toplevel) {
	xdg_toplevel_set_parent(toplevel, NULL);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * IconBuffer --
 *
 *	Make the buffer of an icon from an image of _NET_WM_ICON, whose pixels
 *	are 0xAARRGGBB. Icons must be square, so the image is centered in a
 *	square, and its pixels are premultiplied, as Wayland expects.
 *
 * Results:
 *	The buffer, or NULL if shared memory could not be made. *dataPtr and
 *	*sizePtr are set to the memory of the buffer, which the caller unmaps.
 *
 * Side effects:
 *	Creates a buffer in shared memory.
 *
 *----------------------------------------------------------------------
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
    pool = wl_shm_create_pool(tkXeDisplay->shm, fd, (int32_t)size);
    buffer = wl_shm_pool_create_buffer(pool, 0, side, side, side * 4,
	    WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    *dataPtr = data;
    *sizePtr = size;
    return buffer;
}

/*
 *----------------------------------------------------------------------
 *
 * ApplyIcon --
 *
 *	Set the icon of a toplevel from the images in its _NET_WM_ICON
 *	property ([wm iconphoto]): each image is an array of longs, the width,
 *	the height and the pixels. Only the first image of each size is used.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sets the icon of the toplevel, or removes it if the property is gone.
 *
 *----------------------------------------------------------------------
 */

static void
ApplyIcon(
    TkXeSurface *surfPtr)
{
    TkXeProperty *propPtr = TkXeGetProperty(surfPtr->winPtr,
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

    if (tkXeDisplay->iconManager == NULL || surfPtr->frame == NULL
	    || (toplevel = libdecor_frame_get_xdg_toplevel(surfPtr->frame))
		== NULL) {
	return;
    }
    if (propPtr == NULL || propPtr->format != 32) {
	xdg_toplevel_icon_manager_v1_set_icon(tkXeDisplay->iconManager,
		toplevel, NULL);
	return;
    }
    data = (const long *)(void *)propPtr->data;
    icon = xdg_toplevel_icon_manager_v1_create_icon(tkXeDisplay->iconManager);
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
    xdg_toplevel_icon_manager_v1_set_icon(tkXeDisplay->iconManager, toplevel,
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
 *----------------------------------------------------------------------
 *
 * ApplyNetWmState --
 *
 *	Apply the states in the _NET_WM_STATE property of a toplevel when it
 *	is mapped: maximized or fullscreen.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The frame may be maximized or made fullscreen.
 *
 *----------------------------------------------------------------------
 */

static void
ApplyNetWmState(
    TkXeSurface *surfPtr)
{
    TkXeProperty *propPtr = TkXeGetProperty(surfPtr->winPtr,
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
 *----------------------------------------------------------------------
 *
 * SetNetWmState --
 *
 *	Set the _NET_WM_STATE property of a toplevel to the state of its
 *	frame, as a window manager does, which tells tkUnixWm.c with a
 *	PropertyNotify event.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Changes the property, if the state changed.
 *
 *----------------------------------------------------------------------
 */

static void
SetNetWmState(
    TkXeSurface *surfPtr,
    bool maximized,
    bool fullscreen)
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    long states[3];
    int n = 0;
    TkXeProperty *propPtr = TkXeGetProperty(winPtr, atoms._NET_WM_STATE);

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
 * MinimizeSyncDone --
 *
 *	Called when the compositor answers the sync request sent after a
 *	toplevel was minimized: from then on, the keyboard entering its
 *	surface means that the user deiconified it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees the callback, and clears the minimizeSync field of the surface.
 *
 *----------------------------------------------------------------------
 */

static void
MinimizeSyncDone(
    void *data,
    struct wl_callback *callback,
    TCL_UNUSED(uint32_t))
{
    TkXeSurface *surfPtr = (TkXeSurface *)data;

    wl_callback_destroy(callback);
    surfPtr->minimizeSync = NULL;
}

static const struct wl_callback_listener minimizeSyncListener = {
    MinimizeSyncDone
};

/*
 *----------------------------------------------------------------------
 *
 * Minimize --
 *
 *	Minimize a toplevel. Compositors (Weston) do not expect toplevels to
 *	be minimized before they are shown, so this waits for the first
 *	buffer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Asks the compositor to minimize the toplevel and unmaps its X window,
 *	or else marks the minimization pending until the first commit.
 *
 *----------------------------------------------------------------------
 */

static void
Minimize(
    TkXeSurface *surfPtr)
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
    surfPtr->minimizeSync = wl_display_sync(tkXeDisplay->wlDisplay);
    wl_callback_add_listener(surfPtr->minimizeSync, &minimizeSyncListener,
	    surfPtr);
    XUnmapWindow(surfPtr->winPtr->display, surfPtr->winPtr->id);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlWmSurfaceCommitted --
 *
 *	Called when a buffer of a toplevel was committed: do what had to wait
 *	for it (minimizing), and send a sync request if the buffer has a size
 *	the application gave the window.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May minimize the toplevel, and may replace the resizeSync callback of
 *	the surface.
 *
 *----------------------------------------------------------------------
 */

void
TkWlWmSurfaceCommitted(
    TkXeSurface *surfPtr)
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
	surfPtr->resizeSync = wl_display_sync(tkXeDisplay->wlDisplay);
	wl_callback_add_listener(surfPtr->resizeSync, &resizeSyncListener,
		surfPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWmMapSurface --
 *
 *	Called when a child of the root window is mapped: give it the role of
 *	a decorated toplevel, unless it is override-redirect.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Schedules the creation of the toplevel at idle time, makes a popup, or
 *	activates a toplevel which was iconified.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWmMapSurface(
    TkXeSurface *surfPtr)
{
    TkXeWindow *winPtr = surfPtr->winPtr;

    InitAtoms(winPtr->display);
    if (surfPtr->iconic) {
	/*
	 * Deiconified: the surface kept its role.
	 */

	surfPtr->iconic = false;
	TkXeWmActivate(surfPtr);
	return;
    }
    if (surfPtr->frame || surfPtr->xdgSurface) {
	return;
    }
    if (winPtr->overrideRedirect || tkXeDisplay->decor == NULL) {
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

/*
 *----------------------------------------------------------------------
 *
 * MapToplevel --
 *
 *	Give a surface the role of a toplevel decorated by libdecor, at idle
 *	time, and apply the hints in the properties of its window. If libdecor
 *	failed, the toplevel is undecorated.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Creates and maps the frame; may activate or minimize the toplevel.
 *
 *----------------------------------------------------------------------
 */

static void
MapToplevel(
    void *clientData)
{
    TkXeSurface *surfPtr = (TkXeSurface *)clientData;
    TkXeWindow *winPtr = surfPtr->winPtr;
    long *hints;

    surfPtr->rolePending = false;
    if (!winPtr->mapped || surfPtr->frame) {
	return;
    }
    TkWlSurfaceSetRole(surfPtr, TKWL_ROLE_TOPLEVEL);
    surfPtr->configured = false;
    surfPtr->committed = false;
    surfPtr->frame = libdecor_decorate(tkXeDisplay->decor, surfPtr->surface,
	    &frameInterface, surfPtr);
    if (surfPtr->frame == NULL) {
	/*
	 * libdecor gives no more frames after an error: the toplevel is
	 * undecorated, as without libdecor.
	 */

	MapPopup(surfPtr);
	return;
    }
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

    if (tkXeDisplay->activation && getenv("XDG_ACTIVATION_TOKEN")) {
	xdg_activation_v1_activate(tkXeDisplay->activation,
		getenv("XDG_ACTIVATION_TOKEN"), surfPtr->surface);
	unsetenv("XDG_ACTIVATION_TOKEN");
    }
    hints = GetLongs(winPtr, XA_WM_HINTS, 9);
    if (hints && (hints[0] & StateHint) && hints[2] == IconicState) {
	Minimize(surfPtr);
    }
    wl_display_flush(tkXeDisplay->wlDisplay);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlWmUnmapSurface --
 *
 *	Called when a child of the root window is unmapped: destroy the role
 *	of its surface.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The roles of the surface and of its popups are destroyed.
 *
 *----------------------------------------------------------------------
 */

void
TkWlWmUnmapSurface(
    TkXeSurface *surfPtr)
{
    DestroyRole(surfPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlSurfaceSetFrame --
 *
 *	Set the libdecor frame of a surface, releasing the one it had.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The previous frame is released; the surface is no longer iconic, and
 *	no longer has the size of a maximized or fullscreen window.
 *
 *----------------------------------------------------------------------
 */

void
TkWlSurfaceSetFrame(
    TkXeSurface *surfPtr,
    struct libdecor_frame *frame)
{
    if (surfPtr->frame) {
	libdecor_frame_unref(surfPtr->frame);
    }
    surfPtr->frame = frame;
    surfPtr->iconic = false;
    surfPtr->minimizePending = false;
    surfPtr->boundWidth = surfPtr->boundHeight = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * ClampSize --
 *
 *	Raise a size to the minimum size of the decorations (the header bar of
 *	the GTK plugin of libdecor), which the window cannot be smaller than,
 *	then lower it to the size the compositor gave a maximized or
 *	fullscreen window, which it must not exceed (the compositor ends the
 *	connection otherwise).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	*widthPtr and *heightPtr may change.
 *
 *----------------------------------------------------------------------
 */

static void
ClampSize(
    TkXeSurface *surfPtr,
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
    if (surfPtr->boundWidth > 0 && *widthPtr > surfPtr->boundWidth) {
	*widthPtr = surfPtr->boundWidth;
    }
    if (surfPtr->boundHeight > 0 && *heightPtr > surfPtr->boundHeight) {
	*heightPtr = surfPtr->boundHeight;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ResizeWindow --
 *
 *	Resize the X window of a toplevel as the window manager, which tells
 *	Tk with a ConfigureNotify event.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Resizes the window, if its size differs.
 *
 *----------------------------------------------------------------------
 */

static void
ResizeWindow(
    TkXeSurface *surfPtr,
    int width, int height)
{
    TkXeWindow *winPtr = surfPtr->winPtr;

    if (width != winPtr->width || height != winPtr->height) {
	surfPtr->inConfigure = true;
	XResizeWindow(winPtr->display, winPtr->id, (unsigned)width,
		(unsigned)height);
	surfPtr->inConfigure = false;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeWmSurfaceResized --
 *
 *	Called when Tk resizes a child of the root window: the new size of the
 *	content has to be given to libdecor.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Commits the new size of the frame, and marks the surface as resized
 *	until the compositor has seen it.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWmSurfaceResized(
    TkXeSurface *surfPtr)
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
 * TkXeWmPropertyChanged --
 *
 *	Called when a property of a child of the root window changes: apply it
 *	to its toplevel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Changes the opacity, title, app_id, size limits, parent or icon of the
 *	toplevel.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWmPropertyChanged(
    TkXeSurface *surfPtr,
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
 * TkXeWmClientMessage --
 *
 *	Handle a client message sent to the root window, which is for the
 *	window manager: WM_CHANGE_STATE (iconify), _NET_ACTIVE_WINDOW and
 *	_NET_WM_STATE.
 *
 * Results:
 *	True if the message was for the window manager.
 *
 * Side effects:
 *	May minimize or activate a toplevel, or change whether it is maximized
 *	or fullscreen.
 *
 *----------------------------------------------------------------------
 */

bool
TkXeWmClientMessage(
    XEvent *eventPtr)
{
    TkXeWindow *winPtr;
    TkXeSurface *surfPtr;

    if (eventPtr->type != ClientMessage) {
	return false;
    }
    InitAtoms(eventPtr->xany.display);
    winPtr = TkXeGetWindow(eventPtr->xclient.window);
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
	TkXeWmActivate(surfPtr);
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
		TkXeProperty *propPtr = TkXeGetProperty(winPtr,
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
 * TokenDone --
 *
 *	Called when the compositor made an activation token: activate the
 *	toplevel with it, if it is still shown.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Requests the activation, and destroys the token.
 *
 *----------------------------------------------------------------------
 */

static void
TokenDone(
    void *data,
    struct xdg_activation_token_v1 *token,
    const char *tokenString)
{
    TkXeSurface *surfPtr = TkXeGetSurface((Window)(uintptr_t)data);

    if (surfPtr && surfPtr->winPtr->mapped
	    && (surfPtr->frame || surfPtr->xdgToplevel)) {
	xdg_activation_v1_activate(tkXeDisplay->activation, tokenString,
		surfPtr->surface);
	wl_display_flush(tkXeDisplay->wlDisplay);
    }
    xdg_activation_token_v1_destroy(token);
}

static const struct xdg_activation_token_v1_listener tokenListener = {
    TokenDone
};

/*
 *----------------------------------------------------------------------
 *
 * TkXeWmActivate --
 *
 *	Ask the compositor to activate (raise and focus) a toplevel, with an
 *	activation token for the input the application last received. The
 *	compositor decides whether it does.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Requests an activation token, which TokenDone uses.
 *
 *----------------------------------------------------------------------
 */

void
TkXeWmActivate(
    TkXeSurface *surfPtr)
{
    struct xdg_activation_token_v1 *token;

    if (tkXeDisplay->activation == NULL || surfPtr == NULL
	    || surfPtr == tkXeDisplay->keyboardSurface) {
	return;
    }
    token = xdg_activation_v1_get_activation_token(tkXeDisplay->activation);
    xdg_activation_token_v1_add_listener(token, &tokenListener,
	    (void *)(uintptr_t)surfPtr->winPtr->id);
    if (tkXeDisplay->seat && tkXeDisplay->lastSerial) {
	xdg_activation_token_v1_set_serial(token, tkXeDisplay->lastSerial,
		tkXeDisplay->seat);
    }
    if (tkXeDisplay->keyboardSurface) {
	xdg_activation_token_v1_set_surface(token,
		tkXeDisplay->keyboardSurface->surface);
    }
    xdg_activation_token_v1_commit(token);
    wl_display_flush(tkXeDisplay->wlDisplay);
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

/*
 *----------------------------------------------------------------------
 *
 * XdgSurfaceConfigure --
 *
 *	Called when the compositor configures the xdg_surface of a popup or of
 *	an undecorated toplevel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Acknowledges the configure, and lets the surface be drawn.
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
    TkWlSurfaceConfigured((TkXeSurface *)data);
}

static const struct xdg_surface_listener xdgSurfaceListener = {
    XdgSurfaceConfigure
};

/*
 *----------------------------------------------------------------------
 *
 * PopupConfigure --
 *
 *	Called when the compositor positions a popup: the window keeps the
 *	position Tk computed.
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
 *----------------------------------------------------------------------
 *
 * PopupDone --
 *
 *	Called when the compositor dismissed a popup, because the user clicked
 *	outside of it. Tell Tk as the X server would: the click goes to the
 *	window which has the grab.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Generates a press and a release of button 1 outside of the windows of
 *	the application.
 *
 *----------------------------------------------------------------------
 */

static void
PopupDone(
    void *data,
    TCL_UNUSED(struct xdg_popup *))
{
    TkXeSurface *surfPtr = (TkXeSurface *)data;
    unsigned int state = tkXeDisplay->modState;

    if (!surfPtr->winPtr->mapped || TkpGetCapture() == NULL) {
	return;
    }
    Tk_UpdatePointer(NULL, -10000, -10000, state | Button1Mask);
    Tk_UpdatePointer(NULL, -10000, -10000, state);
}

static const struct xdg_popup_listener popupListener = {
    PopupConfigure,
    PopupDone,
#ifdef XDG_POPUP_REPOSITIONED_SINCE_VERSION
    NULL
#endif
};

/*
 *----------------------------------------------------------------------
 *
 * ToplevelConfigure --
 *
 *	Called when the compositor configures an undecorated toplevel, made
 *	for an override-redirect window which has no surface to be a popup of:
 *	the window keeps its size.
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
ToplevelConfigure(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct xdg_toplevel *),
    TCL_UNUSED(int32_t),
    TCL_UNUSED(int32_t),
    TCL_UNUSED(struct wl_array *))
{
}

/*
 *----------------------------------------------------------------------
 *
 * ToplevelClose --
 *
 *	Called when the user closes an undecorated toplevel: as for a
 *	decorated one, it is sent WM_DELETE_WINDOW.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Queues a WM_DELETE_WINDOW client message.
 *
 *----------------------------------------------------------------------
 */

static void
ToplevelClose(
    void *data,
    TCL_UNUSED(struct xdg_toplevel *))
{
    FrameClose(NULL, data);
}

static const struct xdg_toplevel_listener toplevelListener = {
    ToplevelConfigure,
    ToplevelClose,
#ifdef XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION
    NULL,
#endif
#ifdef XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION
    NULL
#endif
};

/*
 *----------------------------------------------------------------------
 *
 * RoleXdgSurface --
 *
 *	Get the xdg_surface of the role of a surface: that of its frame, or
 *	its own.
 *
 * Results:
 *	The xdg_surface, or NULL if the surface has no role.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static struct xdg_surface *
RoleXdgSurface(
    TkXeSurface *surfPtr)
{
    if (surfPtr->frame) {
	return libdecor_frame_get_xdg_surface(surfPtr->frame);
    }
    return surfPtr->xdgSurface;
}

/*
 *----------------------------------------------------------------------
 *
 * CanBeParent --
 *
 *	Whether a surface can be the parent of a popup: it must have a role,
 *	have been shown (Weston 15 crashes positioning popups of surfaces it
 *	has not shown yet), and not be the popup or one of its descendants.
 *
 * Results:
 *	True if the surface can be the parent of the popup.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static bool
CanBeParent(
    TkXeSurface *parentPtr,
    TkXeSurface *surfPtr)
{
    TkXeSurface *p;

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

/*
 *----------------------------------------------------------------------
 *
 * FindPopupParent --
 *
 *	Find the surface a popup is attached to: the one which received the
 *	input that posted it, or else any surface which is shown.
 *
 * Results:
 *	The parent, or NULL if there is none.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static TkXeSurface *
FindPopupParent(
    TkXeSurface *surfPtr)
{
    TkXeSurface *p;

    if (CanBeParent(tkXeDisplay->pointerSurface, surfPtr)) {
	return tkXeDisplay->pointerSurface;
    }
    if (CanBeParent(tkXeDisplay->keyboardSurface, surfPtr)) {
	return tkXeDisplay->keyboardSurface;
    }
    for (p = tkXeDisplay->surfaces; p; p = p->nextPtr) {
	if (CanBeParent(p, surfPtr)) {
	    return p;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * PopupGrabs --
 *
 *	Whether a popup grabs the input: the Tk window in it is a menu or a
 *	combobox dropdown.
 *
 * Results:
 *	True if the popup grabs the input.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static bool
PopupGrabs(
    TkXeWindow *winPtr)
{
    TkXeWindow *childPtr;

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

/*
 *----------------------------------------------------------------------
 *
 * MapPopup --
 *
 *	Give an override-redirect window the role of a popup at the position
 *	Tk computed relative to its parent, or of an undecorated toplevel if
 *	it has none. Menus and combobox dropdowns grab the input.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Creates the xdg_surface and the popup or toplevel, and commits the
 *	surface.
 *
 *----------------------------------------------------------------------
 */

static void
MapPopup(
    TkXeSurface *surfPtr)
{
    TkXeWindow *winPtr = surfPtr->winPtr;
    TkXeSurface *parentPtr = FindPopupParent(surfPtr);
    int width = winPtr->width + 2 * winPtr->borderWidth;
    int height = winPtr->height + 2 * winPtr->borderWidth;

    if (tkXeDisplay->wmBase == NULL) {
	return;
    }
    TkWlSurfaceSetRole(surfPtr,
	    parentPtr ? TKWL_ROLE_POPUP : TKWL_ROLE_TOPLEVEL);
    surfPtr->configured = false;
    surfPtr->committed = false;
    surfPtr->xdgSurface = xdg_wm_base_get_xdg_surface(tkXeDisplay->wmBase,
	    surfPtr->surface);
    xdg_surface_add_listener(surfPtr->xdgSurface, &xdgSurfaceListener,
	    surfPtr);
    if (parentPtr) {
	struct xdg_positioner *positioner =
		xdg_wm_base_create_positioner(tkXeDisplay->wmBase);
	TkXeWindow *parentWinPtr = parentPtr->winPtr;
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
	if (!tkXeDisplay->weston) {
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
	if (PopupGrabs(winPtr) && tkXeDisplay->seat) {
	    xdg_popup_grab(surfPtr->xdgPopup, tkXeDisplay->seat,
		    tkXeDisplay->lastSerial);
	}
    } else {
	surfPtr->xdgToplevel = xdg_surface_get_toplevel(surfPtr->xdgSurface);
	xdg_toplevel_add_listener(surfPtr->xdgToplevel, &toplevelListener,
		surfPtr);
    }
    wl_surface_commit(surfPtr->surface);
    wl_display_flush(tkXeDisplay->wlDisplay);
}

/*
 *----------------------------------------------------------------------
 *
 * DestroyRole --
 *
 *	Destroy the role of a surface, after those of the popups attached to
 *	it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Destroys the popups, toplevels and frames; the surfaces of the popups
 *	lose their buffers.
 *
 *----------------------------------------------------------------------
 */

static void
DestroyRole(
    TkXeSurface *surfPtr)
{
    TkXeSurface *p;

    if (surfPtr->rolePending) {
	Tcl_CancelIdleCall(MapToplevel, surfPtr);
	surfPtr->rolePending = false;
    }
    for (p = tkXeDisplay->surfaces; p; p = p->nextPtr) {
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

/*
 *----------------------------------------------------------------------
 *
 * FrameConfigure --
 *
 *	Called when the compositor configures the frame of a toplevel: give
 *	the window the size and the state the compositor asks for.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Commits the new state of the frame, sets _NET_WM_STATE, resizes the
 *	window, and lets the surface be drawn.
 *
 *----------------------------------------------------------------------
 */

static void
FrameConfigure(
    struct libdecor_frame *frame,
    struct libdecor_configuration *configuration,
    void *data)
{
    TkXeSurface *surfPtr = (TkXeSurface *)data;
    TkXeWindow *winPtr = surfPtr->winPtr;
    enum libdecor_window_state windowState = LIBDECOR_WINDOW_STATE_NONE;
    struct libdecor_state *state;
    int width, height;

    libdecor_configuration_get_window_state(configuration, &windowState);
    if (!libdecor_configuration_get_content_size(configuration, frame,
	    &width, &height) || width <= 0 || height <= 0) {
	width = height = 0;
    }
    if (windowState & (LIBDECOR_WINDOW_STATE_MAXIMIZED
	    | LIBDECOR_WINDOW_STATE_FULLSCREEN)) {
	surfPtr->boundWidth = width;
	surfPtr->boundHeight = height;
    } else {
	surfPtr->boundWidth = surfPtr->boundHeight = 0;
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

/*
 *----------------------------------------------------------------------
 *
 * FrameClose --
 *
 *	Called when the user closes a toplevel: send it WM_DELETE_WINDOW, as
 *	an X window manager does.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Queues a WM_DELETE_WINDOW client message.
 *
 *----------------------------------------------------------------------
 */

static void
FrameClose(
    TCL_UNUSED(struct libdecor_frame *),
    void *data)
{
    TkXeSurface *surfPtr = (TkXeSurface *)data;
    TkXeWindow *winPtr = surfPtr->winPtr;
    XEvent event;

    memset(&event, 0, sizeof(XEvent));
    event.xclient.type = ClientMessage;
    event.xclient.display = winPtr->display;
    event.xclient.window = winPtr->id;
    event.xclient.message_type = atoms.WM_PROTOCOLS;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long)atoms.WM_DELETE_WINDOW;
    event.xclient.data.l[1] = (long)TkXeCurrentTime();
    TkXeQueueEvent(&event);
}

/*
 *----------------------------------------------------------------------
 *
 * FrameCommit --
 *
 *	Called when libdecor changed the decorations, which need a commit of
 *	the surface.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Commits the surface, if it has a buffer.
 *
 *----------------------------------------------------------------------
 */

static void
FrameCommit(
    TCL_UNUSED(struct libdecor_frame *),
    void *data)
{
    TkXeSurface *surfPtr = (TkXeSurface *)data;

    if (surfPtr->committed) {
	wl_surface_commit(surfPtr->surface);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * FrameDismissPopup --
 *
 *	Called when libdecor dismisses the popups of a seat: Tk has none of
 *	its own there.
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
