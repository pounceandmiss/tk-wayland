/*
 * tkAndroidSelect.c --
 *
 *	The selections of the Android port. Selections owned by the Tk
 *	applications of this process are handled by tkSelect.c. The
 *	CLIPBOARD is shared with the other applications through the
 *	ClipboardManager of Android, with JNI: when Tk claims it, its text is
 *	copied to the clipboard of Android at idle time, and when Tk asks for
 *	a clipboard it does not own, the text is read from there. Android
 *	does not tell applications when another one takes the clipboard, so
 *	when the activity gets the focus back, the owner gets a
 *	SelectionClear event if the clipboard changed meanwhile.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkAndroidInt.h"
#include "tkSelect.h"

static Window clipboardOwner = None;	/* The window which owns the CLIPBOARD
					 * in Tk. */
static Tcl_DString exportedText;	/* The text it was given to Android. */
static jlong exportedTime = -1;		/* The timestamp of the clip which
					 * was set then, to know whether it
					 * changed. */
static bool exportedInitialized = false;
static bool exportPending = false;

static void		ExportIdle(void *clientData);
static jobject		GetClipboardManager(JNIEnv *env);
static bool		GetClipboardText(Tcl_DString *dsPtr);
static bool		HasClipboardText(void);
static jlong		ClipTimestamp(void);
static void		SetClipboardText(const char *text, Tcl_Size length);

/*
 *----------------------------------------------------------------------
 *
 * Failed --
 *
 *	Check whether a Java call threw an exception, and clear it.
 *
 * Results:
 *	Whether there was an exception.
 *
 * Side effects:
 *	The exception is cleared.
 *
 *----------------------------------------------------------------------
 */

static bool
Failed(
    JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env)) {
	(*env)->ExceptionClear(env);
	return true;
    }
    return false;
}

