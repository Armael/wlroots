#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "rootston/desktop.h"
#include "rootston/input.h"
#include "rootston/server.h"

static void popup_destroy(struct roots_view_child *child) {
	assert(child->destroy == popup_destroy);
	struct roots_xdg_popup *popup = (struct roots_xdg_popup *)child;
	if (popup == NULL) {
		return;
	}
	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->new_popup.link);
	view_child_finish(&popup->view_child);
	free(popup);
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	struct roots_xdg_popup *popup =
		wl_container_of(listener, popup, destroy);
	popup_destroy((struct roots_view_child *)popup);
}

static struct roots_xdg_popup *popup_create(struct roots_view *view,
	struct wlr_xdg_popup *wlr_popup);

static void popup_handle_new_popup(struct wl_listener *listener, void *data) {
	struct roots_xdg_popup *popup =
		wl_container_of(listener, popup, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	popup_create(popup->view_child.view, wlr_popup);
}

static struct roots_xdg_popup *popup_create(struct roots_view *view,
		struct wlr_xdg_popup *wlr_popup) {
	struct roots_xdg_popup *popup =
		calloc(1, sizeof(struct roots_xdg_popup));
	if (popup == NULL) {
		return NULL;
	}
	popup->wlr_popup = wlr_popup;
	popup->view_child.destroy = popup_destroy;
	view_child_init(&popup->view_child, view, wlr_popup->base->surface);
	popup->destroy.notify = popup_handle_destroy;
	wl_signal_add(&wlr_popup->base->events.destroy, &popup->destroy);
	popup->new_popup.notify = popup_handle_new_popup;
	wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);
	return popup;
}


static void get_size(const struct roots_view *view, struct wlr_box *box) {
	assert(view->type == ROOTS_XDG_SHELL_VIEW);
	struct wlr_xdg_surface *surface = view->xdg_surface;

	if (surface->geometry->width > 0 && surface->geometry->height > 0) {
		box->width = surface->geometry->width;
		box->height = surface->geometry->height;
	} else {
		box->width = view->wlr_surface->current->width;
		box->height = view->wlr_surface->current->height;
	}
}

static void activate(struct roots_view *view, bool active) {
	assert(view->type == ROOTS_XDG_SHELL_VIEW);
	struct wlr_xdg_surface *surface = view->xdg_surface;
	if (surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		wlr_xdg_toplevel_set_activated(surface, active);
	}
}

static void apply_size_constraints(struct wlr_xdg_surface *surface,
		uint32_t width, uint32_t height, uint32_t *dest_width,
		uint32_t *dest_height) {
	*dest_width = width;
	*dest_height = height;

	struct wlr_xdg_toplevel_state *state = &surface->toplevel_state->current;
	if (width < state->min_width) {
		*dest_width = state->min_width;
	} else if (state->max_width > 0 &&
			width > state->max_width) {
		*dest_width = state->max_width;
	}
	if (height < state->min_height) {
		*dest_height = state->min_height;
	} else if (state->max_height > 0 &&
			height > state->max_height) {
		*dest_height = state->max_height;
	}
}

static void resize(struct roots_view *view, uint32_t width, uint32_t height) {
	assert(view->type == ROOTS_XDG_SHELL_VIEW);
	struct wlr_xdg_surface *surface = view->xdg_surface;
	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	uint32_t constrained_width, constrained_height;
	apply_size_constraints(surface, width, height, &constrained_width,
		&constrained_height);

	wlr_xdg_toplevel_set_size(surface, constrained_width,
		constrained_height);
}

