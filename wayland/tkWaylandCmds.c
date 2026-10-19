/*
 * tkWaylandCmds.c --
 *
 *	Commands for what Wayland has that X11 has not, in the namespace
 *	::tk::wayland (as ::tk::mac for Aqua):
 *
 *	::tk::wayland::appid window ?id?
 *		The application id of a toplevel, which compositors match with
 *		the .desktop file of the application. By default, the class of
 *		the toplevel.
 *	::tk::wayland::decorations window ?server|none?
 *		Whether the toplevel is decorated by the compositor (or by
 *		libdecor), or not at all, for applications which draw their own
 *		title bar.
 *	::tk::wayland::corners window ?radius?
 *		The radius of the rounded corners of the toplevel, when it has
 *		no decorations; square when it is maximized or fullscreen.
 *	::tk::wayland::startmove window
 *	::tk::wayland::startresize window edge
 *	::tk::wayland::windowmenu window ?x y?
 *		Let the user move or resize the toplevel, or show the menu of
 *		the compositor for it; called from a ButtonPress binding, since
 *		the compositor needs the event which started it.
 *	::tk::wayland::inhibitidle window ?boolean?
 *		Prevent the screen from blanking while the toplevel is visible.
 *	::tk::wayland::activationtoken
 *		A token which a program started by the application can use to
 *		activate its window: give it in the environment variable
 *		XDG_ACTIVATION_TOKEN.
 *	::tk::wayland::scale window
 *		The scale of the surface of the toplevel (pixels of the screen
 *		per pixel of Tk).
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkWaylandInt.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

/*
 * What these commands set for a toplevel, which is applied when its surface
 * gets a role.
 */

typedef struct {
    TkWindow *winPtr;		/* The toplevel. */
    char *appId;		/* Or NULL for the class. */
    bool undecorated;
    int cornerRadius;		/* In pixels. */
    bool inhibitIdle;
    struct zwp_idle_inhibitor_v1 *inhibitor;
    TkWlSurface *inhibitedSurface;
				/* The surface of the inhibitor. */
} ToplevelInfo;

static Tcl_HashTable toplevelTable;	/* Maps toplevels to ToplevelInfo. */
static bool initialized = false;
static struct zwp_idle_inhibit_manager_v1 *idleInhibitManager = NULL;

/*
 *----------------------------------------------------------------------
 *
 * Toplevels and their surfaces --
 *
 *----------------------------------------------------------------------
 */

static void
ToplevelDestroyed(
    void *clientData,
    XEvent *eventPtr)
{
    ToplevelInfo *infoPtr = (ToplevelInfo *)clientData;
    Tcl_HashEntry *hPtr;

    if (eventPtr->type != DestroyNotify) {
	return;
    }
    if (infoPtr->inhibitor) {
	zwp_idle_inhibitor_v1_destroy(infoPtr->inhibitor);
    }
    if (infoPtr->appId) {
	Tcl_Free(infoPtr->appId);
    }
    hPtr = Tcl_FindHashEntry(&toplevelTable, infoPtr->winPtr);
    if (hPtr) {
	Tcl_DeleteHashEntry(hPtr);
    }
    Tcl_Free(infoPtr);
}

static ToplevelInfo *
GetInfo(
    TkWindow *winPtr,
    bool create)
{
    Tcl_HashEntry *hPtr;
    ToplevelInfo *infoPtr;
    int isNew;

    if (!initialized) {
	Tcl_InitHashTable(&toplevelTable, TCL_ONE_WORD_KEYS);
	initialized = true;
    }
    if (!create) {
	hPtr = Tcl_FindHashEntry(&toplevelTable, winPtr);
	return hPtr ? (ToplevelInfo *)Tcl_GetHashValue(hPtr) : NULL;
    }
    hPtr = Tcl_CreateHashEntry(&toplevelTable, winPtr, &isNew);
    if (!isNew) {
	return (ToplevelInfo *)Tcl_GetHashValue(hPtr);
    }
    infoPtr = (ToplevelInfo *)Tcl_Alloc(sizeof(ToplevelInfo));
    memset(infoPtr, 0, sizeof(ToplevelInfo));
    infoPtr->winPtr = winPtr;
    Tcl_SetHashValue(hPtr, infoPtr);
    Tk_CreateEventHandler((Tk_Window)winPtr, StructureNotifyMask,
	    ToplevelDestroyed, infoPtr);
    return infoPtr;
}

