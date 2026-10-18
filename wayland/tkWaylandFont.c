/*
 * tkWaylandFont.c --
 *
 *	The font package of the Wayland port. Fonts are matched with
 *	fontconfig and drawn with Cairo. Like the Xft implementation of the
 *	X11 port (tkUnixRFont.c), which this follows, a Tk font is a sorted
 *	list of faces, and every character is drawn with the first face which
 *	has it.
 *
 * Copyright © 2002-2003 Keith Packard
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkWaylandInt.h"
#include "tkWaylandDraw.h"
#include "tkFont.h"
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <cairo-ft.h>

/*
 * A face of a font, at the size of the font. The glyphs and advances of the
 * characters are cached, since measuring is done one character at a time.
 */

typedef struct {
    FcPattern *source;		/* Pattern of the face, from FcFontSort. */
    FcCharSet *charset;		/* Characters in the face. */
    cairo_scaled_font_t *scaledFont;
				/* Created when first used. */
    Tcl_HashTable glyphTable;	/* Maps characters to GlyphInfo. */
} WlFace;

typedef struct {
    unsigned long index;	/* Glyph index. */
    int advance;		/* Rounded advance, in pixels. */
} GlyphInfo;

typedef struct {
    TkFont font;		/* Stuff used by generic font package. Must be
				 * first in structure. */
    WlFace *faces;
    int nfaces;
    FcFontSet *fontset;
    FcPattern *pattern;
} WlFont;

TCL_DECLARE_MUTEX(fontMutex)
#define LOCK Tcl_MutexLock(&fontMutex)
#define UNLOCK Tcl_MutexUnlock(&fontMutex)

static void		FinishedWithFont(WlFont *fontPtr);

void
TkpFontPkgInit(
    TCL_UNUSED(TkMainInfo *))
{
    FcInit();
}

/*
 *----------------------------------------------------------------------
 *
 * GetFace --
 *
 *	Return the index of the first face of a font which has a character,
 *	or 0 if none has it, and make sure its scaled font exists.
 *
 *----------------------------------------------------------------------
 */

static WlFace *
GetFace(
    WlFont *fontPtr,
    int c)
{
    WlFace *facePtr = &fontPtr->faces[0];
    int i;

    if (c > 0) {
	for (i = 0; i < fontPtr->nfaces; i++) {
	    FcCharSet *charset = fontPtr->faces[i].charset;

	    if (charset && FcCharSetHasChar(charset, (FcChar32)c)) {
		facePtr = &fontPtr->faces[i];
		break;
	    }
	}
    }
    if (facePtr->scaledFont == NULL) {
	FcPattern *pat = FcFontRenderPrepare(NULL, fontPtr->pattern,
		facePtr->source);
	cairo_font_face_t *fontFace;
	cairo_matrix_t fontMatrix, ctm;
	cairo_font_options_t *options;
	double pixelSize = 12.0, fixup;

	/*
	 * Use the size requested for the font: the pattern of a bitmap face
	 * (such as color emoji) has the size of its bitmaps, with a matrix
	 * which fontconfig computes to scale them; the font matrix below
	 * does the scaling, so that factor is removed from the matrix.
	 */

	if (FcPatternGetDouble(fontPtr->pattern, FC_PIXEL_SIZE, 0, &pixelSize)
		!= FcResultMatch
		&& FcPatternGetDouble(pat, FC_PIXEL_SIZE, 0, &pixelSize)
		!= FcResultMatch) {
	    pixelSize = 12.0;
	}
	if (FcPatternGetDouble(pat, "pixelsizefixupfactor", 0, &fixup)
		== FcResultMatch && fixup > 0.0) {
	    FcMatrix *matPtr, mat;

	    if (FcPatternGetMatrix(pat, FC_MATRIX, 0, &matPtr)
		    == FcResultMatch) {
		mat = *matPtr;
		mat.xx /= fixup;
		mat.xy /= fixup;
		mat.yx /= fixup;
		mat.yy /= fixup;
		FcPatternDel(pat, FC_MATRIX);
		FcPatternAddMatrix(pat, FC_MATRIX, &mat);
	    }
	}
	fontFace = cairo_ft_font_face_create_for_pattern(pat);
	cairo_matrix_init_scale(&fontMatrix, pixelSize, pixelSize);
	cairo_matrix_init_identity(&ctm);
	options = cairo_font_options_create();
	cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_ON);
	facePtr->scaledFont = cairo_scaled_font_create(fontFace, &fontMatrix,
		&ctm, options);
	cairo_font_options_destroy(options);
	cairo_font_face_destroy(fontFace);
	FcPatternDestroy(pat);
	if (cairo_scaled_font_status(facePtr->scaledFont)
		!= CAIRO_STATUS_SUCCESS) {
	    Tcl_Panic("Cannot create a usable font");
	}
    }
    return facePtr;
}

