/*
 * tkHaikuScript.c --
 *
 *	The widgets of Tk for the scripting of Haiku: the requests for the
 *	property "Widget" of the application or of a window (as hey sends
 *	them: "hey app get Text of Widget .e", "hey app do Widget OK of
 *	Window Login") come as events from the glue, are done by
 *	::tk::haiku::Script (library/haiku.tcl) in the application, and
 *	answered.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkHaikuInt.h"

static TkWindow *	ScriptToplevel(Window window);

/*
 *----------------------------------------------------------------------
 *
 * ScriptToplevel --
 *
 *	The toplevel whose BWindow a request is for: that of the wrapper (or
 *	the override-redirect window) the BWindow shows.
 *
 * Results:
 *	The toplevel, or NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static TkWindow *
ScriptToplevel(
    Window window)		/* The X window of the BWindow. */
{
    Display *display = tkXeDisplay->display;
    TkWindow *winPtr = (TkWindow *)Tk_IdToWindow(display, window);
    TkWindow *topPtr = NULL;
    Window root, parent, *children = NULL;
    unsigned int i, count;

    if (winPtr == NULL || !(winPtr->flags & TK_WRAPPER)) {
	return winPtr;
    }

    /*
     * The wrapper is not in the tree of Tk, but the toplevel is one of its
     * X children, and shares its information of the window manager (the
     * menubar does not).
     */

    if (!XQueryTree(display, window, &root, &parent, &children, &count)) {
	return NULL;
    }
    for (i = 0; i < count && topPtr == NULL; i++) {
	TkWindow *childPtr = (TkWindow *)Tk_IdToWindow(display,
		children[i]);

	if (childPtr && childPtr->wmInfoPtr == winPtr->wmInfoPtr) {
	    topPtr = childPtr;
	}
    }
    if (children) {
	XFree(children);
    }
    return topPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuHandleScript --
 *
 *	Do a scripting request, and answer it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Whatever the request does (a button may be invoked); the reply is
 *	sent.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuHandleScript(
    TkHaikuEvent *eventPtr)	/* A TKHAIKU_SCRIPT event. */
{
    TkHaikuScript script;
    TkWindow *topPtr = NULL;
    Tcl_Interp *interp = NULL;
    Tcl_InterpState state;
    Tcl_Obj *objv[7], *resultPtr;
    Tcl_Obj **elems;
    Tcl_Size numElems;
    int i, code;

    if (!TkHaikuGetScript(eventPtr->u.script, &script)) {
	TkHaikuScriptReply(eventPtr->u.script, &script, "not a request",
		NULL, NULL);
	return;
    }
    if (eventPtr->window) {
	topPtr = ScriptToplevel((Window)eventPtr->window);
	if (topPtr && topPtr->mainPtr) {
	    interp = topPtr->mainPtr->interp;
	}
    } else if (TkGetMainInfoList()) {
	interp = TkGetMainInfoList()->interp;
    }
    if (interp == NULL) {
	TkHaikuScriptReply(eventPtr->u.script, &script, "no application",
		NULL, NULL);
	return;
    }

    objv[0] = Tcl_NewStringObj("::tk::haiku::Script", TCL_INDEX_NONE);
    objv[1] = Tcl_NewStringObj(script.verb, TCL_INDEX_NONE);
    objv[2] = Tcl_NewStringObj(script.property, TCL_INDEX_NONE);
    objv[3] = Tcl_NewStringObj(topPtr ? topPtr->pathName : "",
	    TCL_INDEX_NONE);
    objv[4] = Tcl_NewStringObj(script.form, TCL_INDEX_NONE);
    objv[5] = Tcl_NewStringObj(script.widget, TCL_INDEX_NONE);
    objv[6] = Tcl_NewStringObj(script.value, TCL_INDEX_NONE);
    for (i = 0; i < 7; i++) {
	Tcl_IncrRefCount(objv[i]);
    }
    Tcl_Preserve(interp);
    state = Tcl_SaveInterpState(interp, TCL_OK);
    code = Tcl_EvalObjv(interp, 7, objv, TCL_EVAL_GLOBAL);
    resultPtr = Tcl_GetObjResult(interp);
    Tcl_IncrRefCount(resultPtr);
    Tcl_RestoreInterpState(interp, state);
    Tcl_Release(interp);
    for (i = 0; i < 7; i++) {
	Tcl_DecrRefCount(objv[i]);
    }

    if (code != TCL_OK) {
	TkHaikuScriptReply(eventPtr->u.script, &script,
		Tcl_GetString(resultPtr), NULL, NULL);
    } else if (Tcl_ListObjGetElements(NULL, resultPtr, &numElems,
	    &elems) != TCL_OK || numElems != 2) {
	TkHaikuScriptReply(eventPtr->u.script, &script, "bad result", NULL,
		NULL);
    } else {
	TkHaikuScriptReply(eventPtr->u.script, &script, NULL,
		Tcl_GetString(elems[0]), Tcl_GetString(elems[1]));
    }
    Tcl_DecrRefCount(resultPtr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
