/*
 * tkHaikuKey.c --
 *
 *	Keys on Haiku: the keysyms of the keys and modifiers, and the
 *	keycodes of keysyms, with the keymap of the user. (The names of
 *	keysyms are those of ../xemu/tkXemuKeysym.c.)
 *
 *	The key codes of the Be API are the positions of the keys on the
 *	keyboard; an X keycode is a key code plus 8. The keys which do not
 *	type characters have fixed keysyms; the others have the keysym of the
 *	character they type with Shift, the locks and Option, as with xkb.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkHaikuInt.h"

/*
 * The keys which do not type characters, and the keypad.
 */

typedef struct {
    int key;			/* Key code of the Be API. */
    KeySym keysym;
    KeySym numLocked;		/* On the keypad, with Num Lock. */
} KeyInfo;

static const KeyInfo specialKeys[] = {
    {0x01, XK_Escape, NoSymbol},
    {0x02, XK_F1, NoSymbol},
    {0x03, XK_F2, NoSymbol},
    {0x04, XK_F3, NoSymbol},
    {0x05, XK_F4, NoSymbol},
    {0x06, XK_F5, NoSymbol},
    {0x07, XK_F6, NoSymbol},
    {0x08, XK_F7, NoSymbol},
    {0x09, XK_F8, NoSymbol},
    {0x0a, XK_F9, NoSymbol},
    {0x0b, XK_F10, NoSymbol},
    {0x0c, XK_F11, NoSymbol},
    {0x0d, XK_F12, NoSymbol},
    {0x0e, XK_Print, NoSymbol},
    {0x0f, XK_Scroll_Lock, NoSymbol},
    {0x10, XK_Pause, NoSymbol},
    {0x1e, XK_BackSpace, NoSymbol},
    {0x1f, XK_Insert, NoSymbol},
    {0x20, XK_Home, NoSymbol},
    {0x21, XK_Prior, NoSymbol},
    {0x22, XK_Num_Lock, NoSymbol},
    {0x23, XK_KP_Divide, XK_KP_Divide},
    {0x24, XK_KP_Multiply, XK_KP_Multiply},
    {0x25, XK_KP_Subtract, XK_KP_Subtract},
    {0x26, XK_Tab, NoSymbol},
    {0x34, XK_Delete, NoSymbol},
    {0x35, XK_End, NoSymbol},
    {0x36, XK_Next, NoSymbol},
    {0x37, XK_KP_Home, XK_KP_7},
    {0x38, XK_KP_Up, XK_KP_8},
    {0x39, XK_KP_Prior, XK_KP_9},
    {0x3a, XK_KP_Add, XK_KP_Add},
    {0x3b, XK_Caps_Lock, NoSymbol},
    {0x47, XK_Return, NoSymbol},
    {0x48, XK_KP_Left, XK_KP_4},
    {0x49, XK_KP_Begin, XK_KP_5},
    {0x4a, XK_KP_Right, XK_KP_6},
    {0x4b, XK_Shift_L, NoSymbol},
    {0x56, XK_Shift_R, NoSymbol},
    {0x57, XK_Up, NoSymbol},
    {0x58, XK_KP_End, XK_KP_1},
    {0x59, XK_KP_Down, XK_KP_2},
    {0x5a, XK_KP_Next, XK_KP_3},
    {0x5b, XK_KP_Enter, XK_KP_Enter},
    {0x5c, XK_Control_L, NoSymbol},
    {0x5d, XK_Alt_L, NoSymbol},
    {0x5e, XK_space, NoSymbol},
    {0x5f, XK_Alt_R, NoSymbol},
    {0x60, XK_Control_R, NoSymbol},
    {0x61, XK_Left, NoSymbol},
    {0x62, XK_Down, NoSymbol},
    {0x63, XK_Right, NoSymbol},
    {0x64, XK_KP_Insert, XK_KP_0},
    {0x65, XK_KP_Delete, XK_KP_Decimal},
    {0x66, XK_Super_L, NoSymbol},
    {0x67, XK_Super_R, NoSymbol},
    {0x68, XK_Menu, NoSymbol},
    {0, NoSymbol, NoSymbol}
};

static KeySym		CharKeySym(const char *chars);