static void move_resize(struct roots_view *view, double x, double y,
		uint32_t width, uint32_t height) {
	assert(view->type == ROOTS_XDG_SHELL_VIEW);
	struct roots_xdg_surface *roots_surface = view->roots_xdg_surface;
	struct wlr_xdg_surface *surface = view->xdg_surface;
	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	bool update_x = x != view->x;
	bool update_y = y != view->y;

	uint32_t constrained_width, constrained_height;
	apply_size_constraints(surface, width, height, &constrained_width,
		&constrained_height);

	if (update_x) {
		x = x + width - constrained_width;
	}
	if (update_y) {
		y = y + height - constrained_height;
	}

	view->pending_move_resize.update_x = update_x;
	view->pending_move_resize.update_y = update_y;
	view->pending_move_resize.x = x;
	view->pending_move_resize.y = y;
	view->pending_move_resize.width = constrained_width;
	view->pending_move_resize.height = constrained_height;

	uint32_t serial = wlr_xdg_toplevel_set_size(surface, constrained_width,
		constrained_height);
	if (serial > 0) {
		roots_surface->pending_move_resize_configure_serial = serial;
	} else if (roots_surface->pending_move_resize_configure_serial == 0) {
		view_update_position(view, x, y);
	}
}

static void maximize(struct roots_view *view, bool maximized) {
	assert(view->type == ROOTS_XDG_SHELL_VIEW);
	struct wlr_xdg_surface *surface = view->xdg_surface;
	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	wlr_xdg_toplevel_set_maximized(surface, maximized);
}

static void set_fullscreen(struct roots_view *view, bool fullscreen) {
	assert(view->type == ROOTS_XDG_SHELL_VIEW);
	struct wlr_xdg_surface *surface = view->xdg_surface;
	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	wlr_xdg_toplevel_set_fullscreen(surface, fullscreen);
}

static void close(struct roots_view *view) {
	assert(view->type == ROOTS_XDG_SHELL_VIEW);
	struct wlr_xdg_surface *surface = view->xdg_surface;
	if (surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		wlr_xdg_toplevel_send_close(surface);
	}
}

static void handle_request_move(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, request_move);
	struct roots_view *view = roots_xdg_surface->view;
	struct roots_input *input = view->desktop->server->input;
	struct wlr_xdg_toplevel_move_event *e = data;
	struct roots_seat *seat = input_seat_from_wlr_seat(input, e->seat->seat);
	// TODO verify event serial
	if (!seat || seat->cursor->mode != ROOTS_CURSOR_PASSTHROUGH) {
		return;
	}
	roots_seat_begin_move(seat, view);
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, request_resize);
	struct roots_view *view = roots_xdg_surface->view;
	struct roots_input *input = view->desktop->server->input;
	struct wlr_xdg_toplevel_resize_event *e = data;
	// TODO verify event serial
	struct roots_seat *seat = input_seat_from_wlr_seat(input, e->seat->seat);
	assert(seat);
	if (!seat || seat->cursor->mode != ROOTS_CURSOR_PASSTHROUGH) {
		return;
	}
	roots_seat_begin_resize(seat, view, e->edges);
}

static void handle_request_maximize(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, request_maximize);
	struct roots_view *view = roots_xdg_surface->view;
	struct wlr_xdg_surface *surface = view->xdg_surface;

	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	view_maximize(view, surface->toplevel_state->next.maximized);
}

static void handle_request_fullscreen(struct wl_listener *listener,
		void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, request_fullscreen);
	struct roots_view *view = roots_xdg_surface->view;
	struct wlr_xdg_surface *surface = view->xdg_surface;
	struct wlr_xdg_toplevel_set_fullscreen_event *e = data;

	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	view_set_fullscreen(view, e->fullscreen, e->output);
}

