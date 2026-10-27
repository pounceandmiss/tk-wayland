/*
 * tkAndroidMain.c --
 *
 *	The glue between a NativeActivity and Tk: the entry point of the
 *	library the activity loads (libmain.so), which runs Tcl and Tk in a
 *	thread of their own, and the callbacks of the activity, which post
 *	what happens to it to Tk (tkAndroid.h).
 *
 *	The scripts (the libraries of Tcl and Tk, and the application) are in
 *	a zip linked into the library, between the symbols tkAndroidScripts
 *	and tkAndroidScriptsEnd, which is mounted at //zipfs:/app. The
 *	application is app/main.tcl, or else app/widget (the demos).
 *	Compiled with TK_TEST, the library has the test commands of tktest
 *	(android/build/Makefile, "make test-apk").
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include <tcl.h>
#include <tk.h>
#include "tkAndroid.h"
#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAG "Tk"

extern const unsigned char tkAndroidScripts[];
extern const unsigned char tkAndroidScriptsEnd[];

#ifdef TK_TEST
extern Tcl_LibraryInitProc Tktest_Init;
#endif /* TK_TEST */

/*
 * State of the glue, used in the main thread of the application.
 */

static AInputQueue *inputQueue = NULL;
static bool tclStarted = false;
static char *dataPath = NULL;		/* The internal data directory. */

/*
 * The methods of android.view.KeyEvent which give the character of a key.
 */

static jclass keyEventClass = NULL;
static jmethodID keyEventInit = NULL;
static jmethodID keyEventGetUnicodeChar = NULL;

static void		InitKeyEvent(JNIEnv *env);
static void		RegisterNatives(ANativeActivity *activity);
static int		InputCallback(int fd, int events, void *data);
static int		KeyUnicode(const AInputEvent *event);
static void		LogOutput(void);
static void *		LogThread(void *clientData);
static int		PostDensity(ANativeActivity *activity);
static TCL_NORETURN void ExitProc(void *clientData);
static void *		TclThread(void *clientData);
static int		TranslateKey(const AInputEvent *event);
static int		TranslateMotion(const AInputEvent *event);
static void		WriteFontConfig(void);

/*
 *----------------------------------------------------------------------
 *
 * Output --
 *
 *	The standard output and error of the process go to the log of
 *	Android, read with "adb logcat -s Tk".
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * LogThread, LogOutput --
 *
 *	LogOutput sends the standard output and error of the process to the
 *	log of Android (adb logcat -s Tk), through a pipe which LogThread
 *	reads, a line at a time.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The standard output and error are redirected; a thread is started.
 *
 *----------------------------------------------------------------------
 */

static void *
LogThread(
    void *clientData)		/* The read end of the pipe. */
{
    int fd = (int)(intptr_t)clientData;
    char buf[1024];
    size_t used = 0;
    ssize_t n;

    while ((n = read(fd, buf + used, sizeof(buf) - 1 - used)) > 0) {
	char *line = buf, *nl;

	used += (size_t)n;
	buf[used] = '\0';
	while ((nl = strchr(line, '\n')) != NULL) {
	    *nl = '\0';
	    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, line);
	    line = nl + 1;
	}
	used -= (size_t)(line - buf);
	memmove(buf, line, used);
	if (used == sizeof(buf) - 1) {
	    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, buf);
	    used = 0;
	}
    }
    return NULL;
}

