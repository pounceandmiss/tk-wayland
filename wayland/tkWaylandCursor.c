/*
 * tkWaylandCursor.c --
 *
 *	Cursors of the Wayland port. Tk's cursor names are those of the X
 *	cursor font, which cursor themes provide, together with their CSS
 *	names, which are tried when a theme lacks the X name. Cursors made of
 *	bitmaps are images in wl_shm buffers.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#define _GNU_SOURCE		/* For memfd_create. */
#include "tkWaylandInt.h"
#include <sys/mman.h>

typedef struct {
    TkCursor info;		/* Generic cursor info used by tkCursor.c */
    const char *name;		/* X cursor font name, or NULL for no cursor
				 * or a cursor made of bitmaps. */
    struct wl_buffer *buffer;	/* The image of a cursor made of bitmaps. */
    void *data;
    size_t size;
    int xHot, yHot;
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
 * MakeImageCursor --
 *
 *	Make a cursor from bitmaps in the format of X11 bitmap files: the
 *	pixels set in the mask (all, without a mask) are drawn, with the
 *	foreground where the source is set, else with the background.
 *
 *----------------------------------------------------------------------
 */

static TkWlCursor *
MakeImageCursor(
    const char *source,
    const char *mask,
    int width, int height,
    int xHot, int yHot,
    XColor fg,
    XColor bg)
{
    TkWlCursor *cursorPtr;
    int fd, x, y, stride = width * 4, rowBytes = (width + 7) / 8;
    size_t size = (size_t)stride * height;
    struct wl_shm_pool *pool;
    uint32_t *pixels, fgPixel, bgPixel;

    if (tkWlDisplay == NULL || width <= 0 || height <= 0) {
	return NULL;
    }
    fd = memfd_create("tk-cursor", MFD_CLOEXEC);
    if (fd < 0) {
	return NULL;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
	close(fd);
	return NULL;
    }
    pixels = (uint32_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    fd, 0);
    if (pixels == MAP_FAILED) {
	close(fd);
	return NULL;
    }
    fgPixel = 0xFF000000 | (uint32_t)TkpGetPixel(&fg);
    bgPixel = 0xFF000000 | (uint32_t)TkpGetPixel(&bg);
    for (y = 0; y < height; y++) {
	for (x = 0; x < width; x++) {
	    int byte = y * rowBytes + x / 8, bit = 1 << (x % 8);
	    uint32_t pixel = 0;

	    if (mask == NULL || (mask[byte] & bit)) {
		pixel = (source[byte] & bit) ? fgPixel : bgPixel;
	    }
	    pixels[y * width + x] = pixel;
	}
    }
    pool = wl_shm_create_pool(tkWlDisplay->shm, fd, (int32_t)size);
    cursorPtr = (TkWlCursor *)Tcl_Alloc(sizeof(TkWlCursor));
    memset(cursorPtr, 0, sizeof(TkWlCursor));
    cursorPtr->info.cursor = (Tk_Cursor)cursorPtr;
    cursorPtr->buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
	    stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    cursorPtr->data = pixels;
    cursorPtr->size = size;
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
 *----------------------------------------------------------------------
 */

static TkWlCursor *
CursorFromFiles(
    Tcl_Interp *interp,
    Tk_Window tkwin,
    Tcl_Size argc,
    const char **argv)
{
    char *source = NULL, *mask = NULL;
    int width, height, xHot, yHot, maskWidth, maskHeight, dummy1, dummy2;
    XColor fg, bg;
    TkWlCursor *cursorPtr = NULL;
    const char *fgName = argv[1], *bgName = NULL;

    if (Tcl_IsSafe(interp)) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		"can't get cursor from a file in a safe interpreter", -1));
	Tcl_SetErrorCode(interp, "TK", "SAFE", "CURSOR_FILE", (char *)NULL);
	return NULL;
    }
    if (argc == 4) {
	fgName = argv[2];
	bgName = argv[3];
    }
    if (TkParseColor(Tk_Display(tkwin), Tk_Colormap(tkwin), fgName, &fg)
	    == 0 || (bgName && TkParseColor(Tk_Display(tkwin),
		Tk_Colormap(tkwin), bgName, &bg) == 0)) {
	Tcl_SetObjResult(interp, Tcl_ObjPrintf("invalid color name \"%s\"",
		bgName ? bgName : fgName));
	Tcl_SetErrorCode(interp, "TK", "CURSOR", "COLOR", (char *)NULL);
	return NULL;
    }
    source = TkGetBitmapData(interp, NULL, argv[0] + 1, &width, &height,
	    &xHot, &yHot);
    if (source == NULL) {
	return NULL;
    }
    if (xHot < 0) {
	xHot = yHot = 0;
    }
    if (argc == 4) {
	mask = TkGetBitmapData(interp, NULL, argv[1], &maskWidth, &maskHeight,
		&dummy1, &dummy2);
	if (mask == NULL) {
	    goto done;
	}
	if (maskWidth != width || maskHeight != height) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "source and mask bitmaps have different sizes", -1));
	    Tcl_SetErrorCode(interp, "TK", "CURSOR", "SIZE_MATCH",
		    (char *)NULL);
	    goto done;
	}
    }
    if (argc == 2) {
	/*
	 * Only the foreground is drawn, where the source is set.
	 */

	bg = fg;
    }
    cursorPtr = MakeImageCursor(source, (argc == 4) ? mask : source, width,
	    height, xHot, yHot, fg, bg);
  done:
    Tcl_Free(source);
    if (mask) {
	Tcl_Free(mask);
    }
    return cursorPtr;
}

