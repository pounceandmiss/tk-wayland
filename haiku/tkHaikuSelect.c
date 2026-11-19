/*
 * tkHaikuSelect.c --
 *
 *	The selections of the Haiku port. Selections owned by the Tk
 *	applications of this process are handled by tkSelect.c. The
 *	CLIPBOARD is shared with the other applications through the system
 *	clipboard of Haiku: when Tk claims it, its text is copied there at
 *	idle time, and when Tk asks for a clipboard it does not own, the text
 *	is read from there. Haiku tells when the clipboard changes: if
 *	another application changed it, the owner in Tk gets a SelectionClear
 *	event, as with X.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkHaikuInt.h"
#include "tkSelect.h"

static Window clipboardOwner = None;	/* The window which owns the
					 * CLIPBOARD in Tk. */
static uint32_t exportedCount = 0;	/* The count of changes of the system
					 * clipboard after Tk set it. */
static bool exportPending = false;

static int		AppendProc(void *clientData, Tcl_Interp *interp,
			    const char *portion);
static void		ExportIdle(void *clientData);
static void		ScheduleExport(void);

/*
 *----------------------------------------------------------------------
 *
 * XSetSelectionOwner --
 *
 *	Tk claims a selection, or clears it. When it claims the CLIPBOARD,
 *	its text is copied to the system clipboard at idle time, once the
 *	script has appended it; when it clears the CLIPBOARD, the system
 *	clipboard is emptied.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	An idle callback may be scheduled.
 *
 *----------------------------------------------------------------------
 */

