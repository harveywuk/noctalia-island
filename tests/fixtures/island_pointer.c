// A private virtual pointer for the headless island interaction test.
#include <wayland-client.h>
#include "pointer-client.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
static struct zwlr_virtual_pointer_manager_v1 *manager;
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  (void)data; (void)version;
  if (!strcmp(interface, "zwlr_virtual_pointer_manager_v1"))
    manager = wl_registry_bind(registry,name,&zwlr_virtual_pointer_manager_v1_interface,1);
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) {(void)data;(void)registry;(void)name;}
int main(void) {
  struct wl_display *display=wl_display_connect(NULL);
  if (!display) return 1;
  struct wl_registry *registry=wl_display_get_registry(display);
  const struct wl_registry_listener listener={global,removed};
  wl_registry_add_listener(registry,&listener,NULL);
  wl_display_roundtrip(display);
  if (!manager) return 2;
  struct zwlr_virtual_pointer_v1 *pointer=zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager,NULL);
  wl_display_roundtrip(display);
  char line[128];
  while(fgets(line,sizeof(line),stdin)) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    uint32_t stamp=(uint32_t)(now.tv_sec*1000+now.tv_nsec/1000000);
    unsigned x,y;
    int steps;
    if(sscanf(line,"move %u %u",&x,&y)==2)
      zwlr_virtual_pointer_v1_motion_absolute(pointer,stamp,x,y,1280,720);
    else if(sscanf(line,"scroll %d",&steps)==1) {
      zwlr_virtual_pointer_v1_axis_source(pointer,WL_POINTER_AXIS_SOURCE_WHEEL);
      zwlr_virtual_pointer_v1_axis_discrete(pointer,stamp,WL_POINTER_AXIS_VERTICAL_SCROLL,wl_fixed_from_int(steps*15),steps);
    }
    else if(!strncmp(line,"press",5)) zwlr_virtual_pointer_v1_button(pointer,stamp,BTN_LEFT,WL_POINTER_BUTTON_STATE_PRESSED);
    else if(!strncmp(line,"release",7)) zwlr_virtual_pointer_v1_button(pointer,stamp,BTN_LEFT,WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    puts("ok"); fflush(stdout);
  }
  zwlr_virtual_pointer_v1_destroy(pointer);
  zwlr_virtual_pointer_manager_v1_destroy(manager);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
}
