/*
 * tkHaikuCursor.c --
 *
 *	Cursors of the Haiku port. Tk's cursor names are those of the X cursor
 *	font; each is shown as the system cursor of Haiku closest to it (by
 *	way of its CSS name, from ../xemu/tkXemuMisc.c). Cursors made of
 *	bitmaps are images.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkHaikuInt.h"

typedef struct {
    TkCursor info;		/* Generic cursor information. */
    int id;			/* The BCursorID, or 0 for an image. */
    uint32_t *argb;		/* The image, or NULL. */
    int width, height;
    int xHot, yHot;
} TkHaikuCursor;

/*
 * The BCursorIDs of Haiku, by CSS name.
 */

#define ID_DEFAULT		1
#define ID_NO_CURSOR		12

static const struct {
    const char *cssName;
    int id;
} cursorIds[] = {
    {"default", 1}, {"text", 2}, {"context-menu", 3}, {"copy", 4},
    {"crosshair", 5}, {"cell", 5}, {"pointer", 6}, {"grab", 7},
    {"grabbing", 8}, {"help", 9}, {"vertical-text", 10}, {"move", 11},
    {"not-allowed", 13}, {"wait", 14}, {"progress", 14}, {"n-resize", 15},
    {"e-resize", 16}, {"s-resize", 17}, {"w-resize", 18},
    {"ne-resize", 19}, {"nw-resize", 20}, {"se-resize", 21},
    {"sw-resize", 22}, {"ns-resize", 23}, {"ew-resize", 24},
    {"nesw-resize", 25}, {"nwse-resize", 26}, {"zoom-in", 27},
    {"zoom-out", 28}, {"alias", 29},
    {NULL, 0}
};

/*
 * The cursor shown by Tk, and the BWindow it was last given to.
 */

static Cursor currentCursor = None;
static TkHaikuWindow *cursorWindow = NULL;

static TkHaikuCursor *	MakeImageCursor(const char *source,
			    const char *mask, int width, int height, int xHot,
			    int yHot, XColor fg, XColor bg);

/*
 *----------------------------------------------------------------------
 *
 * MakeImageCursor --
 *
 *	Make a cursor from bitmaps in the format of X11 bitmap files: the
 *	pixels set in the mask (all, without a mask) are drawn, with the
 *	foreground where the source is set, else with the background.
 *
 * Results:
 *	The new cursor, or NULL if it could not be made.
 *
 * Side effects:
 *	Allocates the cursor and its image.
 *
 *----------------------------------------------------------------------
 */

