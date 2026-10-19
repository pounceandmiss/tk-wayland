/*
 * tkWaylandXft.c --
 *
 *	The part of Xft which the bidi font code of the X11 port
 *	(tkUnixBidiFont.c) uses, implemented with Cairo and fontconfig, so
 *	that the Wayland port has the same text layout, shaping and
 *	bidirectional text as the X11 port. Fonts are Cairo scaled fonts made
 *	from fontconfig patterns, and glyphs are drawn with the drawing
 *	contexts of tkWaylandDraw.c.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkWaylandInt.h"
#include "tkWaylandDraw.h"
#include <X11/Xft/Xft.h>
#include <fontconfig/fcfreetype.h>
#include <cairo-ft.h>
#include <stdarg.h>

typedef struct {
    XftFont pub;		/* The fields Xft makes public. */
    cairo_scaled_font_t *scaledFont;
} WlXftFont;

struct _XftDraw {
    Display *display;
    Drawable drawable;
    Region clip;		/* Clip region set with XftDrawSetClip, or
				 * NULL. */
};

/*
 *----------------------------------------------------------------------
 *
 * XftFontOpenPattern --
 *
 *	Open the font of a pattern returned by fontconfig, which the font
 *	takes ownership of on success.
 *
 *	The size of the font is the pixel size of the pattern, transformed by
 *	the matrix of the pattern: for rotated fonts, the rotation, and for
 *	bitmap faces, the factor which scales their bitmaps to the requested
 *	size. Both go into the font matrix of Cairo.
 *
 * Results:
 *	The font, or NULL if it cannot be opened (the pattern is then still
 *	owned by the caller).
 *
 * Side effects:
 *	Allocates the font and a Cairo scaled font.
 *
 *----------------------------------------------------------------------
 */

XftFont *
XftFontOpenPattern(
    TCL_UNUSED(Display *),
    FcPattern *pattern)
{
    WlXftFont *fontPtr;
    FcPattern *facePattern;
    cairo_font_face_t *fontFace;
    cairo_scaled_font_t *scaledFont;
    cairo_matrix_t fontMatrix, ctm;
    cairo_font_options_t *options;
    cairo_font_extents_t extents;
    FcMatrix mat, *matPtr;
    double pixelSize, size, dpi;
    bool rotated;

    if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &pixelSize)
	    != FcResultMatch) {
	if (FcPatternGetDouble(pattern, FC_SIZE, 0, &size) != FcResultMatch) {
	    size = 12.0;
	}
	if (FcPatternGetDouble(pattern, FC_DPI, 0, &dpi) != FcResultMatch) {
	    dpi = 96.0;
	}
	pixelSize = size * dpi / 72.0;
    }
    FcMatrixInit(&mat);
    if (FcPatternGetMatrix(pattern, FC_MATRIX, 0, &matPtr) == FcResultMatch) {
	mat = *matPtr;
    }
    rotated = (mat.xy != 0.0 || mat.yx != 0.0);

    /*
     * Fontconfig matrices have y up, Cairo matrices y down.
     */

    cairo_matrix_init(&fontMatrix, mat.xx * pixelSize, -mat.yx * pixelSize,
	    -mat.xy * pixelSize, mat.yy * pixelSize, 0, 0);
    cairo_matrix_init_identity(&ctm);

    facePattern = FcPatternDuplicate(pattern);
    FcPatternDel(facePattern, FC_MATRIX);
    fontFace = cairo_ft_font_face_create_for_pattern(facePattern);
    FcPatternDestroy(facePattern);
    if (fontFace == NULL) {
	return NULL;
    }
    options = cairo_font_options_create();
    cairo_font_options_set_hint_metrics(options,
	    rotated ? CAIRO_HINT_METRICS_OFF : CAIRO_HINT_METRICS_ON);
    scaledFont = cairo_scaled_font_create(fontFace, &fontMatrix, &ctm,
	    options);
    cairo_font_options_destroy(options);
    cairo_font_face_destroy(fontFace);
    if (cairo_scaled_font_status(scaledFont) != CAIRO_STATUS_SUCCESS) {
	cairo_scaled_font_destroy(scaledFont);
	return NULL;
    }

    fontPtr = (WlXftFont *)Tcl_Alloc(sizeof(WlXftFont));
    fontPtr->scaledFont = scaledFont;
    cairo_scaled_font_extents(scaledFont, &extents);
    fontPtr->pub.ascent = (int)ceil(extents.ascent);
    fontPtr->pub.descent = (int)ceil(extents.descent);
    fontPtr->pub.height = (int)ceil(extents.height);
    fontPtr->pub.max_advance_width = (int)ceil(extents.max_x_advance);
    if (FcPatternGetCharSet(pattern, FC_CHARSET, 0, &fontPtr->pub.charset)
	    != FcResultMatch) {
	fontPtr->pub.charset = NULL;
    }
    fontPtr->pub.pattern = pattern;
    return &fontPtr->pub;
}

