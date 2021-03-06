#define _POSIX_C_SOURCE 200112L
#include <EGL/egl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server.h>
#include <wlr/backend/interface.h>
#include <wlr/backend/x11.h>
#include <wlr/interfaces/wlr_input_device.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/util/log.h>
#include <X11/Xlib-xcb.h>
#include <xcb/glx.h>
#include <xcb/xcb.h>
#ifdef __linux__
#include <linux/input-event-codes.h>
#elif __FreeBSD__
#include <dev/evdev/input-event-codes.h>
#endif
#include "backend/x11.h"
#include "util/signal.h"

static struct wlr_backend_impl backend_impl;
static struct wlr_output_impl output_impl;
static struct wlr_input_device_impl input_device_impl = { 0 };

static uint32_t xcb_button_to_wl(uint32_t button) {
	switch (button) {
	case XCB_BUTTON_INDEX_1: return BTN_LEFT;
	case XCB_BUTTON_INDEX_2: return BTN_MIDDLE;
	case XCB_BUTTON_INDEX_3: return BTN_RIGHT;
	// XXX: I'm not sure the scroll-wheel direction is right
	case XCB_BUTTON_INDEX_4: return BTN_GEAR_UP;
	case XCB_BUTTON_INDEX_5: return BTN_GEAR_DOWN;
	default: return 0;
	}
}

static bool handle_x11_event(struct wlr_x11_backend *x11, xcb_generic_event_t *event) {
	struct wlr_x11_output *output = &x11->output;

	switch (event->response_type) {
	case XCB_EXPOSE: {
		wlr_output_send_frame(&output->wlr_output);
		break;
	}
	case XCB_KEY_PRESS:
	case XCB_KEY_RELEASE: {
		xcb_key_press_event_t *ev = (xcb_key_press_event_t *)event;
		struct wlr_event_keyboard_key key = {
			.time_msec = ev->time,
			.keycode = ev->detail - 8,
			.state = event->response_type == XCB_KEY_PRESS ?
				WLR_KEY_PRESSED : WLR_KEY_RELEASED,
			.update_state = true,
		};

		// TODO use xcb-xkb for more precise modifiers state?
		wlr_keyboard_notify_key(&x11->keyboard, &key);
		x11->time = ev->time;
		break;
	}
	case XCB_BUTTON_PRESS: {
		xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;

		if (ev->detail == XCB_BUTTON_INDEX_4 ||
				ev->detail == XCB_BUTTON_INDEX_5) {
			double delta = (ev->detail == XCB_BUTTON_INDEX_4 ? -15 : 15);
			struct wlr_event_pointer_axis axis = {
				.device = &x11->pointer_dev,
				.time_msec = ev->time,
				.source = WLR_AXIS_SOURCE_WHEEL,
				.orientation = WLR_AXIS_ORIENTATION_VERTICAL,
				.delta = delta,
			};
			wlr_signal_emit_safe(&x11->pointer.events.axis, &axis);
			x11->time = ev->time;
			break;
		}
	}
	/* fallthrough */
	case XCB_BUTTON_RELEASE: {
		xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;

		if (ev->detail != XCB_BUTTON_INDEX_4 &&
				ev->detail != XCB_BUTTON_INDEX_5) {
			struct wlr_event_pointer_button button = {
				.device = &x11->pointer_dev,
				.time_msec = ev->time,
				.button = xcb_button_to_wl(ev->detail),
				.state = event->response_type == XCB_BUTTON_PRESS ?
					WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED,
			};

			wlr_signal_emit_safe(&x11->pointer.events.button, &button);
		}
		x11->time = ev->time;
		break;
	}
	case XCB_MOTION_NOTIFY: {
		xcb_motion_notify_event_t *ev = (xcb_motion_notify_event_t *)event;
		struct wlr_event_pointer_motion_absolute abs = {
			.device = &x11->pointer_dev,
			.time_msec = ev->time,
			.x_mm = ev->event_x,
			.y_mm = ev->event_y,
			.width_mm = output->wlr_output.width,
			.height_mm = output->wlr_output.height,
		};

		wlr_signal_emit_safe(&x11->pointer.events.motion_absolute, &abs);
		x11->time = ev->time;
		break;
	}
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t *)event;

		wlr_output_update_custom_mode(&output->wlr_output, ev->width,
			ev->height, 0);

		// Move the pointer to its new location
		xcb_query_pointer_cookie_t cookie =
			xcb_query_pointer(x11->xcb_conn, output->win);
		xcb_query_pointer_reply_t *pointer =
			xcb_query_pointer_reply(x11->xcb_conn, cookie, NULL);
		if (!pointer) {
			break;
		}

		struct wlr_event_pointer_motion_absolute abs = {
			.device = &x11->pointer_dev,
			.time_msec = x11->time,
			.x_mm = pointer->root_x,
			.y_mm = pointer->root_y,
			.width_mm = output->wlr_output.width,
			.height_mm = output->wlr_output.height,
		};

		wlr_signal_emit_safe(&x11->pointer.events.motion_absolute, &abs);
		break;
	}
	case XCB_GLX_DELETE_QUERIES_ARB: {
		wl_display_terminate(x11->wl_display);
		return true;
		break;
	}
	default:
		break;
	}

	return false;
}