static TkHaikuCursor *
MakeImageCursor(
    const char *source,
    const char *mask,
    int width, int height,
    int xHot, int yHot,
    XColor fg,
    XColor bg)
{
    TkHaikuCursor *cursorPtr;
    int x, y, rowBytes = (width + 7) / 8;
    uint32_t fgPixel, bgPixel;

    if (width <= 0 || height <= 0) {
	return NULL;
    }
    cursorPtr = (TkHaikuCursor *)Tcl_Alloc(sizeof(TkHaikuCursor));
    memset(cursorPtr, 0, sizeof(TkHaikuCursor));
    cursorPtr->info.cursor = (Tk_Cursor)cursorPtr;
    cursorPtr->argb = (uint32_t *)Tcl_Alloc(sizeof(uint32_t) * width
	    * height);
    fgPixel = 0xFF000000 | (uint32_t)TkpGetPixel(&fg);
    bgPixel = 0xFF000000 | (uint32_t)TkpGetPixel(&bg);
    for (y = 0; y < height; y++) {
	for (x = 0; x < width; x++) {
	    int byte = y * rowBytes + x / 8, bit = 1 << (x % 8);
	    uint32_t pixel = 0;

	    if (mask == NULL || (mask[byte] & bit)) {
		pixel = (source[byte] & bit) ? fgPixel : bgPixel;
	    }
	    cursorPtr->argb[y * width + x] = pixel;
	}
    }
    cursorPtr->width = width;
    cursorPtr->height = height;
    cursorPtr->xHot = xHot;
    cursorPtr->yHot = yHot;
    return cursorPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TkGetCursorByName --
 *
 *	Retrieve a cursor by name: a name of the X cursor font, optionally
 *	followed by colors, which are ignored, "none", or, as on X11,
 *	"@sourceFile fg" or "@sourceFile maskFile fg bg" for a cursor made of
 *	bitmap files.
 *
 * Results:
 *	The new cursor, or NULL with an error message in interp if the name is
 *	not valid.
 *
 * Side effects:
 *	Allocates the cursor.
 *
 *----------------------------------------------------------------------
 */

TkCursor *
TkGetCursorByName(
    Tcl_Interp *interp,
    Tk_Window tkwin,
    const char *string)
{
    TkHaikuCursor *cursorPtr = NULL;
    const char **argv = NULL;
    Tcl_Size argc;
    int i, id = 0;

    if (Tcl_SplitList(interp, string, &argc, &argv) != TCL_OK) {
	return NULL;
    }
    if (argc < 1 || argc > 4) {
	goto badString;
    }
    if (argv[0][0] == '@') {
	char *source, *mask = NULL;
	int width, height, xHot, yHot, w, h, d1, d2;
	XColor fg, bg;
	const char *fgName = argv[1], *bgName = NULL;

	if (argc != 2 && argc != 4) {
	    goto badString;
	}
	if (Tcl_IsSafe(interp)) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "can't get cursor from a file in a safe interpreter", -1));
	    Tcl_SetErrorCode(interp, "TK", "SAFE", "CURSOR_FILE",
		    (char *)NULL);
	    Tcl_Free(argv);
	    return NULL;
	}
	if (argc == 4) {
	    fgName = argv[2];
	    bgName = argv[3];
	}
	if (TkParseColor(Tk_Display(tkwin), Tk_Colormap(tkwin), fgName, &fg)
		== 0 || (bgName && TkParseColor(Tk_Display(tkwin),
		Tk_Colormap(tkwin), bgName, &bg) == 0)) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "invalid color name \"%s\"", bgName ? bgName : fgName));
	    Tcl_SetErrorCode(interp, "TK", "CURSOR", "COLOR", (char *)NULL);
	    Tcl_Free(argv);
	    return NULL;
	}
	source = TkGetBitmapData(interp, NULL, argv[0] + 1, &width, &height,
		&xHot, &yHot);
	if (source == NULL) {
	    Tcl_Free(argv);
	    return NULL;
	}
	if (xHot < 0) {
	    xHot = yHot = 0;
	}
	if (argc == 4) {
	    mask = TkGetBitmapData(interp, NULL, argv[1], &w, &h, &d1, &d2);
	    if (mask && (w != width || h != height)) {
		Tcl_SetObjResult(interp, Tcl_NewStringObj(
			"source and mask bitmaps have different sizes", -1));
		Tcl_SetErrorCode(interp, "TK", "CURSOR", "SIZE_MATCH",
			(char *)NULL);
		Tcl_Free(mask);
		mask = NULL;
		Tcl_Free(source);
		Tcl_Free(argv);
		return NULL;
	    }
	} else {
	    bg = fg;
	}
	if (argc == 2 || mask) {
	    cursorPtr = MakeImageCursor(source, (argc == 4) ? mask : source,
		    width, height, xHot, yHot, fg, bg);
	}
	Tcl_Free(source);
	if (mask) {
	    Tcl_Free(mask);
	}
	Tcl_Free(argv);
	return cursorPtr ? &cursorPtr->info : NULL;
    }
    if (argc > 3) {
	goto badString;
    }
    if (!strcmp(argv[0], "none")) {
	id = ID_NO_CURSOR;
    } else {
	for (i = 0; tkXeCursorNames[i].name; i++) {
	    if (!strcmp(tkXeCursorNames[i].name, argv[0])) {
		int j;

		id = ID_DEFAULT;
		for (j = 0; cursorIds[j].cssName; j++) {
		    if (!strcmp(cursorIds[j].cssName,
			    tkXeCursorNames[i].cssName)) {
			id = cursorIds[j].id;
			break;
		    }
		}
		break;
	    }
	}
	if (id == 0) {
	    goto badString;
	}
    }
    Tcl_Free(argv);
    cursorPtr = (TkHaikuCursor *)Tcl_Alloc(sizeof(TkHaikuCursor));
    memset(cursorPtr, 0, sizeof(TkHaikuCursor));
    cursorPtr->info.cursor = (Tk_Cursor)cursorPtr;
    cursorPtr->id = id;
    return &cursorPtr->info;

  badString:
    Tcl_Free(argv);
    Tcl_SetObjResult(interp, Tcl_ObjPrintf("bad cursor spec \"%s\"",
	    string));
    Tcl_SetErrorCode(interp, "TK", "VALUE", "CURSOR", (char *)NULL);
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TkCreateCursorFromData --
 *
 *	Make a cursor from bitmap data in the format of X11 bitmap files.
 *
 * Results:
 *	The new cursor, or NULL if it could not be made.
 *
 * Side effects:
 *	Allocates the cursor and its image.
 *
 *----------------------------------------------------------------------
 */