/*
 *----------------------------------------------------------------------
 *
 * CharKeySym --
 *
 *	The keysym of the first character of a string: the character itself
 *	for Latin-1, otherwise its Unicode keysym.
 *
 * Results:
 *	The keysym, or NoSymbol for a control character.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static KeySym
CharKeySym(
    const char *chars)		/* UTF-8. */
{
    int c;

    if (chars[0] == '\0') {
	return NoSymbol;
    }
    Tcl_UtfToUniChar(chars, &c);
    if (c < 0x20 || c == 0x7F) {
	return NoSymbol;
    }
    return (c <= 0xFF) ? (KeySym)c : (KeySym)c + 0x1000000;
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuKeySym --
 *
 *	The keysym of a key: its fixed keysym if it does not type characters,
 *	or that of the character it types without Control and Command.
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
TkHaikuKeySym(
    int key,			/* Key code of the Be API. */
    uint32_t modifiers,		/* TKHAIKU_SHIFT etc. */
    const char *bytes)		/* The characters typed, or "". */
{
    const KeyInfo *infoPtr;
    char buf[16];

    if (key == 0x26 && (modifiers & TKHAIKU_SHIFT)) {
	return XK_ISO_Left_Tab;
    }
    for (infoPtr = specialKeys; infoPtr->keysym != NoSymbol; infoPtr++) {
	if (infoPtr->key == key) {
	    if (infoPtr->numLocked != NoSymbol
		    && !(modifiers & TKHAIKU_NUM_LOCK)
			    != !(modifiers & TKHAIKU_SHIFT)) {
		return infoPtr->numLocked;
	    }
	    return infoPtr->keysym;
	}
    }
    if (!(modifiers & (TKHAIKU_CONTROL | TKHAIKU_COMMAND))
	    && CharKeySym(bytes) != NoSymbol) {
	return CharKeySym(bytes);
    }
    if (TkHaikuKeymapChars(key, modifiers & (TKHAIKU_SHIFT
	    | TKHAIKU_CAPS_LOCK | TKHAIKU_OPTION), buf, sizeof(buf)) > 0) {
	return CharKeySym(buf);
    }
    return NoSymbol;
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuKeyCode --
 *
 *	The X keycode of a keysym: the key which has it, without Shift or
 *	with it.
 *
 * Results:
 *	The keycode, or 0; *shiftPtr tells whether Shift is needed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

KeyCode
TkHaikuKeyCode(
    KeySym keysym,		/* The keysym. */
    bool *shiftPtr)		/* Returns whether Shift gives the keysym. */
{
    const KeyInfo *infoPtr;
    int key, level;
    char buf[16];

    *shiftPtr = false;
    if (keysym == XK_ISO_Left_Tab) {
	*shiftPtr = true;
	return 0x26 + 8;
    }
    for (infoPtr = specialKeys; infoPtr->keysym != NoSymbol; infoPtr++) {
	if (infoPtr->keysym == keysym) {
	    return (KeyCode)(infoPtr->key + 8);
	}
    }
    for (infoPtr = specialKeys; infoPtr->keysym != NoSymbol; infoPtr++) {
	if (infoPtr->numLocked == keysym) {
	    return (KeyCode)(infoPtr->key + 8);
	}
    }
    for (level = 0; level < 2; level++) {
	for (key = 1; key < 128; key++) {
	    if (TkHaikuKeymapChars(key, level ? TKHAIKU_SHIFT : 0, buf,
		    sizeof(buf)) > 0 && CharKeySym(buf) == keysym) {
		*shiftPtr = (level == 1);
		return (KeyCode)(key + 8);
	    }
	}
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TkHaikuModState --
 *
 *	The X modifier state of the modifiers of Haiku: Command (the Alt key)
 *	is Mod1, Num Lock Mod2 and Option (the Windows key) Mod4, as usual on
 *	X11.
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
TkHaikuModState(
    uint32_t modifiers)		/* TKHAIKU_SHIFT etc. */
{
    unsigned int state = 0;

    if (modifiers & TKHAIKU_SHIFT) {
	state |= ShiftMask;
    }
    if (modifiers & TKHAIKU_CAPS_LOCK) {
	state |= LockMask;
    }
    if (modifiers & TKHAIKU_CONTROL) {
	state |= ControlMask;
    }
    if (modifiers & TKHAIKU_COMMAND) {
	state |= Mod1Mask;
    }
    if (modifiers & TKHAIKU_NUM_LOCK) {
	state |= Mod2Mask;
    }
    if (modifiers & TKHAIKU_OPTION) {
	state |= Mod4Mask;
    }
    return state;
}

/*
 *----------------------------------------------------------------------
 *
 * TkpInitKeymapInfo --
 *
 *	Set the modifier information of a display: the keys which are
 *	modifiers, and the masks of Meta and Alt.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The fields of the display are set.
 *
 *----------------------------------------------------------------------
 */

void
TkpInitKeymapInfo(
    TkDisplay *dispPtr)		/* The display. */
{
    static const KeySym modKeysyms[] = {
	XK_Shift_L, XK_Shift_R, XK_Control_L, XK_Control_R, XK_Caps_Lock,
	XK_Alt_L, XK_Alt_R, XK_Super_L, XK_Super_R, XK_Num_Lock
    };
    size_t i;

    dispPtr->bindInfoStale = 0;
    dispPtr->lockUsage = LU_CAPS;
    dispPtr->modeModMask = 0;
    dispPtr->metaModMask = Mod1Mask;
    dispPtr->altModMask = Mod1Mask;
    if (dispPtr->modKeyCodes != NULL) {
	Tcl_Free(dispPtr->modKeyCodes);
    }
    dispPtr->modKeyCodes = (KeyCode *)Tcl_Alloc(
	    sizeof(modKeysyms) / sizeof(modKeysyms[0]) * sizeof(KeyCode));
    dispPtr->numModKeyCodes = 0;
    for (i = 0; i < sizeof(modKeysyms) / sizeof(modKeysyms[0]); i++) {
	bool shift;
	KeyCode code = TkHaikuKeyCode(modKeysyms[i], &shift);

	if (code != 0) {
	    dispPtr->modKeyCodes[dispPtr->numModKeyCodes++] = code;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkpGetKeySym --
 *
 *	The keysym of a key event: the one stored when it was generated, or
 *	that of its keycode for events which were not.
 *
 * Results:
 *	The keysym, or NoSymbol.
 *
 * Side effects:
 *	May update the keymap information.
 *
 *----------------------------------------------------------------------
 */

KeySym
TkpGetKeySym(
    TkDisplay *dispPtr,		/* The display. */
    XEvent *eventPtr)		/* A key event. */
{
    TkKeyEvent *kePtr = (TkKeyEvent *)eventPtr;

    if (dispPtr->bindInfoStale) {
	TkpInitKeymapInfo(dispPtr);
    }
    if (kePtr->keysym != NoSymbol) {
	return kePtr->keysym;
    }
    if (eventPtr->xkey.keycode < 8) {
	return NoSymbol;
    }
    return TkHaikuKeySym((int)eventPtr->xkey.keycode - 8,
	    (eventPtr->xkey.state & ShiftMask) ? TKHAIKU_SHIFT : 0, "");
}

/*
 *----------------------------------------------------------------------
 *
 * TkpGetString --
 *
 *	The characters of a key event: those stored when it was generated,
 *	or those of its keysym for events which were not.
 *
 * Results:
 *	The string, in the DString.
 *
 * Side effects:
 *	The string is stored in the event.
 *
 *----------------------------------------------------------------------
 */

const char *
TkpGetString(
    TCL_UNUSED(TkWindow *),
    XEvent *eventPtr,		/* A key event. */
    Tcl_DString *dsPtr)		/* Initialized; returns the string. */
{
    TkKeyEvent *kePtr = (TkKeyEvent *)eventPtr;

    if (kePtr->charValuePtr == NULL) {
	char buf[8];
	int len = 0;

	if (eventPtr->type == KeyPress) {
	    KeySym keysym = kePtr->keysym;

	    if (keysym >= 0x20 && keysym <= 0xFF && keysym != 0x7F) {
		len = Tcl_UniCharToUtf((int)keysym, buf);
	    } else if (keysym >= 0x1000100 && keysym <= 0x110FFFF) {
		len = Tcl_UniCharToUtf((int)(keysym - 0x1000000), buf);
	    }
	}
	kePtr->charValuePtr = (char *)Tcl_Alloc(len + 1);
	memcpy(kePtr->charValuePtr, buf, len);
	kePtr->charValuePtr[len] = '\0';
	kePtr->charValueLen = len;
    }
    Tcl_DStringSetLength(dsPtr, kePtr->charValueLen);
    memcpy(Tcl_DStringValue(dsPtr), kePtr->charValuePtr,
	    kePtr->charValueLen + 1);
    return Tcl_DStringValue(dsPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TkpSetKeycodeAndState --
 *
 *	Set the keycode of an event generated with [event generate], and add
 *	Shift to its state if the keysym is the one of the key with Shift, as
 *	on X11.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The event is changed.
 *
 *----------------------------------------------------------------------
 */

void
TkpSetKeycodeAndState(
    TCL_UNUSED(Tk_Window),
    KeySym keySym,		/* The keysym of the event. */
    XEvent *eventPtr)		/* The event to fill. */
{
    bool shift = false;

    eventPtr->xkey.keycode = (keySym == NoSymbol) ? 0
	    : TkHaikuKeyCode(keySym, &shift);
    if (shift) {
	eventPtr->xkey.state |= ShiftMask;
    }
    ((TkKeyEvent *)eventPtr)->keysym = keySym;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
