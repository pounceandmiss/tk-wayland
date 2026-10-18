/*
 * tkWaylandCursor.c --
 *
 *	Cursors of the Wayland port. Tk's cursor names are those of the X
 *	cursor font, which cursor themes provide, together with their CSS
 *	names, which are tried when a theme lacks the X name.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkWaylandInt.h"

typedef struct {
    TkCursor info;		/* Generic cursor info used by tkCursor.c */
    const char *name;		/* X cursor font name, or NULL for no
				 * cursor. */
} TkWlCursor;

/*
 * The cursors of the X cursor font, with CSS names to use if the theme does
 * not have the X name.
 */

static const struct CursorName {
    const char *name;
    const char *cssName;
} cursorNames[] = {
    {"X_cursor",		"not-allowed"},
    {"arrow",			"default"},
    {"based_arrow_down",	"s-resize"},
    {"based_arrow_up",		"n-resize"},
    {"boat",			"default"},
    {"bogosity",		"not-allowed"},
    {"bottom_left_corner",	"sw-resize"},
    {"bottom_right_corner",	"se-resize"},
    {"bottom_side",		"s-resize"},
    {"bottom_tee",		"s-resize"},
    {"box_spiral",		"default"},
    {"center_ptr",		"default"},
    {"circle",			"not-allowed"},
    {"clock",			"wait"},
    {"coffee_mug",		"wait"},
    {"cross",			"crosshair"},
    {"cross_reverse",		"crosshair"},
    {"crosshair",		"crosshair"},
    {"diamond_cross",		"crosshair"},
    {"dot",			"default"},
    {"dotbox",			"default"},
    {"double_arrow",		"ns-resize"},
    {"draft_large",		"default"},
    {"draft_small",		"default"},
    {"draped_box",		"default"},
    {"exchange",		"default"},
    {"fleur",			"move"},
    {"gobbler",			"default"},
    {"gumby",			"default"},
    {"hand1",			"pointer"},
    {"hand2",			"pointer"},
    {"heart",			"default"},
    {"icon",			"default"},
    {"iron_cross",		"crosshair"},
    {"left_ptr",		"default"},
    {"left_side",		"w-resize"},
    {"left_tee",		"w-resize"},
    {"leftbutton",		"default"},
    {"ll_angle",		"sw-resize"},
    {"lr_angle",		"se-resize"},
    {"man",			"default"},
    {"middlebutton",		"default"},
    {"mouse",			"default"},
    {"pencil",			"default"},
    {"pirate",			"not-allowed"},
    {"plus",			"cell"},
    {"question_arrow",		"help"},
    {"right_ptr",		"default"},
    {"right_side",		"e-resize"},
    {"right_tee",		"e-resize"},
    {"rightbutton",		"default"},
    {"rtl_logo",		"default"},
    {"sailboat",		"default"},
    {"sb_down_arrow",		"s-resize"},
    {"sb_h_double_arrow",	"ew-resize"},
    {"sb_left_arrow",		"w-resize"},
    {"sb_right_arrow",		"e-resize"},
    {"sb_up_arrow",		"n-resize"},
    {"sb_v_double_arrow",	"ns-resize"},
    {"shuttle",			"default"},
    {"sizing",			"nwse-resize"},
    {"spider",			"default"},
    {"spraycan",		"default"},
    {"star",			"default"},
    {"target",			"crosshair"},
    {"tcross",			"crosshair"},
    {"top_left_arrow",		"default"},
    {"top_left_corner",		"nw-resize"},
    {"top_right_corner",	"ne-resize"},
    {"top_side",		"n-resize"},
    {"top_tee",			"n-resize"},
    {"trek",			"default"},
    {"ul_angle",		"nw-resize"},
    {"umbrella",		"default"},
    {"ur_angle",		"ne-resize"},
    {"watch",			"wait"},
    {"xterm",			"text"},
    {NULL,			NULL}
};

static Cursor currentCursor = None;

/*
 *----------------------------------------------------------------------
 *
 * TkGetCursorByName --
 *
 *	Retrieve a cursor by name. Accepts the names of the X cursor font,
 *	optionally followed by colors, which are ignored, and "none".
 *
 *----------------------------------------------------------------------
 */

TkCursor *
TkGetCursorByName(
    Tcl_Interp *interp,
    TCL_UNUSED(Tk_Window),
    const char *string)
{
    TkWlCursor *cursorPtr;
    const char **argv = NULL;
    const char *name = NULL;
    Tcl_Size argc;
    int i;

    if (Tcl_SplitList(interp, string, &argc, &argv) != TCL_OK) {
	return NULL;
    }
    if (argc < 1 || argc > 3) {
	goto badString;
    }
    if (strcmp(argv[0], "none")) {
	for (i = 0; cursorNames[i].name; i++) {
	    if (!strcmp(cursorNames[i].name, argv[0])) {
		name = cursorNames[i].name;
		break;
	    }
	}
	if (name == NULL) {
	    goto badString;
	}
    }
    Tcl_Free(argv);
    cursorPtr = (TkWlCursor *)Tcl_Alloc(sizeof(TkWlCursor));
    cursorPtr->info.cursor = (Tk_Cursor)cursorPtr;
    cursorPtr->name = name;
    return &cursorPtr->info;

  badString:
    Tcl_Free(argv);
    Tcl_SetObjResult(interp, Tcl_ObjPrintf("bad cursor spec \"%s\"",
	    string));
    Tcl_SetErrorCode(interp, "TK", "VALUE", "CURSOR", (char *)NULL);
    return NULL;
}