/*
 * The surface of a toplevel: that of its wrapper, the X window of which is
 * the child of the root window which contains it.
 */

static TkWlSurface *
ToplevelSurface(
    TkWindow *winPtr)
{
    TkWlWindow *wlWinPtr;

    if (winPtr->window == None
	    || (wlWinPtr = TkWlGetWindow(winPtr->window)) == NULL
	    || (wlWinPtr = TkWlTopWindow(wlWinPtr)) == NULL) {
	return NULL;
    }
    return wlWinPtr->surface;
}

/*
 * The Tk toplevel in a surface: the X window of the surface is the wrapper of
 * the toplevel, or the toplevel itself.
 */

static TkWindow *
SurfaceToplevel(
    TkWlSurface *surfPtr)
{
    TkWlWindow *wlWinPtr;

    for (wlWinPtr = surfPtr->winPtr; wlWinPtr;
	    wlWinPtr = wlWinPtr->firstChild) {
	TkWindow *winPtr = (TkWindow *)Tk_IdToWindow(wlWinPtr->display,
		wlWinPtr->id);

	if (winPtr && (winPtr->flags & TK_TOP_LEVEL)) {
	    return winPtr;
	}
	if (winPtr && !(winPtr->flags & TK_WRAPPER)) {
	    break;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlCmdsAppId, TkWlCmdsSurfaceMapped, TkWlCmdsSurfaceUnmapped --
 *
 *	Called by tkWaylandWm.c: the app_id set for the toplevel of a surface,
 *	and the settings applied when a surface gets or loses its role.
 *
 *----------------------------------------------------------------------
 */

const char *
TkWlCmdsAppId(
    TkWlSurface *surfPtr)
{
    TkWindow *winPtr = SurfaceToplevel(surfPtr);
    ToplevelInfo *infoPtr = winPtr ? GetInfo(winPtr, false) : NULL;

    return infoPtr ? infoPtr->appId : NULL;
}

static void
UpdateInhibitor(
    ToplevelInfo *infoPtr)
{
    TkWlSurface *surfPtr = ToplevelSurface(infoPtr->winPtr);
    bool want = infoPtr->inhibitIdle && surfPtr
	    && (surfPtr->frame || surfPtr->xdgSurface);

    if (infoPtr->inhibitor
	    && (!want || infoPtr->inhibitedSurface != surfPtr)) {
	zwp_idle_inhibitor_v1_destroy(infoPtr->inhibitor);
	infoPtr->inhibitor = NULL;
	infoPtr->inhibitedSurface = NULL;
    }
    if (want && !infoPtr->inhibitor && idleInhibitManager) {
	infoPtr->inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
		idleInhibitManager, surfPtr->surface);
	infoPtr->inhibitedSurface = surfPtr;
    }
}

/*
 * Rounded corners are for windows without decorations.
 */

static void
ApplyCorners(
    ToplevelInfo *infoPtr)
{
    TkWlSurface *surfPtr = ToplevelSurface(infoPtr->winPtr);

    if (surfPtr) {
	TkWlSurfaceSetCorners(surfPtr,
		infoPtr->undecorated ? infoPtr->cornerRadius : 0,
		surfPtr->squareCorners);
    }
}

void
TkWlCmdsSurfaceMapped(
    TkWlSurface *surfPtr)
{
    TkWindow *winPtr = SurfaceToplevel(surfPtr);
    ToplevelInfo *infoPtr = winPtr ? GetInfo(winPtr, false) : NULL;

    if (infoPtr == NULL) {
	return;
    }
    if (surfPtr->frame) {
	libdecor_frame_set_visibility(surfPtr->frame, !infoPtr->undecorated);
    }
    ApplyCorners(infoPtr);
    UpdateInhibitor(infoPtr);
}

void
TkWlCmdsSurfaceUnmapped(
    TkWlSurface *surfPtr)
{
    TkWindow *winPtr = SurfaceToplevel(surfPtr);
    ToplevelInfo *infoPtr = winPtr ? GetInfo(winPtr, false) : NULL;

    if (infoPtr && infoPtr->inhibitor && infoPtr->inhibitedSurface == surfPtr) {
	zwp_idle_inhibitor_v1_destroy(infoPtr->inhibitor);
	infoPtr->inhibitor = NULL;
	infoPtr->inhibitedSurface = NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * The commands --
 *
 *----------------------------------------------------------------------
 */

static TkWindow *
GetToplevel(
    Tcl_Interp *interp,
    Tcl_Obj *pathObj)
{
    Tk_Window tkwin = Tk_NameToWindow(interp, Tcl_GetString(pathObj),
	    Tk_MainWindow(interp));

    if (tkwin == NULL) {
	return NULL;
    }
    if (!Tk_IsTopLevel(tkwin)) {
	Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		"window \"%s\" isn't a top-level window", Tk_PathName(tkwin)));
	Tcl_SetErrorCode(interp, "TK", "LOOKUP", "TOPLEVEL",
		Tk_PathName(tkwin), (char *)NULL);
	return NULL;
    }
    return (TkWindow *)tkwin;
}

/*
 * The surface of a mapped toplevel, with a role which can be moved, resized
 * and given a window menu, or NULL with an error.
 */

static TkWlSurface *
MappedSurface(
    Tcl_Interp *interp,
    TkWindow *winPtr)
{
    TkWlSurface *surfPtr = ToplevelSurface(winPtr);

    if (surfPtr == NULL || !surfPtr->configured
	    || (surfPtr->frame == NULL && surfPtr->xdgToplevel == NULL)) {
	Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		"window \"%s\" is not shown as a toplevel", winPtr->pathName));
	Tcl_SetErrorCode(interp, "TK", "WAYLAND", "NOT_SHOWN", (char *)NULL);
	return NULL;
    }
    return surfPtr;
}