/*
 * Return the glyph of a character in a face, and its advance.
 */

static GlyphInfo *
GetGlyph(
    WlFace *facePtr,
    int c)
{
    Tcl_HashEntry *hPtr;
    int isNew;
    GlyphInfo *infoPtr;

    hPtr = Tcl_CreateHashEntry(&facePtr->glyphTable, INT2PTR(c), &isNew);
    if (!isNew) {
	return (GlyphInfo *)Tcl_GetHashValue(hPtr);
    }
    infoPtr = (GlyphInfo *)Tcl_Alloc(sizeof(GlyphInfo));
    infoPtr->index = 0;
    infoPtr->advance = 0;
    LOCK;
    {
	FT_Face face = cairo_ft_scaled_font_lock_face(facePtr->scaledFont);

	if (face) {
	    cairo_glyph_t glyph;
	    cairo_text_extents_t extents;

	    infoPtr->index = FcFreeTypeCharIndex(face, (FcChar32)c);
	    cairo_ft_scaled_font_unlock_face(facePtr->scaledFont);
	    glyph.index = infoPtr->index;
	    glyph.x = glyph.y = 0;
	    cairo_scaled_font_glyph_extents(facePtr->scaledFont, &glyph, 1,
		    &extents);
	    infoPtr->advance = (int)lround(extents.x_advance);
	}
    }
    UNLOCK;
    Tcl_SetHashValue(hPtr, infoPtr);
    return infoPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * InitFont --
 *
 *	Fill in a font from a fontconfig pattern, which it takes ownership
 *	of.
 *
 *----------------------------------------------------------------------
 */

static void
GetTkFontAttributes(
    Tk_Window tkwin,
    FcPattern *pattern,
    TkFontAttributes *faPtr)
{
    FcChar8 *family = (FcChar8 *)"Unknown";
    double size;
    int weight, slant;

    FcPatternGetString(pattern, FC_FAMILY, 0, &family);
    if (FcPatternGetDouble(pattern, FC_SIZE, 0, &size) != FcResultMatch) {
	double pixelSize;

	if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &pixelSize)
		== FcResultMatch) {
	    size = -pixelSize;
	} else {
	    size = 12.0;
	}
    }
    if (FcPatternGetInteger(pattern, FC_WEIGHT, 0, &weight)
	    != FcResultMatch) {
	weight = FC_WEIGHT_MEDIUM;
    }
    if (FcPatternGetInteger(pattern, FC_SLANT, 0, &slant) != FcResultMatch) {
	slant = FC_SLANT_ROMAN;
    }
    faPtr->family = Tk_GetUid((const char *)family);
    faPtr->size = TkFontGetPoints(tkwin, size);
    faPtr->weight = (weight > FC_WEIGHT_MEDIUM) ? TK_FW_BOLD : TK_FW_NORMAL;
    faPtr->slant = (slant > FC_SLANT_ROMAN) ? TK_FS_ITALIC : TK_FS_ROMAN;
    faPtr->underline = 0;
    faPtr->overstrike = 0;
}

