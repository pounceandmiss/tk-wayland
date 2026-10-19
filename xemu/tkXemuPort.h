/*
 * tkXemuPort.h --
 *
 *	This file is included by all of the Tk C files. It contains
 *	information that may be configuration-dependent, such as #includes
 *	for system include files and a few other things.
 *
 *	The ports which run on the emulated X server of xemu (Wayland) share
 *	most of their code with the X11 port: Tk itself draws through the
 *	subset of Xlib declared in tkIntXlibDecls.h, which xemu implements
 *	with Cairo (as the Aqua and Windows ports implement it on top of their
 *	native APIs).
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _XEMUPORT
#define _XEMUPORT

#include "tkUnixPort.h"

/*
 * Photo images are composited by the Xlib emulation, so they can be drawn
 * with their alpha channel directly.
 */

#define TK_CAN_RENDER_RGBA
MODULE_SCOPE int TkpPutRGBAImage(
	Display *display, Drawable drawable, GC gc, XImage *image,
	int src_x, int src_y, int dest_x, int dest_y,
	unsigned int width, unsigned int height);

/*
 * The visual is 24-bit TrueColor, so a pixel is 0xRRGGBB.
 */

#define TkpGetPixel(p) ((((unsigned long) (p)->red >> 8) << 16) \
	| (((unsigned long) (p)->green >> 8) << 8) \
	| ((unsigned long) (p)->blue >> 8))

/*
 * The pointer emulation of tkPointer.c is shared with the Aqua and Windows
 * ports; these are the functions it needs from the port.
 */

struct TkWindow;
MODULE_SCOPE void	TkPointerDeadWindow(struct TkWindow *winPtr);
MODULE_SCOPE void	TkpSetCapture(struct TkWindow *winPtr);
MODULE_SCOPE Tk_Window	TkpGetCapture(void);
MODULE_SCOPE void	TkpSetCursor(Cursor cursor);

#endif /* _XEMUPORT */