static int
AppIdCmd(
    TCL_UNUSED(void *),
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    TkWindow *winPtr;
    ToplevelInfo *infoPtr;
    TkWlSurface *surfPtr;

    if (objc != 2 && objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "window ?id?");
	return TCL_ERROR;
    }
    if ((winPtr = GetToplevel(interp, objv[1])) == NULL) {
	return TCL_ERROR;
    }
    infoPtr = GetInfo(winPtr, true);
    if (objc == 3) {
	const char *id = Tcl_GetString(objv[2]);

	if (infoPtr->appId) {
	    Tcl_Free(infoPtr->appId);
	    infoPtr->appId = NULL;
	}
	if (*id) {
	    infoPtr->appId = (char *)Tcl_Alloc(strlen(id) + 1);
	    strcpy(infoPtr->appId, id);
	}
	surfPtr = ToplevelSurface(winPtr);
	if (surfPtr) {
	    TkWlWmPropertyChanged(surfPtr, XA_WM_CLASS);
	}
    }
    Tcl_SetObjResult(interp, Tcl_NewStringObj(
	    infoPtr->appId ? infoPtr->appId : Tk_Class(winPtr), -1));
    return TCL_OK;
}

static int
DecorationsCmd(
    TCL_UNUSED(void *),
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    static const char *const modes[] = {"server", "none", NULL};
    TkWindow *winPtr;
    ToplevelInfo *infoPtr;
    TkWlSurface *surfPtr;
    int mode;

    if (objc != 2 && objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "window ?server|none?");
	return TCL_ERROR;
    }
    if ((winPtr = GetToplevel(interp, objv[1])) == NULL) {
	return TCL_ERROR;
    }
    infoPtr = GetInfo(winPtr, true);
    if (objc == 3) {
	if (Tcl_GetIndexFromObj(interp, objv[2], modes, "mode", 0, &mode)
		!= TCL_OK) {
	    return TCL_ERROR;
	}
	infoPtr->undecorated = (mode == 1);
	surfPtr = ToplevelSurface(winPtr);
	if (surfPtr && surfPtr->frame) {
	    libdecor_frame_set_visibility(surfPtr->frame,
		    !infoPtr->undecorated);
	}
	ApplyCorners(infoPtr);
    }
    Tcl_SetObjResult(interp, Tcl_NewStringObj(
	    infoPtr->undecorated ? "none" : "server", -1));
    return TCL_OK;
}