static void handle_surface_commit(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_surface =
		wl_container_of(listener, roots_surface, surface_commit);
	struct roots_view *view = roots_surface->view;
	struct wlr_xdg_surface *surface = view->xdg_surface;

	view_apply_damage(view);

	struct wlr_box size;
	get_size(view, &size);
	view_update_size(view, size.width, size.height);

	uint32_t pending_serial =
		roots_surface->pending_move_resize_configure_serial;
	if (pending_serial > 0 && pending_serial >= surface->configure_serial) {
		double x = view->x;
		double y = view->y;
		if (view->pending_move_resize.update_x) {
			x = view->pending_move_resize.x + view->pending_move_resize.width -
				size.width;
		}
		if (view->pending_move_resize.update_y) {
			y = view->pending_move_resize.y + view->pending_move_resize.height -
				size.height;
		}
		view_update_position(view, x, y);

		if (pending_serial == surface->configure_serial) {
			roots_surface->pending_move_resize_configure_serial = 0;
		}
	}
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	popup_create(roots_xdg_surface->view, wlr_popup);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct roots_xdg_surface *roots_xdg_surface =
		wl_container_of(listener, roots_xdg_surface, destroy);
	wl_list_remove(&roots_xdg_surface->surface_commit.link);
	wl_list_remove(&roots_xdg_surface->destroy.link);
	wl_list_remove(&roots_xdg_surface->new_popup.link);
	wl_list_remove(&roots_xdg_surface->request_move.link);
	wl_list_remove(&roots_xdg_surface->request_resize.link);
	wl_list_remove(&roots_xdg_surface->request_maximize.link);
	wl_list_remove(&roots_xdg_surface->request_fullscreen.link);
	wl_list_remove(&roots_xdg_surface->view->link);
	view_finish(roots_xdg_surface->view);
	free(roots_xdg_surface->view);
	free(roots_xdg_surface);
}

void handle_xdg_shell_surface(struct wl_listener *listener, void *data) {
	struct wlr_xdg_surface *surface = data;
	assert(surface->role != WLR_XDG_SURFACE_ROLE_NONE);

	if (surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		wlr_log(L_DEBUG, "new xdg popup");
		return;
	}

	struct roots_desktop *desktop =
		wl_container_of(listener, desktop, xdg_shell_surface);

	wlr_log(L_DEBUG, "new xdg toplevel: title=%s, app_id=%s",
		surface->title, surface->app_id);
	wlr_xdg_surface_ping(surface);

	struct roots_xdg_surface *roots_surface =
		calloc(1, sizeof(struct roots_xdg_surface));
	if (!roots_surface) {
		return;
	}
	roots_surface->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&surface->surface->events.commit,
		&roots_surface->surface_commit);
	roots_surface->destroy.notify = handle_destroy;
	wl_signal_add(&surface->events.destroy, &roots_surface->destroy);
	roots_surface->request_move.notify = handle_request_move;
	wl_signal_add(&surface->events.request_move, &roots_surface->request_move);
	roots_surface->request_resize.notify = handle_request_resize;
	wl_signal_add(&surface->events.request_resize,
		&roots_surface->request_resize);
	roots_surface->request_maximize.notify = handle_request_maximize;
	wl_signal_add(&surface->events.request_maximize,
		&roots_surface->request_maximize);
	roots_surface->request_fullscreen.notify = handle_request_fullscreen;
	wl_signal_add(&surface->events.request_fullscreen,
		&roots_surface->request_fullscreen);
	roots_surface->new_popup.notify = handle_new_popup;
	wl_signal_add(&surface->events.new_popup, &roots_surface->new_popup);

	struct roots_view *view = calloc(1, sizeof(struct roots_view));
	if (!view) {
		free(roots_surface);
		return;
	}
	view->type = ROOTS_XDG_SHELL_VIEW;

	view->xdg_surface = surface;
	view->roots_xdg_surface = roots_surface;
	view->wlr_surface = surface->surface;
	view->activate = activate;
	view->resize = resize;
	view->move_resize = move_resize;
	view->maximize = maximize;
	view->set_fullscreen = set_fullscreen;
	view->close = close;
	roots_surface->view = view;

	struct wlr_box box;
	get_size(view, &box);
	view->width = box.width;
	view->height = box.height;

	view_init(view, desktop);
	wl_list_insert(&desktop->views, &view->link);

	view_setup(view);
}