static WlFont *
InitFont(
    Tk_Window tkwin,
    FcPattern *pattern,
    WlFont *fontPtr)
{
    double dpi = round(TkScalingLevel(tkwin) * 96);
    FcFontSet *set;
    FcCharSet *charset;
    FcResult result;
    FcPattern *rendered;
    cairo_font_extents_t extents;
    WlFace *facePtr;
    int i, spacing, iWidth;

    if (!fontPtr) {
	fontPtr = (WlFont *)Tcl_Alloc(sizeof(WlFont));
    }
    memset(((char *)fontPtr) + sizeof(TkFont), 0,
	    sizeof(WlFont) - sizeof(TkFont));

    FcPatternDel(pattern, FC_PIXEL_SIZE);
    FcPatternDel(pattern, FC_DPI);
    FcPatternAddDouble(pattern, FC_DPI, dpi);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    LOCK;
    set = FcFontSort(NULL, pattern, FcTrue, NULL, &result);
    UNLOCK;
    if (!set || set->nfont == 0) {
	if (set) {
	    FcFontSetDestroy(set);
	}
	Tcl_Free(fontPtr);
	return NULL;
    }
    fontPtr->fontset = set;
    fontPtr->pattern = pattern;
    fontPtr->faces = (WlFace *)Tcl_Alloc(set->nfont * sizeof(WlFace));
    fontPtr->nfaces = set->nfont;
    for (i = 0; i < set->nfont; i++) {
	facePtr = &fontPtr->faces[i];
	facePtr->source = set->fonts[i];
	facePtr->scaledFont = NULL;
	if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &charset)
		== FcResultMatch) {
	    facePtr->charset = FcCharSetCopy(charset);
	} else {
	    facePtr->charset = NULL;
	}
	Tcl_InitHashTable(&facePtr->glyphTable, TCL_ONE_WORD_KEYS);
    }

    facePtr = GetFace(fontPtr, 0);
    rendered = FcFontRenderPrepare(NULL, pattern, facePtr->source);
    GetTkFontAttributes(tkwin, rendered, &fontPtr->font.fa);
    if (FcPatternGetInteger(rendered, FC_SPACING, 0, &spacing)
	    != FcResultMatch) {
	spacing = FC_PROPORTIONAL;
    }
    FcPatternDestroy(rendered);

    cairo_scaled_font_extents(facePtr->scaledFont, &extents);
    fontPtr->font.fid = (Font)(uintptr_t)fontPtr;
    fontPtr->font.fm.ascent = (int)ceil(extents.ascent);
    fontPtr->font.fm.descent = (int)ceil(extents.descent);
    fontPtr->font.fm.maxWidth = (int)ceil(extents.max_x_advance);
    fontPtr->font.fm.fixed = (spacing != FC_PROPORTIONAL);

    /*
     * Fontconfig does not report the position or thickness of underlines;
     * use the same defaults as the Xft implementation.
     */

    fontPtr->font.underlinePos = fontPtr->font.fm.descent / 2;
    Tk_MeasureChars((Tk_Font)&fontPtr->font, "I", 1, -1, 0, &iWidth);
    fontPtr->font.underlineHeight = iWidth / 3;
    if (fontPtr->font.underlineHeight == 0) {
	fontPtr->font.underlineHeight = 1;
    }
    if (fontPtr->font.underlineHeight + fontPtr->font.underlinePos
	    > fontPtr->font.fm.descent) {
	fontPtr->font.underlineHeight = fontPtr->font.fm.descent
		- fontPtr->font.underlinePos;
	if (fontPtr->font.underlineHeight == 0) {
	    fontPtr->font.underlinePos--;
	    fontPtr->font.underlineHeight = 1;
	}
    }
    return fontPtr;
}

