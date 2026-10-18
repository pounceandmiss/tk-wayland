/*
 * tkWaylandSelect.c --
 *
 *	The CLIPBOARD and PRIMARY selections of the Wayland port, with
 *	wl_data_device and the primary selection protocol.
 *
 *	Selections owned by the Tk applications of this process are handled
 *	by tkSelect.c. When Tk claims a selection with XSetSelectionOwner, a
 *	data source is offered to the compositor, which gets the data from the
 *	selection handlers of Tk; when another client takes the selection, the
 *	owner gets a SelectionClear event, as with X. Selections owned by other
 *	clients are read from the offers of the compositor.
 *
 * Copyright © 2026 The Tk developers.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#define _GNU_SOURCE		/* For pipe2. */
#include "tkWaylandInt.h"
#include "tkSelect.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include <poll.h>

/*
 * An offer of the compositor, with its MIME types. Clipboard and primary
 * selection offers have the same interface, so they share this structure.
 */

typedef struct Offer {
    void *offer;		/* wl_data_offer or
				 * zwp_primary_selection_offer_v1. */
    bool primary;
    Tcl_Size numTypes;
    char **types;
} Offer;

/*
 * A selection Tk owns, offered to the compositor.
 */

typedef struct Source {
    void *source;		/* wl_data_source or
				 * zwp_primary_selection_source_v1. */
    bool primary;
    Atom selection;
    Window owner;		/* The window which owns the selection. */
} Source;

static struct wl_data_device_manager *dataDeviceManager = NULL;
static struct wl_data_device *dataDevice = NULL;
static struct zwp_primary_selection_device_manager_v1 *primaryManager = NULL;
static struct zwp_primary_selection_device_v1 *primaryDevice = NULL;

static Offer *clipboardOffer = NULL;	/* The current selections of the */
static Offer *primaryOffer = NULL;	/* compositor. */
static Offer *dndOffer = NULL;		/* Drag and drop is not supported:
					 * this is only to destroy it. */
static Source *clipboardSource = NULL;	/* The selections Tk owns. */
static Source *primarySource = NULL;

/*
 * The MIME types of text, in order of preference.
 */

static const char *const textTypes[] = {
    "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING",
    "TEXT", NULL
};

static bool
IsTextType(
    const char *type)
{
    int i;

    for (i = 0; textTypes[i]; i++) {
	if (!strcmp(type, textTypes[i])) {
	    return true;
	}
    }
    return !strcmp(type, "COMPOUND_TEXT");
}

/*
 *----------------------------------------------------------------------
 *
 * Offers --
 *
 *----------------------------------------------------------------------
 */

static Offer *
NewOffer(
    void *offer,
    bool primary)
{
    Offer *offerPtr = (Offer *)Tcl_Alloc(sizeof(Offer));

    offerPtr->offer = offer;
    offerPtr->primary = primary;
    offerPtr->numTypes = 0;
    offerPtr->types = NULL;
    return offerPtr;
}

static void
AddType(
    Offer *offerPtr,
    const char *type)
{
    offerPtr->types = (char **)Tcl_Realloc(offerPtr->types,
	    (offerPtr->numTypes + 1) * sizeof(char *));
    offerPtr->types[offerPtr->numTypes] = (char *)Tcl_Alloc(strlen(type) + 1);
    strcpy(offerPtr->types[offerPtr->numTypes], type);
    offerPtr->numTypes++;
}

static bool
HasType(
    Offer *offerPtr,
    const char *type)
{
    Tcl_Size i;

    for (i = 0; i < offerPtr->numTypes; i++) {
	if (!strcmp(offerPtr->types[i], type)) {
	    return true;
	}
    }
    return false;
}

static void
FreeOffer(
    Offer *offerPtr)
{
    Tcl_Size i;

    if (offerPtr == NULL) {
	return;
    }
    if (offerPtr->primary) {
	zwp_primary_selection_offer_v1_destroy(
		(struct zwp_primary_selection_offer_v1 *)offerPtr->offer);
    } else {
	wl_data_offer_destroy((struct wl_data_offer *)offerPtr->offer);
    }
    for (i = 0; i < offerPtr->numTypes; i++) {
	Tcl_Free(offerPtr->types[i]);
    }
    Tcl_Free(offerPtr->types);
    Tcl_Free(offerPtr);
}

static void
DataOfferOffer(
    void *data,
    TCL_UNUSED(struct wl_data_offer *),
    const char *type)
{
    AddType((Offer *)data, type);
}

static void
DataOfferSourceActions(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_offer *),
    TCL_UNUSED(uint32_t))
{
}

