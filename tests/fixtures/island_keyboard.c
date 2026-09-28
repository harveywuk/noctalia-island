// Virtual keyboard used only on the private test compositor.
#define _GNU_SOURCE
#include <wayland-client.h>
#include "keyboard-client.h"
#include <xkbcommon/xkbcommon.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static struct zwp_virtual_keyboard_manager_v1 *manager;
static struct wl_seat *seat;
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  (void)data; (void)version;
  if (!strcmp(interface, "zwp_virtual_keyboard_manager_v1"))
    manager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
  if (!strcmp(interface, "wl_seat"))
    seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) {(void)data; (void)registry; (void)name;}
int main(void) {
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) return 1;
  struct wl_registry *registry = wl_display_get_registry(display);
  const struct wl_registry_listener listener = {global, removed};
  wl_registry_add_listener(registry, &listener, NULL);
  wl_display_roundtrip(display);
  if (!manager || !seat) return 2;
  struct zwp_virtual_keyboard_v1 *keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager, seat);
  struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
  char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  size_t size = strlen(text) + 1;
  int fd = memfd_create("island-test-keymap", 0);
  if (fd < 0 || write(fd, text, size) != (ssize_t)size) return 3;
  zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
  wl_display_roundtrip(display);
  uint32_t shift = 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
  close(fd); free(text); xkb_keymap_unref(keymap); xkb_context_unref(context);
  unsigned key;
  char line[64];
  while (fgets(line, sizeof(line), stdin)) {
    uint32_t modifiers = 0;
    if (!strncmp(line, "shift-tab", 9)) { key = 15; modifiers = shift; }
    else if (sscanf(line, "%u", &key) != 1) return 4;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    uint32_t stamp = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
    zwp_virtual_keyboard_v1_modifiers(keyboard, modifiers, 0, 0, 0);
    zwp_virtual_keyboard_v1_key(keyboard, stamp, key, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(keyboard, stamp + 1, key, WL_KEYBOARD_KEY_STATE_RELEASED);
    zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
    wl_display_roundtrip(display);
    puts("ok"); fflush(stdout);
  }
  zwp_virtual_keyboard_v1_destroy(keyboard);
  wl_display_disconnect(display);
}
