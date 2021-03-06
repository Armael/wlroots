#ifndef WLR_BACKEND_H
#define WLR_BACKEND_H

#include <wayland-server.h>
#include <wlr/backend/session.h>
#include <wlr/render/egl.h>

struct wlr_backend_impl;

struct wlr_backend {
	const struct wlr_backend_impl *impl;

	struct {
		struct wl_signal destroy;
		struct wl_signal new_input;
		struct wl_signal new_output;
	} events;
};

struct wlr_backend *wlr_backend_autocreate(struct wl_display *display);
bool wlr_backend_start(struct wlr_backend *backend);
void wlr_backend_destroy(struct wlr_backend *backend);
struct wlr_egl *wlr_backend_get_egl(struct wlr_backend *backend);
struct wlr_renderer *wlr_backend_get_renderer(struct wlr_backend *backend);

uint32_t usec_to_msec(uint64_t usec);

#endif
