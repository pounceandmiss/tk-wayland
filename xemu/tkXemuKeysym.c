/*
 * tkXemuKeysym.c --
 *
 *	The names of keysyms, for the ports of the X server emulation which
 *	do not have xkbcommon (Android, Haiku; see tkXemuXkb.h): the Xlib
 *	emulation gets them from the table of Tk here.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkXemuInt.h"

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
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