static void
FinishedWithFont(
    WlFont *fontPtr)
{
    int i;

    for (i = 0; i < fontPtr->nfaces; i++) {
	WlFace *facePtr = &fontPtr->faces[i];
	Tcl_HashSearch search;
	Tcl_HashEntry *hPtr;

	if (facePtr->scaledFont) {
	    cairo_scaled_font_destroy(facePtr->scaledFont);
	}
	if (facePtr->charset) {
	    FcCharSetDestroy(facePtr->charset);
	}
	for (hPtr = Tcl_FirstHashEntry(&facePtr->glyphTable, &search); hPtr;
		hPtr = Tcl_NextHashEntry(&search)) {
	    Tcl_Free(Tcl_GetHashValue(hPtr));
	}
	Tcl_DeleteHashTable(&facePtr->glyphTable);
    }
    if (fontPtr->faces) {
	Tcl_Free(fontPtr->faces);
    }
    if (fontPtr->pattern) {
	FcPatternDestroy(fontPtr->pattern);
    }
    if (fontPtr->fontset) {
	FcFontSetDestroy(fontPtr->fontset);
    }
    fontPtr->faces = NULL;
    fontPtr->nfaces = 0;
    fontPtr->pattern = NULL;
    fontPtr->fontset = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TkpGetNativeFont --
 *
 *	Map a platform-specific font name to a font: XLFD names, which Tk
 *	expects on X11, and fontconfig names.
 *
 *----------------------------------------------------------------------
 */

TkFont *
TkpGetNativeFont(
    Tk_Window tkwin,
    const char *name)
{
    FcPattern *pattern;
    WlFont *fontPtr;

    if (!strcmp(name, "fixed")) {
	pattern = FcNameParse((const FcChar8 *)"monospace:size=10");
    } else {
	pattern = TkWlXlfdParse(name);
    }
    if (pattern == NULL) {
	return NULL;
    }
    fontPtr = InitFont(tkwin, pattern, NULL);
    if (fontPtr == NULL) {
	FcPatternDestroy(pattern);
	return NULL;
    }
    return &fontPtr->font;
}

TkFont *
TkpGetFontFromAttributes(
    TkFont *tkFontPtr,
    Tk_Window tkwin,
    const TkFontAttributes *faPtr)
{
    FcPattern *pattern = FcPatternCreate();
    WlFont *fontPtr = (WlFont *)tkFontPtr;

    if (faPtr->family) {
	FcPatternAddString(pattern, FC_FAMILY,
		(const FcChar8 *)faPtr->family);
    }
    if (faPtr->size > 0.0) {
	FcPatternAddDouble(pattern, FC_SIZE, faPtr->size);
    } else if (faPtr->size < 0.0) {
	FcPatternAddDouble(pattern, FC_SIZE,
		TkFontGetPoints(tkwin, faPtr->size));
    } else {
	FcPatternAddDouble(pattern, FC_SIZE, 12.0);
    }
    FcPatternAddInteger(pattern, FC_WEIGHT,
	    (faPtr->weight == TK_FW_BOLD) ? FC_WEIGHT_BOLD : FC_WEIGHT_MEDIUM);
    FcPatternAddInteger(pattern, FC_SLANT,
	    (faPtr->slant == TK_FS_ITALIC) ? FC_SLANT_ITALIC
	    : (faPtr->slant == TK_FS_OBLIQUE) ? FC_SLANT_OBLIQUE
	    : FC_SLANT_ROMAN);

    if (fontPtr != NULL) {
	FinishedWithFont(fontPtr);
    }
    fontPtr = InitFont(tkwin, pattern, fontPtr);
    if (fontPtr == NULL) {
	FcPatternDestroy(pattern);
	return NULL;
    }
    fontPtr->font.fa.underline = faPtr->underline;
    fontPtr->font.fa.overstrike = faPtr->overstrike;
    return &fontPtr->font;
}

void
TkpDeleteFont(
    TkFont *tkFontPtr)
{
    FinishedWithFont((WlFont *)tkFontPtr);
}

void
TkpGetFontFamilies(
    Tcl_Interp *interp,
    TCL_UNUSED(Tk_Window))
{
    Tcl_Obj *resultPtr = Tcl_NewListObj(0, NULL);
    FcPattern *pattern = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, (char *)NULL);
    FcFontSet *list;
    int i;

    LOCK;
    list = FcFontList(NULL, pattern, os);
    UNLOCK;
    for (i = 0; list && i < list->nfont; i++) {
	FcChar8 *family;

	if (FcPatternGetString(list->fonts[i], FC_FAMILY, 0, &family)
		== FcResultMatch) {
	    Tcl_ListObjAppendElement(NULL, resultPtr,
		    Tcl_NewStringObj((const char *)family, TCL_INDEX_NONE));
	}
    }
    if (list) {
	FcFontSetDestroy(list);
    }
    FcObjectSetDestroy(os);
    FcPatternDestroy(pattern);
    Tcl_SetObjResult(interp, resultPtr);
}