static int x11_event(int fd, uint32_t mask, void *data) {
	struct wlr_x11_backend *x11 = data;

	if ((mask & WL_EVENT_HANGUP) || (mask & WL_EVENT_ERROR)) {
		wl_display_terminate(x11->wl_display);
		return 0;
	}

	xcb_generic_event_t *e;
	bool quit = false;
	while (!quit && (e = xcb_poll_for_event(x11->xcb_conn))) {
		quit = handle_x11_event(x11, e);
		free(e);
	}

	return 0;
}

static int signal_frame(void *data) {
	struct wlr_x11_backend *x11 = data;
	wlr_output_send_frame(&x11->output.wlr_output);
	wl_event_source_timer_update(x11->frame_timer, 16);
	return 0;
}

static void init_atom(struct wlr_x11_backend *x11, struct wlr_x11_atom *atom,
		uint8_t only_if_exists, const char *name) {
	atom->cookie = xcb_intern_atom(x11->xcb_conn, only_if_exists, strlen(name),
		name);
	atom->reply = xcb_intern_atom_reply(x11->xcb_conn, atom->cookie, NULL);
}

static bool wlr_x11_backend_start(struct wlr_backend *backend) {
	struct wlr_x11_backend *x11 = (struct wlr_x11_backend *)backend;
	struct wlr_x11_output *output = &x11->output;

	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	uint32_t values[2] = {
		x11->screen->white_pixel,
		XCB_EVENT_MASK_EXPOSURE |
		XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
		XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
		XCB_EVENT_MASK_POINTER_MOTION |
		XCB_EVENT_MASK_STRUCTURE_NOTIFY
	};

	output->x11 = x11;

	wlr_output_init(&output->wlr_output, &x11->backend, &output_impl,
		x11->wl_display);
	snprintf(output->wlr_output.name, sizeof(output->wlr_output.name), "X11-1");

	output->win = xcb_generate_id(x11->xcb_conn);
	xcb_create_window(x11->xcb_conn, XCB_COPY_FROM_PARENT, output->win,
		x11->screen->root, 0, 0, 1024, 768, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		x11->screen->root_visual, mask, values);

	output->surf = wlr_egl_create_surface(&x11->egl, &output->win);
	if (!output->surf) {
		wlr_log(L_ERROR, "Failed to create EGL surface");
		return false;
	}

	init_atom(x11, &x11->atoms.wm_protocols, 1, "WM_PROTOCOLS");
	init_atom(x11, &x11->atoms.wm_delete_window, 0, "WM_DELETE_WINDOW");
	init_atom(x11, &x11->atoms.net_wm_name, 1, "_NET_WM_NAME");
	init_atom(x11, &x11->atoms.utf8_string, 0, "UTF8_STRING");

	xcb_change_property(x11->xcb_conn, XCB_PROP_MODE_REPLACE, output->win,
		x11->atoms.wm_protocols.reply->atom, XCB_ATOM_ATOM, 32, 1,
		&x11->atoms.wm_delete_window.reply->atom);

	char title[32];
	if (snprintf(title, sizeof(title), "wlroots - %s", output->wlr_output.name)) {
		xcb_change_property(x11->xcb_conn, XCB_PROP_MODE_REPLACE, output->win,
			x11->atoms.net_wm_name.reply->atom,
			x11->atoms.utf8_string.reply->atom, 8,
			strlen(title), title);
	}

	xcb_map_window(x11->xcb_conn, output->win);
	xcb_flush(x11->xcb_conn);
	wlr_output_update_enabled(&output->wlr_output, true);

	wlr_signal_emit_safe(&x11->backend.events.new_output, output);
	wlr_signal_emit_safe(&x11->backend.events.new_input, &x11->keyboard_dev);
	wlr_signal_emit_safe(&x11->backend.events.new_input, &x11->pointer_dev);

	wl_event_source_timer_update(x11->frame_timer, 16);

	return true;
}

