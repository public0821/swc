#include "seat.h"

#include "evdev_device.h"
#include "util.h"
#include "binding.h"
#include "event.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct evdev_device_entry
{
    struct swc_evdev_device device;
    struct wl_listener event_listener;
    struct swc_seat * seat;
    struct wl_list link;
};

static void handle_key(struct swc_seat * seat, uint32_t time, uint32_t key,
                       uint32_t state)
{
    uint32_t * pressed_key;
    struct swc_keyboard * keyboard = &seat->keyboard;
    struct swc_xkb * xkb = &seat->xkb;
    struct wl_display * display;
    uint32_t serial;
    enum xkb_key_direction direction;

    if (keyboard->focus.resource)
    {
        struct wl_client * focus_client
            = wl_resource_get_client(keyboard->focus.resource);
        display = wl_client_get_display(focus_client);
    }

    /* Update keyboard state state */
    wl_array_for_each(pressed_key, &keyboard->keys)
    {
        if (*pressed_key == key)
        {
            /* Ignore repeat evdev events. */
            if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
                return;
            else
            {
                /* Remove the key from the array */
                uint32_t bytes_to_copy = keyboard->keys.size + 1
                    - (((void *) pressed_key) - keyboard->keys.data);

                if (bytes_to_copy > 0)
                    memmove(pressed_key, pressed_key + 1, bytes_to_copy);

                keyboard->keys.size -= sizeof key;

                break;
            }
        }
    }

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    {
        //keyboard->grab_key = key;
        //keyboard->grab_time = time;
        pressed_key = wl_array_add(&keyboard->keys, sizeof key);
        *pressed_key = key;
    }

    /* See if the key press is not handled by the compositor */
    if (!(keyboard->handler && keyboard->handler->key)
        || !keyboard->handler->key(keyboard, time, key, state))
    {
        if (keyboard->focus.resource)
        {
            serial = wl_display_next_serial(display);
            wl_keyboard_send_key(keyboard->focus.resource, serial, time, key, state);

            if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
                printf("\t-> sent to client\n");
        }
    }

    /* Update XKB state. Apparently the keycodes are offset by 8 in XKB. */
    direction = state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN
                                                       : XKB_KEY_UP;
    xkb_state_update_key(xkb->state, key + 8, direction);

    {
        uint32_t mods_depressed, mods_latched, mods_locked, mods_active;
        uint32_t group;

        mods_depressed = xkb_state_serialize_mods(xkb->state, XKB_STATE_DEPRESSED);
        mods_latched = xkb_state_serialize_mods(xkb->state, XKB_STATE_LATCHED);
        mods_locked = xkb_state_serialize_mods(xkb->state, XKB_STATE_LOCKED);
        mods_active = mods_depressed | mods_latched;

        group = xkb_state_serialize_layout(xkb->state, XKB_STATE_LAYOUT_EFFECTIVE);

        if (mods_depressed != keyboard->modifiers.mods_depressed
            || mods_latched != keyboard->modifiers.mods_latched
            || mods_locked != keyboard->modifiers.mods_locked
            || group != keyboard->modifiers.group)
        {
            if (keyboard->focus.resource)
            {
                serial = wl_display_next_serial(display);
                wl_keyboard_send_modifiers(keyboard->focus.resource, serial,
                                           mods_depressed, mods_latched,
                                           mods_locked, group);
            }
        }

        keyboard->modifiers.mods_depressed = mods_depressed;
        keyboard->modifiers.mods_latched = mods_latched;
        keyboard->modifiers.mods_locked = mods_locked;
        keyboard->modifiers.group = group;
    }
}

static void handle_button(struct swc_seat * seat, uint32_t time,
                          uint32_t button, uint32_t state)
{
}

static void handle_relative_motion(struct swc_seat * seat, uint32_t time,
                                   wl_fixed_t dx, wl_fixed_t dy)
{
}

static void handle_evdev_event(struct wl_listener * listener, void * data)
{
    struct evdev_device_entry * entry;
    struct swc_event * event = data;
    struct swc_evdev_device_event_data * evdev_data = event->data;

    entry = wl_container_of(listener, entry, event_listener);

    switch (event->type)
    {
        case SWC_EVDEV_DEVICE_EVENT_KEY:
            handle_key(entry->seat, evdev_data->time, evdev_data->key.key,
                       evdev_data->key.state);
            break;
        case SWC_EVDEV_DEVICE_EVENT_BUTTON:
            handle_button(entry->seat, evdev_data->time,
                          evdev_data->button.button, evdev_data->button.state);
            break;
        case SWC_EVDEV_DEVICE_EVENT_RELATIVE_MOTION:
            handle_relative_motion(entry->seat, evdev_data->time,
                                   evdev_data->relative_motion.dx,
                                   evdev_data->relative_motion.dy);
            break;
        case SWC_EVDEV_DEVICE_EVENT_ABSOLUTE_MOTION:
            break;
    }
}

/* Wayland Seat Interface */
static void get_pointer(struct wl_client * client, struct wl_resource * resource,
                        uint32_t id)
{
    struct swc_seat * seat = resource->data;
    struct swc_pointer * pointer = &seat->pointer;

    swc_pointer_bind(pointer, client, id);

    if (pointer->focus.surface
        && wl_resource_get_client(pointer->focus.surface->resource) == client)
    {
        swc_pointer_set_focus(pointer, pointer->focus.surface);
    }
}