static int
CornersCmd(
    TCL_UNUSED(void *),
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    TkWindow *winPtr;
    ToplevelInfo *infoPtr;
    int radius;

    if (objc != 2 && objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "window ?radius?");
	return TCL_ERROR;
    }
    if ((winPtr = GetToplevel(interp, objv[1])) == NULL) {
	return TCL_ERROR;
    }
    infoPtr = GetInfo(winPtr, true);
    if (objc == 3) {
	if (Tk_GetPixelsFromObj(interp, (Tk_Window)winPtr, objv[2], &radius)
		!= TCL_OK) {
	    return TCL_ERROR;
	}
	if (radius < 0) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "bad radius \"%s\": must be a non-negative distance",
		    Tcl_GetString(objv[2])));
	    Tcl_SetErrorCode(interp, "TK", "VALUE", "RADIUS", (char *)NULL);
	    return TCL_ERROR;
	}
	infoPtr->cornerRadius = radius;
	ApplyCorners(infoPtr);
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(infoPtr->cornerRadius));
    return TCL_OK;
}

static int
StartMoveCmd(
    TCL_UNUSED(void *),
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    TkWindow *winPtr;
    TkWlSurface *surfPtr;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "window");
	return TCL_ERROR;
    }
    if ((winPtr = GetToplevel(interp, objv[1])) == NULL
	    || (surfPtr = MappedSurface(interp, winPtr)) == NULL) {
	return TCL_ERROR;
    }
    if (tkWlDisplay->seat == NULL) {
	return TCL_OK;
    }
    if (surfPtr->frame) {
	libdecor_frame_move(surfPtr->frame, tkWlDisplay->seat,
		tkWlDisplay->lastSerial);
    } else {
	xdg_toplevel_move(surfPtr->xdgToplevel, tkWlDisplay->seat,
		tkWlDisplay->lastSerial);
    }
    wl_display_flush(tkWlDisplay->wlDisplay);
    return TCL_OK;
}

