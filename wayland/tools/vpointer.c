/*
 * vpointer.c --
 *
 *	A virtual pointer for testing Tk on wlroots compositors (such as
 *	headless sway), which have no pointer device. It reads commands from
 *	its standard input, one per line, and keeps the pointer while it runs:
 *
 *	    move X Y		move to X,Y in the layout (pixels)
 *	    press N		press button N (1 left, 2 middle, 3 right)
 *	    release N		release button N
 *	    scroll DY		scroll vertically by DY (positive: down)
 *	    fscroll DY		scroll vertically by DY pixels with fingers
 *				(as on a touchpad)
 *
 *	Usage: vpointer WIDTH HEIGHT (the size of the layout).
 *
 *	Build it with the wlr-virtual-pointer-unstable-v1 protocol of
 *	wlr-protocols:
 *
 *	    P=/usr/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml
 *	    wayland-scanner client-header $P vp-client.h
 *	    wayland-scanner private-code $P vp-protocol.c
 *	    cc -o vpointer vpointer.c vp-protocol.c -lwayland-client
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "vp-client.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *manager;

static void
Global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
    (void)data; (void)version;
    if (!strcmp(interface, wl_seat_interface.name) && !seat) {
	seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (!strcmp(interface,
	    zwlr_virtual_pointer_manager_v1_interface.name)) {
	manager = wl_registry_bind(registry, name,
		&zwlr_virtual_pointer_manager_v1_interface, 1);
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

static uint32_t
Now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int
main(int argc, char **argv)
{
    struct wl_display *display = wl_display_connect(NULL);
    struct zwlr_virtual_pointer_v1 *pointer;
    unsigned width, height;
    char line[256];
    static const uint32_t buttons[] = {0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT};

    if (argc != 3 || !display) {
	fprintf(stderr, "usage: vpointer WIDTH HEIGHT (with WAYLAND_DISPLAY)\n");
	return 1;
    }
    width = atoi(argv[1]);
    height = atoi(argv[2]);
    wl_registry_add_listener(wl_display_get_registry(display),
	    &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!manager || !seat) {
	fprintf(stderr, "vpointer: no virtual pointer support\n");
	return 1;
    }
    pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager,
	    seat);
    wl_display_roundtrip(display);
    printf("ready\n");
    fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
	int x, y, n;

	if (sscanf(line, "move %d %d", &x, &y) == 2) {
	    zwlr_virtual_pointer_v1_motion_absolute(pointer, Now(), x, y,
		    width, height);
	} else if (sscanf(line, "press %d", &n) == 1 && n >= 1 && n <= 3) {
	    zwlr_virtual_pointer_v1_button(pointer, Now(), buttons[n],
		    WL_POINTER_BUTTON_STATE_PRESSED);
	} else if (sscanf(line, "release %d", &n) == 1 && n >= 1 && n <= 3) {
	    zwlr_virtual_pointer_v1_button(pointer, Now(), buttons[n],
		    WL_POINTER_BUTTON_STATE_RELEASED);
	} else if (sscanf(line, "fscroll %d", &n) == 1) {
	    zwlr_virtual_pointer_v1_axis_source(pointer,
		    WL_POINTER_AXIS_SOURCE_FINGER);
	    zwlr_virtual_pointer_v1_axis(pointer, Now(),
		    WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(n));
	} else if (sscanf(line, "scroll %d", &n) == 1) {
	    zwlr_virtual_pointer_v1_axis_source(pointer,
		    WL_POINTER_AXIS_SOURCE_WHEEL);
	    zwlr_virtual_pointer_v1_axis_discrete(pointer, Now(),
		    WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(15 * n),
		    n);
	} else {
	    continue;
	}
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_roundtrip(display);
	printf("ok\n");
	fflush(stdout);
    }
    zwlr_virtual_pointer_v1_destroy(pointer);
    wl_display_roundtrip(display);
    return 0;
}