int
XSetSelectionOwner(
    Display *display,		/* The display. */
    Atom selection,		/* The selection. */
    Window owner,		/* The new owner, or None. */
    TCL_UNUSED(Time))
{
    TkXeNextRequest(display);
    if (selection != XInternAtom(display, "CLIPBOARD", False)) {
	return Success;
    }
    if (owner == None && clipboardOwner != None) {
	/*
	 * As with X, the CLIPBOARD is empty once its owner cleared it.
	 */

	TkHaikuClipboardClear();
	exportedCount = TkHaikuClipboardCount();
    }
    clipboardOwner = owner;
    if (owner != None) {
	ScheduleExport();
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * ScheduleExport --
 *
 *	Copy the CLIPBOARD to the system clipboard at idle time, once the
 *	script has appended its text.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An idle callback may be scheduled.
 *
 *----------------------------------------------------------------------
 */

static void
ScheduleExport(void)
{
    if (!exportPending) {
	exportPending = true;
	Tcl_DoWhenIdle(ExportIdle, NULL);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkSelUpdateClipboard --
 *
 *	Called by Tk when the text of the CLIPBOARD changed, which Tk
 *	otherwise only tells by claiming the selection (XSetSelectionOwner)
 *	the first time: the system clipboard is set again, at idle time.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An idle callback may be scheduled.
 *
 *----------------------------------------------------------------------
 */

void
TkSelUpdateClipboard(
    TCL_UNUSED(TkWindow *),
    TCL_UNUSED(clipboardOption))
{
    if (clipboardOwner != None) {
	ScheduleExport();
    }
}

/*
 *----------------------------------------------------------------------
 *
 * AppendProc --
 *
 *	Append a portion of a selection to a DString.
 *
 * Results:
 *	TCL_OK.
 *
 * Side effects:
 *	The DString grows.
 *
 *----------------------------------------------------------------------
 */

static int
AppendProc(
    void *clientData,
    TCL_UNUSED(Tcl_Interp *),
    const char *portion)
{
    Tcl_DStringAppend((Tcl_DString *)clientData, portion, TCL_INDEX_NONE);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ExportIdle --
 *
 *	Copy the text of the CLIPBOARD Tk owns to the system clipboard.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The system clipboard is set.
 *
 *----------------------------------------------------------------------
 */

static void
ExportIdle(
    TCL_UNUSED(void *))
{
    TkWindow *winPtr;
    Tcl_Interp *interp = NULL;
    Tcl_InterpState state;
    Tcl_DString ds;

    exportPending = false;
    if (clipboardOwner == None || tkXeDisplay == NULL) {
	return;
    }
    winPtr = (TkWindow *)Tk_IdToWindow(tkXeDisplay->display, clipboardOwner);

    /*
     * The clipboard is owned by a window which belongs to no application:
     * the application is the one which last appended to the clipboard.
     */

    if (winPtr && winPtr->mainPtr) {
	interp = winPtr->mainPtr->interp;
    } else if (winPtr && winPtr->dispPtr->clipboardAppPtr) {
	interp = winPtr->dispPtr->clipboardAppPtr->interp;
    }
    if (interp == NULL) {
	return;
    }
    Tcl_DStringInit(&ds);
    state = Tcl_SaveInterpState(interp, TCL_OK);
    if (Tk_GetSelection(interp, (Tk_Window)winPtr,
	    Tk_InternAtom((Tk_Window)winPtr, "CLIPBOARD"),
	    Tk_InternAtom((Tk_Window)winPtr, "UTF8_STRING"), AppendProc,
	    &ds) != TCL_OK) {
	Tcl_DStringSetLength(&ds, 0);
    }
    Tcl_RestoreInterpState(interp, state);
    TkHaikuClipboardSet(Tcl_DStringValue(&ds),
	    (size_t)Tcl_DStringLength(&ds));
    exportedCount = TkHaikuClipboardCount();
    Tcl_DStringFree(&ds);
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuClipboardChanged --
 *
 *	Called when the system clipboard changed: if another application
 *	changed it, the owner of the CLIPBOARD in Tk loses it, as with X.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A SelectionClear event may be queued.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuClipboardChanged(void)
{
    XEvent event;

    if (clipboardOwner == None || exportPending
	    || TkHaikuClipboardCount() == exportedCount) {
	return;
    }
    memset(&event, 0, sizeof(XEvent));
    event.xselectionclear.type = SelectionClear;
    event.xselectionclear.display = tkXeDisplay->display;
    event.xselectionclear.window = clipboardOwner;
    event.xselectionclear.selection = XInternAtom(tkXeDisplay->display,
	    "CLIPBOARD", False);
    event.xselectionclear.time = TkXeCurrentTime();
    clipboardOwner = None;
    if (TkXeGetWindow(event.xselectionclear.window)) {
	TkXeQueueEvent(&event);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkSelGetSelection --
 *
 *	Retrieve a selection which is not owned by a Tk application of this
 *	process: only the CLIPBOARD, as text, from the system clipboard.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	The proc is called with the text.
 *
 *----------------------------------------------------------------------
 */

int
TkSelGetSelection(
    Tcl_Interp *interp,		/* For the result. */
    Tk_Window tkwin,		/* The window asking. */
    Atom selection,		/* The selection. */
    Atom target,		/* The form wanted. */
    Tk_GetSelProc *proc,	/* Called with the data. */
    void *clientData)		/* For proc. */
{
    const char *targetName = Tk_GetAtomName(tkwin, target);
    char *text;
    size_t length;
    int result;

    if (selection != Tk_InternAtom(tkwin, "CLIPBOARD")
	    || !TkHaikuClipboardGet(&text, &length)) {
	goto notDefined;
    }
    if (!strcmp(targetName, "TARGETS")) {
	free(text);
	return proc(clientData, interp, "TARGETS UTF8_STRING STRING TEXT");
    }
    if (strcmp(targetName, "UTF8_STRING") && strcmp(targetName, "STRING")
	    && strcmp(targetName, "TEXT")
	    && strcmp(targetName, "text/plain;charset=utf-8")
	    && strcmp(targetName, "text/plain")) {
	free(text);
	goto notDefined;
    }
    result = proc(clientData, interp, text);
    free(text);
    return result;

  notDefined:
    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
	    "%s selection doesn't exist or form \"%s\" not defined",
	    Tk_GetAtomName(tkwin, selection), targetName));
    Tcl_SetErrorCode(interp, "TK", "SELECTION", "EXISTS", (char *)NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TkSelEventProc, TkSelPropProc --
 *
 *	Only SelectionClear events are generated: the selection is not
 *	exchanged through windows and properties.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A SelectionClear event clears the selection of Tk.
 *
 *----------------------------------------------------------------------
 */

void
TkSelEventProc(
    Tk_Window tkwin,
    XEvent *eventPtr)
{
    if (eventPtr->type == SelectionClear) {
	TkSelClearSelection(tkwin, eventPtr);
    }
}

void
TkSelPropProc(
    TCL_UNUSED(XEvent *))
{
}

/*
 *----------------------------------------------------------------------
 *
 * TkClipCleanup --
 *
 *	Nothing to clean up: the system clipboard keeps its text.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
TkClipCleanup(
    TCL_UNUSED(TkDisplay *))
{
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
