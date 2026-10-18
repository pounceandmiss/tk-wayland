/*
 * Xft.h --
 *
 *	The part of the Xft API which the bidi font code of the X11 port
 *	(tkUnixBidiFont.c) uses, implemented by the Wayland port with Cairo and
 *	fontconfig (tkWaylandXft.c). The types and the macros are those of Xft,
 *	most of which are those of fontconfig.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKWAYLAND_XFT_H_
#define _TKWAYLAND_XFT_H_

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define XFT_FAMILY		FC_FAMILY
#define XFT_FOUNDRY		FC_FOUNDRY
#define XFT_SIZE		FC_SIZE
#define XFT_PIXEL_SIZE		FC_PIXEL_SIZE
#define XFT_WEIGHT		FC_WEIGHT
#define XFT_SLANT		FC_SLANT
#define XFT_SPACING		FC_SPACING
#define XFT_DPI			FC_DPI
#define XFT_ENCODING		"encoding"

#define XFT_PROPORTIONAL	FC_PROPORTIONAL
#define XFT_WEIGHT_MEDIUM	FC_WEIGHT_MEDIUM
#define XFT_WEIGHT_BOLD		FC_WEIGHT_BOLD
#define XFT_SLANT_ROMAN		FC_SLANT_ROMAN
#define XFT_SLANT_ITALIC	FC_SLANT_ITALIC
#define XFT_SLANT_OBLIQUE	FC_SLANT_OBLIQUE

typedef FcPattern XftPattern;
typedef FcFontSet XftFontSet;
typedef FcResult XftResult;
#define XftResultMatch		FcResultMatch
#define XftTypeString		FcTypeString
#define XftTypeDouble		FcTypeDouble
#define XftTypeInteger		FcTypeInteger

#define XftPatternCreate	FcPatternCreate
#define XftPatternDel		FcPatternDel
#define XftPatternBuild		FcPatternBuild
#define XftFontSetDestroy	FcFontSetDestroy
#define XftPatternAddInteger	FcPatternAddInteger
#define XftPatternAddDouble	FcPatternAddDouble
#define XftPatternAddString(p, o, s) \
	FcPatternAddString((p), (o), (const FcChar8 *)(s))
#define XftPatternGetInteger	FcPatternGetInteger
#define XftPatternGetDouble	FcPatternGetDouble
#define XftPatternGetString(p, o, n, s) \
	FcPatternGetString((p), (o), (n), (FcChar8 **)(void *)(s))

typedef struct {
    unsigned short red, green, blue, alpha;
} XRenderColor;

typedef struct {
    unsigned long pixel;
    XRenderColor color;
} XftColor;

typedef struct {
    unsigned short width, height;
    short x, y;
    short xOff, yOff;
} XGlyphInfo;

typedef struct _XftFont {
    int ascent;
    int descent;
    int height;
    int max_advance_width;
    FcCharSet *charset;
    FcPattern *pattern;
} XftFont;

typedef struct {
    XftFont *font;
    FT_UInt glyph;
    short x, y;
} XftGlyphFontSpec;

typedef struct _XftDraw XftDraw;

MODULE_SCOPE XftFont *	XftFontOpenPattern(Display *dpy, FcPattern *pattern);
MODULE_SCOPE XftFont *	XftFontOpen(Display *dpy, int screen, ...);
MODULE_SCOPE void		XftFontClose(Display *dpy, XftFont *font);
MODULE_SCOPE FT_UInt		XftCharIndex(Display *dpy, XftFont *font, FcChar32 ucs4);
MODULE_SCOPE void		XftGlyphExtents(Display *dpy, XftFont *font,
		    const FT_UInt *glyphs, int nglyphs, XGlyphInfo *extents);
MODULE_SCOPE FT_Face		XftLockFace(XftFont *font);
MODULE_SCOPE void		XftUnlockFace(XftFont *font);
MODULE_SCOPE Bool		XftDefaultSubstitute(Display *dpy, int screen,
		    FcPattern *pattern);
MODULE_SCOPE FcPattern *	XftXlfdParse(const char *xlfdOrig, Bool ignoreScalable,
		    Bool complete);
MODULE_SCOPE XftFontSet *	XftListFonts(Display *dpy, int screen, ...);
MODULE_SCOPE XftDraw *	XftDrawCreate(Display *dpy, Drawable drawable,
		    Visual *visual, Colormap colormap);
MODULE_SCOPE void		XftDrawChange(XftDraw *draw, Drawable drawable);
MODULE_SCOPE void		XftDrawDestroy(XftDraw *draw);
MODULE_SCOPE Bool		XftDrawSetClip(XftDraw *draw, Region r);
MODULE_SCOPE void		XftDrawGlyphFontSpec(XftDraw *draw, const XftColor *color,
		    const XftGlyphFontSpec *glyphs, int len);

#endif /* _TKWAYLAND_XFT_H_ */