TkCursor *
TkGetCursorByName(
    Tcl_Interp *interp,
    Tk_Window tkwin,
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
    if (argc < 1 || argc > 4) {
	goto badString;
    }
    if (argv[0][0] == '@') {
	if (argc != 2 && argc != 4) {
	    goto badString;
	}
	cursorPtr = CursorFromFiles(interp, tkwin, argc, argv);
	Tcl_Free(argv);
	return cursorPtr ? &cursorPtr->info : NULL;
    }
    if (argc > 3) {
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
    memset(cursorPtr, 0, sizeof(TkWlCursor));
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
    TkWlCursor *cursorPtr = MakeImageCursor(source, mask, width, height,
	    xHot, yHot, fg, bg);

    return cursorPtr ? &cursorPtr->info : NULL;
}

void
TkpFreeCursor(
    TkCursor *cursorPtr)
{
    TkWlCursor *wlCursorPtr = (TkWlCursor *)cursorPtr;

    if ((Cursor)cursorPtr->cursor == currentCursor) {
	currentCursor = None;
	TkWlUpdateCursor();
    }
    if (wlCursorPtr->buffer) {
	wl_buffer_destroy(wlCursorPtr->buffer);
	munmap(wlCursorPtr->data, wlCursorPtr->size);
	wlCursorPtr->buffer = NULL;
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
    if (cursorPtr && cursorPtr->buffer) {
	if (tkWlDisplay->cursorSurface == NULL) {
	    tkWlDisplay->cursorSurface = wl_compositor_create_surface(
		    tkWlDisplay->compositor);
	}
	wl_surface_set_buffer_scale(tkWlDisplay->cursorSurface, 1);
	wl_surface_attach(tkWlDisplay->cursorSurface, cursorPtr->buffer, 0, 0);
	wl_surface_damage_buffer(tkWlDisplay->cursorSurface, 0, 0, INT32_MAX,
		INT32_MAX);
	wl_surface_commit(tkWlDisplay->cursorSurface);
	wl_pointer_set_cursor(tkWlDisplay->pointer, tkWlDisplay->pointerSerial,
		tkWlDisplay->cursorSurface, cursorPtr->xHot, cursorPtr->yHot);
	return;
    }
    if (cursorPtr && cursorPtr->name == NULL) {
	wl_pointer_set_cursor(tkWlDisplay->pointer,
		tkWlDisplay->pointerSerial, NULL, 0, 0);
	return;
    }
    scale = (int)ceil(tkWlDisplay->pointerSurface->scale);
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