static int
StartResizeCmd(
    TCL_UNUSED(void *),
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    static const char *const edges[] = {
	"n", "s", "w", "nw", "sw", "e", "ne", "se", NULL
    };
    static const enum libdecor_resize_edge decorEdges[] = {
	LIBDECOR_RESIZE_EDGE_TOP, LIBDECOR_RESIZE_EDGE_BOTTOM,
	LIBDECOR_RESIZE_EDGE_LEFT, LIBDECOR_RESIZE_EDGE_TOP_LEFT,
	LIBDECOR_RESIZE_EDGE_BOTTOM_LEFT, LIBDECOR_RESIZE_EDGE_RIGHT,
	LIBDECOR_RESIZE_EDGE_TOP_RIGHT, LIBDECOR_RESIZE_EDGE_BOTTOM_RIGHT
    };
    static const enum xdg_toplevel_resize_edge xdgEdges[] = {
	XDG_TOPLEVEL_RESIZE_EDGE_TOP, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,
	XDG_TOPLEVEL_RESIZE_EDGE_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,
	XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,
	XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
	XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT
    };
    TkWindow *winPtr;
    TkWlSurface *surfPtr;
    int edge;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "window edge");
	return TCL_ERROR;
    }
    if ((winPtr = GetToplevel(interp, objv[1])) == NULL
	    || Tcl_GetIndexFromObj(interp, objv[2], edges, "edge", 0, &edge)
		!= TCL_OK
	    || (surfPtr = MappedSurface(interp, winPtr)) == NULL) {
	return TCL_ERROR;
    }
    if (tkWlDisplay->seat == NULL) {
	return TCL_OK;
    }
    if (surfPtr->frame) {
	libdecor_frame_resize(surfPtr->frame, tkWlDisplay->seat,
		tkWlDisplay->lastSerial, decorEdges[edge]);
    } else {
	xdg_toplevel_resize(surfPtr->xdgToplevel, tkWlDisplay->seat,
		tkWlDisplay->lastSerial, xdgEdges[edge]);
    }
    wl_display_flush(tkWlDisplay->wlDisplay);
    return TCL_OK;
}

static int
WindowMenuCmd(
    TCL_UNUSED(void *),
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    TkWindow *winPtr;
    TkWlSurface *surfPtr;
    int x = 0, y = 0;

    if (objc != 2 && objc != 4) {
	Tcl_WrongNumArgs(interp, 1, objv, "window ?x y?");
	return TCL_ERROR;
    }
    if ((winPtr = GetToplevel(interp, objv[1])) == NULL
	    || (objc == 4 && (Tcl_GetIntFromObj(interp, objv[2], &x) != TCL_OK
		|| Tcl_GetIntFromObj(interp, objv[3], &y) != TCL_OK))
	    || (surfPtr = MappedSurface(interp, winPtr)) == NULL) {
	return TCL_ERROR;
    }
    if (objc == 2 && tkWlDisplay->pointerSurface == surfPtr) {
	x = (int)tkWlDisplay->pointerX;
	y = (int)tkWlDisplay->pointerY;
    }
    if (tkWlDisplay->seat == NULL) {
	return TCL_OK;
    }
    if (surfPtr->frame) {
	libdecor_frame_show_window_menu(surfPtr->frame, tkWlDisplay->seat,
		tkWlDisplay->lastSerial, x, y);
    } else {
	xdg_toplevel_show_window_menu(surfPtr->xdgToplevel, tkWlDisplay->seat,
		tkWlDisplay->lastSerial, x, y);
    }
    wl_display_flush(tkWlDisplay->wlDisplay);
    return TCL_OK;
}

static int
InhibitIdleCmd(
    TCL_UNUSED(void *),
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    TkWindow *winPtr;
    ToplevelInfo *infoPtr;
    int inhibit;

    if (objc != 2 && objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "window ?boolean?");
	return TCL_ERROR;
    }
    if ((winPtr = GetToplevel(interp, objv[1])) == NULL) {
	return TCL_ERROR;
    }
    infoPtr = GetInfo(winPtr, true);
    if (objc == 3) {
	if (Tcl_GetBooleanFromObj(interp, objv[2], &inhibit) != TCL_OK) {
	    return TCL_ERROR;
	}
	infoPtr->inhibitIdle = inhibit;
	UpdateInhibitor(infoPtr);
	wl_display_flush(tkWlDisplay->wlDisplay);
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(infoPtr->inhibitIdle));
    return TCL_OK;
}

/*
 * ::tk::wayland::activationtoken: the token is made by the compositor, which
 * is waited for.
 */

static void
TokenDone(
    void *data,
    TCL_UNUSED(struct xdg_activation_token_v1 *),
    const char *token)
{
    Tcl_Obj **resultPtr = (Tcl_Obj **)data;

    *resultPtr = Tcl_NewStringObj(token, -1);
    Tcl_IncrRefCount(*resultPtr);
}