static void wlr_x11_backend_destroy(struct wlr_backend *backend) {
	if (!backend) {
		return;
	}

	struct wlr_x11_backend *x11 = (struct wlr_x11_backend *)backend;

	struct wlr_x11_output *output = &x11->output;
	wlr_output_destroy(&output->wlr_output);

	wlr_signal_emit_safe(&x11->pointer_dev.events.destroy, &x11->pointer_dev);
	wlr_signal_emit_safe(&x11->keyboard_dev.events.destroy, &x11->keyboard_dev);
	// TODO probably need to use wlr_keyboard_destroy, but the devices need to
	// be malloced for that to work
	if (x11->keyboard_dev.keyboard->keymap) {
		xkb_keymap_unref(x11->keyboard_dev.keyboard->keymap);
	}
	if (x11->keyboard_dev.keyboard->xkb_state) {
		xkb_state_unref(x11->keyboard_dev.keyboard->xkb_state);
	}

	wlr_signal_emit_safe(&backend->events.destroy, backend);

	wl_list_remove(&x11->display_destroy.link);

	wl_event_source_remove(x11->frame_timer);
	wlr_egl_finish(&x11->egl);

	xcb_disconnect(x11->xcb_conn);
	free(x11);
}

static struct wlr_egl *wlr_x11_backend_get_egl(struct wlr_backend *backend) {
	struct wlr_x11_backend *x11 = (struct wlr_x11_backend *)backend;
	return &x11->egl;
}

static struct wlr_renderer *wlr_x11_backend_get_renderer(
		struct wlr_backend *backend) {
	struct wlr_x11_backend *x11 = (struct wlr_x11_backend *)backend;
	return x11->renderer;
}

static struct wlr_backend_impl backend_impl = {
	.start = wlr_x11_backend_start,
	.destroy = wlr_x11_backend_destroy,
	.get_egl = wlr_x11_backend_get_egl,
	.get_renderer = wlr_x11_backend_get_renderer,
};

bool wlr_backend_is_x11(struct wlr_backend *backend) {
	return backend->impl == &backend_impl;
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_x11_backend *x11 =
		wl_container_of(listener, x11, display_destroy);
	wlr_x11_backend_destroy(&x11->backend);
}

