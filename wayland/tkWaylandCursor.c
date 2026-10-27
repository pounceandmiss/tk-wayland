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
 * Results:
 *	The new cursor, or NULL if it could not be made.
 *
 * Side effects:
 *	Allocates the cursor, and a wl_shm buffer with its image.
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

    if (tkXeDisplay == NULL || width <= 0 || height <= 0) {
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
    pool = wl_shm_create_pool(tkXeDisplay->shm, fd, (int32_t)size);
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
 * CursorFromFiles --
 *
 *	Make a cursor from X11 bitmap files, as given in the cursor names
 *	"@sourceFile fg" and "@sourceFile maskFile fg bg", already split into
 *	the list argv of argc elements.
 *
 * Results:
 *	The new cursor, or NULL with an error message in interp.
 *
 * Side effects:
 *	Reads the bitmap files, and allocates the cursor and its image.
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
 *	Allocates the cursor; a cursor made of bitmap files has an image in a
 *	wl_shm buffer.
 *
 *----------------------------------------------------------------------
 */

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
	for (i = 0; tkXeCursorNames[i].name; i++) {
	    if (!strcmp(tkXeCursorNames[i].name, argv[0])) {
		name = tkXeCursorNames[i].name;
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
 *	Allocates the cursor, and a wl_shm buffer with its image.
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
    TkWlCursor *cursorPtr = MakeImageCursor(source, mask, width, height,
	    xHot, yHot, fg, bg);

    return cursorPtr ? &cursorPtr->info : NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TkpFreeCursor --
 *
 *	Release the resources of a cursor; tkCursor.c frees the cursor itself.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The image of a cursor made of bitmaps is freed. If the cursor is the
 *	one shown, the default cursor is shown instead.
 *
 *----------------------------------------------------------------------
 */

void
TkpFreeCursor(
    TkCursor *cursorPtr)
{
    TkWlCursor *wlCursorPtr = (TkWlCursor *)cursorPtr;

    if ((Cursor)cursorPtr->cursor == currentCursor) {
	currentCursor = None;
	TkXeUpdateCursor();
    }
    if (wlCursorPtr->buffer) {
	wl_buffer_destroy(wlCursorPtr->buffer);
	munmap(wlCursorPtr->data, wlCursorPtr->size);
	wlCursorPtr->buffer = NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * XCreatePixmapCursor, XCreateGlyphCursor --
 *
 *	Xlib functions which Tk does not use on Wayland, where cursors are
 *	made by TkGetCursorByName and TkCreateCursorFromData.
 *
 * Results:
 *	None, the cursor of the parent window.
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

/*
 *----------------------------------------------------------------------
 *
 * XFreeCursor --
 *
 *	Free a cursor made by XCreatePixmapCursor or XCreateGlyphCursor, which
 *	make none.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
XFreeCursor(
    Display *display,
    TCL_UNUSED(Cursor))
{
    TkXeNextRequest(display);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * LoadCursor --
 *
 *	Find a cursor of the cursor theme by its name in the X cursor font, or
 *	by its CSS name if the theme lacks the X name, or else the default
 *	cursor. The theme is loaded at the size given by XCURSOR_SIZE (24 by
 *	default) times the scale.
 *
 * Results:
 *	The cursor, or NULL if the theme cannot be loaded or has none of these
 *	cursors.
 *
 * Side effects:
 *	The cursor theme is (re)loaded the first time and when the scale
 *	changes.
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

    if (tkXeDisplay->cursorTheme == NULL || themeScale != scale) {
	const char *sizeString = getenv("XCURSOR_SIZE");
	int size = sizeString ? atoi(sizeString) : 0;

	if (size <= 0) {
	    size = 24;
	}
	if (tkXeDisplay->cursorTheme) {
	    wl_cursor_theme_destroy(tkXeDisplay->cursorTheme);
	}
	tkXeDisplay->cursorTheme = wl_cursor_theme_load(
		getenv("XCURSOR_THEME"), size * scale, tkXeDisplay->shm);
	themeScale = scale;
	if (tkXeDisplay->cursorTheme == NULL) {
	    return NULL;
	}
    }
    cursor = wl_cursor_theme_get_cursor(tkXeDisplay->cursorTheme, name);
    if (cursor) {
	return cursor;
    }
    for (i = 0; tkXeCursorNames[i].name; i++) {
	if (!strcmp(tkXeCursorNames[i].name, name)) {
	    cursor = wl_cursor_theme_get_cursor(tkXeDisplay->cursorTheme,
		    tkXeCursorNames[i].cssName);
	    break;
	}
    }
    if (cursor == NULL) {
	cursor = wl_cursor_theme_get_cursor(tkXeDisplay->cursorTheme,
		"left_ptr");
    }
    if (cursor == NULL) {
	cursor = wl_cursor_theme_get_cursor(tkXeDisplay->cursorTheme,
		"default");
    }
    return cursor;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeUpdateCursor --
 *
 *	Show the current cursor on the surface under the pointer, at the scale
 *	of that surface.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Creates the surface of the cursor if needed, attaches the image of the
 *	cursor to it, and sets it as the cursor of the pointer.
 *
 *----------------------------------------------------------------------
 */

void
TkXeUpdateCursor(void)
{
    TkWlCursor *cursorPtr = (TkWlCursor *)currentCursor;
    struct wl_cursor *cursor;
    struct wl_cursor_image *image;
    struct wl_buffer *buffer;
    int scale;

    if (tkXeDisplay == NULL || tkXeDisplay->pointer == NULL
	    || tkXeDisplay->pointerSurface == NULL) {
	return;
    }
    if (cursorPtr && cursorPtr->buffer) {
	if (tkXeDisplay->cursorSurface == NULL) {
	    tkXeDisplay->cursorSurface = wl_compositor_create_surface(
		    tkXeDisplay->compositor);
	}
	wl_surface_set_buffer_scale(tkXeDisplay->cursorSurface, 1);
	wl_surface_attach(tkXeDisplay->cursorSurface, cursorPtr->buffer, 0, 0);
	wl_surface_damage_buffer(tkXeDisplay->cursorSurface, 0, 0, INT32_MAX,
		INT32_MAX);
	wl_surface_commit(tkXeDisplay->cursorSurface);
	wl_pointer_set_cursor(tkXeDisplay->pointer, tkXeDisplay->pointerSerial,
		tkXeDisplay->cursorSurface, cursorPtr->xHot, cursorPtr->yHot);
	return;
    }
    if (cursorPtr && cursorPtr->name == NULL) {
	wl_pointer_set_cursor(tkXeDisplay->pointer,
		tkXeDisplay->pointerSerial, NULL, 0, 0);
	return;
    }
    scale = (int)ceil(tkXeDisplay->pointerSurface->scale);
    cursor = LoadCursor(cursorPtr ? cursorPtr->name : "left_ptr", scale);
    if (cursor == NULL || cursor->image_count < 1) {
	return;
    }
    image = cursor->images[0];
    buffer = wl_cursor_image_get_buffer(image);
    if (buffer == NULL) {
	return;
    }
    if (tkXeDisplay->cursorSurface == NULL) {
	tkXeDisplay->cursorSurface = wl_compositor_create_surface(
		tkXeDisplay->compositor);
    }
    wl_surface_set_buffer_scale(tkXeDisplay->cursorSurface, scale);
    wl_surface_attach(tkXeDisplay->cursorSurface, buffer, 0, 0);
    wl_surface_damage_buffer(tkXeDisplay->cursorSurface, 0, 0,
	    (int32_t)image->width, (int32_t)image->height);
    wl_surface_commit(tkXeDisplay->cursorSurface);
    wl_pointer_set_cursor(tkXeDisplay->pointer, tkXeDisplay->pointerSerial,
	    tkXeDisplay->cursorSurface, (int32_t)image->hotspot_x / scale,
	    (int32_t)image->hotspot_y / scale);
}

/*
 *----------------------------------------------------------------------
 *
 * TkpSetCursor --
 *
 *	Set the cursor of the pointer. Called by tkPointer.c when the pointer
 *	enters a window with another cursor; None means the default cursor.
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
    if (cursor == currentCursor) {
	return;
    }
    currentCursor = cursor;
    TkXeUpdateCursor();
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
