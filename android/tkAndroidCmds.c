/*
 * tkAndroidCmds.c --
 *
 *	The commands of the Android port, in the namespace ::tk::android, as
 *	::tk::mac on Aqua and ::tk::wayland on Wayland:
 *
 *	::tk::android::scale ?scale?
 *		The number of pixels of the screen per logical pixel. Setting
 *		it makes everything larger or smaller; 0 goes back to the
 *		density of the screen.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkAndroidInt.h"

static Tcl_ObjCmdProc	ScaleObjCmd;

/*
 *----------------------------------------------------------------------
 *
 * ScaleObjCmd --
 *
 *	Implements ::tk::android::scale.
 *
 * Results:
 *	A standard Tcl result: the scale.
 *
 * Side effects:
 *	Setting the scale resizes the screen of the emulated display, and
 *	everything is drawn again.
 *
 *----------------------------------------------------------------------
 */

static int
ScaleObjCmd(
    TCL_UNUSED(void *),
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    double scale;

    if (objc > 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "?scale?");
	return TCL_ERROR;
    }
    if (tkXeDisplay == NULL) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj("no display", -1));
	return TCL_ERROR;
    }
    if (objc == 2) {
	if (Tcl_GetDoubleFromObj(interp, objv[1], &scale) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (scale < 0.0 || scale > 16.0) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "bad scale \"%s\": must be between 0 and 16",
		    Tcl_GetString(objv[1])));
	    return TCL_ERROR;
	}
	tkXeDisplay->forcedScale = scale;
	TkAndroidUpdateScale();
    }
    Tcl_SetObjResult(interp, Tcl_NewDoubleObj(tkXeDisplay->scale));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidCmdsInit --
 *
 *	Create the commands in ::tk::android.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Commands are created.
 *
 *----------------------------------------------------------------------
 */

int
TkAndroidCmdsInit(
    Tcl_Interp *interp)		/* The interpreter. */
{
    Tcl_CreateObjCommand(interp, "::tk::android::scale", ScaleObjCmd,
	    NULL, NULL);
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