/*
 *----------------------------------------------------------------------
 *
 * XftFontOpen --
 *
 *	Open the font matching a list of pattern elements, ended by NULL.
 *
 * Results:
 *	The font, or NULL if no font matches or it cannot be opened.
 *
 * Side effects:
 *	Allocates the font, which owns the matched pattern.
 *
 *----------------------------------------------------------------------
 */

XftFont *
XftFontOpen(
    Display *display,
    int screen,
    ...)
{
    va_list args;
    FcPattern *pattern, *match;
    FcResult result;
    XftFont *fontPtr;

    va_start(args, screen);
    pattern = FcPatternVaBuild(NULL, args);
    va_end(args);
    if (pattern == NULL) {
	return NULL;
    }
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (match == NULL) {
	return NULL;
    }
    fontPtr = XftFontOpenPattern(display, match);
    if (fontPtr == NULL) {
	FcPatternDestroy(match);
    }
    return fontPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * XftFontClose --
 *
 *	Close a font opened by XftFontOpenPattern or XftFontOpen.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees the font, its Cairo scaled font and its pattern.
 *
 *----------------------------------------------------------------------
 */

void
XftFontClose(
    TCL_UNUSED(Display *),
    XftFont *font)
{
    WlXftFont *fontPtr = (WlXftFont *)font;

    cairo_scaled_font_destroy(fontPtr->scaledFont);
    FcPatternDestroy(fontPtr->pub.pattern);
    Tcl_Free(fontPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * XftLockFace, XftUnlockFace --
 *
 *	Lock the FreeType face of a font, for direct use with FreeType, and
 *	unlock it.
 *
 * Results:
 *	XftLockFace returns the face, or NULL if it cannot be locked.
 *	XftUnlockFace returns nothing.
 *
 * Side effects:
 *	The face is locked by Cairo until it is unlocked.
 *
 *----------------------------------------------------------------------
 */

FT_Face
XftLockFace(
    XftFont *font)
{
    return cairo_ft_scaled_font_lock_face(((WlXftFont *)font)->scaledFont);
}

void
XftUnlockFace(
    XftFont *font)
{
    cairo_ft_scaled_font_unlock_face(((WlXftFont *)font)->scaledFont);
}

/*
 *----------------------------------------------------------------------
 *
 * XftCharIndex --
 *
 *	The glyph of a character in a font.
 *
 * Results:
 *	The index of the glyph, or 0 if the font has none for the character.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

FT_UInt
XftCharIndex(
    TCL_UNUSED(Display *),
    XftFont *font,
    FcChar32 ucs4)
{
    FT_Face face = XftLockFace(font);
    FT_UInt index = 0;

    if (face) {
	index = FcFreeTypeCharIndex(face, ucs4);
	XftUnlockFace(font);
    }
    return index;
}

/*
 *----------------------------------------------------------------------
 *
 * XftGlyphExtents --
 *
 *	The ink extents and the advance of a run of glyphs. Advances are
 *	rounded to whole pixels, as with hinted Xft fonts.
 *
 * Results:
 *	None; the extents are stored in *extents.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
XftGlyphExtents(
    TCL_UNUSED(Display *),
    XftFont *font,
    const FT_UInt *glyphs,
    int nglyphs,
    XGlyphInfo *extents)
{
    cairo_scaled_font_t *scaledFont = ((WlXftFont *)font)->scaledFont;
    double x = 0.0, y = 0.0;
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    bool ink = false;
    int i;

    for (i = 0; i < nglyphs; i++) {
	cairo_glyph_t glyph;
	cairo_text_extents_t te;

	glyph.index = glyphs[i];
	glyph.x = glyph.y = 0;
	cairo_scaled_font_glyph_extents(scaledFont, &glyph, 1, &te);
	if (te.width > 0 && te.height > 0) {
	    double gx1 = x + te.x_bearing, gy1 = y + te.y_bearing;
	    double gx2 = gx1 + te.width, gy2 = gy1 + te.height;

	    if (!ink) {
		x1 = gx1;
		y1 = gy1;
		x2 = gx2;
		y2 = gy2;
		ink = true;
	    } else {
		x1 = (gx1 < x1) ? gx1 : x1;
		y1 = (gy1 < y1) ? gy1 : y1;
		x2 = (gx2 > x2) ? gx2 : x2;
		y2 = (gy2 > y2) ? gy2 : y2;
	    }
	}
	x += round(te.x_advance);
	y += round(te.y_advance);
    }
    x1 = floor(x1);
    y1 = floor(y1);
    extents->x = (short)-x1;
    extents->y = (short)-y1;
    extents->width = (unsigned short)(ink ? ceil(x2) - x1 : 0);
    extents->height = (unsigned short)(ink ? ceil(y2) - y1 : 0);
    extents->xOff = (short)x;
    extents->yOff = (short)y;
}

/*
 *----------------------------------------------------------------------
 *
 * Patterns and lists --
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * XftDefaultSubstitute --
 *
 *	Add the default values of fontconfig to a pattern.
 *
 * Results:
 *	True.
 *
 * Side effects:
 *	The pattern is modified.
 *
 *----------------------------------------------------------------------
 */

Bool
XftDefaultSubstitute(
    TCL_UNUSED(Display *),
    TCL_UNUSED(int),
    FcPattern *pattern)
{
    FcDefaultSubstitute(pattern);
    return True;
}

/*
 *----------------------------------------------------------------------
 *
 * XftXlfdParse --
 *
 *	Make a pattern from an XLFD font name.
 *
 * Results:
 *	The new pattern, or NULL if the name is not an XLFD name.
 *
 * Side effects:
 *	Allocates the pattern, which the caller destroys.
 *
 *----------------------------------------------------------------------
 */

FcPattern *
XftXlfdParse(
    const char *name,
    TCL_UNUSED(Bool),
    TCL_UNUSED(Bool))
{
    return TkWlXlfdParse(name);
}

/*
 *----------------------------------------------------------------------
 *
 * XftListFonts --
 *
 *	List the fonts matching a list of pattern elements ended by NULL, with
 *	the objects in a list of object names ended by NULL.
 *
 * Results:
 *	The set of the fonts found, which the caller destroys.
 *
 * Side effects:
 *	Allocates the font set.
 *
 *----------------------------------------------------------------------
 */

XftFontSet *
XftListFonts(
    TCL_UNUSED(Display *),
    int screen,
    ...)
{
    va_list args;
    FcPattern *pattern;
    FcObjectSet *objects;
    FcFontSet *list;
    const char *first;

    va_start(args, screen);
    pattern = FcPatternVaBuild(NULL, args);
    first = va_arg(args, const char *);
    objects = FcObjectSetVaBuild(first, args);
    va_end(args);
    list = FcFontList(NULL, pattern, objects);
    FcObjectSetDestroy(objects);
    FcPatternDestroy(pattern);
    return list;
}

/*
 *----------------------------------------------------------------------
 *
 * Drawing --
 *
 *	Xft does not use the clip region of the GC, but its own, which the
 *	X11 code sets with XftDrawSetClip.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * XftDrawCreate --
 *
 *	Make the drawing context of Xft for a drawable.
 *
 * Results:
 *	The new drawing context.
 *
 * Side effects:
 *	Allocates the drawing context.
 *
 *----------------------------------------------------------------------
 */

XftDraw *
XftDrawCreate(
    Display *display,
    Drawable drawable,
    TCL_UNUSED(Visual *),
    TCL_UNUSED(Colormap))
{
    XftDraw *draw = (XftDraw *)Tcl_Alloc(sizeof(XftDraw));

    draw->display = display;
    draw->drawable = drawable;
    draw->clip = NULL;
    return draw;
}

/*
 *----------------------------------------------------------------------
 *
 * XftDrawChange --
 *
 *	Change the drawable of a drawing context.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The context draws into the new drawable.
 *
 *----------------------------------------------------------------------
 */

void
XftDrawChange(
    XftDraw *draw,
    Drawable drawable)
{
    draw->drawable = drawable;
}

/*
 *----------------------------------------------------------------------
 *
 * XftDrawDestroy --
 *
 *	Destroy a drawing context.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees the context and its clip region.
 *
 *----------------------------------------------------------------------
 */

void
XftDrawDestroy(
    XftDraw *draw)
{
    if (draw->clip) {
	XDestroyRegion(draw->clip);
    }
    Tcl_Free(draw);
}

/*
 *----------------------------------------------------------------------
 *
 * XftDrawSetClip --
 *
 *	Set the clip region of a drawing context, or remove it if the region
 *	is NULL.
 *
 * Results:
 *	True.
 *
 * Side effects:
 *	The context keeps a copy of the region, and frees its previous one.
 *
 *----------------------------------------------------------------------
 */

Bool
XftDrawSetClip(
    XftDraw *draw,
    Region r)
{
    if (draw->clip) {
	XDestroyRegion(draw->clip);
	draw->clip = NULL;
    }
    if (r) {
	draw->clip = XCreateRegion();
	XUnionRegion(r, r, draw->clip);
    }
    return True;
}

/*
 *----------------------------------------------------------------------
 *
 * XftDrawGlyphFontSpec --
 *
 *	Draw glyphs, each with its font and position, in a color.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The glyphs are drawn in the drawable of the context, clipped by its
 *	clip region, and the area drawn is damaged.
 *
 *----------------------------------------------------------------------
 */

void
XftDrawGlyphFontSpec(
    XftDraw *draw,
    const XftColor *color,
    const XftGlyphFontSpec *glyphs,
    int len)
{
    TkWlDrawContext ctx;
    cairo_glyph_t *cairoGlyphs;
    GC gc;
    int i, start, x1, y1, x2, y2;

    if (len <= 0) {
	return;
    }
    gc = XCreateGC(draw->display, draw->drawable, 0, NULL);
    if (draw->clip) {
	XSetRegion(draw->display, gc, draw->clip);
    }
    if (!TkWlBeginDraw(draw->drawable, gc, &ctx)) {
	XFreeGC(draw->display, gc);
	return;
    }
    cairo_set_antialias(ctx.cr, CAIRO_ANTIALIAS_DEFAULT);
    cairo_set_source_rgba(ctx.cr, color->color.red / 65535.0,
	    color->color.green / 65535.0, color->color.blue / 65535.0,
	    color->color.alpha / 65535.0);

    /*
     * Draw the runs of glyphs of the same font, and compute the extent of
     * the damage, generously since glyphs may be rotated.
     */

    cairoGlyphs = (cairo_glyph_t *)Tcl_Alloc(len * sizeof(cairo_glyph_t));
    x1 = x2 = glyphs[0].x;
    y1 = y2 = glyphs[0].y;
    for (start = 0; start < len; start = i) {
	XftFont *font = glyphs[start].font;
	int pad = font->height + font->max_advance_width;

	for (i = start; i < len && glyphs[i].font == font; i++) {
	    cairoGlyphs[i - start].index = glyphs[i].glyph;
	    cairoGlyphs[i - start].x = glyphs[i].x;
	    cairoGlyphs[i - start].y = glyphs[i].y;
	    if (glyphs[i].x - pad < x1) x1 = glyphs[i].x - pad;
	    if (glyphs[i].y - pad < y1) y1 = glyphs[i].y - pad;
	    if (glyphs[i].x + pad > x2) x2 = glyphs[i].x + pad;
	    if (glyphs[i].y + pad > y2) y2 = glyphs[i].y + pad;
	}
	cairo_set_scaled_font(ctx.cr, ((WlXftFont *)font)->scaledFont);
	cairo_show_glyphs(ctx.cr, cairoGlyphs, i - start);
    }
    Tcl_Free(cairoGlyphs);
    TkWlEndDraw(&ctx, x1, y1, x2 - x1 + 1, y2 - y1 + 1);
    XFreeGC(draw->display, gc);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