/*
 * Cursors made of bitmaps cannot be shown yet; the default cursor is used.
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
    TkWlCursor *cursorPtr = (TkWlCursor *)Tcl_Alloc(sizeof(TkWlCursor));

    cursorPtr->info.cursor = (Tk_Cursor)cursorPtr;
    cursorPtr->name = "left_ptr";
    return &cursorPtr->info;
}

void
TkpFreeCursor(
    TkCursor *cursorPtr)
{
    if ((Cursor)cursorPtr->cursor == currentCursor) {
	currentCursor = None;
    }
}

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
    TkWlNextRequest(display);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * TkpSetCursor --
 *
 *	Set the cursor of the pointer. Called by tkPointer.c when the pointer
 *	enters a window with another cursor; None means the default cursor.
 *
 *----------------------------------------------------------------------
 */

static struct wl_cursor *
LoadCursor(
    const char *name,
    int scale)
{
    static int themeScale = 0;
    int i;
    struct wl_cursor *cursor;

    if (tkWlDisplay->cursorTheme == NULL || themeScale != scale) {
	const char *sizeString = getenv("XCURSOR_SIZE");
	int size = sizeString ? atoi(sizeString) : 0;

	if (size <= 0) {
	    size = 24;
	}
	if (tkWlDisplay->cursorTheme) {
	    wl_cursor_theme_destroy(tkWlDisplay->cursorTheme);
	}
	tkWlDisplay->cursorTheme = wl_cursor_theme_load(
		getenv("XCURSOR_THEME"), size * scale, tkWlDisplay->shm);
	themeScale = scale;
	if (tkWlDisplay->cursorTheme == NULL) {
	    return NULL;
	}
    }
    cursor = wl_cursor_theme_get_cursor(tkWlDisplay->cursorTheme, name);
    if (cursor) {
	return cursor;
    }
    for (i = 0; cursorNames[i].name; i++) {
	if (!strcmp(cursorNames[i].name, name)) {
	    cursor = wl_cursor_theme_get_cursor(tkWlDisplay->cursorTheme,
		    cursorNames[i].cssName);
	    break;
	}
    }
    if (cursor == NULL) {
	cursor = wl_cursor_theme_get_cursor(tkWlDisplay->cursorTheme,
		"left_ptr");
    }
    if (cursor == NULL) {
	cursor = wl_cursor_theme_get_cursor(tkWlDisplay->cursorTheme,
		"default");
    }
    return cursor;
}

void
TkWlUpdateCursor(void)
{
    TkWlCursor *cursorPtr = (TkWlCursor *)currentCursor;
    struct wl_cursor *cursor;
    struct wl_cursor_image *image;
    struct wl_buffer *buffer;
    int scale;

    if (tkWlDisplay == NULL || tkWlDisplay->pointer == NULL
	    || tkWlDisplay->pointerSurface == NULL) {
	return;
    }
    if (cursorPtr && cursorPtr->name == NULL) {
	wl_pointer_set_cursor(tkWlDisplay->pointer,
		tkWlDisplay->pointerSerial, NULL, 0, 0);
	return;
    }
    scale = tkWlDisplay->pointerSurface->scale;
    cursor = LoadCursor(cursorPtr ? cursorPtr->name : "left_ptr", scale);
    if (cursor == NULL || cursor->image_count < 1) {
	return;
    }
    image = cursor->images[0];
    buffer = wl_cursor_image_get_buffer(image);
    if (buffer == NULL) {
	return;
    }
    if (tkWlDisplay->cursorSurface == NULL) {
	tkWlDisplay->cursorSurface = wl_compositor_create_surface(
		tkWlDisplay->compositor);
    }
    wl_surface_set_buffer_scale(tkWlDisplay->cursorSurface, scale);
    wl_surface_attach(tkWlDisplay->cursorSurface, buffer, 0, 0);
    wl_surface_damage_buffer(tkWlDisplay->cursorSurface, 0, 0,
	    (int32_t)image->width, (int32_t)image->height);
    wl_surface_commit(tkWlDisplay->cursorSurface);
    wl_pointer_set_cursor(tkWlDisplay->pointer, tkWlDisplay->pointerSerial,
	    tkWlDisplay->cursorSurface, (int32_t)image->hotspot_x / scale,
	    (int32_t)image->hotspot_y / scale);
}

void
TkpSetCursor(
    Cursor cursor)
{
    if (cursor == currentCursor) {
	return;
    }
    currentCursor = cursor;
    TkWlUpdateCursor();
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
