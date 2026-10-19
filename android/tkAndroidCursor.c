/*
 * tkAndroidCursor.c --
 *
 *	Cursors on Android. Touches have no cursor, and the system shows its
 *	own pointer for mice, so cursors are only remembered, to be
 *	configured and queried.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkAndroidInt.h"

static TkCursor *	NewCursor(void);

/*
 *----------------------------------------------------------------------
 *
 * NewCursor --
 *
 *	Allocate a cursor.
 *
 * Results:
 *	The cursor.
 *
 * Side effects:
 *	Memory is allocated, freed by tkCursor.c after TkpFreeCursor.
 *
 *----------------------------------------------------------------------
 */

static TkCursor *
NewCursor(void)
{
    TkCursor *cursorPtr = (TkCursor *)Tcl_Alloc(sizeof(TkCursor));

    memset(cursorPtr, 0, sizeof(TkCursor));
    cursorPtr->cursor = (Tk_Cursor)cursorPtr;
    return cursorPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkGetCursorByName --
 *
 *	Make a cursor from a description: a name with optional colors, or
 *	files.
 *
 * Results:
 *	The cursor, or NULL with an error in the interpreter if the
 *	description is not a list of 1 to 4 elements.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

TkCursor *
TkGetCursorByName(
    Tcl_Interp *interp,		/* For errors. */
    TCL_UNUSED(Tk_Window),
    const char *string)		/* The description of the cursor. */
{
    const char **argv = NULL;
    Tcl_Size argc;

    if (Tcl_SplitList(interp, string, &argc, &argv) != TCL_OK) {
	return NULL;
    }
    Tcl_Free(argv);
    if (argc < 1 || argc > 4) {
	Tcl_SetObjResult(interp, Tcl_ObjPrintf("bad cursor spec \"%s\"",
		string));
	Tcl_SetErrorCode(interp, "TK", "VALUE", "CURSOR", (char *)NULL);
	return NULL;
    }
    return NewCursor();
}

/*
 *----------------------------------------------------------------------
 *
 * TkCreateCursorFromData, TkpFreeCursor, TkpSetCursor, TkXeUpdateCursor --
 *
 *	Cursors made of bitmaps, freeing cursors and showing them. Android
 *	shows no pointer cursor, so cursors have nothing to show.
 *
 * Results:
 *	TkCreateCursorFromData returns a new cursor; the others nothing.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

TkCursor *
TkCreateCursorFromData(
    TCL_UNUSED(Tk_Window),
    TCL_UNUSED(const char *),
    TCL_UNUSED(const char *),
    TCL_UNUSED(int),
    TCL_UNUSED(int),
    TCL_UNUSED(int),
    TCL_UNUSED(int),
    TCL_UNUSED(XColor),
    TCL_UNUSED(XColor))
{
    return NewCursor();
}

void
TkpFreeCursor(
    TCL_UNUSED(TkCursor *))
{
}

void
TkpSetCursor(
    TCL_UNUSED(Cursor))
{
}

void
TkXeUpdateCursor(void)
{
}

/*
 *----------------------------------------------------------------------
 *
 * XCreatePixmapCursor, XCreateGlyphCursor, XFreeCursor --
 *
 *	Not used by Tk on this platform.
 *
 * Results:
 *	None (the cursor), or Success.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Cursor
XCreatePixmapCursor(
    TCL_UNUSED(Display *),
    TCL_UNUSED(Pixmap),
    TCL_UNUSED(Pixmap),
    TCL_UNUSED(XColor *),
    TCL_UNUSED(XColor *),
    TCL_UNUSED(unsigned int),
    TCL_UNUSED(unsigned int))
{
    return None;
}

Cursor
XCreateGlyphCursor(
    TCL_UNUSED(Display *),
    TCL_UNUSED(Font),
    TCL_UNUSED(Font),
    TCL_UNUSED(unsigned int),
    TCL_UNUSED(unsigned int),
    TCL_UNUSED(XColor const *),
    TCL_UNUSED(XColor const *))
{
    return None;
}

int
XFreeCursor(
    Display *display,
    TCL_UNUSED(Cursor))
{
    TkXeNextRequest(display);
    return Success;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
