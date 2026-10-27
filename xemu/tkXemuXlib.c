/*
 * tkXemuXlib.c --
 *
 *	The parts of the X server emulation which do not deal with windows
 *	or drawing: the display structure, atoms, properties, regions,
 *	colors, keysyms and functions which have nothing to do without an X
 *	server.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#define XLIB_ILLEGAL_ACCESS
#include "tkXemuInt.h"
#include <fontconfig/fontconfig.h>

/*
 * Regions are pixman regions.
 */

struct _XRegion {
    pixman_region32_t rgn;
};

/*
 *----------------------------------------------------------------------
 *
 * TkXeOpenXDisplay --
 *
 *	Create the emulated X display of the port.
 *
 * Results:
 *	The new display.
 *
 * Side effects:
 *	Allocates the display, its screen and visual, and the root window.
 *
 *----------------------------------------------------------------------
 */

Display *
TkXeOpenXDisplay(
    const char *name,		/* Name of the display. */
    int fd,			/* File descriptor of the connection. */
    int width, int height,	/* Size of the screen, in pixels. */
    int mmWidth, int mmHeight)	/* Size of the screen, in millimeters. */
{
    Display *display = (Display *)Tcl_Alloc(sizeof(Display));
    Screen *screen = (Screen *)Tcl_Alloc(sizeof(Screen));
    Visual *visual = (Visual *)Tcl_Alloc(sizeof(Visual));
    Depth *depth = (Depth *)Tcl_Alloc(sizeof(Depth));

    memset(display, 0, sizeof(Display));
    memset(screen, 0, sizeof(Screen));
    memset(visual, 0, sizeof(Visual));
    memset(depth, 0, sizeof(Depth));

    display->fd = fd;
    display->proto_major_version = 11;
    display->proto_minor_version = 0;
    display->vendor = (char *)TKXE_VENDOR;
    display->release = 1;
    display->byte_order = LSBFirst;
    display->bitmap_unit = 32;
    display->bitmap_pad = 32;
    display->bitmap_bit_order = LSBFirst;
    display->display_name = (char *)Tcl_Alloc(strlen(name) + 1);
    strcpy(display->display_name, name);
    display->default_screen = 0;
    display->nscreens = 1;
    display->screens = screen;
    display->min_keycode = 8;
    display->max_keycode = 255;

    visual->visualid = 1;
    visual->c_class = TrueColor;
    visual->red_mask = 0xFF0000;
    visual->green_mask = 0x00FF00;
    visual->blue_mask = 0x0000FF;
    visual->bits_per_rgb = 8;
    visual->map_entries = 256;

    depth->depth = 24;
    depth->nvisuals = 1;
    depth->visuals = visual;

    screen->display = display;
    screen->width = width;
    screen->height = height;
    screen->mwidth = mmWidth;
    screen->mheight = mmHeight;
    screen->ndepths = 1;
    screen->depths = depth;
    screen->root_depth = 24;
    screen->root_visual = visual;
    screen->cmap = 1;
    screen->white_pixel = 0xFFFFFF;
    screen->black_pixel = 0x000000;
    screen->max_maps = 1;
    screen->min_maps = 1;
    screen->backing_store = NotUseful;
    screen->save_unders = False;
    screen->root_input_mask = 0;
    screen->root = TkXeCreateRootWindow(display, width, height)->id;
    return display;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeSetScreenSize --
 *
 *	Update the size of the screen when the outputs change.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The size of the screen and of the root window change. The size in
 *	millimeters is that of 96 dpi.
 *
 *----------------------------------------------------------------------
 */

void
TkXeSetScreenSize(
    Display *display,
    int width, int height,
    int mmWidth, int mmHeight)
{
    Screen *screen = ScreenOfDisplay(display, 0);
    TkXeWindow *rootPtr = TkXeGetWindow(screen->root);

    screen->width = width;
    screen->height = height;

    /*
     * Logical pixels have 96 dpi, whatever the physical size of the screen
     * (which is often unknown or wrong), as in other toolkits. Higher
     * resolutions are handled with the scale of the surfaces.
     */

    (void)mmWidth;
    (void)mmHeight;
    screen->mwidth = (int)lround(width * 25.4 / 96.0);
    screen->mheight = (int)lround(height * 25.4 / 96.0);
    rootPtr->width = width;
    rootPtr->height = height;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeNextRequest --
 *
 *	Count a request, as Xlib does, for the serial numbers of the events
 *	and for Tk's error handlers.
 *
 * Results:
 *	The serial number of the request.
 *
 * Side effects:
 *	The request counter of the display is incremented.
 *
 *----------------------------------------------------------------------
 */

unsigned long
TkXeNextRequest(
    Display *display)
{
    return ++display->request;
}

/*
 *----------------------------------------------------------------------
 *
 * Atoms --
 *
 *	There is no server, so atoms are only valid in this process. The
 *	predefined atoms have their X11 values, so Xatom.h can be used.
 *
 *----------------------------------------------------------------------
 */

static const char *const predefinedAtoms[] = {
    NULL, "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL",
    "COLORMAP", "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2",
    "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6",
    "CUT_BUFFER7", "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT",
    "RECTANGLE", "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP",
    "RGB_BLUE_MAP", "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP",
    "RGB_RED_MAP", "STRING", "VISUALID", "WINDOW", "WM_COMMAND",
    "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE",
    "WM_NAME", "WM_NORMAL_HINTS", "WM_SIZE_HINTS", "WM_ZOOM_HINTS",
    "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE", "SUPERSCRIPT_X",
    "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y", "UNDERLINE_POSITION",
    "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT",
    "ITALIC_ANGLE", "X_HEIGHT", "QUAD_WIDTH", "WEIGHT", "POINT_SIZE",
    "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME", "FAMILY_NAME",
    "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR"
};

#define NUM_PREDEFINED_ATOMS \
	((Atom)(sizeof(predefinedAtoms) / sizeof(predefinedAtoms[0])))

static Tcl_HashTable atomTable;		/* Maps names to atoms. */
static Tcl_HashTable atomNameTable;	/* Maps atoms to names. */
static Atom nextAtom = NUM_PREDEFINED_ATOMS;
static int atomsInitialized = 0;
TCL_DECLARE_MUTEX(atomMutex)

/*
 *----------------------------------------------------------------------
 *
 * InitAtoms --
 *
 *	Create the tables of atoms, with the predefined atoms. Called with
 *	atomMutex locked.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The tables are initialized.
 *
 *----------------------------------------------------------------------
 */

static void
InitAtoms(void)
{
    Atom atom;
    int isNew;

    Tcl_InitHashTable(&atomTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&atomNameTable, TCL_ONE_WORD_KEYS);
    for (atom = 1; atom < NUM_PREDEFINED_ATOMS; atom++) {
	Tcl_HashEntry *hPtr = Tcl_CreateHashEntry(&atomTable,
		predefinedAtoms[atom], &isNew);

	Tcl_SetHashValue(hPtr, INT2PTR(atom));
	hPtr = Tcl_CreateHashEntry(&atomNameTable, INT2PTR(atom), &isNew);
	Tcl_SetHashValue(hPtr, (void *)predefinedAtoms[atom]);
    }
    atomsInitialized = 1;
}

/*
 *----------------------------------------------------------------------
 *
 * XInternAtom --
 *
 *	Get the atom of a name, creating it unless onlyIfExists is true. There
 *	is no server, so atoms are only valid in this process; the predefined
 *	atoms have their X11 values.
 *
 * Results:
 *	The atom, or None if it does not exist and onlyIfExists is true.
 *
 * Side effects:
 *	A new atom may be created.
 *
 *----------------------------------------------------------------------
 */

Atom
XInternAtom(
    Display *display,
    const char *name,
    Bool onlyIfExists)
{
    Tcl_HashEntry *hPtr;
    Atom atom = None;
    int isNew;

    if (display) {
	TkXeNextRequest(display);
    }
    Tcl_MutexLock(&atomMutex);
    if (!atomsInitialized) {
	InitAtoms();
    }
    hPtr = Tcl_FindHashEntry(&atomTable, name);
    if (hPtr) {
	atom = (Atom)PTR2INT(Tcl_GetHashValue(hPtr));
    } else if (!onlyIfExists) {
	atom = nextAtom++;
	hPtr = Tcl_CreateHashEntry(&atomTable, name, &isNew);
	Tcl_SetHashValue(hPtr, INT2PTR(atom));
	hPtr = Tcl_CreateHashEntry(&atomNameTable, INT2PTR(atom), &isNew);
	Tcl_SetHashValue(hPtr, Tcl_GetHashKey(&atomTable,
		Tcl_FindHashEntry(&atomTable, name)));
    }
    Tcl_MutexUnlock(&atomMutex);
    return atom;
}

/*
 *----------------------------------------------------------------------
 *
 * XGetAtomName --
 *
 *	Get the name of an atom.
 *
 * Results:
 *	The name, which the caller frees with XFree, or NULL if the atom does
 *	not exist.
 *
 * Side effects:
 *	Memory is allocated for the name.
 *
 *----------------------------------------------------------------------
 */

char *
XGetAtomName(
    Display *display,
    Atom atom)
{
    Tcl_HashEntry *hPtr;
    char *result = NULL;

    if (display) {
	TkXeNextRequest(display);
    }
    Tcl_MutexLock(&atomMutex);
    if (!atomsInitialized) {
	InitAtoms();
    }
    hPtr = Tcl_FindHashEntry(&atomNameTable, INT2PTR(atom));
    if (hPtr) {
	const char *name = (const char *)Tcl_GetHashValue(hPtr);

	result = (char *)Tcl_Alloc(strlen(name) + 1);
	strcpy(result, name);
    }
    Tcl_MutexUnlock(&atomMutex);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Properties --
 *
 *	Window properties are only stored, for the windows of this process.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * TkXeGetProperty --
 *
 *	Find a property of a window.
 *
 * Results:
 *	The property, or NULL if the window does not have it.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

TkXeProperty *
TkXeGetProperty(
    TkXeWindow *winPtr,
    Atom name)
{
    TkXeProperty *propPtr;

    for (propPtr = winPtr->properties; propPtr; propPtr = propPtr->nextPtr) {
	if (propPtr->name == name) {
	    return propPtr;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * PropertyChanged --
 *
 *	Report the change of a property of a window: send a PropertyNotify
 *	event if the window selected them, and tell the window manager.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event may be queued, and the window manager may update the
 *	surface of the window.
 *
 *----------------------------------------------------------------------
 */

static void
PropertyChanged(
    TkXeWindow *winPtr,
    Atom property,
    int state)
{
    if (winPtr->eventMask & PropertyChangeMask) {
	XEvent event;

	memset(&event, 0, sizeof(XEvent));
	event.xproperty.type = PropertyNotify;
	event.xproperty.display = winPtr->display;
	event.xproperty.window = winPtr->id;
	event.xproperty.atom = property;
	event.xproperty.time = TkXeCurrentTime();
	event.xproperty.state = state;
	TkXeQueueEvent(&event);
    }
    if (winPtr->surface && state == PropertyNewValue) {
	TkXeWmPropertyChanged(winPtr->surface, property);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PropertyItemSize --
 *
 *	Get the size in memory of the items of a property of a format: format
 *	32 is stored in longs, as in Xlib.
 *
 * Results:
 *	The size in bytes.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static size_t
PropertyItemSize(
    int format)
{
    return (format == 8) ? 1 : (format == 16) ? sizeof(short) : sizeof(long);
}

/*
 *----------------------------------------------------------------------
 *
 * XChangeProperty --
 *
 *	Replace, prepend to or append to a property of a window. Properties
 *	are
 *	only stored, for the windows of this process.
 *
 * Results:
 *	Success, BadWindow if the window does not exist, or BadMatch if the
 *	type or format differs from those of the property in a prepend or
 *	append.
 *
 * Side effects:
 *	The property is changed, and the change reported (see
 *	PropertyChanged).
 *
 *----------------------------------------------------------------------
 */

int
XChangeProperty(
    Display *display,
    Window w,
    Atom property,
    Atom type,
    int format,
    int mode,
    _Xconst unsigned char *data,
    int nelements)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);
    TkXeProperty *propPtr;
    size_t itemSize = PropertyItemSize(format);
    unsigned char *newData;
    unsigned long nitems;

    TkXeNextRequest(display);
    if (winPtr == NULL) {
	TkXeError(display, w, BadWindow, X_ChangeProperty);
	return BadWindow;
    }
    if (nelements < 0) {
	return BadWindow;
    }
    propPtr = TkXeGetProperty(winPtr, property);
    if (propPtr == NULL) {
	propPtr = (TkXeProperty *)Tcl_Alloc(sizeof(TkXeProperty));
	memset(propPtr, 0, sizeof(TkXeProperty));
	propPtr->name = property;
	propPtr->nextPtr = winPtr->properties;
	winPtr->properties = propPtr;
	mode = PropModeReplace;
    } else if (mode != PropModeReplace
	    && (propPtr->type != type || propPtr->format != format)) {
	return BadMatch;
    }

    if (nelements == 0 || data == NULL) {
	/* memcpy() must not be given NULL, even for no data. */
	nelements = 0;
	data = (const unsigned char *)"";
    }
    nitems = (mode == PropModeReplace) ? (unsigned long)nelements
	    : propPtr->nitems + nelements;
    newData = (unsigned char *)Tcl_Alloc(nitems * itemSize + 1);
    switch (mode) {
    case PropModeReplace:
	memcpy(newData, data, nelements * itemSize);
	break;
    case PropModePrepend:
	memcpy(newData, data, nelements * itemSize);
	memcpy(newData + nelements * itemSize, propPtr->data,
		propPtr->nitems * itemSize);
	break;
    default:
	memcpy(newData, propPtr->data, propPtr->nitems * itemSize);
	memcpy(newData + propPtr->nitems * itemSize, data,
		nelements * itemSize);
	break;
    }
    newData[nitems * itemSize] = '\0';
    if (propPtr->data) {
	Tcl_Free(propPtr->data);
    }
    propPtr->data = newData;
    propPtr->nitems = nitems;
    propPtr->type = type;
    propPtr->format = format;
    PropertyChanged(winPtr, property, PropertyNewValue);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XGetWindowProperty --
 *
 *	Get part of a property of a window, as the X server returns it:
 *	offset and length are in 32-bit units.
 *
 * Results:
 *	Success, BadWindow if the window does not exist, or BadValue if the
 *	offset is beyond the end of the property. The data, which the caller
 *	frees with XFree, and its description are stored in the output
 *	arguments.
 *
 * Side effects:
 *	Memory is allocated for the data. The property is deleted if asked to
 *	and all of it was returned.
 *
 *----------------------------------------------------------------------
 */

int
XGetWindowProperty(
    Display *display,
    Window w,
    Atom property,
    long offset,
    long length,
    Bool deleteProp,
    Atom reqType,
    Atom *actualTypePtr,
    int *actualFormatPtr,
    unsigned long *nitemsPtr,
    unsigned long *bytesAfterPtr,
    unsigned char **propPtrPtr)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);
    TkXeProperty *propPtr;
    size_t itemSize, total, start, count;

    TkXeNextRequest(display);
    *actualTypePtr = None;
    *actualFormatPtr = 0;
    *nitemsPtr = 0;
    *bytesAfterPtr = 0;
    *propPtrPtr = NULL;
    if (winPtr == NULL) {
	TkXeError(display, w, BadWindow, X_GetProperty);
	return BadWindow;
    }
    propPtr = TkXeGetProperty(winPtr, property);
    if (propPtr == NULL) {
	return Success;
    }
    *actualTypePtr = propPtr->type;
    *actualFormatPtr = propPtr->format;
    itemSize = PropertyItemSize(propPtr->format);
    total = propPtr->nitems * ((propPtr->format == 32) ? 4 : itemSize);
    if (reqType != AnyPropertyType && reqType != propPtr->type) {
	*bytesAfterPtr = total;
	return Success;
    }

    /*
     * Offset and length are in 32-bit units, as in the protocol.
     */

    start = (size_t)offset * 4;
    if (start > total) {
	return BadValue;
    }
    count = total - start;
    if ((size_t)length * 4 < count) {
	count = (size_t)length * 4;
    }
    *bytesAfterPtr = total - start - count;
    if (propPtr->format == 32) {
	start /= 4;
	count /= 4;
    } else if (propPtr->format == 16) {
	start /= 2;
	count /= 2;
    }
    *propPtrPtr = (unsigned char *)Tcl_Alloc(count * itemSize + 1);
    memcpy(*propPtrPtr, propPtr->data + start * itemSize, count * itemSize);
    (*propPtrPtr)[count * itemSize] = '\0';
    *nitemsPtr = count;
    if (deleteProp && *bytesAfterPtr == 0) {
	XDeleteProperty(display, w, property);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XDeleteProperty --
 *
 *	Delete a property of a window.
 *
 * Results:
 *	Success, or BadWindow if the window does not exist.
 *
 * Side effects:
 *	The property is freed, and its deletion reported.
 *
 *----------------------------------------------------------------------
 */

int
XDeleteProperty(
    Display *display,
    Window w,
    Atom property)
{
    TkXeWindow *winPtr = TkXeGetWindow(w);
    TkXeProperty **prevPtrPtr;

    TkXeNextRequest(display);
    if (winPtr == NULL) {
	TkXeError(display, w, BadWindow, X_DeleteProperty);
	return BadWindow;
    }
    for (prevPtrPtr = &winPtr->properties; *prevPtrPtr;
	    prevPtrPtr = &(*prevPtrPtr)->nextPtr) {
	TkXeProperty *propPtr = *prevPtrPtr;

	if (propPtr->name == property) {
	    *prevPtrPtr = propPtr->nextPtr;
	    Tcl_Free(propPtr->data);
	    Tcl_Free(propPtr);
	    PropertyChanged(winPtr, property, PropertyDelete);
	    break;
	}
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeFreeProperties --
 *
 *	Free the properties of a window which is destroyed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The properties are freed, without being reported.
 *
 *----------------------------------------------------------------------
 */

void
TkXeFreeProperties(
    TkXeWindow *winPtr)
{
    while (winPtr->properties) {
	TkXeProperty *propPtr = winPtr->properties;

	winPtr->properties = propPtr->nextPtr;
	Tcl_Free(propPtr->data);
	Tcl_Free(propPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * XFree --
 *
 *	Free memory returned by the Xlib emulation; NULL is ignored.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	The memory is freed.
 *
 *----------------------------------------------------------------------
 */

int
XFree(
    void *data)
{
    if (data) {
	Tcl_Free(data);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * Regions --
 *
 *	Regions are pixman regions.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * TkXePixmanRegion --
 *
 *	Get the pixman region of a region.
 *
 * Results:
 *	The pixman region.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

pixman_region32_t *
TkXePixmanRegion(
    Region r)
{
    return &r->rgn;
}

/*
 *----------------------------------------------------------------------
 *
 * XCreateRegion, XDestroyRegion --
 *
 *	Create an empty region, and destroy a region (NULL is ignored).
 *
 * Results:
 *	XCreateRegion returns the new region, XDestroyRegion Success.
 *
 * Side effects:
 *	Memory is allocated or freed.
 *
 *----------------------------------------------------------------------
 */

Region
XCreateRegion(void)
{
    Region r = (Region)Tcl_Alloc(sizeof(struct _XRegion));

    pixman_region32_init(&r->rgn);
    return r;
}

int
XDestroyRegion(
    Region r)
{
    if (r) {
	pixman_region32_fini(&r->rgn);
	Tcl_Free(r);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XUnionRectWithRegion, XUnionRegion, XIntersectRegion,
 * XSubtractRegion, XXorRegion, XOffsetRegion --
 *
 *	Operations on regions, done by pixman.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	The result is stored in the destination region (or r for
 *	XOffsetRegion).
 *
 *----------------------------------------------------------------------
 */

int
XUnionRectWithRegion(
    XRectangle *rectPtr,
    Region src,
    Region dst)
{
    pixman_region32_union_rect(&dst->rgn, &src->rgn, rectPtr->x, rectPtr->y,
	    rectPtr->width, rectPtr->height);
    return Success;
}

int
XUnionRegion(
    Region a,
    Region b,
    Region dst)
{
    pixman_region32_union(&dst->rgn, &a->rgn, &b->rgn);
    return Success;
}

int
XIntersectRegion(
    Region a,
    Region b,
    Region dst)
{
    pixman_region32_intersect(&dst->rgn, &a->rgn, &b->rgn);
    return Success;
}

int
XSubtractRegion(
    Region a,
    Region b,
    Region dst)
{
    pixman_region32_subtract(&dst->rgn, &a->rgn, &b->rgn);
    return Success;
}

int
XXorRegion(
    Region a,
    Region b,
    Region dst)
{
    pixman_region32_t aMinusB, bMinusA;

    pixman_region32_init(&aMinusB);
    pixman_region32_init(&bMinusA);
    pixman_region32_subtract(&aMinusB, &a->rgn, &b->rgn);
    pixman_region32_subtract(&bMinusA, &b->rgn, &a->rgn);
    pixman_region32_union(&dst->rgn, &aMinusB, &bMinusA);
    pixman_region32_fini(&aMinusB);
    pixman_region32_fini(&bMinusA);
    return Success;
}

int
XOffsetRegion(
    Region r,
    int dx, int dy)
{
    pixman_region32_translate(&r->rgn, dx, dy);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XEmptyRegion, XEqualRegion, XPointInRegion, XRectInRegion,
 * XClipBox --
 *
 *	Queries of regions, done by pixman.
 *
 * Results:
 *	Whether the region is empty, the regions are equal, or the point is in
 *	the region; RectangleIn, RectanglePart or RectangleOut for
 *	XRectInRegion; Success for XClipBox, which stores the bounding box of
 *	the region in *rectPtr.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Bool
XEmptyRegion(
    Region r)
{
    return !pixman_region32_not_empty(&r->rgn);
}

Bool
XEqualRegion(
    Region a,
    Region b)
{
    return pixman_region32_equal(&a->rgn, &b->rgn);
}

Bool
XPointInRegion(
    Region r,
    int x, int y)
{
    return pixman_region32_contains_point(&r->rgn, x, y, NULL);
}

int
XRectInRegion(
    Region r,
    int x, int y,
    unsigned width, unsigned height)
{
    pixman_box32_t box;

    box.x1 = x;
    box.y1 = y;
    box.x2 = x + (int)width;
    box.y2 = y + (int)height;
    switch (pixman_region32_contains_rectangle(&r->rgn, &box)) {
    case PIXMAN_REGION_IN:
	return RectangleIn;
    case PIXMAN_REGION_PART:
	return RectanglePart;
    default:
	return RectangleOut;
    }
}

int
XClipBox(
    Region r,
    XRectangle *rectPtr)
{
    pixman_box32_t *box = pixman_region32_extents(&r->rgn);

    rectPtr->x = (short)box->x1;
    rectPtr->y = (short)box->y1;
    rectPtr->width = (unsigned short)(box->x2 - box->x1);
    rectPtr->height = (unsigned short)(box->y2 - box->y1);
    return Success;
}

/*
 * XPolygonRegion: the region is built one scan line at a time, from the
 * spans between the crossings of the edges with the center of the line.
 */

typedef struct {
    int x;			/* Where the edge crosses the scan line. */
    int dir;			/* 1 if the edge goes down, -1 if up. */
} Crossing;

/*
 *----------------------------------------------------------------------
 *
 * CompareCrossings --
 *
 *	Comparison function of qsort for the crossings of XPolygonRegion.
 *
 * Results:
 *	Negative, zero or positive as the first crossing is left of, at or
 *	right of the second.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
CompareCrossings(
    const void *a,
    const void *b)
{
    return ((const Crossing *)a)->x - ((const Crossing *)b)->x;
}

/*
 *----------------------------------------------------------------------
 *
 * XPolygonRegion --
 *
 *	Make the region of a polygon, with the even-odd or winding fill rule.
 *	The region is built one scan line at a time, from the spans between
 *	the crossings of the edges with the center of the line.
 *
 * Results:
 *	The new region.
 *
 * Side effects:
 *	Memory is allocated for the region.
 *
 *----------------------------------------------------------------------
 */

Region
XPolygonRegion(
    XPoint *points,
    int n,
    int fillRule)
{
    Region r = XCreateRegion();
    int i, y, yMin, yMax;
    Crossing *xs;

    if (n < 3) {
	return r;
    }
    yMin = yMax = points[0].y;
    for (i = 1; i < n; i++) {
	if (points[i].y < yMin) {
	    yMin = points[i].y;
	}
	if (points[i].y > yMax) {
	    yMax = points[i].y;
	}
    }
    xs = (Crossing *)Tcl_Alloc(n * sizeof(Crossing));
    for (y = yMin; y < yMax; y++) {
	double cy = y + 0.5;
	int count = 0;

	for (i = 0; i < n; i++) {
	    XPoint *p = &points[i], *q = &points[(i + 1) % n];

	    if ((p->y <= cy && q->y > cy) || (q->y <= cy && p->y > cy)) {
		xs[count].x = (int)floor(p->x
			+ (cy - p->y) * (q->x - p->x) / (q->y - p->y) + 0.5);
		xs[count].dir = (q->y > p->y) ? 1 : -1;
		count++;
	    }
	}
	qsort(xs, count, sizeof(Crossing), CompareCrossings);
	if (fillRule == WindingRule) {
	    int w = 0, start = 0;

	    for (i = 0; i < count; i++) {
		int wasInside = (w != 0);

		w += xs[i].dir;
		if (!wasInside && w != 0) {
		    start = xs[i].x;
		} else if (wasInside && w == 0 && xs[i].x > start) {
		    pixman_region32_union_rect(&r->rgn, &r->rgn, start, y,
			    xs[i].x - start, 1);
		}
	    }
	} else {
	    for (i = 0; i + 1 < count; i += 2) {
		if (xs[i + 1].x > xs[i].x) {
		    pixman_region32_union_rect(&r->rgn, &r->rgn, xs[i].x, y,
			    xs[i + 1].x - xs[i].x, 1);
		}
	    }
	}
    }
    Tcl_Free(xs);
    return r;
}

/*
 *----------------------------------------------------------------------
 *
 * Colors --
 *
 *	The visual is TrueColor, so colors need not be allocated.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * XAllocColor --
 *
 *	Allocate a color: the visual is TrueColor, so this only computes the
 *	pixel of the color, rounded to 8 bits per component.
 *
 * Results:
 *	1.
 *
 * Side effects:
 *	The color is rounded, and its pixel stored in it.
 *
 *----------------------------------------------------------------------
 */

Status
XAllocColor(
    Display *display,
    TCL_UNUSED(Colormap),
    XColor *colorPtr)
{
    TkXeNextRequest(display);
    colorPtr->red = (colorPtr->red >> 8) * 0x101;
    colorPtr->green = (colorPtr->green >> 8) * 0x101;
    colorPtr->blue = (colorPtr->blue >> 8) * 0x101;
    colorPtr->pixel = TkpGetPixel(colorPtr);
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * XLookupColor, XAllocNamedColor --
 *
 *	Parse the name of a color, and allocate it.
 *
 * Results:
 *	1 if the name is one of a color, 0 otherwise.
 *
 * Side effects:
 *	The exact and screen colors are stored in the output arguments.
 *
 *----------------------------------------------------------------------
 */

Status
XLookupColor(
    Display *display,
    Colormap colormap,
    const char *name,
    XColor *exactPtr,
    XColor *screenPtr)
{
    if (!XParseColor(display, colormap, name, exactPtr)) {
	return 0;
    }
    *screenPtr = *exactPtr;
    return XAllocColor(display, colormap, screenPtr);
}

Status
XAllocNamedColor(
    Display *display,
    Colormap colormap,
    const char *name,
    XColor *screenPtr,
    XColor *exactPtr)
{
    return XLookupColor(display, colormap, name, exactPtr, screenPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * XQueryColors --
 *
 *	Get the components of the pixels of colors.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	The components are stored in the colors.
 *
 *----------------------------------------------------------------------
 */

int
XQueryColors(
    Display *display,
    TCL_UNUSED(Colormap),
    XColor *colors,
    int ncolors)
{
    int i;

    TkXeNextRequest(display);
    for (i = 0; i < ncolors; i++) {
	unsigned long p = colors[i].pixel;

	colors[i].red = ((p >> 16) & 0xFF) * 0x101;
	colors[i].green = ((p >> 8) & 0xFF) * 0x101;
	colors[i].blue = (p & 0xFF) * 0x101;
	colors[i].flags = DoRed | DoGreen | DoBlue;
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XFreeColors, XCreateColormap, XFreeColormap --
 *
 *	Colormaps: there is one, of the TrueColor visual, and colors are not
 *	allocated in it.
 *
 * Results:
 *	XCreateColormap returns the default colormap, the others Success.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
XFreeColors(
    Display *display,
    TCL_UNUSED(Colormap),
    TCL_UNUSED(unsigned long *),
    TCL_UNUSED(int),
    TCL_UNUSED(unsigned long))
{
    TkXeNextRequest(display);
    return Success;
}

Colormap
XCreateColormap(
    Display *display,
    TCL_UNUSED(Window),
    TCL_UNUSED(Visual *),
    TCL_UNUSED(int))
{
    TkXeNextRequest(display);
    return DefaultColormap(display, 0);
}

int
XFreeColormap(
    Display *display,
    TCL_UNUSED(Colormap))
{
    TkXeNextRequest(display);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XVisualIDFromVisual --
 *
 *	Get the id of a visual.
 *
 * Results:
 *	The id.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

VisualID
XVisualIDFromVisual(
    Visual *visual)
{
    return visual->visualid;
}

/*
 *----------------------------------------------------------------------
 *
 * XGetVisualInfo --
 *
 *	Get the visuals which match a template: the only visual is the 24-bit
 *	TrueColor one.
 *
 * Results:
 *	The visual, which the caller frees with XFree, or NULL if it does not
 *	match; the number of visuals is stored in *nitemsPtr.
 *
 * Side effects:
 *	Memory is allocated for the result.
 *
 *----------------------------------------------------------------------
 */

XVisualInfo *
XGetVisualInfo(
    Display *display,
    long vinfoMask,
    XVisualInfo *vinfoTemplate,
    int *nitemsPtr)
{
    Visual *visual = DefaultVisual(display, 0);
    XVisualInfo *info;

    *nitemsPtr = 0;
    if (((vinfoMask & VisualIDMask)
	    && (vinfoTemplate->visualid != visual->visualid))
	    || ((vinfoMask & VisualScreenMask) && (vinfoTemplate->screen != 0))
	    || ((vinfoMask & VisualDepthMask) && (vinfoTemplate->depth != 24))
	    || ((vinfoMask & VisualClassMask)
		    && (vinfoTemplate->c_class != visual->c_class))
	    || ((vinfoMask & VisualColormapSizeMask)
		    && (vinfoTemplate->colormap_size != visual->map_entries))
	    || ((vinfoMask & VisualBitsPerRGBMask)
		    && (vinfoTemplate->bits_per_rgb != visual->bits_per_rgb))
	    || ((vinfoMask & VisualRedMaskMask)
		    && (vinfoTemplate->red_mask != visual->red_mask))
	    || ((vinfoMask & VisualGreenMaskMask)
		    && (vinfoTemplate->green_mask != visual->green_mask))
	    || ((vinfoMask & VisualBlueMaskMask)
		    && (vinfoTemplate->blue_mask != visual->blue_mask))) {
	return NULL;
    }
    info = (XVisualInfo *)Tcl_Alloc(sizeof(XVisualInfo));
    info->visual = visual;
    info->visualid = visual->visualid;
    info->screen = 0;
    info->depth = 24;
    info->c_class = visual->c_class;
    info->colormap_size = visual->map_entries;
    info->bits_per_rgb = visual->bits_per_rgb;
    info->red_mask = visual->red_mask;
    info->green_mask = visual->green_mask;
    info->blue_mask = visual->blue_mask;
    *nitemsPtr = 1;
    return info;
}

/*
 *----------------------------------------------------------------------
 *
 * Keysyms and keycodes --
 *
 *	Keycodes are the evdev codes plus 8, as in X11 servers which use
 *	XKB, and the keymap of the compositor is used to translate them.
 *
 *----------------------------------------------------------------------
 */

static Tcl_HashTable keysymNameTable;	/* Maps keysyms to their names. */
static int keysymNamesInitialized = 0;
TCL_DECLARE_MUTEX(keysymMutex)

/*
 *----------------------------------------------------------------------
 *
 * XKeysymToString --
 *
 *	Get the name of a keysym, from xkbcommon.
 *
 * Results:
 *	The name, which must not be freed, or NULL if the keysym has none.
 *
 * Side effects:
 *	The names are kept in a table, since they must not be freed.
 *
 *----------------------------------------------------------------------
 */

char *
XKeysymToString(
    KeySym keysym)
{
    char buf[64];
    Tcl_HashEntry *hPtr;
    int isNew;
    char *name;

    if (xkb_keysym_get_name((xkb_keysym_t)keysym, buf, sizeof(buf)) < 0
	    || keysym == NoSymbol) {
	return NULL;
    }

    /*
     * The result must not be freed, so names are kept in a table.
     */

    Tcl_MutexLock(&keysymMutex);
    if (!keysymNamesInitialized) {
	Tcl_InitHashTable(&keysymNameTable, TCL_ONE_WORD_KEYS);
	keysymNamesInitialized = 1;
    }
    hPtr = Tcl_CreateHashEntry(&keysymNameTable, INT2PTR(keysym), &isNew);
    if (isNew) {
	name = (char *)Tcl_Alloc(strlen(buf) + 1);
	strcpy(name, buf);
	Tcl_SetHashValue(hPtr, name);
    }
    name = (char *)Tcl_GetHashValue(hPtr);
    Tcl_MutexUnlock(&keysymMutex);
    return name;
}

/*
 *----------------------------------------------------------------------
 *
 * XStringToKeysym --
 *
 *	Get the keysym of a name, from xkbcommon.
 *
 * Results:
 *	The keysym, or NoSymbol.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

KeySym
XStringToKeysym(
    const char *name)
{
    xkb_keysym_t keysym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);

    return (keysym == XKB_KEY_NoSymbol) ? NoSymbol : (KeySym)keysym;
}

/*
 *----------------------------------------------------------------------
 *
 * XKeysymToKeycode --
 *
 *	Find a keycode which produces a keysym in the keymap of the
 *	compositor.
 *	Keycodes are the evdev codes plus 8, as in X11 servers which use XKB.
 *
 * Results:
 *	The keycode, or 0 if no key produces the keysym.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

KeyCode
XKeysymToKeycode(
    Display *display,
    KeySym keysym)
{
    struct xkb_keymap *keymap = tkXeDisplay ? tkXeDisplay->keymap : NULL;
    xkb_keycode_t kc, min, max;

    (void)display;
    if (keymap == NULL) {
	return 0;
    }
    min = xkb_keymap_min_keycode(keymap);
    max = xkb_keymap_max_keycode(keymap);
    if (max > 255) {
	max = 255;
    }
    for (kc = min; kc <= max; kc++) {
	xkb_layout_index_t layout, numLayouts =
		xkb_keymap_num_layouts_for_key(keymap, kc);

	for (layout = 0; layout < numLayouts; layout++) {
	    xkb_level_index_t level, numLevels =
		    xkb_keymap_num_levels_for_key(keymap, kc, layout);

	    for (level = 0; level < numLevels; level++) {
		const xkb_keysym_t *syms;
		int i, n = xkb_keymap_key_get_syms_by_level(keymap, kc,
			layout, level, &syms);

		for (i = 0; i < n; i++) {
		    if (syms[i] == keysym) {
			return (KeyCode)kc;
		    }
		}
	    }
	}
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * XkbKeycodeToKeysym --
 *
 *	Get the keysym of a key in a group and level of the keymap of the
 *	compositor.
 *
 * Results:
 *	The keysym, or NoSymbol.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

KeySym
XkbKeycodeToKeysym(
    TCL_UNUSED(Display *),
    unsigned int keycode,
    int group,
    int level)
{
    struct xkb_keymap *keymap = tkXeDisplay ? tkXeDisplay->keymap : NULL;
    const xkb_keysym_t *syms;

    if (keymap == NULL || group < 0 || level < 0) {
	return NoSymbol;
    }
    if (xkb_keymap_key_get_syms_by_level(keymap, keycode,
	    (xkb_layout_index_t)group, (xkb_level_index_t)level,
	    &syms) < 1) {
	return NoSymbol;
    }
    return syms[0];
}

/*
 *----------------------------------------------------------------------
 *
 * XkbOpenDisplay --
 *
 *	Tk does not open displays with Xkb without an X server.
 *
 * Results:
 *	NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Display *
XkbOpenDisplay(
    TCL_UNUSED(const char *),
    TCL_UNUSED(int *),
    TCL_UNUSED(int *),
    TCL_UNUSED(int *),
    TCL_UNUSED(int *),
    TCL_UNUSED(int *))
{
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * XGetModifierMapping --
 *
 *	Get the keys of the modifiers, as X11 servers usually map them: Shift,
 *	Lock, Control, Mod1 (Alt), Mod2 (Num Lock), Mod4 (Super) and Mod5
 *	(Level 3).
 *
 * Results:
 *	The map, which the caller frees with XFreeModifiermap.
 *
 * Side effects:
 *	Memory is allocated for the map.
 *
 *----------------------------------------------------------------------
 */

XModifierKeymap *
XGetModifierMapping(
    Display *display)
{
    static const KeySym modKeysyms[8][2] = {
	{XK_Shift_L, XK_Shift_R}, {XK_Caps_Lock, NoSymbol},
	{XK_Control_L, XK_Control_R}, {XK_Alt_L, XK_Alt_R},
	{XK_Num_Lock, NoSymbol}, {NoSymbol, NoSymbol},
	{XK_Super_L, XK_Super_R}, {XK_ISO_Level3_Shift, XK_Mode_switch}
    };
    XModifierKeymap *map = (XModifierKeymap *)
	    Tcl_Alloc(sizeof(XModifierKeymap));
    int i, j;

    map->max_keypermod = 2;
    map->modifiermap = (KeyCode *)Tcl_Alloc(16 * sizeof(KeyCode));
    for (i = 0; i < 8; i++) {
	for (j = 0; j < 2; j++) {
	    map->modifiermap[2 * i + j] = (modKeysyms[i][j] == NoSymbol) ? 0
		    : XKeysymToKeycode(display, modKeysyms[i][j]);
	}
    }
    return map;
}

/*
 *----------------------------------------------------------------------
 *
 * XFreeModifiermap --
 *
 *	Free a map returned by XGetModifierMapping; NULL is ignored.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	The map is freed.
 *
 *----------------------------------------------------------------------
 */

int
XFreeModifiermap(
    XModifierKeymap *map)
{
    if (map) {
	Tcl_Free(map->modifiermap);
	Tcl_Free(map);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XRefreshKeyboardMapping, XmbLookupString --
 *
 *	The keymap of the compositor is used directly, and there are no X
 *	input
 *	methods: input methods use text-input-v3.
 *
 * Results:
 *	XRefreshKeyboardMapping returns Success, XmbLookupString 0 with no
 *	keysym.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
XRefreshKeyboardMapping(
    TCL_UNUSED(XMappingEvent *))
{
    return Success;
}

int
XmbLookupString(
    TCL_UNUSED(XIC),
    TCL_UNUSED(XKeyPressedEvent *),
    TCL_UNUSED(char *),
    TCL_UNUSED(int),
    KeySym *keysymPtr,
    Status *statusPtr)
{
    if (keysymPtr) {
	*keysymPtr = NoSymbol;
    }
    if (statusPtr) {
	*statusPtr = XLookupNone;
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TkXeXlfdParse --
 *
 *	Make a fontconfig pattern from an XLFD font name, which Tk expects to
 *	work as on X11.
 *
 * Results:
 *	The pattern, which the caller destroys, or NULL if the name is not an
 *	XLFD name.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

FcPattern *
TkXeXlfdParse(
    const char *name)
{
    FcPattern *pattern;
    const char *fields[14];
    char *copy, *p;
    int n = 0;

    if (name[0] != '-') {
	return NULL;
    }
    copy = (char *)Tcl_Alloc(strlen(name) + 1);
    strcpy(copy, name + 1);
    p = copy;
    fields[n++] = p;
    while (*p && n < 14) {
	if (*p == '-') {
	    *p = '\0';
	    fields[n++] = p + 1;
	}
	p++;
    }
    if (n < 8) {
	Tcl_Free(copy);
	return NULL;
    }
    pattern = FcPatternCreate();

    /*
     * Fields: foundry, family, weight, slant, setwidth, addstyle, pixel
     * size, point size (in tenths), ...
     */

    if (strcmp(fields[1], "*") && fields[1][0]) {
	FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)fields[1]);
    }
    if (!strcmp(fields[2], "bold") || !strcmp(fields[2], "demibold")) {
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
    } else if (strcmp(fields[2], "*")) {
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_MEDIUM);
    }
    if (!strcmp(fields[3], "i")) {
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
    } else if (!strcmp(fields[3], "o")) {
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_OBLIQUE);
    } else if (strcmp(fields[3], "*")) {
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
    }
    if (strcmp(fields[7], "*") && atoi(fields[7]) > 0) {
	FcPatternAddDouble(pattern, FC_SIZE, atoi(fields[7]) / 10.0);
    } else if (strcmp(fields[6], "*") && atoi(fields[6]) > 0) {
	FcPatternAddDouble(pattern, FC_PIXEL_SIZE, atoi(fields[6]));
    } else {
	FcPatternAddDouble(pattern, FC_SIZE, 12.0);
    }
    Tcl_Free(copy);
    return pattern;
}

/*
 *----------------------------------------------------------------------
 *
 * Fonts and GCs --
 *
 *	Tk draws text with its own font code, so server fonts are only ids.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * XLoadFont, XUnloadFont --
 *
 *	Tk draws text with its own font code, so server fonts are only ids.
 *
 * Results:
 *	XLoadFont returns 1, XUnloadFont Success.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Font
XLoadFont(
    Display *display,
    TCL_UNUSED(const char *))
{
    TkXeNextRequest(display);
    return (Font)1;
}

int
XUnloadFont(
    Display *display,
    TCL_UNUSED(Font))
{
    TkXeNextRequest(display);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XGetGCValues --
 *
 *	Get the values of a GC: a GC is its values.
 *
 * Results:
 *	1.
 *
 * Side effects:
 *	The values are copied to *values.
 *
 *----------------------------------------------------------------------
 */

Status
XGetGCValues(
    TCL_UNUSED(Display *),
    GC gc,
    TCL_UNUSED(unsigned long),
    XGCValues *values)
{
    *values = *gc;
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * XQueryColor --
 *
 *	Get the components of the pixel of a color.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	The components are stored in the color.
 *
 *----------------------------------------------------------------------
 */

int
XQueryColor(
    Display *display,
    Colormap colormap,
    XColor *colorPtr)
{
    return XQueryColors(display, colormap, colorPtr, 1);
}

/*
 *----------------------------------------------------------------------
 *
 * Functions without any effect without an X server --
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * IgnoreError --
 *
 *	The error handler before one is set: errors are ignored, where Xlib
 *	would print them and exit.
 *
 * Results:
 *	0.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
IgnoreError(
    TCL_UNUSED(Display *),
    TCL_UNUSED(XErrorEvent *))
{
    return 0;
}

static XErrorHandler errorHandler = IgnoreError;

/*
 *----------------------------------------------------------------------
 *
 * TkXeError --
 *
 *	Report an error of the last request to the error handler, as Xlib
 *	does with the errors the X server sends (here at once).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Calls the error handler (Tk's, which calls the handlers created with
 *	Tk_CreateErrorHandler).
 *
 *----------------------------------------------------------------------
 */

void
TkXeError(
    Display *display,
    XID resource,
    int code,
    int request)
{
    XErrorEvent event;

    memset(&event, 0, sizeof(XErrorEvent));
    event.type = 0;
    event.display = display;
    event.resourceid = resource;
    event.serial = display->request;
    event.error_code = (unsigned char)code;
    event.request_code = (unsigned char)request;
    event.minor_code = 0;
    errorHandler(display, &event);
}

/*
 *----------------------------------------------------------------------
 *
 * XSetErrorHandler --
 *
 *	Set the error handler, which TkXeError calls.
 *
 * Results:
 *	The previous handler.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

XErrorHandler
XSetErrorHandler(
    XErrorHandler handler)
{
    XErrorHandler prev = errorHandler;

    errorHandler = handler;
    return prev;
}

/*
 *----------------------------------------------------------------------
 *
 * XNoOp, XSynchronize, XGrabServer, XUngrabServer, XForceScreenSaver,
 * XResetScreenSaver, XListHosts --
 *
 *	Requests which have no effect without an X server.
 *
 * Results:
 *	XSynchronize returns NULL, XListHosts NULL with no hosts and access
 *	control on, the others Success or nothing.
 *
 * Side effects:
 *	The request is counted.
 *
 *----------------------------------------------------------------------
 */

int
XNoOp(
    Display *display)
{
    TkXeNextRequest(display);
    return Success;
}

int
(*XSynchronize(
    Display *display,
    TCL_UNUSED(Bool)))(Display *)
{
    (void)display;
    return NULL;
}

int
XGrabServer(
    Display *display)
{
    TkXeNextRequest(display);
    return Success;
}

int
XUngrabServer(
    Display *display)
{
    TkXeNextRequest(display);
    return Success;
}

int
XForceScreenSaver(
    Display *display,
    TCL_UNUSED(int))
{
    TkXeNextRequest(display);
    return Success;
}

int
XResetScreenSaver(
    Display *display)
{
    TkXeNextRequest(display);
    return Success;
}

XHostAddress *
XListHosts(
    TCL_UNUSED(Display *),
    int *nhostsPtr,
    Bool *statePtr)
{
    /*
     * Only the process can reach the emulated server: access control is on,
     * with no hosts allowed, which [send] takes for a secure server.
     */

    *nhostsPtr = 0;
    *statePtr = True;
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * XStringListToTextProperty --
 *
 *	Make a text property of a list of strings, separated by null bytes.
 *
 * Results:
 *	1.
 *
 * Side effects:
 *	The value, which the caller frees with XFree, is allocated.
 *
 *----------------------------------------------------------------------
 */

Status
XStringListToTextProperty(
    char **list,
    int count,
    XTextProperty *textPtr)
{
    size_t len = 0;
    int i;
    char *p;

    for (i = 0; i < count; i++) {
	len += strlen(list[i]) + 1;
    }
    textPtr->value = (unsigned char *)Tcl_Alloc(len + 1);
    p = (char *)textPtr->value;
    for (i = 0; i < count; i++) {
	strcpy(p, list[i]);
	p += strlen(list[i]) + 1;
    }
    *p = '\0';
    textPtr->encoding = XA_STRING;
    textPtr->format = 8;
    textPtr->nitems = (len > 0) ? len - 1 : 0;
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * XFilterEvent, XCreateIC, XGContextFromGC, XRootWindow, XBell --
 *
 *	Functions for things which do not exist without an X server: X input
 *	methods, server GC contexts and the bell; the root window is the
 *	emulated one.
 *
 * Results:
 *	XFilterEvent returns False, XCreateIC NULL, XGContextFromGC 0,
 *	XRootWindow the root window, XBell Success.
 *
 * Side effects:
 *	None, except that XBell counts a request.
 *
 *----------------------------------------------------------------------
 */

Bool
XFilterEvent(
    TCL_UNUSED(XEvent *),
    TCL_UNUSED(Window))
{
    return False;
}

XIC
XCreateIC(
    TCL_UNUSED(XIM), ...)
{
    return NULL;
}

GContext
XGContextFromGC(
    TCL_UNUSED(GC))
{
    return 0;
}

Window
XRootWindow(
    Display *display,
    int screen)
{
    return RootWindow(display, screen);
}

int
XBell(
    Display *display,
    TCL_UNUSED(int))
{
    TkXeNextRequest(display);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * X event queue --
 *
 *	Events are queued directly on the Tk event queue, so there is no X
 *	event queue.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * XPutBackEvent --
 *
 *	Put an event back: events are queued directly on the Tk event queue,
 *	so there is no X event queue.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	The event is queued at the head of the Tk event queue.
 *
 *----------------------------------------------------------------------
 */

int
XPutBackEvent(
    TCL_UNUSED(Display *),
    XEvent *eventPtr)
{
    Tk_QueueWindowEvent(eventPtr, TCL_QUEUE_HEAD);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * XNextEvent, XWindowEvent --
 *
 *	There is no X event queue to read events from.
 *
 * Results:
 *	BadImplementation, with an empty event.
 *
 * Side effects:
 *	The event is cleared.
 *
 *----------------------------------------------------------------------
 */

int
XNextEvent(
    TCL_UNUSED(Display *),
    XEvent *eventPtr)
{
    memset(eventPtr, 0, sizeof(XEvent));
    return BadImplementation;
}

int
XWindowEvent(
    TCL_UNUSED(Display *),
    TCL_UNUSED(Window),
    TCL_UNUSED(long),
    XEvent *eventPtr)
{
    memset(eventPtr, 0, sizeof(XEvent));
    return BadImplementation;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