static void get_keyboard(struct wl_client * client, struct wl_resource * resource,
                         uint32_t id)
{
    struct wl_resource * client_resource;
    struct swc_seat * seat = wl_resource_get_user_data(resource);
    struct swc_keyboard * keyboard = &seat->keyboard;

    client_resource = swc_keyboard_bind(keyboard, client, id);

    wl_keyboard_send_keymap(client_resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                            seat->xkb.keymap.fd, seat->xkb.keymap.size);

    if (keyboard->focus.surface
        && wl_resource_get_client(keyboard->focus.surface->resource) == client)
    {
        printf("focusing\n");
        swc_keyboard_set_focus(keyboard, keyboard->focus.surface);
    }
}

static void get_touch(struct wl_client * client, struct wl_resource * resource,
               uint32_t id)
{
    /*
    struct wl_resource * client_resource;
    struct swc_seat * seat = resource->data;
    struct wl_touch * touch = &seat->touch;

    client_resource = wl_client_add_object(client, &wl_touch_interface,
                                           NULL, id, seat);
    client_resource->destroy = &swc_unbind_resource;

    wl_list_insert(&touch->resources, &client_resource->link);
    */
}

struct wl_seat_interface seat_implementation = {
    .get_pointer = &get_pointer,
    .get_keyboard = &get_keyboard,
    .get_touch = &get_touch
};

static void bind_seat(struct wl_client * client, void * data, uint32_t version,
                      uint32_t id)
{
    struct swc_seat * seat = data;
    struct wl_resource * resource;

    resource = wl_client_add_object(client, &wl_seat_interface,
                                    &seat_implementation, id, seat);
    wl_list_insert(&seat->resources, &resource->link);
    wl_resource_set_destructor(resource, &swc_unbind_resource);

    wl_seat_send_capabilities(resource, seat->capabilities);
}

static void update_capabilities(struct swc_seat * seat)
{
    struct wl_resource * resource;

    wl_list_for_each(resource, &seat->resources, link)
        wl_seat_send_capabilities(resource, seat->capabilities);
}

static void add_device(struct swc_seat * seat, struct udev_device * udev_device)
{
    const char * device_seat;
    const char * device_path;
    struct evdev_device_entry * entry;

    device_seat = udev_device_get_property_value(udev_device, "ID_SEAT");

    /* If the ID_SEAT property is not set, the device belongs to seat0. */
    if (!device_seat)
        device_seat = "seat0";

    if (strcmp(device_seat, seat->name) != 0)
        return;

    entry = malloc(sizeof *entry);

    if (!entry)
    {
        printf("could not allocate evdev device\n");
        return;
    }

    entry->seat = seat;
    entry->event_listener.notify = &handle_evdev_event;

    if (!swc_evdev_device_initialize(&entry->device, udev_device))
    {
        free(entry);
        return;
    }

    wl_signal_add(&entry->device.event_signal, &entry->event_listener);

    if (!(seat->capabilities & WL_SEAT_CAPABILITY_POINTER)
        && entry->device.capabilities & WL_SEAT_CAPABILITY_POINTER)
    {
        printf("initializing pointer\n");
        swc_pointer_initialize(&seat->pointer);
        seat->capabilities |= WL_SEAT_CAPABILITY_POINTER;
        update_capabilities(seat);
    }

    if (!(seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD)
        && entry->device.capabilities & WL_SEAT_CAPABILITY_KEYBOARD)
    {
        printf("initializing keyboard\n");
        swc_keyboard_initialize(&seat->keyboard);
        seat->capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
        update_capabilities(seat);
    }

    seat->capabilities |= entry->device.capabilities;

    wl_list_insert(&seat->devices, &entry->link);
}

bool swc_seat_initialize(struct swc_seat * seat, struct udev * udev,
                         const char * seat_name)
{
    seat->name = strdup(seat_name);
    seat->capabilities = 0;

    if (!swc_xkb_initialize(&seat->xkb))
    {
        printf("could not initialize XKB\n");
        goto error_name;
    }

    wl_list_init(&seat->resources);
    wl_signal_init(&seat->destroy_signal);
    wl_list_init(&seat->devices);
    swc_seat_add_devices(seat, udev);

    return true;

  error_name:
    free(seat->name);
  error_base:
    return false;
}

void swc_seat_finish(struct swc_seat * seat)
{
    struct evdev_device_entry * entry, * tmp;

    wl_signal_emit(&seat->destroy_signal, seat);

    if (seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD)
        swc_keyboard_finish(&seat->keyboard);

    if (seat->capabilities & WL_SEAT_CAPABILITY_POINTER)
        swc_pointer_finish(&seat->pointer);

    free(seat->name);
    swc_xkb_finish(&seat->xkb);

    wl_list_for_each_safe(entry, tmp, &seat->devices, link)
    {
        swc_evdev_device_finish(&entry->device);
        free(entry);
    }
}

void swc_seat_add_globals(struct swc_seat * seat, struct wl_display * display)
{
    wl_display_add_global(display, &wl_seat_interface, seat, &bind_seat);
}

void swc_seat_add_event_sources(struct swc_seat * seat,
                                struct wl_event_loop * event_loop)
{
    struct evdev_device_entry * entry;

    wl_list_for_each(entry, &seat->devices, link)
    {
        swc_evdev_device_add_event_sources(&entry->device, event_loop);
    }
}

void swc_seat_add_devices(struct swc_seat * seat, struct udev * udev)
{
    struct udev_enumerate * enumerate;
    struct udev_list_entry * entry;
    const char * path;
    struct udev_device * device;

    enumerate = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_add_match_sysname(enumerate, "event[0-9]*");

    udev_enumerate_scan_devices(enumerate);

    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate))
    {
        path = udev_list_entry_get_name(entry);
        device = udev_device_new_from_syspath(udev, path);
        add_device(seat, device);
        udev_device_unref(device);
    }

    udev_enumerate_unref(enumerate);
}

