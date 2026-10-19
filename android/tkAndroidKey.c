/*
 * tkAndroidKey.c --
 *
 *	Keysyms on Android: the names of keysyms, which the Xlib emulation
 *	gets from xkbcommon on Wayland and from the table of Tk here, and the
 *	keysyms of the keys of Android.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkAndroidInt.h"
#include <android/keycodes.h>
#include <android/input.h>

/*
 * The names of the keysyms.
 */

typedef struct {
    const char *name;
    KeySym value;
} KeysymName;

static const KeysymName keysymNames[] = {
#include "ks_names.h"
    {NULL, 0}
};

static Tcl_HashTable nameToKeysym;	/* Names to keysyms. */
static Tcl_HashTable keysymToName;	/* Keysyms to their first name. */
static int namesInitialized = 0;
TCL_DECLARE_MUTEX(namesMutex)

/*
 * The keysyms of the keys which do not produce characters, and of the
 * modifier keys.
 */

typedef struct {
    int keyCode;		/* AKEYCODE_*. */
    KeySym keysym;
} KeyInfo;

static const KeyInfo specialKeys[] = {
    {AKEYCODE_DEL, XK_BackSpace},
    {AKEYCODE_FORWARD_DEL, XK_Delete},
    {AKEYCODE_ENTER, XK_Return},
    {AKEYCODE_NUMPAD_ENTER, XK_KP_Enter},
    {AKEYCODE_DPAD_CENTER, XK_Return},
    {AKEYCODE_TAB, XK_Tab},
    {AKEYCODE_ESCAPE, XK_Escape},
    {AKEYCODE_DPAD_UP, XK_Up},
    {AKEYCODE_DPAD_DOWN, XK_Down},
    {AKEYCODE_DPAD_LEFT, XK_Left},
    {AKEYCODE_DPAD_RIGHT, XK_Right},
    {AKEYCODE_MOVE_HOME, XK_Home},
    {AKEYCODE_MOVE_END, XK_End},
    {AKEYCODE_PAGE_UP, XK_Prior},
    {AKEYCODE_PAGE_DOWN, XK_Next},
    {AKEYCODE_INSERT, XK_Insert},
    {AKEYCODE_F1, XK_F1},
    {AKEYCODE_F2, XK_F2},
    {AKEYCODE_F3, XK_F3},
    {AKEYCODE_F4, XK_F4},
    {AKEYCODE_F5, XK_F5},
    {AKEYCODE_F6, XK_F6},
    {AKEYCODE_F7, XK_F7},
    {AKEYCODE_F8, XK_F8},
    {AKEYCODE_F9, XK_F9},
    {AKEYCODE_F10, XK_F10},
    {AKEYCODE_F11, XK_F11},
    {AKEYCODE_F12, XK_F12},
    {AKEYCODE_SHIFT_LEFT, XK_Shift_L},
    {AKEYCODE_SHIFT_RIGHT, XK_Shift_R},
    {AKEYCODE_CTRL_LEFT, XK_Control_L},
    {AKEYCODE_CTRL_RIGHT, XK_Control_R},
    {AKEYCODE_ALT_LEFT, XK_Alt_L},
    {AKEYCODE_ALT_RIGHT, XK_Alt_R},
    {AKEYCODE_META_LEFT, XK_Super_L},
    {AKEYCODE_META_RIGHT, XK_Super_R},
    {AKEYCODE_CAPS_LOCK, XK_Caps_Lock},
    {AKEYCODE_NUM_LOCK, XK_Num_Lock},
    {AKEYCODE_SCROLL_LOCK, XK_Scroll_Lock},
    {AKEYCODE_SYSRQ, XK_Print},
    {AKEYCODE_BREAK, XK_Pause},
    {AKEYCODE_MENU, XK_Menu},
    {AKEYCODE_SPACE, XK_space},
    {0, NoSymbol}
};

static void		InitNames(void);

/*
 *----------------------------------------------------------------------
 *
 * InitNames --
 *
 *	Fill the tables of the names of the keysyms, once.
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
InitNames(void)
{
    const KeysymName *namePtr;
    int isNew;

    Tcl_MutexLock(&namesMutex);
    if (!namesInitialized) {
	Tcl_InitHashTable(&nameToKeysym, TCL_STRING_KEYS);
	Tcl_InitHashTable(&keysymToName, TCL_ONE_WORD_KEYS);
	for (namePtr = keysymNames; namePtr->name; namePtr++) {
	    Tcl_HashEntry *hPtr = Tcl_CreateHashEntry(&nameToKeysym,
		    namePtr->name, &isNew);

	    Tcl_SetHashValue(hPtr, INT2PTR(namePtr->value));
	    hPtr = Tcl_CreateHashEntry(&keysymToName,
		    INT2PTR(namePtr->value), &isNew);
	    if (isNew) {
		Tcl_SetHashValue(hPtr, (void *)namePtr->name);
	    }
	}
	namesInitialized = 1;
    }
    Tcl_MutexUnlock(&namesMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * xkb_keysym_get_name, xkb_keysym_from_name --
 *
 *	The functions of xkbcommon which the Xlib emulation uses to convert
 *	keysyms to their names and back. Unicode keysyms are named U+hex
 *	digits, as in xkbcommon.
 *
 * Results:
 *	xkb_keysym_get_name returns the length of the name, or -1 if the
 *	keysym has none; xkb_keysym_from_name returns the keysym, or
 *	XKB_KEY_NoSymbol.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
xkb_keysym_get_name(
    xkb_keysym_t keysym,	/* The keysym. */
    char *buffer,		/* Returns the name. */
    size_t size)		/* The size of the buffer. */
{
    Tcl_HashEntry *hPtr;

    InitNames();
    hPtr = Tcl_FindHashEntry(&keysymToName, INT2PTR(keysym));
    if (hPtr) {
	return snprintf(buffer, size, "%s",
		(const char *)Tcl_GetHashValue(hPtr));
    }
    if (keysym >= 0x1000100 && keysym <= 0x110FFFF) {
	return snprintf(buffer, size, "U%04X", keysym - 0x1000000);
    }
    return -1;
}