void
TkpGetSubFonts(
    Tcl_Interp *interp,
    Tk_Font tkfont)
{
    WlFont *fontPtr = (WlFont *)tkfont;
    Tcl_Obj *resultPtr = Tcl_NewListObj(0, NULL);
    int i;

    for (i = 0; i < fontPtr->nfaces; i++) {
	FcPattern *pattern = FcFontRenderPrepare(NULL, fontPtr->pattern,
		fontPtr->faces[i].source);
	FcChar8 *family = (FcChar8 *)"Unknown";
	FcChar8 *foundry = (FcChar8 *)"Unknown";
	Tcl_Obj *objv[3];

	FcPatternGetString(pattern, FC_FAMILY, 0, &family);
	FcPatternGetString(pattern, FC_FOUNDRY, 0, &foundry);
	objv[0] = Tcl_NewStringObj((const char *)family, TCL_INDEX_NONE);
	objv[1] = Tcl_NewStringObj((const char *)foundry, TCL_INDEX_NONE);
	objv[2] = Tcl_NewStringObj("Unknown", TCL_INDEX_NONE);
	Tcl_ListObjAppendElement(NULL, resultPtr, Tcl_NewListObj(3, objv));
	FcPatternDestroy(pattern);
    }
    Tcl_SetObjResult(interp, resultPtr);
}

void
TkpGetFontAttrsForChar(
    Tk_Window tkwin,
    Tk_Font tkfont,
    int c,
    TkFontAttributes *faPtr)
{
    WlFont *fontPtr = (WlFont *)tkfont;
    WlFace *facePtr = GetFace(fontPtr, c);
    FcPattern *pattern = FcFontRenderPrepare(NULL, fontPtr->pattern,
	    facePtr->source);

    GetTkFontAttributes(tkwin, pattern, faPtr);
    FcPatternDestroy(pattern);
    faPtr->underline = fontPtr->font.fa.underline;
    faPtr->overstrike = fontPtr->font.fa.overstrike;
}

/*
 *----------------------------------------------------------------------
 *
 * Tk_MeasureChars --
 *
 *	Determine the number of characters from the string that will fit in
 *	the given horizontal span.
 *
 *----------------------------------------------------------------------
 */

int
Tk_MeasureChars(
    Tk_Font tkfont,
    const char *source,
    Tcl_Size numBytes,
    int maxLength,
    int flags,
    int *lengthPtr)
{
    WlFont *fontPtr = (WlFont *)tkfont;
    int curX = 0, curByte = 0, sawNonSpace = 0;
    int termByte = 0, termX = 0;

    while (numBytes > 0) {
	int c, newX, newByte;
	Tcl_Size clen = Tcl_UtfToUniChar(source, &c);

	if (clen <= 0) {
	    break;
	}
	source += clen;
	numBytes -= clen;
	if (c < 256 && isspace(c)) {
	    if (sawNonSpace) {
		termByte = curByte;
		termX = curX;
		sawNonSpace = 0;
	    }
	} else {
	    sawNonSpace = 1;
	}

	newX = curX + GetGlyph(GetFace(fontPtr, c), c)->advance;
	newByte = curByte + (int)clen;
	if (maxLength >= 0 && newX > maxLength) {
	    if (flags & TK_PARTIAL_OK ||
		    (flags & TK_AT_LEAST_ONE && curByte == 0)) {
		curX = newX;
		curByte = newByte;
	    } else if (flags & TK_WHOLE_WORDS) {
		if (!((flags & TK_AT_LEAST_ONE) && (termX == 0))) {
		    curX = termX;
		    curByte = termByte;
		}
	    }
	    break;
	}
	curX = newX;
	curByte = newByte;
    }
    *lengthPtr = curX;
    return curByte;
}

