/*
 * vinputmethod.c --
 *
 *	A virtual input method for testing Tk on wlroots compositors (such as
 *	headless sway), which enable the text input of clients only while an
 *	input method runs. It prints a line whenever the compositor activates
 *	or deactivates it, which is when an on-screen keyboard would show or
 *	hide:
 *
 *	    activate
 *	    deactivate
 *
 *	It prints "ready" once it has its input method, and runs until its
 *	standard input is closed.
 *
 *	"make vinputmethod" builds it in the build directory of Tk, with the
 *	input-method-unstable-v2 protocol of wlroots in this directory; "make
 *	test-sway" uses it.
 */

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "im-client.h"

static struct wl_seat *seat;
static struct zwp_input_method_manager_v2 *manager;
static int pendingActive, active;

static void
Global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
    (void)data; (void)version;
    if (!strcmp(interface, wl_seat_interface.name) && !seat) {
	seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (!strcmp(interface,
	    zwp_input_method_manager_v2_interface.name)) {
	manager = wl_registry_bind(registry, name,
		&zwp_input_method_manager_v2_interface, 1);
    }
}

static void
GlobalRemove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registryListener = {
    Global, GlobalRemove
};

static void
Activate(void *data, struct zwp_input_method_v2 *im)
{
    (void)data; (void)im;
    pendingActive = 1;
}

static void
Deactivate(void *data, struct zwp_input_method_v2 *im)
{
    (void)data; (void)im;
    pendingActive = 0;
}

static void
SurroundingText(void *data, struct zwp_input_method_v2 *im,
	const char *text, uint32_t cursor, uint32_t anchor)
{
    (void)data; (void)im; (void)text; (void)cursor; (void)anchor;
}

static void
TextChangeCause(void *data, struct zwp_input_method_v2 *im, uint32_t cause)
{
    (void)data; (void)im; (void)cause;
}

static void
ContentType(void *data, struct zwp_input_method_v2 *im, uint32_t hint,
	uint32_t purpose)
{
    (void)data; (void)im; (void)hint; (void)purpose;
}

/*
 * Pending changes apply at "done": the state is printed when it changes,
 * and a new activation while active (the client enabled its text input
 * again, for another text field) is printed as such.
 */

static void
Done(void *data, struct zwp_input_method_v2 *im)
{
    (void)data; (void)im;
    if (pendingActive != active) {
	active = pendingActive;
	printf("%s\n", active ? "activate" : "deactivate");
	fflush(stdout);
    }
}

static void
Unavailable(void *data, struct zwp_input_method_v2 *im)
{
    (void)data; (void)im;
    fprintf(stderr, "vinputmethod: another input method is running\n");
    exit(1);
}

static const struct zwp_input_method_v2_listener imListener = {
    Activate, Deactivate, SurroundingText, TextChangeCause, ContentType,
    Done, Unavailable
};

int
main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    struct zwp_input_method_v2 *im;
    struct pollfd fds[2];

    if (display == NULL) {
	fprintf(stderr, "vinputmethod: cannot connect to the compositor\n");
	return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display),
	    &registryListener, NULL);
    wl_display_roundtrip(display);
    if (seat == NULL || manager == NULL) {
	fprintf(stderr, "vinputmethod: no seat or input method manager\n");
	return 1;
    }
    im = zwp_input_method_manager_v2_get_input_method(manager, seat);
    zwp_input_method_v2_add_listener(im, &imListener, NULL);
    wl_display_roundtrip(display);
    printf("ready\n");
    fflush(stdout);

    fds[0].fd = wl_display_get_fd(display);
    fds[0].events = POLLIN;
    fds[1].fd = 0;
    fds[1].events = POLLIN;
    for (;;) {
	char buf[256];

	wl_display_flush(display);
	if (poll(fds, 2, -1) < 0) {
	    break;
	}
	if (fds[0].revents & POLLIN) {
	    if (wl_display_dispatch(display) < 0) {
		break;
	    }
	}
	if ((fds[1].revents & (POLLIN | POLLHUP))
		&& read(0, buf, sizeof(buf)) <= 0) {
	    break;
	}
    }
    return 0;
}