struct wlr_backend *wlr_x11_backend_create(struct wl_display *display,
		const char *x11_display) {
	struct wlr_x11_backend *x11 = calloc(1, sizeof(*x11));
	if (!x11) {
		return NULL;
	}

	wlr_backend_init(&x11->backend, &backend_impl);
	x11->wl_display = display;

	x11->xlib_conn = XOpenDisplay(x11_display);
	if (!x11->xlib_conn) {
		wlr_log(L_ERROR, "Failed to open X connection");
		return NULL;
	}

	x11->xcb_conn = XGetXCBConnection(x11->xlib_conn);
	if (!x11->xcb_conn || xcb_connection_has_error(x11->xcb_conn)) {
		wlr_log(L_ERROR, "Failed to open xcb connection");
		goto error_x11;
	}

	XSetEventQueueOwner(x11->xlib_conn, XCBOwnsEventQueue);

	int fd = xcb_get_file_descriptor(x11->xcb_conn);
	struct wl_event_loop *ev = wl_display_get_event_loop(display);
	int events = WL_EVENT_READABLE | WL_EVENT_ERROR | WL_EVENT_HANGUP;
	x11->event_source = wl_event_loop_add_fd(ev, fd, events, x11_event, x11);
	if (!x11->event_source) {
		wlr_log(L_ERROR, "Could not create event source");
		goto error_x11;
	}

	x11->frame_timer = wl_event_loop_add_timer(ev, signal_frame, x11);

	x11->screen = xcb_setup_roots_iterator(xcb_get_setup(x11->xcb_conn)).data;

	if (!wlr_egl_init(&x11->egl, EGL_PLATFORM_X11_KHR, x11->xlib_conn, NULL,
			x11->screen->root_visual)) {
		goto error_event;
	}

	x11->renderer = wlr_gles2_renderer_create(&x11->backend);
	if (x11->renderer == NULL) {
		wlr_log(L_ERROR, "Failed to create renderer");
	}

	wlr_input_device_init(&x11->keyboard_dev, WLR_INPUT_DEVICE_KEYBOARD,
		&input_device_impl, "X11 keyboard", 0, 0);
	wlr_keyboard_init(&x11->keyboard, NULL);
	x11->keyboard_dev.keyboard = &x11->keyboard;

	wlr_input_device_init(&x11->pointer_dev, WLR_INPUT_DEVICE_POINTER,
		&input_device_impl, "X11 pointer", 0, 0);
	wlr_pointer_init(&x11->pointer, NULL);
	x11->pointer_dev.pointer = &x11->pointer;

	x11->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &x11->display_destroy);

	return &x11->backend;

error_event:
	wl_event_source_remove(x11->event_source);
error_x11:
	xcb_disconnect(x11->xcb_conn);
	XCloseDisplay(x11->xlib_conn);
	free(x11);
	return NULL;
}

static bool output_set_custom_mode(struct wlr_output *wlr_output, int32_t width,
		int32_t height, int32_t refresh) {
	struct wlr_x11_output *output = (struct wlr_x11_output *)wlr_output;
	struct wlr_x11_backend *x11 = output->x11;

	const uint32_t values[] = { width, height };
	xcb_configure_window(x11->xcb_conn, output->win,
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
	return true;
}

static void output_transform(struct wlr_output *wlr_output, enum wl_output_transform transform) {
	struct wlr_x11_output *output = (struct wlr_x11_output *)wlr_output;
	output->wlr_output.transform = transform;
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_x11_output *output = (struct wlr_x11_output *)wlr_output;
	struct wlr_x11_backend *x11 = output->x11;

	eglDestroySurface(x11->egl.display, output->surf);
	xcb_destroy_window(x11->xcb_conn, output->win);
	// output has been allocated on the stack, do not free it
}

static bool output_make_current(struct wlr_output *wlr_output, int *buffer_age) {
	struct wlr_x11_output *output = (struct wlr_x11_output *)wlr_output;
	struct wlr_x11_backend *x11 = output->x11;

	return wlr_egl_make_current(&x11->egl, output->surf, buffer_age);
}

static bool output_swap_buffers(struct wlr_output *wlr_output,
		pixman_region32_t *damage) {
	struct wlr_x11_output *output = (struct wlr_x11_output *)wlr_output;
	struct wlr_x11_backend *x11 = output->x11;

	return wlr_egl_swap_buffers(&x11->egl, output->surf, damage);
}

static struct wlr_output_impl output_impl = {
	.set_custom_mode = output_set_custom_mode,
	.transform = output_transform,
	.destroy = output_destroy,
	.make_current = output_make_current,
	.swap_buffers = output_swap_buffers,
};

bool wlr_output_is_x11(struct wlr_output *wlr_output) {
	return wlr_output->impl == &output_impl;
}

bool wlr_input_device_is_x11(struct wlr_input_device *wlr_dev) {
	return wlr_dev->impl == &input_device_impl;
}