xkb_keysym_t
xkb_keysym_from_name(
    const char *name,		/* The name. */
    TCL_UNUSED(int))		/* flags */
{
    Tcl_HashEntry *hPtr;

    InitNames();
    hPtr = Tcl_FindHashEntry(&nameToKeysym, name);
    if (hPtr) {
	return (xkb_keysym_t)PTR2INT(Tcl_GetHashValue(hPtr));
    }
    if (name[0] == 'U' && isxdigit(UCHAR(name[1]))) {
	char *end;
	unsigned long c = strtoul(name + 1, &end, 16);

	if (*end == '\0' && c >= 0x100 && c <= 0x10FFFF) {
	    return (xkb_keysym_t)(c + 0x1000000);
	}
    }
    return XKB_KEY_NoSymbol;
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidKeySym --
 *
 *	The keysym of a key of Android: the keysym of the key, if it does not
 *	produce a character, or of the character it produces.
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
TkAndroidKeySym(
    int keyCode,		/* AKEYCODE_*. */
    int metaState,		/* AMETA_*. */
    int unicode)		/* The character of the key, or 0 if unknown. */
{
    const KeyInfo *infoPtr;

    if (keyCode == AKEYCODE_TAB && (metaState & AMETA_SHIFT_ON)) {
	return XK_ISO_Left_Tab;
    }
    for (infoPtr = specialKeys; infoPtr->keysym != NoSymbol; infoPtr++) {
	if (infoPtr->keyCode == keyCode) {
	    return infoPtr->keysym;
	}
    }
    if (unicode >= 0x20 && unicode <= 0xFF && unicode != 0x7F) {
	return (KeySym)unicode;
    }
    if (unicode > 0xFF && unicode <= 0x10FFFF) {
	return (KeySym)unicode + 0x1000000;
    }
    if (keyCode >= AKEYCODE_A && keyCode <= AKEYCODE_Z) {
	return ((metaState & AMETA_SHIFT_ON) ? 'A' : 'a') + keyCode - AKEYCODE_A;
    }
    if (keyCode >= AKEYCODE_0 && keyCode <= AKEYCODE_9) {
	return '0' + keyCode - AKEYCODE_0;
    }
    return NoSymbol;
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidKeyCode --
 *
 *	The X keycode of the key of a keysym (the Android keycode plus 8),
 *	for the keys known.
 *
 * Results:
 *	The keycode, or 0.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

KeyCode
TkAndroidKeyCode(
    KeySym keysym)		/* The keysym. */
{
    const KeyInfo *infoPtr;

    for (infoPtr = specialKeys; infoPtr->keysym != NoSymbol; infoPtr++) {
	if (infoPtr->keysym == keysym) {
	    return (KeyCode)(infoPtr->keyCode + 8);
	}
    }
    if (keysym >= 'a' && keysym <= 'z') {
	return (KeyCode)(AKEYCODE_A + keysym - 'a' + 8);
    }
    if (keysym >= 'A' && keysym <= 'Z') {
	return (KeyCode)(AKEYCODE_A + keysym - 'A' + 8);
    }
    if (keysym >= '0' && keysym <= '9') {
	return (KeyCode)(AKEYCODE_0 + keysym - '0' + 8);
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidModState --
 *
 *	The X modifier state of an Android meta state.
 *
 * Results:
 *	ShiftMask etc.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

unsigned int
TkAndroidModState(
    int metaState)		/* AMETA_*. */
{
    unsigned int state = 0;

    if (metaState & AMETA_SHIFT_ON) {
	state |= ShiftMask;
    }
    if (metaState & AMETA_CAPS_LOCK_ON) {
	state |= LockMask;
    }
    if (metaState & AMETA_CTRL_ON) {
	state |= ControlMask;
    }
    if (metaState & AMETA_ALT_ON) {
	state |= Mod1Mask;
    }
    if (metaState & AMETA_NUM_LOCK_ON) {
	state |= Mod2Mask;
    }
    if (metaState & AMETA_META_ON) {
	state |= Mod4Mask;
    }
    return state;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