static void
DataOfferAction(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_offer *),
    TCL_UNUSED(uint32_t))
{
}

static const struct wl_data_offer_listener dataOfferListener = {
    .offer = DataOfferOffer,
    .source_actions = DataOfferSourceActions,
    .action = DataOfferAction
};

static void
PrimaryOfferOffer(
    void *data,
    TCL_UNUSED(struct zwp_primary_selection_offer_v1 *),
    const char *type)
{
    AddType((Offer *)data, type);
}

static const struct zwp_primary_selection_offer_v1_listener
	primaryOfferListener = {
    PrimaryOfferOffer
};

/*
 *----------------------------------------------------------------------
 *
 * Devices --
 *
 *	A new offer is announced before the event which uses it (selection,
 *	or enter for drag and drop).
 *
 *----------------------------------------------------------------------
 */

static Offer *newOffer = NULL;		/* The offer announced last. */

static void
DataDeviceDataOffer(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_device *),
    struct wl_data_offer *offer)
{
    newOffer = NewOffer(offer, false);
    wl_data_offer_add_listener(offer, &dataOfferListener, newOffer);
}

static Offer *
TakeOffer(
    void *offer)
{
    Offer *offerPtr = NULL;

    if (offer && newOffer && newOffer->offer == offer) {
	offerPtr = newOffer;
    }
    newOffer = NULL;
    return offerPtr;
}

static void
DataDeviceEnter(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_device *),
    TCL_UNUSED(uint32_t),
    TCL_UNUSED(struct wl_surface *),
    TCL_UNUSED(wl_fixed_t),
    TCL_UNUSED(wl_fixed_t),
    struct wl_data_offer *offer)
{
    FreeOffer(dndOffer);
    dndOffer = TakeOffer(offer);
}

static void
DataDeviceLeave(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_device *))
{
    FreeOffer(dndOffer);
    dndOffer = NULL;
}

static void
DataDeviceMotion(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_device *),
    TCL_UNUSED(uint32_t),
    TCL_UNUSED(wl_fixed_t),
    TCL_UNUSED(wl_fixed_t))
{
}

static void
DataDeviceDrop(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_device *))
{
}

static void
DataDeviceSelection(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_device *),
    struct wl_data_offer *offer)
{
    FreeOffer(clipboardOffer);
    clipboardOffer = TakeOffer(offer);
}

static const struct wl_data_device_listener dataDeviceListener = {
    .data_offer = DataDeviceDataOffer,
    .enter = DataDeviceEnter,
    .leave = DataDeviceLeave,
    .motion = DataDeviceMotion,
    .drop = DataDeviceDrop,
    .selection = DataDeviceSelection
};

static void
PrimaryDeviceDataOffer(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zwp_primary_selection_device_v1 *),
    struct zwp_primary_selection_offer_v1 *offer)
{
    newOffer = NewOffer(offer, true);
    zwp_primary_selection_offer_v1_add_listener(offer, &primaryOfferListener,
	    newOffer);
}

static void
PrimaryDeviceSelection(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct zwp_primary_selection_device_v1 *),
    struct zwp_primary_selection_offer_v1 *offer)
{
    FreeOffer(primaryOffer);
    primaryOffer = TakeOffer(offer);
}

static const struct zwp_primary_selection_device_v1_listener
	primaryDeviceListener = {
    PrimaryDeviceDataOffer,
    PrimaryDeviceSelection
};

/*
 *----------------------------------------------------------------------
 *
 * TkWlSelectionBind --
 *
 *	Called for the globals of the registry: bind the managers of the
 *	selections.
 *
 * Results:
 *	True if the global was used.
 *
 *----------------------------------------------------------------------
 */