static void
LogOutput(void)
{
    int fds[2];
    pthread_t thread;

    if (pipe(fds) < 0) {
	return;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    if (pthread_create(&thread, NULL, LogThread,
	    (void *)(intptr_t)fds[0]) == 0) {
	pthread_detach(thread);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * WriteFontConfig --
 *
 *	Fontconfig is configured with a file written in the data directory:
 *	the fonts of the system, a cache in the data directory, and aliases
 *	of the generic families Tk uses to the fonts of Android. The aliases
 *	of the families of other systems come first, since fontconfig applies
 *	the rules in order.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Writes fonts.conf, and sets FONTCONFIG_FILE.
 *
 *----------------------------------------------------------------------
 */

static void
WriteFontConfig(void)
{
    char path[1024];
    FILE *f;

    snprintf(path, sizeof(path), "%s/fonts.conf", dataPath);
    f = fopen(path, "w");
    if (f == NULL) {
	return;
    }
    fprintf(f,
	"<?xml version=\"1.0\"?>\n"
	"<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
	"<fontconfig>\n"
	"  <dir>/system/fonts</dir>\n"
	"  <dir>/product/fonts</dir>\n"
	"  <cachedir>%s/fontconfig</cachedir>\n"
	"  <alias><family>Helvetica</family>"
	"<default><family>sans-serif</family></default></alias>\n"
	"  <alias><family>Arial</family>"
	"<default><family>sans-serif</family></default></alias>\n"
	"  <alias><family>Times</family>"
	"<default><family>serif</family></default></alias>\n"
	"  <alias><family>Courier</family>"
	"<default><family>monospace</family></default></alias>\n"
	"  <alias><family>sans-serif</family>\n"
	"    <prefer><family>Roboto</family><family>Noto Sans</family>"
	"</prefer></alias>\n"
	"  <alias><family>serif</family>\n"
	"    <prefer><family>Noto Serif</family></prefer></alias>\n"
	"  <alias><family>monospace</family>\n"
	"    <prefer><family>Droid Sans Mono</family>"
	"<family>Cutive Mono</family></prefer></alias>\n"
	"  <match target=\"pattern\">\n"
	"    <test qual=\"all\" name=\"family\" compare=\"not_eq\">"
	"<string>sans-serif</string></test>\n"
	"    <test qual=\"all\" name=\"family\" compare=\"not_eq\">"
	"<string>serif</string></test>\n"
	"    <test qual=\"all\" name=\"family\" compare=\"not_eq\">"
	"<string>monospace</string></test>\n"
	"    <edit name=\"family\" mode=\"append_last\">"
	"<string>sans-serif</string></edit>\n"
	"  </match>\n"
	"</fontconfig>\n", dataPath);
    fclose(f);
    setenv("FONTCONFIG_FILE", path, 1);
}

/*
 *----------------------------------------------------------------------
 *
 * ExitProc --
 *
 *	Called by Tcl_Exit ([exit], or the end of the event loop): finalize
 *	Tcl, as Tcl_Exit does without an exit procedure, then finish the
 *	activity, whose destruction ends the process, and the thread. The
 *	main thread does not call Tcl.
 *
 * Results:
 *	None; does not return.
 *
 * Side effects:
 *	The channels are flushed and closed, the exit handlers run, and the
 *	activity finishes.
 *
 *----------------------------------------------------------------------
 */

static TCL_NORETURN void
ExitProc(
    void *clientData)		/* The exit status. */
{
    (void)clientData;
    Tcl_Finalize();
    fflush(stdout);
    ANativeActivity_finish(tkAndroidActivity);
    pthread_exit(NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * TclThread --
 *
 *	The thread of Tcl and Tk: create the interpreter, run the
 *	application and the event loop, and finish the activity when the
 *	application exits.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Everything.
 *
 *----------------------------------------------------------------------
 */

static void *
TclThread(
    void *clientData)		/* The activity. */
{
    ANativeActivity *activity = (ANativeActivity *)clientData;
    Tcl_Interp *interp;
    const char *script;
    char path[1024];

    snprintf(path, sizeof(path), "%s/tmp", dataPath);
    mkdir(path, 0700);
    setenv("TMPDIR", path, 1);
    setenv("HOME", dataPath, 1);
    setenv("TCL_LIBRARY", "//zipfs:/app/tcl_library", 1);
    setenv("TK_LIBRARY", "//zipfs:/app/tk_library", 1);
    WriteFontConfig();

    Tcl_FindExecutable(NULL);
    Tcl_SetExitProc(ExitProc);
    if (TclZipfs_MountBuffer(NULL, tkAndroidScripts,
	    (size_t)(tkAndroidScriptsEnd - tkAndroidScripts), "//zipfs:/app",
	    0) != TCL_OK) {
	__android_log_write(ANDROID_LOG_ERROR, LOG_TAG,
		"cannot mount the scripts");
	goto done;
    }
    interp = Tcl_CreateInterp();
    Tcl_SetVar2(interp, "argv0", NULL, "wish", TCL_GLOBAL_ONLY);
    Tcl_SetVar2(interp, "argv", NULL, "", TCL_GLOBAL_ONLY);
    Tcl_SetVar2(interp, "argc", NULL, "0", TCL_GLOBAL_ONLY);
    Tcl_SetVar2(interp, "tcl_interactive", NULL, "0", TCL_GLOBAL_ONLY);
    if (Tcl_Init(interp) != TCL_OK || Tk_Init(interp) != TCL_OK) {
	fprintf(stderr, "%s\n", Tcl_GetStringResult(interp));
	goto done;
    }
    Tcl_StaticLibrary(interp, "Tk", Tk_Init, Tk_SafeInit);
#ifdef TK_TEST
    if (Tktest_Init(interp) != TCL_OK) {
	fprintf(stderr, "%s\n", Tcl_GetStringResult(interp));
	goto done;
    }
    Tcl_StaticLibrary(interp, "Tktest", Tktest_Init, 0);
#endif /* TK_TEST */

    {
	Tcl_Obj *pathPtr = Tcl_NewStringObj("//zipfs:/app/app/main.tcl", -1);

	Tcl_IncrRefCount(pathPtr);
	script = (Tcl_FSAccess(pathPtr, R_OK) == 0)
		? "//zipfs:/app/app/main.tcl" : "//zipfs:/app/app/widget";
	Tcl_DecrRefCount(pathPtr);
    }
    Tcl_SetVar2(interp, "argv0", NULL, script, TCL_GLOBAL_ONLY);
    if (Tcl_EvalFile(interp, script) != TCL_OK) {
	Tcl_Obj *options = Tcl_GetReturnOptions(interp, TCL_ERROR);
	Tcl_Obj *key = Tcl_NewStringObj("-errorinfo", -1);
	Tcl_Obj *info = NULL;

	Tcl_IncrRefCount(key);
	Tcl_DictObjGet(NULL, options, key, &info);
	fprintf(stderr, "%s\n", info ? Tcl_GetString(info)
		: Tcl_GetStringResult(interp));
	Tcl_DecrRefCount(key);
    }
    Tk_MainLoop();
    Tcl_Exit(0);

  done:
    ANativeActivity_finish(activity);
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Input --
 *
 *	The input queue of the activity is read in the main thread: events
 *	are translated and posted to Tk, and the character of key events is
 *	got from Java, which knows the layout of the keyboard.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * InitKeyEvent --
 *
 *	Find the methods of android.view.KeyEvent which give the character of
 *	a key, once.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The class is kept in a global reference.
 *
 *----------------------------------------------------------------------
 */

static void
InitKeyEvent(
    JNIEnv *env)		/* The environment of the main thread. */
{
    jclass cls = (*env)->FindClass(env, "android/view/KeyEvent");

    if (cls == NULL) {
	(*env)->ExceptionClear(env);
	return;
    }
    keyEventClass = (jclass)(*env)->NewGlobalRef(env, cls);
    keyEventInit = (*env)->GetMethodID(env, cls, "<init>", "(JJIIIIII)V");
    keyEventGetUnicodeChar = (*env)->GetMethodID(env, cls, "getUnicodeChar",
	    "(I)I");
    (*env)->DeleteLocalRef(env, cls);
    if ((*env)->ExceptionCheck(env)) {
	(*env)->ExceptionClear(env);
	keyEventClass = NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * KeyUnicode --
 *
 *	The character a key produces, without the Control, Alt and Meta
 *	modifiers, as with X, from the key character map of its device.
 *
 * Results:
 *	The character, or 0.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
KeyUnicode(
    const AInputEvent *event)	/* A key event. */
{
    JNIEnv *env = tkAndroidActivity->env;
    int meta = AKeyEvent_getMetaState(event)
	    & (AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_RIGHT_ON
	    | AMETA_CAPS_LOCK_ON | AMETA_NUM_LOCK_ON);
    jobject keyEvent;
    int unicode;

    if (keyEventClass == NULL) {
	return 0;
    }
    keyEvent = (*env)->NewObject(env, keyEventClass, keyEventInit,
	    (jlong)(AKeyEvent_getDownTime(event) / 1000000),
	    (jlong)(AKeyEvent_getEventTime(event) / 1000000),
	    (jint)AKeyEvent_getAction(event), (jint)AKeyEvent_getKeyCode(event),
	    (jint)AKeyEvent_getRepeatCount(event), (jint)meta,
	    (jint)AInputEvent_getDeviceId(event),
	    (jint)AKeyEvent_getScanCode(event));
    if (keyEvent == NULL) {
	(*env)->ExceptionClear(env);
	return 0;
    }
    unicode = (*env)->CallIntMethod(env, keyEvent, keyEventGetUnicodeChar,
	    (jint)meta);
    (*env)->DeleteLocalRef(env, keyEvent);
    if ((*env)->ExceptionCheck(env)) {
	(*env)->ExceptionClear(env);
	return 0;
    }
    return (unicode < 0) ? 0 : unicode;
}

/*
 *----------------------------------------------------------------------
 *
 * TranslateKey --
 *
 *	Post a key event to Tk. The back key is the back button; the keys
 *	of the system are left to it.
 *
 * Results:
 *	Whether the event was handled.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static int
TranslateKey(
    const AInputEvent *event)	/* A key event. */
{
    TkAndroidEvent ev;
    int keyCode = AKeyEvent_getKeyCode(event);
    int action = AKeyEvent_getAction(event);

    switch (keyCode) {
    case AKEYCODE_BACK:
	if (action == AKEY_EVENT_ACTION_UP) {
	    memset(&ev, 0, sizeof(ev));
	    ev.type = TKANDROID_BACK;
	    TkAndroidPostEvent(&ev);
	}
	return 1;
    case AKEYCODE_VOLUME_UP:
    case AKEYCODE_VOLUME_DOWN:
    case AKEYCODE_VOLUME_MUTE:
    case AKEYCODE_HOME:
    case AKEYCODE_APP_SWITCH:
    case AKEYCODE_POWER:
	return 0;
    }
    if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) {
	return 0;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = TKANDROID_KEY;
    ev.time = AKeyEvent_getEventTime(event) / 1000000;
    ev.u.key.down = (action == AKEY_EVENT_ACTION_DOWN);
    ev.u.key.keyCode = keyCode;
    ev.u.key.metaState = AKeyEvent_getMetaState(event);
    ev.u.key.unicode = KeyUnicode(event);
    TkAndroidPostEvent(&ev);
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * TranslateMotion --
 *
 *	Post a touch or mouse event to Tk: each finger which touches, moves
 *	or is lifted.
 *
 * Results:
 *	Whether the event was handled.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static int
TranslateMotion(
    const AInputEvent *event)	/* A motion event. */
{
    TkAndroidEvent ev;
    int action = AMotionEvent_getAction(event);
    int masked = action & AMOTION_EVENT_ACTION_MASK;
    size_t index = 0, i;
    bool isMouse = (AMotionEvent_getToolType(event, 0)
	    == AMOTION_EVENT_TOOL_TYPE_MOUSE);

    memset(&ev, 0, sizeof(ev));
    ev.time = AMotionEvent_getEventTime(event) / 1000000;
    if (masked == AMOTION_EVENT_ACTION_SCROLL) {
	ev.type = TKANDROID_SCROLL;
	ev.u.scroll.x = AMotionEvent_getX(event, 0);
	ev.u.scroll.y = AMotionEvent_getY(event, 0);
	ev.u.scroll.dx = AMotionEvent_getAxisValue(event,
		AMOTION_EVENT_AXIS_HSCROLL, 0);
	ev.u.scroll.dy = AMotionEvent_getAxisValue(event,
		AMOTION_EVENT_AXIS_VSCROLL, 0);
	TkAndroidPostEvent(&ev);
	return 1;
    }

    ev.type = TKANDROID_POINTER;
    ev.u.pointer.isMouse = isMouse;
    if (isMouse) {
	switch (masked) {
	case AMOTION_EVENT_ACTION_HOVER_EXIT:
	    ev.u.pointer.action = TKANDROID_LEAVE;
	    break;
	default:
	    ev.u.pointer.action = TKANDROID_HOVER;
	    break;
	}
	ev.u.pointer.x = AMotionEvent_getX(event, 0);
	ev.u.pointer.y = AMotionEvent_getY(event, 0);
	ev.u.pointer.buttons = AMotionEvent_getButtonState(event);
	TkAndroidPostEvent(&ev);
	return 1;
    }

    switch (masked) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
	index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
		>> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
	ev.u.pointer.action = (masked == AMOTION_EVENT_ACTION_DOWN
		|| masked == AMOTION_EVENT_ACTION_POINTER_DOWN)
		? TKANDROID_DOWN : TKANDROID_UP;
	ev.u.pointer.id = AMotionEvent_getPointerId(event, index);
	ev.u.pointer.x = AMotionEvent_getX(event, index);
	ev.u.pointer.y = AMotionEvent_getY(event, index);
	TkAndroidPostEvent(&ev);
	break;
    case AMOTION_EVENT_ACTION_MOVE:
    case AMOTION_EVENT_ACTION_CANCEL:
	ev.u.pointer.action = (masked == AMOTION_EVENT_ACTION_MOVE)
		? TKANDROID_MOVE : TKANDROID_CANCEL;
	for (i = 0; i < AMotionEvent_getPointerCount(event); i++) {
	    ev.u.pointer.id = AMotionEvent_getPointerId(event, i);
	    ev.u.pointer.x = AMotionEvent_getX(event, i);
	    ev.u.pointer.y = AMotionEvent_getY(event, i);
	    TkAndroidPostEvent(&ev);
	}
	break;
    }
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * InputCallback --
 *
 *	Read the input queue of the activity, when the looper of the main
 *	thread says it has events.
 *
 * Results:
 *	1, to keep the callback.
 *
 * Side effects:
 *	Events are posted to Tk.
 *
 *----------------------------------------------------------------------
 */

static int
InputCallback(
    int fd,
    int events,
    void *data)
{
    AInputQueue *queue = (AInputQueue *)data;
    AInputEvent *event = NULL;

    (void)fd;
    (void)events;
    while (AInputQueue_getEvent(queue, &event) >= 0) {
	int handled = 0;

	if (AInputQueue_preDispatchEvent(queue, event)) {
	    continue;
	}
	switch (AInputEvent_getType(event)) {
	case AINPUT_EVENT_TYPE_KEY:
	    handled = TranslateKey(event);
	    break;
	case AINPUT_EVENT_TYPE_MOTION:
	    handled = TranslateMotion(event);
	    break;
	}
	AInputQueue_finishEvent(queue, event, handled);
    }
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * The native methods of TkActivity.java --
 *
 *	The soft keyboard commits text, or deletes some, through the
 *	InputConnection of the activity, in the main thread.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * NativeCommitText, NativeKeyboardChanged, NativeDeleteText --
 *
 *	TkActivity.nativeCommitText, nativeKeyboardChanged and
 *	nativeDeleteText: post the text the soft keyboard commits, whether it
 *	was shown or hidden, or the deletion it asks for, to Tk.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static void JNICALL
NativeCommitText(
    JNIEnv *env,
    jclass cls,
    jstring text)		/* The committed text. */
{
    TkAndroidEvent ev;
    jsize length = (*env)->GetStringLength(env, text);
    const jchar *chars;

    (void)cls;
    if (length <= 0) {
	return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = TKANDROID_TEXT;
    ev.u.text.chars = (unsigned short *)malloc(length * sizeof(jchar));
    ev.u.text.length = length;
    chars = (*env)->GetStringChars(env, text, NULL);
    memcpy(ev.u.text.chars, chars, length * sizeof(jchar));
    (*env)->ReleaseStringChars(env, text, chars);
    TkAndroidPostEvent(&ev);
}

static void JNICALL
NativeKeyboardChanged(
    JNIEnv *env,
    jclass cls,
    jboolean shown)		/* Whether the keyboard is shown. */
{
    TkAndroidEvent ev;

    (void)env;
    (void)cls;
    memset(&ev, 0, sizeof(ev));
    ev.type = TKANDROID_KEYBOARD;
    ev.u.keyboardShown = (shown != 0);
    TkAndroidPostEvent(&ev);
}

static void JNICALL
NativeDeleteText(
    JNIEnv *env,
    jclass cls,
    jint before,		/* Characters before the cursor. */
    jint after)			/* Characters after the cursor. */
{
    TkAndroidEvent ev;

    (void)env;
    (void)cls;
    memset(&ev, 0, sizeof(ev));
    ev.type = TKANDROID_DELETE;
    ev.u.del.before = before;
    ev.u.del.after = after;
    TkAndroidPostEvent(&ev);
}

/*
 *----------------------------------------------------------------------
 *
 * RegisterNatives --
 *
 *	Register the native methods of TkActivity.java, when the activity is
 *	one.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The methods are registered.
 *
 *----------------------------------------------------------------------
 */

static void
RegisterNatives(
    ANativeActivity *activity)
{
    static const JNINativeMethod methods[] = {
	{"nativeCommitText", "(Ljava/lang/String;)V",
		(void *)NativeCommitText},
	{"nativeDeleteText", "(II)V", (void *)NativeDeleteText},
	{"nativeKeyboardChanged", "(Z)V", (void *)NativeKeyboardChanged}
    };
    JNIEnv *env = activity->env;
    jclass cls = (*env)->GetObjectClass(env, activity->clazz);

    if ((*env)->RegisterNatives(env, cls, methods, 3) != JNI_OK) {
	(*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, cls);
}

/*
 *----------------------------------------------------------------------
 *
 * The callbacks of the activity --
 *
 *	They run in the main thread of the application, and post events to
 *	Tk.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * PostSimple --
 *
 *	Post an event which has no data.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static void
PostSimple(
    TkAndroidEventType type)
{
    TkAndroidEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    TkAndroidPostEvent(&ev);
}

/*
 *----------------------------------------------------------------------
 *
 * PostDensity --
 *
 *	Post the density of the screen.
 *
 * Results:
 *	The density, in dots per inch.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static int
PostDensity(
    ANativeActivity *activity)
{
    AConfiguration *config = AConfiguration_new();
    TkAndroidEvent ev;

    AConfiguration_fromAssetManager(config, activity->assetManager);
    memset(&ev, 0, sizeof(ev));
    ev.type = TKANDROID_CONFIG;
    ev.u.density = AConfiguration_getDensity(config);
    AConfiguration_delete(config);
    TkAndroidPostEvent(&ev);
    return ev.u.density;
}

/*
 *----------------------------------------------------------------------
 *
 * OnResume, OnPause --
 *
 *	The activity becomes visible, or no longer is.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static void
OnResume(
    ANativeActivity *activity)
{
    (void)activity;
    PostSimple(TKANDROID_RESUME);
}

static void
OnPause(
    ANativeActivity *activity)
{
    (void)activity;
    PostSimple(TKANDROID_PAUSE);
}

/*
 *----------------------------------------------------------------------
 *
 * OnDestroy --
 *
 *	The activity is destroyed: Tk lives as long as its activity, so the
 *	process exits.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The process exits.
 *
 *----------------------------------------------------------------------
 */

static void
OnDestroy(
    ANativeActivity *activity)
{
    (void)activity;
    exit(0);
}

/*
 *----------------------------------------------------------------------
 *
 * OnWindowFocusChanged --
 *
 *	The window of the activity gains or loses the focus.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static void
OnWindowFocusChanged(
    ANativeActivity *activity,
    int hasFocus)
{
    TkAndroidEvent ev;

    (void)activity;
    memset(&ev, 0, sizeof(ev));
    ev.type = TKANDROID_FOCUS;
    ev.u.hasFocus = hasFocus;
    TkAndroidPostEvent(&ev);
}

/*
 *----------------------------------------------------------------------
 *
 * OnNativeWindowCreated --
 *
 *	The activity has a window, which Tk gets a reference to.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static void
OnNativeWindowCreated(
    ANativeActivity *activity,
    ANativeWindow *window)
{
    TkAndroidEvent ev;

    (void)activity;
    ANativeWindow_acquire(window);
    memset(&ev, 0, sizeof(ev));
    ev.type = TKANDROID_WINDOW_CREATED;
    ev.u.window = window;
    TkAndroidPostEvent(&ev);
}

/*
 *----------------------------------------------------------------------
 *
 * OnNativeWindowDestroyed --
 *
 *	The window of the activity goes away: it must not be drawn into once
 *	this returns, so Tk is waited for, but not forever, since it may be
 *	busy (the reference it holds keeps the window valid anyway).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static void
OnNativeWindowDestroyed(
    ANativeActivity *activity,
    ANativeWindow *window)
{
    TkAndroidEvent ev;

    (void)activity;
    ANativeWindow_acquire(window);
    memset(&ev, 0, sizeof(ev));
    ev.type = TKANDROID_WINDOW_DESTROYED;
    ev.u.window = window;
    TkAndroidPostEventAndWait(&ev, 500);
}

/*
 *----------------------------------------------------------------------
 *
 * OnNativeWindowResized, OnNativeWindowRedrawNeeded --
 *
 *	The window of the activity was resized, or must be drawn again.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static void
OnNativeWindowResized(
    ANativeActivity *activity,
    ANativeWindow *window)
{
    (void)activity;
    (void)window;
    PostSimple(TKANDROID_WINDOW_RESIZED);
}

static void
OnNativeWindowRedrawNeeded(
    ANativeActivity *activity,
    ANativeWindow *window)
{
    (void)activity;
    (void)window;
    PostSimple(TKANDROID_REDRAW);
}

/*
 *----------------------------------------------------------------------
 *
 * OnInputQueueCreated, OnInputQueueDestroyed --
 *
 *	Read the input queue of the activity in the looper of the main
 *	thread, or stop.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The queue is attached to the looper, or detached.
 *
 *----------------------------------------------------------------------
 */

static void
OnInputQueueCreated(
    ANativeActivity *activity,
    AInputQueue *queue)
{
    (void)activity;
    inputQueue = queue;
    AInputQueue_attachLooper(queue, ALooper_forThread(), 1, InputCallback,
	    queue);
}

static void
OnInputQueueDestroyed(
    ANativeActivity *activity,
    AInputQueue *queue)
{
    (void)activity;
    AInputQueue_detachLooper(queue);
    if (inputQueue == queue) {
	inputQueue = NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * OnContentRectChanged --
 *
 *	The part of the window not covered by the system bars and the soft
 *	keyboard changed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static void
OnContentRectChanged(
    ANativeActivity *activity,
    const ARect *rect)
{
    TkAndroidEvent ev;

    (void)activity;
    memset(&ev, 0, sizeof(ev));
    ev.type = TKANDROID_CONTENT_RECT;
    ev.u.rect.left = rect->left;
    ev.u.rect.top = rect->top;
    ev.u.rect.right = rect->right;
    ev.u.rect.bottom = rect->bottom;
    TkAndroidPostEvent(&ev);
}

/*
 *----------------------------------------------------------------------
 *
 * OnConfigurationChanged --
 *
 *	The configuration changed: its density may have.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	An event is posted.
 *
 *----------------------------------------------------------------------
 */

static void
OnConfigurationChanged(
    ANativeActivity *activity)
{
    PostDensity(activity);
}

/*
 *----------------------------------------------------------------------
 *
 * ANativeActivity_onCreate --
 *
 *	The entry point of the library: set the callbacks of the activity,
 *	and start Tcl the first time.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Starts the thread of Tcl.
 *
 *----------------------------------------------------------------------
 */

JNIEXPORT void
ANativeActivity_onCreate(
    ANativeActivity *activity,
    void *savedState,
    size_t savedStateSize)
{
    pthread_t thread;
    pthread_attr_t attr;

    (void)savedState;
    (void)savedStateSize;
    activity->callbacks->onResume = OnResume;
    activity->callbacks->onPause = OnPause;
    activity->callbacks->onDestroy = OnDestroy;
    activity->callbacks->onWindowFocusChanged = OnWindowFocusChanged;
    activity->callbacks->onNativeWindowCreated = OnNativeWindowCreated;
    activity->callbacks->onNativeWindowDestroyed = OnNativeWindowDestroyed;
    activity->callbacks->onNativeWindowResized = OnNativeWindowResized;
    activity->callbacks->onNativeWindowRedrawNeeded =
	    OnNativeWindowRedrawNeeded;
    activity->callbacks->onInputQueueCreated = OnInputQueueCreated;
    activity->callbacks->onInputQueueDestroyed = OnInputQueueDestroyed;
    activity->callbacks->onContentRectChanged = OnContentRectChanged;
    activity->callbacks->onConfigurationChanged = OnConfigurationChanged;
    tkAndroidActivity = activity;
    InitKeyEvent(activity->env);
    RegisterNatives(activity);
    PostDensity(activity);
    if (tclStarted) {
	return;
    }
    tclStarted = true;
    dataPath = strdup(activity->internalDataPath);
    mkdir(dataPath, 0700);
    LogOutput();

    /*
     * Tcl wants a large stack.
     */

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, TclThread, activity) != 0) {
	__android_log_write(ANDROID_LOG_ERROR, LOG_TAG,
		"cannot start the thread of Tcl");
    }
    pthread_attr_destroy(&attr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