int
Tk_MeasureCharsInContext(
    Tk_Font tkfont,
    const char *source,
    TCL_UNUSED(Tcl_Size),
    Tcl_Size rangeStart,
    Tcl_Size rangeLength,
    int maxLength,
    int flags,
    int *lengthPtr)
{
    return Tk_MeasureChars(tkfont, source + rangeStart, rangeLength,
	    maxLength, flags, lengthPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * DrawString --
 *
 *	Draw a string with its origin at x,y, rotated by an angle in degrees
 *	(counterclockwise), and return the advance. Runs of characters of the
 *	same face are drawn together.
 *
 *----------------------------------------------------------------------
 */

#define NUM_GLYPHS 256

static void
FlushGlyphs(
    cairo_t *cr,
    WlFace *facePtr,
    cairo_glyph_t *glyphs,
    int *nPtr)
{
    if (*nPtr > 0) {
	cairo_set_scaled_font(cr, facePtr->scaledFont);
	cairo_show_glyphs(cr, glyphs, *nPtr);
	*nPtr = 0;
    }
}

static int
DrawString(
    Drawable drawable,
    GC gc,
    WlFont *fontPtr,
    const char *source,
    Tcl_Size numBytes,
    double x, double y,
    double angle)
{
    TkWlDrawContext ctx;
    cairo_glyph_t glyphs[NUM_GLYPHS];
    WlFace *currentFace = NULL;
    int n = 0, advance = 0, ascent = fontPtr->font.fm.ascent;
    int descent = fontPtr->font.fm.descent;
    bool drawing = TkWlBeginDraw(drawable, gc, &ctx);

    if (drawing) {
	cairo_save(ctx.cr);
	cairo_translate(ctx.cr, x, y);
	if (angle != 0.0) {
	    cairo_rotate(ctx.cr, -angle * M_PI / 180.0);
	}
	cairo_set_antialias(ctx.cr, CAIRO_ANTIALIAS_DEFAULT);
    }
    while (numBytes > 0) {
	int c;
	Tcl_Size clen = Tcl_UtfToUniChar(source, &c);
	WlFace *facePtr;
	GlyphInfo *infoPtr;

	if (clen <= 0) {
	    break;
	}
	source += clen;
	numBytes -= clen;
	facePtr = GetFace(fontPtr, c);
	infoPtr = GetGlyph(facePtr, c);
	if (drawing) {
	    if (facePtr != currentFace || n == NUM_GLYPHS) {
		if (currentFace) {
		    FlushGlyphs(ctx.cr, currentFace, glyphs, &n);
		}
		currentFace = facePtr;
	    }
	    glyphs[n].index = infoPtr->index;
	    glyphs[n].x = advance;
	    glyphs[n].y = 0;
	    n++;
	}
	advance += infoPtr->advance;
    }
    if (drawing) {
	if (currentFace) {
	    FlushGlyphs(ctx.cr, currentFace, glyphs, &n);
	}
	cairo_restore(ctx.cr);
	if (angle == 0.0) {
	    TkWlEndDraw(&ctx, (int)x - ascent, (int)y - ascent - 1,
		    advance + 2 * ascent, ascent + descent + 2);
	} else {
	    int r = advance + ascent + descent;

	    TkWlEndDraw(&ctx, (int)x - r - 1, (int)y - r - 1, 2 * r + 2,
		    2 * r + 2);
	}
    }
    return advance;
}

void
Tk_DrawChars(
    Display *display,
    Drawable drawable,
    GC gc,
    Tk_Font tkfont,
    const char *source,
    Tcl_Size numBytes,
    int x, int y)
{
    WlFont *fontPtr = (WlFont *)tkfont;
    int width;

    TkWlNextRequest(display);
    width = DrawString(drawable, gc, fontPtr, source, numBytes, x, y, 0.0);
    if (fontPtr->font.fa.underline != 0) {
	XFillRectangle(display, drawable, gc, x,
		y + fontPtr->font.underlinePos, (unsigned)width,
		(unsigned)fontPtr->font.underlineHeight);
    }
    if (fontPtr->font.fa.overstrike != 0) {
	y -= fontPtr->font.fm.descent + (fontPtr->font.fm.ascent) / 10;
	XFillRectangle(display, drawable, gc, x, y, (unsigned)width,
		(unsigned)fontPtr->font.underlineHeight);
    }
}

void
TkDrawAngledChars(
    Display *display,
    Drawable drawable,
    GC gc,
    Tk_Font tkfont,
    const char *source,
    Tcl_Size numBytes,
    double x, double y,
    double angle)
{
    WlFont *fontPtr = (WlFont *)tkfont;
    double width, sinA = sin(angle * M_PI / 180.0);
    double cosA = cos(angle * M_PI / 180.0);

    TkWlNextRequest(display);
    width = DrawString(drawable, gc, fontPtr, source, numBytes, x, y, angle);
    if (fontPtr->font.fa.underline || fontPtr->font.fa.overstrike) {
	double barHeight = fontPtr->font.underlineHeight;
	double offsets[2];
	int i, nbars = 0;

	if (fontPtr->font.fa.underline) {
	    offsets[nbars++] = fontPtr->font.underlinePos;
	}
	if (fontPtr->font.fa.overstrike) {
	    offsets[nbars++] = -fontPtr->font.fm.descent
		    - fontPtr->font.fm.ascent / 10;
	}
	for (i = 0; i < nbars; i++) {
	    double dy = offsets[i];
	    XPoint points[4];

	    points[0].x = (short)lround(x + dy * sinA);
	    points[0].y = (short)lround(y + dy * cosA);
	    points[1].x = (short)lround(x + dy * sinA + width * cosA);
	    points[1].y = (short)lround(y + dy * cosA - width * sinA);
	    points[2].x = (short)lround(x + (dy + barHeight) * sinA
		    + width * cosA);
	    points[2].y = (short)lround(y + (dy + barHeight) * cosA
		    - width * sinA);
	    points[3].x = (short)lround(x + (dy + barHeight) * sinA);
	    points[3].y = (short)lround(y + (dy + barHeight) * cosA);
	    XFillPolygon(display, drawable, gc, points, 4, Convex,
		    CoordModeOrigin);
	}
    }
}

void
Tk_DrawCharsInContext(
    Display *display,
    Drawable drawable,
    GC gc,
    Tk_Font tkfont,
    const char *source,
    TCL_UNUSED(Tcl_Size),
    Tcl_Size rangeStart,
    Tcl_Size rangeLength,
    int x, int y)
{
    int widthUntilStart;

    Tk_MeasureChars(tkfont, source, rangeStart, -1, 0, &widthUntilStart);
    Tk_DrawChars(display, drawable, gc, tkfont, source + rangeStart,
	    rangeLength, x + widthUntilStart, y);
}

void
TkpDrawAngledCharsInContext(
    Display *display,
    Drawable drawable,
    GC gc,
    Tk_Font tkfont,
    const char *source,
    TCL_UNUSED(Tcl_Size),
    Tcl_Size rangeStart,
    Tcl_Size rangeLength,
    double x, double y,
    double angle)
{
    int widthUntilStart;
    double sinA = sin(angle * M_PI / 180.0), cosA = cos(angle * M_PI / 180.0);

    Tk_MeasureChars(tkfont, source, rangeStart, -1, 0, &widthUntilStart);
    TkDrawAngledChars(display, drawable, gc, tkfont, source + rangeStart,
	    rangeLength, x + cosA * widthUntilStart,
	    y - sinA * widthUntilStart, angle);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