bool
TkWlSelectionBind(
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
    if (!strcmp(interface, wl_data_device_manager_interface.name)) {
	dataDeviceManager = (struct wl_data_device_manager *)
		wl_registry_bind(registry, name,
		&wl_data_device_manager_interface, version < 3 ? version : 3);
	return true;
    }
    if (!strcmp(interface,
	    zwp_primary_selection_device_manager_v1_interface.name)) {
	primaryManager = (struct zwp_primary_selection_device_manager_v1 *)
		wl_registry_bind(registry, name,
		&zwp_primary_selection_device_manager_v1_interface, 1);
	return true;
    }
    return false;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWlSelectionInit --
 *
 *	Create the devices of the selections, once the seat is known.
 *
 *----------------------------------------------------------------------
 */

void
TkWlSelectionInit(void)
{
    if (tkWlDisplay->seat == NULL) {
	return;
    }
    if (dataDeviceManager && dataDevice == NULL) {
	dataDevice = wl_data_device_manager_get_data_device(dataDeviceManager,
		tkWlDisplay->seat);
	wl_data_device_add_listener(dataDevice, &dataDeviceListener, NULL);
    }
    if (primaryManager && primaryDevice == NULL) {
	primaryDevice = zwp_primary_selection_device_manager_v1_get_device(
		primaryManager, tkWlDisplay->seat);
	zwp_primary_selection_device_v1_add_listener(primaryDevice,
		&primaryDeviceListener, NULL);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Sources --
 *
 *	The compositor asks a source for its data in a MIME type, which is
 *	got from the selection handlers of Tk, and written to a file
 *	descriptor.
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

static void
SendData(
    Source *srcPtr,
    const char *type,
    int fd)
{
    TkWindow *winPtr = (TkWindow *)Tk_IdToWindow(tkWlDisplay->display,
	    srcPtr->owner);
    Tcl_DString ds;
    Tcl_InterpState state;
    const char *p;
    Tcl_Size left;

    Tcl_DStringInit(&ds);
    if (winPtr && winPtr->mainPtr && winPtr->mainPtr->interp) {
	Tcl_Interp *interp = winPtr->mainPtr->interp;
	Atom target = IsTextType(type)
		? Tk_InternAtom((Tk_Window)winPtr, "UTF8_STRING")
		: Tk_InternAtom((Tk_Window)winPtr, type);

	state = Tcl_SaveInterpState(interp, TCL_OK);
	if (Tk_GetSelection(interp, (Tk_Window)winPtr, srcPtr->selection,
		target, AppendProc, &ds) != TCL_OK) {
	    Tcl_DStringSetLength(&ds, 0);
	}
	Tcl_RestoreInterpState(interp, state);
    }

    /*
     * The compositor gives a pipe to the reader, which reads it all.
     */

    p = Tcl_DStringValue(&ds);
    left = Tcl_DStringLength(&ds);
    while (left > 0) {
	ssize_t n = write(fd, p, (size_t)left);

	if (n < 0) {
	    if (errno == EINTR) {
		continue;
	    }
	    break;
	}
	p += n;
	left -= n;
    }
    close(fd);
    Tcl_DStringFree(&ds);
}

static void
FreeSource(
    Source *srcPtr)
{
    if (srcPtr->primary) {
	zwp_primary_selection_source_v1_destroy(
		(struct zwp_primary_selection_source_v1 *)srcPtr->source);
    } else {
	wl_data_source_destroy((struct wl_data_source *)srcPtr->source);
    }
    Tcl_Free(srcPtr);
}

/*
 * Another client took the selection: tell the owner, as the X server does.
 */

static void
SourceCancelled(
    Source *srcPtr)
{
    Source **currentPtr = srcPtr->primary ? &primarySource : &clipboardSource;

    if (*currentPtr == srcPtr) {
	XEvent event;

	*currentPtr = NULL;
	memset(&event, 0, sizeof(XEvent));
	event.xselectionclear.type = SelectionClear;
	event.xselectionclear.display = tkWlDisplay->display;
	event.xselectionclear.window = srcPtr->owner;
	event.xselectionclear.selection = srcPtr->selection;
	event.xselectionclear.time = TkWlCurrentTime();
	if (TkWlGetWindow(srcPtr->owner)) {
	    TkWlQueueEvent(&event);
	}
    }
    FreeSource(srcPtr);
}

static void
DataSourceTarget(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_source *),
    TCL_UNUSED(const char *))
{
}

static void
DataSourceSend(
    void *data,
    TCL_UNUSED(struct wl_data_source *),
    const char *type,
    int32_t fd)
{
    SendData((Source *)data, type, fd);
}

static void
DataSourceCancelled(
    void *data,
    TCL_UNUSED(struct wl_data_source *))
{
    SourceCancelled((Source *)data);
}

static void
DataSourceDndDropPerformed(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_source *))
{
}

static void
DataSourceDndFinished(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_source *))
{
}

static void
DataSourceAction(
    TCL_UNUSED(void *),
    TCL_UNUSED(struct wl_data_source *),
    TCL_UNUSED(uint32_t))
{
}

static const struct wl_data_source_listener dataSourceListener = {
    .target = DataSourceTarget,
    .send = DataSourceSend,
    .cancelled = DataSourceCancelled,
    .dnd_drop_performed = DataSourceDndDropPerformed,
    .dnd_finished = DataSourceDndFinished,
    .action = DataSourceAction
};

