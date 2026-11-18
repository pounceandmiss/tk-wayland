/*
 * tkXemuXkb.h --
 *
 *	The part of xkbcommon which the Xlib emulation uses, for the ports
 *	which do not have xkbcommon (Android, Haiku): the names of keysyms,
 *	implemented by tkXemuKeysym.c with the table of Tk, and a keymap,
 *	which these ports do not have (they translate keys themselves, and
 *	TkpSetKeycodeAndState gives the keycodes of keysyms).
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKXEMUXKB
#define _TKXEMUXKB

#include <stdint.h>

typedef uint32_t xkb_keysym_t;
typedef uint32_t xkb_keycode_t;
typedef uint32_t xkb_layout_index_t;
typedef uint32_t xkb_level_index_t;
struct xkb_keymap;

#define XKB_KEY_NoSymbol	0
#define XKB_KEYSYM_NO_FLAGS	0

MODULE_SCOPE int	xkb_keysym_get_name(xkb_keysym_t keysym, char *buffer,
			    size_t size);
MODULE_SCOPE xkb_keysym_t xkb_keysym_from_name(const char *name, int flags);

static inline xkb_keycode_t
xkb_keymap_min_keycode(
    TCL_UNUSED(struct xkb_keymap *))
{
    return 0;
}

static inline xkb_keycode_t
xkb_keymap_max_keycode(
    TCL_UNUSED(struct xkb_keymap *))
{
    return 0;
}

static inline xkb_layout_index_t
xkb_keymap_num_layouts_for_key(
    TCL_UNUSED(struct xkb_keymap *),
    TCL_UNUSED(xkb_keycode_t))
{
    return 0;
}

static inline xkb_level_index_t
xkb_keymap_num_levels_for_key(
    TCL_UNUSED(struct xkb_keymap *),
    TCL_UNUSED(xkb_keycode_t),
    TCL_UNUSED(xkb_layout_index_t))
{
    return 0;
}

static inline int
xkb_keymap_key_get_syms_by_level(
    TCL_UNUSED(struct xkb_keymap *),
    TCL_UNUSED(xkb_keycode_t),
    TCL_UNUSED(xkb_layout_index_t),
    TCL_UNUSED(xkb_level_index_t),
    TCL_UNUSED(const xkb_keysym_t **))
{
    return 0;
}

#endif /* _TKXEMUXKB */