TkCursor *
TkCreateCursorFromData(
    TCL_UNUSED(Tk_Window),
    const char *source,
    const char *mask,
    int width, int height,
    int xHot, int yHot,
    XColor fg,
    XColor bg)
{
    TkHaikuCursor *cursorPtr = MakeImageCursor(source, mask, width, height,
	    xHot, yHot, fg, bg);

    return cursorPtr ? &cursorPtr->info : NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TkpFreeCursor --
 *
 *	Free the image of a cursor (tkCursor.c frees the cursor).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory is freed.
 *
 *----------------------------------------------------------------------
 */

void
TkpFreeCursor(
    TkCursor *cursorPtr)
{
    TkHaikuCursor *hPtr = (TkHaikuCursor *)cursorPtr;

    if ((Cursor)hPtr == currentCursor) {
	currentCursor = None;
    }
    if (hPtr->argb) {
	Tcl_Free(hPtr->argb);
	hPtr->argb = NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkpSetCursor, TkXeUpdateCursor --
 *
 *	Set the cursor of the pointer, called by tkPointer.c when the pointer
 *	enters a window, and show it on the BWindow under the pointer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The cursor is shown if it changed.
 *
 *----------------------------------------------------------------------
 */

void
TkpSetCursor(
    Cursor cursor)
{
    if (cursor == currentCursor && tkXeDisplay && tkXeDisplay->pointerSurface
	    && tkXeDisplay->pointerSurface->window == cursorWindow) {
	return;
    }
    currentCursor = cursor;
    TkXeUpdateCursor();
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuForgetCursorWindow --
 *
 *	Forget a BWindow which is destroyed, if it has the cursor (a new
 *	BWindow could have its address).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The cursor is set on the next window which gets it.
 *
 *----------------------------------------------------------------------
 */

void
TkHaikuForgetCursorWindow(
    TkHaikuWindow *window)	/* The BWindow. */
{
    if (cursorWindow == window) {
	cursorWindow = NULL;
    }
}

void
TkXeUpdateCursor(void)
{
    TkHaikuCursor *cursorPtr = (TkHaikuCursor *)currentCursor;
    TkXeSurface *surfPtr = tkXeDisplay ? tkXeDisplay->pointerSurface : NULL;

    if (surfPtr == NULL || surfPtr->window == NULL) {
	return;
    }
    cursorWindow = surfPtr->window;
    if (cursorPtr == NULL) {
	TkHaikuWindowSetCursor(surfPtr->window, ID_DEFAULT, NULL, 0, 0, 0, 0);
    } else if (cursorPtr->argb) {
	TkHaikuWindowSetCursor(surfPtr->window, 0, cursorPtr->argb,
		cursorPtr->width, cursorPtr->height, cursorPtr->xHot,
		cursorPtr->yHot);
    } else {
	TkHaikuWindowSetCursor(surfPtr->window, cursorPtr->id, NULL, 0, 0,
		0, 0);
    }
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