/*
 *----------------------------------------------------------------------
 *
 * GetClipboardManager --
 *
 *	The ClipboardManager of the activity:
 *	activity.getSystemService("clipboard").
 *
 * Results:
 *	A local reference, or NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static jobject
GetClipboardManager(
    JNIEnv *env)		/* The JNI environment. */
{
    jobject activity = tkAndroidActivity->clazz;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID getSystemService = (*env)->GetMethodID(env, cls,
	    "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring name = (*env)->NewStringUTF(env, "clipboard");
    jobject manager;

    manager = (*env)->CallObjectMethod(env, activity, getSystemService, name);
    (*env)->DeleteLocalRef(env, name);
    (*env)->DeleteLocalRef(env, cls);
    if (Failed(env)) {
	return NULL;
    }
    return manager;
}

/*
 *----------------------------------------------------------------------
 *
 * GetClipboardText, SetClipboardText --
 *
 *	Read the text of the clipboard of Android, or set it. Strings are
 *	converted from and to UTF-16, since the UTF-8 of JNI is not quite
 *	UTF-8.
 *
 * Results:
 *	GetClipboardText returns whether the clipboard has text, which is
 *	appended to the DString.
 *
 * Side effects:
 *	SetClipboardText sets the clipboard.
 *
 *----------------------------------------------------------------------
 */

static bool
GetClipboardText(
    Tcl_DString *dsPtr)		/* Initialized; returns the text. */
{
    JNIEnv *env = TkAndroidGetEnv();
    jobject manager, clip, item, text = NULL, string = NULL;
    jclass cls;
    bool result = false;

    if (env == NULL || (manager = GetClipboardManager(env)) == NULL) {
	return false;
    }
    (*env)->PushLocalFrame(env, 16);
    cls = (*env)->GetObjectClass(env, manager);
    clip = (*env)->CallObjectMethod(env, manager, (*env)->GetMethodID(env,
	    cls, "getPrimaryClip", "()Landroid/content/ClipData;"));
    if (Failed(env) || clip == NULL) {
	goto done;
    }
    cls = (*env)->GetObjectClass(env, clip);
    if ((*env)->CallIntMethod(env, clip, (*env)->GetMethodID(env, cls,
	    "getItemCount", "()I")) < 1 || Failed(env)) {
	goto done;
    }
    item = (*env)->CallObjectMethod(env, clip, (*env)->GetMethodID(env, cls,
	    "getItemAt", "(I)Landroid/content/ClipData$Item;"), 0);
    if (Failed(env) || item == NULL) {
	goto done;
    }
    cls = (*env)->GetObjectClass(env, item);
    text = (*env)->CallObjectMethod(env, item, (*env)->GetMethodID(env, cls,
	    "coerceToText", "(Landroid/content/Context;)Ljava/lang/CharSequence;"),
	    tkAndroidActivity->clazz);
    if (Failed(env) || text == NULL) {
	goto done;
    }
    cls = (*env)->GetObjectClass(env, text);
    string = (*env)->CallObjectMethod(env, text, (*env)->GetMethodID(env,
	    cls, "toString", "()Ljava/lang/String;"));
    if (Failed(env) || string == NULL) {
	goto done;
    }
    {
	const jchar *chars = (*env)->GetStringChars(env, (jstring)string,
		NULL);
	jsize length = (*env)->GetStringLength(env, (jstring)string);

	Tcl_Char16ToUtfDString((const unsigned short *)chars, length, dsPtr);
	(*env)->ReleaseStringChars(env, (jstring)string, chars);
	result = true;
    }

  done:
    (*env)->PopLocalFrame(env, NULL);
    (*env)->DeleteLocalRef(env, manager);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * HasClipboardText --
 *
 *	Whether the clipboard of Android has text, from the description of
 *	its clip: reading the clip itself makes Android tell the user that
 *	the application read the clipboard.
 *
 * Results:
 *	As above.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static bool
HasClipboardText(void)
{
    JNIEnv *env = TkAndroidGetEnv();
    jobject manager, description;
    jclass cls;
    jstring mimeType;
    bool result = false;

    if (env == NULL || (manager = GetClipboardManager(env)) == NULL) {
	return false;
    }
    (*env)->PushLocalFrame(env, 8);
    cls = (*env)->GetObjectClass(env, manager);
    description = (*env)->CallObjectMethod(env, manager,
	    (*env)->GetMethodID(env, cls, "getPrimaryClipDescription",
	    "()Landroid/content/ClipDescription;"));
    if (!Failed(env) && description != NULL) {
	cls = (*env)->GetObjectClass(env, description);
	mimeType = (*env)->NewStringUTF(env, "text/*");
	result = (*env)->CallBooleanMethod(env, description,
		(*env)->GetMethodID(env, cls, "hasMimeType",
		"(Ljava/lang/String;)Z"), mimeType) && !Failed(env);
    }
    (*env)->PopLocalFrame(env, NULL);
    (*env)->DeleteLocalRef(env, manager);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ClipTimestamp --
 *
 *	The time the clip of the clipboard of Android was set, which tells
 *	whether it changed without reading it.
 *
 * Results:
 *	The timestamp, or -1 if there is no clip.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static jlong
ClipTimestamp(void)
{
    JNIEnv *env = TkAndroidGetEnv();
    jobject manager, description;
    jclass cls;
    jlong result = -1;

    if (env == NULL || (manager = GetClipboardManager(env)) == NULL) {
	return -1;
    }
    (*env)->PushLocalFrame(env, 8);
    cls = (*env)->GetObjectClass(env, manager);
    description = (*env)->CallObjectMethod(env, manager,
	    (*env)->GetMethodID(env, cls, "getPrimaryClipDescription",
	    "()Landroid/content/ClipDescription;"));
    if (!Failed(env) && description != NULL) {
	cls = (*env)->GetObjectClass(env, description);
	result = (*env)->CallLongMethod(env, description,
		(*env)->GetMethodID(env, cls, "getTimestamp", "()J"));
	if (Failed(env)) {
	    result = -1;
	}
    }
    (*env)->PopLocalFrame(env, NULL);
    (*env)->DeleteLocalRef(env, manager);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * SetClipboardText --
 *
 *	Put text in the clipboard of Android, as a plain text clip.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Other applications see the text in the clipboard.
 *
 *----------------------------------------------------------------------
 */

static void
SetClipboardText(
    const char *text,		/* The text, in UTF-8. */
    Tcl_Size length)		/* Its length in bytes. */
{
    JNIEnv *env = TkAndroidGetEnv();
    jobject manager, clip;
    jclass cls, clipDataClass;
    jstring label, string;
    Tcl_DString ds;

    if (env == NULL || (manager = GetClipboardManager(env)) == NULL) {
	return;
    }
    (*env)->PushLocalFrame(env, 16);
    Tcl_DStringInit(&ds);
    Tcl_UtfToChar16DString(text, length, &ds);
    string = (*env)->NewString(env, (const jchar *)Tcl_DStringValue(&ds),
	    (jsize)(Tcl_DStringLength(&ds) / 2));
    Tcl_DStringFree(&ds);
    label = (*env)->NewStringUTF(env, "Tk");
    clipDataClass = (*env)->FindClass(env, "android/content/ClipData");
    if (Failed(env) || clipDataClass == NULL) {
	goto done;
    }
    clip = (*env)->CallStaticObjectMethod(env, clipDataClass,
	    (*env)->GetStaticMethodID(env, clipDataClass, "newPlainText",
	    "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)"
	    "Landroid/content/ClipData;"), label, string);
    if (Failed(env) || clip == NULL) {
	goto done;
    }
    cls = (*env)->GetObjectClass(env, manager);
    (*env)->CallVoidMethod(env, manager, (*env)->GetMethodID(env, cls,
	    "setPrimaryClip", "(Landroid/content/ClipData;)V"), clip);
    Failed(env);

  done:
    (*env)->PopLocalFrame(env, NULL);
    (*env)->DeleteLocalRef(env, manager);
}

/*
 *----------------------------------------------------------------------
 *
 * XSetSelectionOwner --
 *
 *	Tk claims a selection. When it claims the CLIPBOARD, its text is
 *	copied to Android at idle time, once the script has appended it.
 *
 * Results:
 *	Success.
 *
 * Side effects:
 *	An idle callback may be scheduled.
 *
 *----------------------------------------------------------------------
 */

int
XSetSelectionOwner(
    Display *display,		/* The display. */
    Atom selection,		/* The selection. */
    Window owner,		/* The new owner, or None. */
    TCL_UNUSED(Time))
{
    TkXeNextRequest(display);
    if (selection != XInternAtom(display, "CLIPBOARD", False)) {
	return Success;
    }
    clipboardOwner = owner;
    if (owner != None && !exportPending) {
	exportPending = true;
	Tcl_DoWhenIdle(ExportIdle, NULL);
    }
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * AppendProc --
 *
 *	Append a portion of a selection to a DString.
 *
 * Results:
 *	TCL_OK.
 *
 * Side effects:
 *	The DString grows.
 *
 *----------------------------------------------------------------------
 */

static int
AppendProc(
    void *clientData,
    TCL_UNUSED(Tcl_Interp *),
    const char *portion)
{
    Tcl_DStringAppend((Tcl_DString *)clientData, portion, TCL_INDEX_NONE);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ExportIdle --
 *
 *	Copy the text of the CLIPBOARD Tk owns to the clipboard of Android.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The clipboard of Android is set, and the text remembered.
 *
 *----------------------------------------------------------------------
 */

static void
ExportIdle(
    TCL_UNUSED(void *))
{
    TkWindow *winPtr;
    Tcl_Interp *interp = NULL;
    Tcl_InterpState state;

    exportPending = false;
    if (clipboardOwner == None || tkXeDisplay == NULL) {
	return;
    }
    winPtr = (TkWindow *)Tk_IdToWindow(tkXeDisplay->display, clipboardOwner);

    /*
     * The clipboard is owned by a window which belongs to no application:
     * the application is the one which last appended to the clipboard.
     */

    if (winPtr && winPtr->mainPtr) {
	interp = winPtr->mainPtr->interp;
    } else if (winPtr && winPtr->dispPtr->clipboardAppPtr) {
	interp = winPtr->dispPtr->clipboardAppPtr->interp;
    }
    if (interp == NULL) {
	return;
    }
    if (exportedInitialized) {
	Tcl_DStringFree(&exportedText);
    } else {
	Tcl_DStringInit(&exportedText);
	exportedInitialized = true;
    }
    state = Tcl_SaveInterpState(interp, TCL_OK);
    if (Tk_GetSelection(interp, (Tk_Window)winPtr,
	    Tk_InternAtom((Tk_Window)winPtr, "CLIPBOARD"),
	    Tk_InternAtom((Tk_Window)winPtr, "UTF8_STRING"), AppendProc,
	    &exportedText) != TCL_OK) {
	Tcl_DStringSetLength(&exportedText, 0);
    }
    Tcl_RestoreInterpState(interp, state);
    SetClipboardText(Tcl_DStringValue(&exportedText),
	    Tcl_DStringLength(&exportedText));
    exportedTime = ClipTimestamp();
}

/*
 *----------------------------------------------------------------------
 *
 * TkAndroidClipboardCheck --
 *
 *	Called when the activity gets the focus: if the clipboard of Android
 *	no longer has the clip Tk gave it (its timestamp changed: reading it
 *	would make Android tell the user), another application took it, and
 *	the owner loses the CLIPBOARD, as with X.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A SelectionClear event may be queued.
 *
 *----------------------------------------------------------------------
 */

void
TkAndroidClipboardCheck(void)
{
    if (clipboardOwner == None || exportPending || !exportedInitialized) {
	return;
    }
    if (ClipTimestamp() != exportedTime) {
	XEvent event;

	memset(&event, 0, sizeof(XEvent));
	event.xselectionclear.type = SelectionClear;
	event.xselectionclear.display = tkXeDisplay->display;
	event.xselectionclear.window = clipboardOwner;
	event.xselectionclear.selection = XInternAtom(tkXeDisplay->display,
		"CLIPBOARD", False);
	event.xselectionclear.time = TkXeCurrentTime();
	clipboardOwner = None;
	if (TkXeGetWindow(event.xselectionclear.window)) {
	    TkXeQueueEvent(&event);
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkSelGetSelection --
 *
 *	Retrieve a selection which is not owned by a Tk application of this
 *	process: only the CLIPBOARD, as text, from Android.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	The proc is called with the text.
 *
 *----------------------------------------------------------------------
 */

int
TkSelGetSelection(
    Tcl_Interp *interp,		/* For the result. */
    Tk_Window tkwin,		/* The window asking. */
    Atom selection,		/* The selection. */
    Atom target,		/* The form wanted. */
    Tk_GetSelProc *proc,	/* Called with the data. */
    void *clientData)		/* For proc. */
{
    const char *targetName = Tk_GetAtomName(tkwin, target);
    Tcl_DString ds;
    int result;

    Tcl_DStringInit(&ds);
    if (selection != Tk_InternAtom(tkwin, "CLIPBOARD")) {
	goto notDefined;
    }
    if (!strcmp(targetName, "TARGETS")) {
	/*
	 * Without reading the clipboard.
	 */

	if (!HasClipboardText()) {
	    goto notDefined;
	}
	return proc(clientData, interp, "TARGETS UTF8_STRING STRING TEXT");
    }
    if (!GetClipboardText(&ds)) {
	goto notDefined;
    }
    if (strcmp(targetName, "UTF8_STRING") && strcmp(targetName, "STRING")
	    && strcmp(targetName, "TEXT")
	    && strcmp(targetName, "text/plain;charset=utf-8")
	    && strcmp(targetName, "text/plain")) {
	goto notDefined;
    }
    result = proc(clientData, interp, Tcl_DStringValue(&ds));
    Tcl_DStringFree(&ds);
    return result;

  notDefined:
    Tcl_DStringFree(&ds);
    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
	    "%s selection doesn't exist or form \"%s\" not defined",
	    Tk_GetAtomName(tkwin, selection), targetName));
    Tcl_SetErrorCode(interp, "TK", "SELECTION", "EXISTS", (char *)NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TkSelEventProc, TkSelPropProc --
 *
 *	Only SelectionClear events are generated: the selection is not
 *	exchanged through windows and properties.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A SelectionClear event clears the selection of Tk.
 *
 *----------------------------------------------------------------------
 */

void
TkSelEventProc(
    Tk_Window tkwin,
    XEvent *eventPtr)
{
    if (eventPtr->type == SelectionClear) {
	TkSelClearSelection(tkwin, eventPtr);
    }
}

void
TkSelPropProc(
    TCL_UNUSED(XEvent *))
{
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