static const struct xdg_activation_token_v1_listener tokenListener = {
    .done = TokenDone
};

static int
ActivationTokenCmd(
    TCL_UNUSED(void *),
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    struct xdg_activation_token_v1 *token;
    Tcl_Obj *resultObj = NULL;
    int i;

    if (objc != 1) {
	Tcl_WrongNumArgs(interp, 1, objv, NULL);
	return TCL_ERROR;
    }
    if (tkWlDisplay->activation == NULL) {
	return TCL_OK;
    }
    token = xdg_activation_v1_get_activation_token(tkWlDisplay->activation);
    xdg_activation_token_v1_add_listener(token, &tokenListener, &resultObj);
    if (tkWlDisplay->seat && tkWlDisplay->lastSerial) {
	xdg_activation_token_v1_set_serial(token, tkWlDisplay->lastSerial,
		tkWlDisplay->seat);
    }
    if (tkWlDisplay->keyboardSurface) {
	xdg_activation_token_v1_set_surface(token,
		tkWlDisplay->keyboardSurface->surface);
    }
    xdg_activation_token_v1_commit(token);
    for (i = 0; i < 10 && resultObj == NULL; i++) {
	if (wl_display_roundtrip(tkWlDisplay->wlDisplay) < 0) {
	    break;
	}
    }
    xdg_activation_token_v1_destroy(token);
    if (resultObj) {
	Tcl_SetObjResult(interp, resultObj);
	Tcl_DecrRefCount(resultObj);
    }
    return TCL_OK;
}

static int
ScaleCmd(
    TCL_UNUSED(void *),
    Tcl_Interp *interp,
    Tcl_Size objc,
    Tcl_Obj *const objv[])
{
    TkWindow *winPtr;
    TkWlSurface *surfPtr;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "window");
	return TCL_ERROR;
    }
    if ((winPtr = GetToplevel(interp, objv[1])) == NULL) {
	return TCL_ERROR;
    }
    surfPtr = ToplevelSurface(winPtr);
    Tcl_SetObjResult(interp, Tcl_NewDoubleObj(surfPtr ? surfPtr->scale : 1.0));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlCmdsBind, TkWaylandCmdsInit --
 *
 *	Bind the globals the commands need, and create the commands.
 *
 *----------------------------------------------------------------------
 */

bool
TkWlCmdsBind(
    struct wl_registry *registry,
    uint32_t name,
    const char *interface)
{
    if (!strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name)) {
	idleInhibitManager = (struct zwp_idle_inhibit_manager_v1 *)
		wl_registry_bind(registry, name,
		&zwp_idle_inhibit_manager_v1_interface, 1);
	return true;
    }
    return false;
}

int
TkWaylandCmdsInit(
    Tcl_Interp *interp)
{
    static const struct {
	const char *name;
	Tcl_ObjCmdProc2 *proc;
    } commands[] = {
	{"::tk::wayland::appid", AppIdCmd},
	{"::tk::wayland::decorations", DecorationsCmd},
	{"::tk::wayland::corners", CornersCmd},
	{"::tk::wayland::startmove", StartMoveCmd},
	{"::tk::wayland::startresize", StartResizeCmd},
	{"::tk::wayland::windowmenu", WindowMenuCmd},
	{"::tk::wayland::inhibitidle", InhibitIdleCmd},
	{"::tk::wayland::activationtoken", ActivationTokenCmd},
	{"::tk::wayland::scale", ScaleCmd},
	{NULL, NULL}
    };
    int i;

    Tcl_CreateNamespace(interp, "::tk::wayland", NULL, NULL);
    for (i = 0; commands[i].name; i++) {
	Tcl_CreateObjCommand2(interp, commands[i].name, commands[i].proc,
		NULL, NULL);
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