static void
PrimarySourceSend(
    void *data,
    TCL_UNUSED(struct zwp_primary_selection_source_v1 *),
    const char *type,
    int32_t fd)
{
    SendData((Source *)data, type, fd);
}

static void
PrimarySourceCancelled(
    void *data,
    TCL_UNUSED(struct zwp_primary_selection_source_v1 *))
{
    SourceCancelled((Source *)data);
}

static const struct zwp_primary_selection_source_v1_listener
	primarySourceListener = {
    PrimarySourceSend,
    PrimarySourceCancelled
};

/*
 *----------------------------------------------------------------------
 *
 * XSetSelectionOwner --
 *
 *	Offer a selection which Tk claims to the compositor. The selection is
 *	offered as text, and in the types of the selection handlers of the
 *	owner which look like MIME types.
 *
 *----------------------------------------------------------------------
 */

static void
OfferType(
    Source *srcPtr,
    const char *type)
{
    if (srcPtr->primary) {
	zwp_primary_selection_source_v1_offer(
		(struct zwp_primary_selection_source_v1 *)srcPtr->source, type);
    } else {
	wl_data_source_offer((struct wl_data_source *)srcPtr->source, type);
    }
}

int
XSetSelectionOwner(
    Display *display,
    Atom selection,
    Window owner,
    TCL_UNUSED(Time))
{
    bool primary;
    Source **currentPtr, *srcPtr;
    TkWindow *winPtr;
    TkSelHandler *selPtr;
    int i;

    TkWlNextRequest(display);
    if (selection == XA_PRIMARY) {
	primary = true;
	currentPtr = &primarySource;
	if (primaryDevice == NULL) {
	    return Success;
	}
    } else if (selection == XInternAtom(display, "CLIPBOARD", False)) {
	primary = false;
	currentPtr = &clipboardSource;
	if (dataDevice == NULL) {
	    return Success;
	}
    } else {
	return Success;
    }

    if (*currentPtr) {
	FreeSource(*currentPtr);
	*currentPtr = NULL;
    }
    if (owner == None) {
	if (primary) {
	    zwp_primary_selection_device_v1_set_selection(primaryDevice, NULL,
		    tkWlDisplay->lastSerial);
	} else {
	    wl_data_device_set_selection(dataDevice, NULL,
		    tkWlDisplay->lastSerial);
	}
	return Success;
    }

    srcPtr = (Source *)Tcl_Alloc(sizeof(Source));
    srcPtr->primary = primary;
    srcPtr->selection = selection;
    srcPtr->owner = owner;
    if (primary) {
	srcPtr->source = zwp_primary_selection_device_manager_v1_create_source(
		primaryManager);
	zwp_primary_selection_source_v1_add_listener(
		(struct zwp_primary_selection_source_v1 *)srcPtr->source,
		&primarySourceListener, srcPtr);
    } else {
	srcPtr->source = wl_data_device_manager_create_data_source(
		dataDeviceManager);
	wl_data_source_add_listener((struct wl_data_source *)srcPtr->source,
		&dataSourceListener, srcPtr);
    }
    for (i = 0; textTypes[i]; i++) {
	OfferType(srcPtr, textTypes[i]);
    }
    winPtr = (TkWindow *)Tk_IdToWindow(display, owner);
    for (selPtr = winPtr ? winPtr->selHandlerList : NULL; selPtr;
	    selPtr = selPtr->nextPtr) {
	const char *type = Tk_GetAtomName((Tk_Window)winPtr, selPtr->target);

	if (selPtr->selection == selection && strchr(type, '/')
		&& !IsTextType(type)) {
	    OfferType(srcPtr, type);
	}
    }
    if (primary) {
	zwp_primary_selection_device_v1_set_selection(primaryDevice,
		(struct zwp_primary_selection_source_v1 *)srcPtr->source,
		tkWlDisplay->lastSerial);
    } else {
	wl_data_device_set_selection(dataDevice,
		(struct wl_data_source *)srcPtr->source,
		tkWlDisplay->lastSerial);
    }
    *currentPtr = srcPtr;
    wl_display_flush(tkWlDisplay->wlDisplay);
    return Success;
}

/*
 *----------------------------------------------------------------------
 *
 * TkSelGetSelection --
 *
 *	Retrieve a selection which is not owned by a Tk application of this
 *	process, from the offer of the compositor.
 *
 *----------------------------------------------------------------------
 */

/*
 * Read the data of an offer in a type. Wayland events are dispatched while
 * waiting, since the owner may be this process.
 */

