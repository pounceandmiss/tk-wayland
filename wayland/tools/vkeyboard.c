/*
 * vkeyboard.c --
 *
 *	A virtual keyboard for testing Tk on wlroots compositors (such as
 *	headless sway), which have no keyboard device. It gives the seat a
 *	keyboard with the default keymap of xkbcommon (all the keys of a usual
 *	keyboard, which [event generate] needs) and keeps it until it is
 *	killed or the compositor ends. It types nothing.
 *
 *	Usage: vkeyboard (with WAYLAND_DISPLAY).
 *
 *	"make vkeyboard" builds it in the build directory of Tk, with the
 *	virtual-keyboard-unstable-v1 protocol of wlroots in this directory;
 *	"make test-sway" uses it.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "vk-client.h"

static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *manager;

static void
Global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
    (void)data; (void)version;
    if (!strcmp(interface, wl_seat_interface.name) && !seat) {
	seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (!strcmp(interface,
	    zwp_virtual_keyboard_manager_v1_interface.name)) {
	manager = wl_registry_bind(registry, name,
		&zwp_virtual_keyboard_manager_v1_interface, 1);
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

int
main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    struct xkb_context *context;
    struct xkb_keymap *keymap;
    struct zwp_virtual_keyboard_v1 *keyboard;
    char *text;
    size_t size;
    int fd;

    if (!display) {
	fprintf(stderr, "usage: vkeyboard (with WAYLAND_DISPLAY)\n");
	return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display),
	    &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!manager || !seat) {
	fprintf(stderr, "vkeyboard: no virtual keyboard support\n");
	return 1;
    }

    /*
     * The keymap is given in a file, with its terminating null.
     */

    context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    keymap = context ? xkb_keymap_new_from_names(context, NULL,
	    XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    text = keymap ? xkb_keymap_get_as_string(keymap,
	    XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    if (text == NULL) {
	fprintf(stderr, "vkeyboard: cannot compile the default keymap\n");
	return 1;
    }
    size = strlen(text) + 1;
    fd = memfd_create("vkeyboard-keymap", MFD_CLOEXEC);
    if (fd < 0 || write(fd, text, size) != (ssize_t)size) {
	fprintf(stderr, "vkeyboard: cannot write the keymap\n");
	return 1;
    }
    keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
	    manager, seat);
    zwp_virtual_keyboard_v1_keymap(keyboard,
	    WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
    wl_display_roundtrip(display);
    close(fd);
    free(text);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    printf("ready\n");
    fflush(stdout);
    while (wl_display_dispatch(display) != -1) {
	/* Keep the keyboard. */
    }
    return 0;
}