static int
ReceiveData(
    Offer *offerPtr,
    const char *type,
    Tcl_DString *dsPtr)
{
    int fds[2];
    Tcl_Time start, now;

    if (pipe2(fds, O_CLOEXEC) < 0) {
	return TCL_ERROR;
    }
    if (offerPtr->primary) {
	zwp_primary_selection_offer_v1_receive(
		(struct zwp_primary_selection_offer_v1 *)offerPtr->offer, type,
		fds[1]);
    } else {
	wl_data_offer_receive((struct wl_data_offer *)offerPtr->offer, type,
		fds[1]);
    }
    close(fds[1]);
    wl_display_flush(tkWlDisplay->wlDisplay);

    Tcl_GetTime(&start);
    while (true) {
	struct pollfd pfd[2];
	char buf[4096];
	int timeout;

	Tcl_GetTime(&now);
	timeout = 5000 - (int)((now.sec - start.sec) * 1000
		+ (now.usec - start.usec) / 1000);
	if (timeout <= 0) {
	    close(fds[0]);
	    return TCL_ERROR;
	}
	pfd[0].fd = fds[0];
	pfd[0].events = POLLIN;
	pfd[1].fd = wl_display_get_fd(tkWlDisplay->wlDisplay);
	pfd[1].events = POLLIN;
	if (poll(pfd, 2, timeout) < 0) {
	    if (errno == EINTR) {
		continue;
	    }
	    close(fds[0]);
	    return TCL_ERROR;
	}
	if (pfd[1].revents & POLLIN) {
	    wl_display_dispatch(tkWlDisplay->wlDisplay);
	}
	if (pfd[0].revents & (POLLIN | POLLHUP)) {
	    ssize_t n = read(fds[0], buf, sizeof(buf));

	    if (n > 0) {
		Tcl_DStringAppend(dsPtr, buf, (Tcl_Size)n);
	    } else if (n == 0 || errno != EINTR) {
		break;
	    }
	}
    }
    close(fds[0]);
    return TCL_OK;
}

int
TkSelGetSelection(
    Tcl_Interp *interp,
    Tk_Window tkwin,
    Atom selection,
    Atom target,
    Tk_GetSelProc *proc,
    void *clientData)
{
    Offer *offerPtr = NULL;
    const char *targetName = Tk_GetAtomName(tkwin, target);
    const char *type = NULL;
    Tcl_DString ds;
    int i, result;

    if (selection == XA_PRIMARY) {
	offerPtr = primaryOffer;
    } else if (selection == Tk_InternAtom(tkwin, "CLIPBOARD")) {
	offerPtr = clipboardOffer;
    }
    if (offerPtr == NULL) {
	goto notDefined;
    }

    if (!strcmp(targetName, "TARGETS")) {
	/*
	 * The types of the offer, with the X names of text.
	 */

	Tcl_Size j;
	bool text = false;

	Tcl_DStringInit(&ds);
	Tcl_DStringAppendElement(&ds, "TARGETS");
	for (j = 0; j < offerPtr->numTypes; j++) {
	    if (IsTextType(offerPtr->types[j])) {
		text = true;
	    } else {
		Tcl_DStringAppendElement(&ds, offerPtr->types[j]);
	    }
	}
	if (text) {
	    Tcl_DStringAppendElement(&ds, "UTF8_STRING");
	    Tcl_DStringAppendElement(&ds, "STRING");
	    Tcl_DStringAppendElement(&ds, "TEXT");
	}
	result = proc(clientData, interp, Tcl_DStringValue(&ds));
	Tcl_DStringFree(&ds);
	return result;
    }

    if (IsTextType(targetName)) {
	for (i = 0; textTypes[i]; i++) {
	    if (HasType(offerPtr, textTypes[i])) {
		type = textTypes[i];
		break;
	    }
	}
    } else if (HasType(offerPtr, targetName)) {
	type = targetName;
    }
    if (type == NULL) {
	goto notDefined;
    }

    Tcl_DStringInit(&ds);
    if (ReceiveData(offerPtr, type, &ds) != TCL_OK) {
	Tcl_DStringFree(&ds);
	Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		"selection owner didn't respond"));
	Tcl_SetErrorCode(interp, "TK", "SELECTION", "TIMEOUT", (char *)NULL);
	return TCL_ERROR;
    }
    result = proc(clientData, interp, Tcl_DStringValue(&ds));
    Tcl_DStringFree(&ds);
    return result;

  notDefined:
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
 *	Only SelectionClear events are generated.
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
