#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wordexp.h>
#include "background-image.h"
#include "cairo.h"
#include "comm.h"
#include "log.h"
#include "loop.h"
#include "password-buffer.h"
#include "pool-buffer.h"
#include "seat.h"
#include "swaylock.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "fullscreen-shell-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-server-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "viewporter-server-protocol.h"

#define WL_OUTPUT_MM_PER_PIX 0.264
#define WL_OUTPUT_VERSION 4

#define TIMEOUT_CONNECT 5000
#define TIMEOUT_SURFACE 10000

static void bind_wl_output(struct wl_client *client, void *data,
		uint32_t version, uint32_t id);
static void render_fallback_surface(struct swaylock_surface *surface);
static void output_redraw_timeout(void *data);
static bool run_plugin_command(struct swaylock_state *state, struct swaylock_surface *output);
static void setup_clientless_mode(struct swaylock_state *state);

extern char **environ;

static uint32_t parse_color(const char *color) {
	if (color[0] == '#') {
		++color;
	}

	int len = strlen(color);
	if (len != 6 && len != 8) {
		swaylock_log(LOG_DEBUG, "Invalid color %s, defaulting to 0xFFFFFFFF",
				color);
		return 0xFFFFFFFF;
	}
	uint32_t res = (uint32_t)strtoul(color, NULL, 16);
	if (strlen(color) == 6) {
		res = (res << 8) | 0xFF;
	}
	return res;
}

bool set_cloexec(int fd) {
	int flags = fcntl(fd, F_GETFD);
	if (flags == -1) {
		return false;
	}
	flags = flags | FD_CLOEXEC;
	if (fcntl(fd, F_SETFD, flags) == -1) {
		return false;
	}
	return true;
}

int lenient_strcmp(char *a, char *b) {
	if (a == b) {
		return 0;
	} else if (!a) {
		return -1;
	} else if (!b) {
		return 1;
	} else {
		return strcmp(a, b);
	}
}

static void daemonize(void) {
	int fds[2];
	if (pipe(fds) != 0) {
		swaylock_log(LOG_ERROR, "Failed to pipe");
		exit(1);
	}
	if (fork() == 0) {
		setsid();
		close(fds[0]);
		int devnull = open("/dev/null", O_RDWR);
		dup2(STDOUT_FILENO, devnull);
		dup2(STDERR_FILENO, devnull);
		close(devnull);
		uint8_t success = 0;
		if (chdir("/") != 0) {
			write(fds[1], &success, 1);
			exit(1);
		}
		success = 1;
		if (write(fds[1], &success, 1) != 1) {
			exit(1);
		}
		close(fds[1]);
	} else {
		close(fds[1]);
		uint8_t success;
		if (read(fds[0], &success, 1) != 1 || !success) {
			swaylock_log(LOG_ERROR, "Failed to daemonize");
			exit(1);
		}
		close(fds[0]);
		exit(0);
	}
}

static void destroy_surface(struct swaylock_surface *surface) {
	struct swaylock_state *state = surface->state;
	wl_list_remove(&surface->link);
	if (surface->plugin_surface) {
		// todo: proper cleanup
		zwlr_layer_surface_v1_send_closed(surface->plugin_surface->layer_surface);
		surface->plugin_surface->sway_surface = NULL;
		surface->plugin_surface->inert = true;
	}

	if (surface->nested_server_output) {
		wl_global_remove(surface->nested_server_output);
		// Unlink the resources; calling wl_resource_remove might be unsafe?
		struct wl_resource *output, *tmp;
		wl_resource_for_each_safe(output, tmp, &surface->nested_server_xdg_output_resources) {
			wl_list_remove(wl_resource_get_link(output));
			wl_list_insert(&state->stale_xdg_output_resources, wl_resource_get_link(output));
		}
		wl_resource_for_each_safe(output, tmp, &surface->nested_server_wl_output_resources) {
			wl_list_remove(wl_resource_get_link(output));
			wl_list_insert(&state->stale_wl_output_resources, wl_resource_get_link(output));
		}
	}
	if (surface->client_submission_timer) {
		loop_remove_timer(state->eventloop, surface->client_submission_timer);
	}
	if (surface->ext_session_lock_surface_v1 != NULL) {
		ext_session_lock_surface_v1_destroy(surface->ext_session_lock_surface_v1);
	}
	if (surface->fractional_scale) {
		wp_fractional_scale_v1_destroy(surface->fractional_scale);
	}
	if (surface->viewport) {
		wp_viewport_destroy(surface->viewport);
	}
	if (surface->subsurface) {
		wl_subsurface_destroy(surface->subsurface);
	}
	if (surface->child) {
		wl_surface_destroy(surface->child);
	}
	if (surface->surface != NULL) {
		wl_surface_destroy(surface->surface);
	}
	destroy_buffer(&surface->indicator_buffers[0]);
	destroy_buffer(&surface->indicator_buffers[1]);
	wl_output_release(surface->output);
	free(surface);
}

static const struct ext_session_lock_surface_v1_listener ext_session_lock_surface_v1_listener;

static void fract_scale_preferred_scale(void *data, struct wp_fractional_scale_v1 *wp_fractional_scale_v1,
		uint32_t scale) {
	struct swaylock_surface *surface = data;
	assert(scale > 0);
	if (surface->last_fractional_scale == scale) {
		return;
	}
	/* Forward the fractional scale update to plugin surface, if one is present */
	surface->last_fractional_scale = scale;
	if (surface->plugin_surface) {
		if (surface->plugin_surface->fractional_scale) {
			wp_fractional_scale_v1_send_preferred_scale(surface->plugin_surface->fractional_scale, scale);
		}
	}
}

static const struct wp_fractional_scale_v1_listener  fract_scale_listener = {
	.preferred_scale = fract_scale_preferred_scale,
};

static cairo_surface_t *select_image(struct swaylock_state *state,
		struct swaylock_surface *surface);

static bool surface_is_opaque(struct swaylock_surface *surface) {
	if (surface->image) {
		return cairo_surface_get_content(surface->image) == CAIRO_CONTENT_COLOR;
	}
	return (surface->state->args.colors.background & 0xff) == 0xff;
}

static void create_surface(struct swaylock_surface *surface) {
	struct swaylock_state *state = surface->state;

	surface->image = select_image(state, surface);

	surface->surface = wl_compositor_create_surface(state->compositor);
	assert(surface->surface);

	surface->child = wl_compositor_create_surface(state->compositor);
	assert(surface->child);
	surface->subsurface = wl_subcompositor_get_subsurface(state->subcompositor, surface->child, surface->surface);
	assert(surface->subsurface);
	wl_subsurface_set_sync(surface->subsurface);

	surface->ext_session_lock_surface_v1 = ext_session_lock_v1_get_lock_surface(
			state->ext_session_lock_v1, surface->surface, surface->output);
	ext_session_lock_surface_v1_add_listener(surface->ext_session_lock_surface_v1,
			&ext_session_lock_surface_v1_listener, surface);

	if (surface_is_opaque(surface) &&
			surface->state->args.mode != BACKGROUND_MODE_CENTER &&
			surface->state->args.mode != BACKGROUND_MODE_FIT) {
		struct wl_region *region =
			wl_compositor_create_region(surface->state->compositor);
		wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
		wl_surface_set_opaque_region(surface->surface, region);
		wl_region_destroy(region);
	}

	if (state->forward.fractional_scale) {
		surface->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
			state->forward.fractional_scale, surface->surface);
		wp_fractional_scale_v1_add_listener(surface->fractional_scale, &fract_scale_listener, surface);
		assert(surface->fractional_scale);
	}

	if (state->forward.viewporter) {
		surface->viewport = wp_viewporter_get_viewport(state->forward.viewporter, surface->surface);
		assert(surface->viewport);
	}

	// Plugin should provide a surface quickly enough, after compositor
	// has made the necessary details available
	surface->client_submission_timer = loop_add_timer(state->eventloop,
		TIMEOUT_SURFACE, output_redraw_timeout, surface);

	// Run command, now that we know the output's name and description,
	// and can pass these along to the plugin program using environment
	// variables, so it can e.g. decide which wallpaper program to run.
	if (state->args.plugin_per_output) {
		if (!run_plugin_command(state, surface)) {
			setup_clientless_mode(state);
		}
	}

	surface->created = true;
}

static void forward_configure(struct swaylock_surface *surface, bool first_configure, uint32_t serial) {
	if (first_configure && (surface->width > 0 && surface->height > 0)) {
		// delay output creation until we know exactly what layer
		// surface size we are provided with.
		surface->nested_server_output = wl_global_create(
			surface->state->server.display, &wl_output_interface,
			WL_OUTPUT_VERSION, surface, bind_wl_output);

		surface->first_configure_serial = serial;
		surface->used_first_configure = false;
	} else if (surface->width > 0 && surface->height > 0) {
		struct wl_resource *output;
		wl_resource_for_each(output, &surface->nested_server_wl_output_resources) {
			wl_output_send_geometry(output, 0, 0, surface->physical_width,
				surface->physical_height, surface->subpixel,
				"swaylock", "swaylock", surface->output_transform);
			wl_output_send_mode(output, 1, surface->mode_width, surface->mode_height, 0);
			wl_output_send_scale(output, surface->scale);
			wl_output_send_done(output);
		}
		struct wl_resource *xdg_output;
		wl_resource_for_each(xdg_output, &surface->nested_server_xdg_output_resources) {
			zxdg_output_v1_send_logical_size(xdg_output, surface->width, surface->height);
			zxdg_output_v1_send_done(xdg_output);
		}
		if (surface->plugin_surface) {
			/* reconfigure plugin surface with new size */
			if (surface->plugin_surface->has_been_configured) {
				/* wait until the first commit/configure cycle is over */
				struct swaylock_bg_client *bg_client = surface->client ?
					surface->client : surface->state->server.main_client;
				uint32_t plugin_serial = bg_client->serial++;
				add_serial_pair(surface->plugin_surface, serial, plugin_serial, surface->width, surface->height, false);
				zwlr_layer_surface_v1_send_configure(surface->plugin_surface->layer_surface,
					plugin_serial, surface->width, surface->height);
			}
		}
	}
}

static void ext_session_lock_surface_v1_handle_configure(void *data,
		struct ext_session_lock_surface_v1 *lock_surface, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct swaylock_surface *surface = data;
	bool first_configure = surface->width <= 0 || surface->height <= 0;
	bool size_change = surface->width != width || surface->height != height;
	surface->width = width;
	surface->height = height;
	surface->newest_serial = serial;
	/* Quoting the spec:
	 *	Sending an ack_configure request consumes the configure event
	 *	referenced by the given serial, as well as all older configure
	 *	events sent on this object.
	 *
	 * wlr-layer-shell and xdg-shell do not have equivalent language.
	 *
	 * This makes mixing client vs plugin rendering tricky.
	 *
	 */
	if (surface->state->server.display) {
		forward_configure(surface, first_configure, serial);
		surface->has_newer_serial = true;
	} else {
		ext_session_lock_surface_v1_ack_configure(surface->ext_session_lock_surface_v1, serial);
		surface->has_newer_serial = false;
		render_fallback_surface(surface);
	}
	if (surface->has_buffer) {
		render_frame(surface);
	}
	if (size_change && !first_configure) {
		// Only start timer if the old one has entirely elapsed.
		// todo: eventually launch timers for every-configure that
		// needs an update. Problem: do noop-configures or reverted
		// configures need acknowledgement?
		if (!surface->client_submission_timer) {
			surface->client_submission_timer = loop_add_timer(surface->state->eventloop,
				TIMEOUT_SURFACE, output_redraw_timeout, surface);
		}
	}
}

static const struct ext_session_lock_surface_v1_listener ext_session_lock_surface_v1_listener = {
	.configure = ext_session_lock_surface_v1_handle_configure,
};

static const struct wl_callback_listener surface_frame_listener;

static void surface_frame_handle_done(void *data, struct wl_callback *callback,
		uint32_t time) {
	struct swaylock_surface *surface = data;

	wl_callback_destroy(callback);
	surface->frame_pending = false;

	if (surface->dirty) {
		// Schedule a frame in case the surface is damaged again
		struct wl_callback *callback = wl_surface_frame(surface->surface);
		wl_callback_add_listener(callback, &surface_frame_listener, surface);
		surface->frame_pending = true;

		if (surface->has_buffer) {
			render_frame(surface);
		}
		surface->dirty = false;
	}
}

static const struct wl_callback_listener surface_frame_listener = {
	.done = surface_frame_handle_done,
};

void damage_surface(struct swaylock_surface *surface) {
	if (!surface->created) {
		return;
	}
	if (surface->width == 0 || surface->height == 0) {
		// Not yet configured
		return;
	}

	surface->dirty = true;
	if (surface->frame_pending) {
		return;
	}

	if (surface->has_buffer) {
		struct wl_callback *callback = wl_surface_frame(surface->surface);
		wl_callback_add_listener(callback, &surface_frame_listener, surface);
		surface->frame_pending = true;
		wl_surface_commit(surface->surface);
	}
}

void damage_state(struct swaylock_state *state) {
	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state->surfaces, link) {
		damage_surface(surface);
	}
}

static void handle_wl_output_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t width_mm, int32_t height_mm,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
	struct swaylock_surface *surface = data;
	surface->subpixel = subpixel;
	surface->physical_width = width_mm;
	surface->physical_height = height_mm;
	surface->output_transform = transform;
	if (surface->state->run_display) {
		damage_surface(surface);
	}
}

static void handle_wl_output_mode(void *data, struct wl_output *output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	struct swaylock_surface *surface = data;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		surface->mode_width = width;
		surface->mode_height = height;
	}
}

static void handle_wl_output_done(void *data, struct wl_output *output) {
	struct swaylock_surface *surface = data;
	if (!surface->created && surface->state->run_display) {
		if (!surface->output_name || !surface->output_description) {
			swaylock_log(LOG_ERROR, "wl_output::done received, but no name/desription pair yet. Delaying surface creation until these arrived.");
			return;
		}

		create_surface(surface);
	}
}

static void handle_wl_output_scale(void *data, struct wl_output *output,
		int32_t factor) {
	struct swaylock_surface *surface = data;
	surface->scale = factor;

	if (surface->state->run_display) {
		damage_surface(surface);
	}
}

static void handle_wl_output_name(void *data, struct wl_output *output,
		const char *name) {
	struct swaylock_surface *surface = data;
	free(surface->output_name);
	surface->output_name = strdup(name);
}

static void handle_wl_output_description(void *data, struct wl_output *output,
		const char *description) {
	struct swaylock_surface *surface = data;
	free(surface->output_description);
	surface->output_description = strdup(description);
}

struct wl_output_listener _wl_output_listener = {
	.geometry = handle_wl_output_geometry,
	.mode = handle_wl_output_mode,
	.done = handle_wl_output_done,
	.scale = handle_wl_output_scale,
	.name = handle_wl_output_name,
	.description = handle_wl_output_description,
};

static void ext_session_lock_v1_handle_locked(void *data, struct ext_session_lock_v1 *lock) {
	struct swaylock_state *state = data;
	state->locked = true;
}

static void ext_session_lock_v1_handle_finished(void *data, struct ext_session_lock_v1 *lock) {
	swaylock_log(LOG_ERROR, "Failed to lock session -- "
			"is another lockscreen running?");
	exit(2);
}

static const struct ext_session_lock_v1_listener ext_session_lock_v1_listener = {
	.locked = ext_session_lock_v1_handle_locked,
	.finished = ext_session_lock_v1_handle_finished,
};


static void wl_shm_handle_format(void *data, struct wl_shm *wl_shm, uint32_t format) {
	struct forward_state *forward = data;
	uint32_t *new_fmts = realloc(forward->shm_formats, sizeof(uint32_t) * (forward->shm_formats_len + 1));
	if (new_fmts) {
		forward->shm_formats = new_fmts;
		forward->shm_formats[forward->shm_formats_len] = format;
		forward->shm_formats_len++;
	}

}

static const struct wl_shm_listener shm_listener = {
	.format = wl_shm_handle_format,
};

static void linux_dmabuf_handle_format(void *data, struct zwp_linux_dmabuf_v1 *linux_dmabuf,
		uint32_t format) {
	/* ignore, can be reconstructed from modifier list */
}

static void linux_dmabuf_handle_modifier(void *data, struct zwp_linux_dmabuf_v1 *linux_dmabuf,
		uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo) {
	/* ignore, can be reconstructed from modifier list */
	struct forward_state *forward = data;
	// todo: quadratic runtime, fix. also sort these?
	struct dmabuf_modifier_pair *new_fmts = realloc(forward->dmabuf_formats, sizeof(struct dmabuf_modifier_pair) * (forward->dmabuf_formats_len + 1));
	if (new_fmts) {
		forward->dmabuf_formats = new_fmts;
		forward->dmabuf_formats[forward->dmabuf_formats_len].format = format;
		forward->dmabuf_formats[forward->dmabuf_formats_len].modifier_lo = modifier_lo;
		forward->dmabuf_formats[forward->dmabuf_formats_len].modifier_hi = modifier_hi;
		forward->dmabuf_formats_len++;
	}
}

static const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_listener = {
	.format = linux_dmabuf_handle_format,
	.modifier = linux_dmabuf_handle_modifier,
};

static void dmabuf_feedback_done(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1) {
	/* cleanup outdated tranches */
	struct forward_state *forward = data;
	for (size_t i = 0; i < forward->current.tranches_len; i++) {
		wl_array_release(&forward->current.tranches[i].indices);
	}
	free(forward->current.tranches);
	if (forward->current.table_fd != -1) {
		close(forward->current.table_fd);
	}

	forward->current = forward->pending;

	/* reset pending, keeping last main_device/table_fd values */
	forward->pending.tranches = NULL;
	forward->pending.tranches_len = 0;
	if (forward->current.table_fd != -1) {
		forward->pending.table_fd = dup(forward->current.table_fd);
		if (!set_cloexec(forward->pending.table_fd)) {
			swaylock_log(LOG_ERROR, "Failed to set cloexec for dmabuf fd");
		}
	}

	/* notify all the client's feedback objects */
	struct wl_resource *resource;
	wl_resource_for_each(resource, &forward->feedback_instances) {
		send_dmabuf_feedback_data(resource, &forward->current);
	}
}
static void dmabuf_feedback_format_table(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
		int32_t fd, uint32_t size) {
	struct forward_state *forward = data;
	if (forward->pending.table_fd != -1) {
		close(forward->pending.table_fd);
	}
	forward->pending.table_fd  = fd;
	forward->pending.table_fd_size = size;
}
static void dmabuf_feedback_main_device(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
		struct wl_array *device) {
	struct forward_state *forward = data;
	memcpy(&forward->pending.main_device, device->data, sizeof(forward->pending.main_device));

}
static void dmabuf_feedback_tranche_done(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1) {
	struct forward_state *forward = data;
	struct feedback_tranche  *new_tranches = realloc(forward->pending.tranches, sizeof(struct feedback_tranche) * (forward->pending.tranches_len + 1));
	if (!new_tranches) {
		swaylock_log(LOG_ERROR, "failed to expand tranche list");
		return;
	}

	forward->pending.tranches = new_tranches;
	forward->pending.tranches[forward->pending.tranches_len] = forward->pending_tranche;
	forward->pending.tranches_len++;
	/* reset the pending tranche state */
	memset(&forward->pending_tranche.tranche_device, 0, sizeof(dev_t));
	wl_array_init(&forward->pending_tranche.indices);
	forward->pending_tranche.flags = 0;
}

static void dmabuf_feedback_tranche_target_device(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
		struct wl_array *device) {
	struct forward_state *forward = data;
	memcpy(&forward->pending_tranche.tranche_device, device->data, sizeof(forward->pending_tranche.tranche_device));
}

static void dmabuf_feedback_tranche_formats(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
		struct wl_array *indices) {
	struct forward_state *forward = data;
	if (wl_array_copy(&forward->pending_tranche.indices, indices) == -1) {
		swaylock_log(LOG_ERROR, "failed to copy tranche format list");
	}
}
static void dmabuf_feedback_tranche_flags(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
		uint32_t flags) {
	struct forward_state *forward = data;
	forward->pending_tranche.flags = flags;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
	.done = dmabuf_feedback_done,
	.format_table = dmabuf_feedback_format_table,
	.main_device = dmabuf_feedback_main_device,
	.tranche_done = dmabuf_feedback_tranche_done,
	.tranche_target_device = dmabuf_feedback_tranche_target_device,
	.tranche_formats = dmabuf_feedback_tranche_formats,
	.tranche_flags = dmabuf_feedback_tranche_flags,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct swaylock_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		/* version 5 required for wl_surface::offset */
		state->compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, version >= 5 ? 5 : 4);
		state->forward.compositor = state->compositor;
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		state->subcompositor = wl_registry_bind(registry, name,
				&wl_subcompositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name,
				&wl_shm_interface, 1);
		state->forward.shm = state->shm;
		wl_shm_add_listener(state->shm, &shm_listener, &state->forward);
	} else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= 3) {
		/* ^^ version >= 3 is needed to acquire the modifier list in some form */

		/* this instance is used to route forwarded requests/events through */
		state->forward.linux_dmabuf = wl_registry_bind(
					registry, name, &zwp_linux_dmabuf_v1_interface, version >= 4 ? 4 : version);
		zwp_linux_dmabuf_v1_add_listener(state->forward.linux_dmabuf, &linux_dmabuf_listener, &state->forward);
		if (version >= 4) {
			state->dmabuf_default_feedback = zwp_linux_dmabuf_v1_get_default_feedback(state->forward.linux_dmabuf);
			zwp_linux_dmabuf_feedback_v1_add_listener(state->dmabuf_default_feedback, &dmabuf_feedback_listener, &state->forward);

			memset(&state->forward.pending, 0, sizeof(state->forward.pending));
			memset(&state->forward.current, 0, sizeof(state->forward.current));
			state->forward.pending.table_fd = -1;
			state->forward.current.table_fd = -1;
		}
	} else if (strcmp(interface, wl_drm_interface.name) == 0) {
		state->forward.drm = wl_registry_bind(registry, name,
				&wl_drm_interface, 2);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat *seat = wl_registry_bind(
				registry, name, &wl_seat_interface, 4);
		struct swaylock_seat *swaylock_seat =
			calloc(1, sizeof(struct swaylock_seat));
		swaylock_seat->state = state;
		wl_seat_add_listener(seat, &seat_listener, swaylock_seat);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct swaylock_surface *surface =
			calloc(1, sizeof(struct swaylock_surface));
		surface->state = state;
		/* version 4 needed to learn name/description */
		surface->output = wl_registry_bind(registry, name,
				&wl_output_interface, 4);
		surface->output_global_name = name;
		wl_output_add_listener(surface->output, &_wl_output_listener, surface);
		wl_list_insert(&state->surfaces, &surface->link);

		wl_list_init(&surface->nested_server_wl_output_resources);
		wl_list_init(&surface->nested_server_xdg_output_resources);
	} else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
		state->ext_session_lock_manager_v1 = wl_registry_bind(registry, name,
				&ext_session_lock_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		state->forward.fractional_scale = wl_registry_bind(registry, name,
			&wp_fractional_scale_manager_v1_interface, version >= 1 ? 1 : version);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		state->forward.viewporter = wl_registry_bind(registry, name,
			&wp_viewporter_interface, version >= 1 ? 1 : version);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	struct swaylock_state *state = data;
	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state->surfaces, link) {
		if (surface->output_global_name == name) {
			destroy_surface(surface);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static int sigusr_fds[2] = {-1, -1};

void do_sigusr(int sig) {
	(void)write(sigusr_fds[1], "1", 1);
}

static cairo_surface_t *select_image(struct swaylock_state *state,
		struct swaylock_surface *surface) {
	struct swaylock_image *image;
	cairo_surface_t *default_image = NULL;
	wl_list_for_each(image, &state->images, link) {
		if (lenient_strcmp(image->output_name, surface->output_name) == 0) {
			return image->cairo_surface;
		} else if (!image->output_name) {
			default_image = image->cairo_surface;
		}
	}
	return default_image;
}

static char *join_args(char **argv, int argc) {
	assert(argc > 0);
	int len = 0, i;
	for (i = 0; i < argc; ++i) {
		len += strlen(argv[i]) + 1;
	}
	char *res = malloc(len);
	len = 0;
	for (i = 0; i < argc; ++i) {
		strcpy(res + len, argv[i]);
		len += strlen(argv[i]);
		res[len++] = ' ';
	}
	res[len - 1] = '\0';
	return res;
}

static void load_image(char *arg, struct swaylock_state *state) {
	// [[<output>]:]<path>
	struct swaylock_image *image = calloc(1, sizeof(struct swaylock_image));
	char *separator = strchr(arg, ':');
	if (separator) {
		*separator = '\0';
		image->output_name = separator == arg ? NULL : strdup(arg);
		image->path = strdup(separator + 1);
	} else {
		image->output_name = NULL;
		image->path = strdup(arg);
	}

	struct swaylock_image *iter_image, *temp;
	wl_list_for_each_safe(iter_image, temp, &state->images, link) {
		if (lenient_strcmp(iter_image->output_name, image->output_name) == 0) {
			if (image->output_name) {
				swaylock_log(LOG_DEBUG,
						"Replacing image defined for output %s with %s",
						image->output_name, image->path);
			} else {
				swaylock_log(LOG_DEBUG, "Replacing default image with %s",
						image->path);
			}
			wl_list_remove(&iter_image->link);
			free(iter_image->cairo_surface);
			free(iter_image->output_name);
			free(iter_image->path);
			free(iter_image);
			break;
		}
	}

	// The shell will not expand ~ to the value of $HOME when an output name is
	// given. Also, any image paths given in the config file need to have shell
	// expansions performed
	wordexp_t p;
	while (strstr(image->path, "  ")) {
		image->path = realloc(image->path, strlen(image->path) + 2);
		char *ptr = strstr(image->path, "  ") + 1;
		memmove(ptr + 1, ptr, strlen(ptr) + 1);
		*ptr = '\\';
	}
	if (wordexp(image->path, &p, 0) == 0) {
		free(image->path);
		image->path = join_args(p.we_wordv, p.we_wordc);
		wordfree(&p);
	}

	// Load the actual image
	image->cairo_surface = load_background_image(image->path);
	if (!image->cairo_surface) {
		free(image);
		return;
	}
	wl_list_insert(&state->images, &image->link);
	swaylock_log(LOG_DEBUG, "Loaded image %s for output %s", image->path,
			image->output_name ? image->output_name : "*");
}

static void set_default_colors(struct swaylock_colors *colors) {
	colors->background = 0xFFFFFFFF;
	colors->bs_highlight = 0xDB3300FF;
	colors->key_highlight = 0x33DB00FF;
	colors->caps_lock_bs_highlight = 0xDB3300FF;
	colors->caps_lock_key_highlight = 0x33DB00FF;
	colors->separator = 0x000000FF;
	colors->layout_background = 0x000000C0;
	colors->layout_border = 0x00000000;
	colors->layout_text = 0xFFFFFFFF;
	colors->inside = (struct swaylock_colorset){
		.input = 0x000000C0,
		.cleared = 0xE5A445C0,
		.caps_lock = 0x000000C0,
		.verifying = 0x0072FFC0,
		.wrong = 0xFA0000C0,
	};
	colors->line = (struct swaylock_colorset){
		.input = 0x000000FF,
		.cleared = 0x000000FF,
		.caps_lock = 0x000000FF,
		.verifying = 0x000000FF,
		.wrong = 0x000000FF,
	};
	colors->ring = (struct swaylock_colorset){
		.input = 0x337D00FF,
		.cleared = 0xE5A445FF,
		.caps_lock = 0xE5A445FF,
		.verifying = 0x3300FFFF,
		.wrong = 0x7D3300FF,
	};
	colors->text = (struct swaylock_colorset){
		.input = 0xE5A445FF,
		.cleared = 0x000000FF,
		.caps_lock = 0xE5A445FF,
		.verifying = 0x000000FF,
		.wrong = 0x000000FF,
	};
}

enum line_mode {
	LM_LINE,
	LM_INSIDE,
	LM_RING,
};

static int parse_options(int argc, char **argv, struct swaylock_state *state,
		enum line_mode *line_mode, char **config_path) {
	enum long_option_codes {
		LO_BS_HL_COLOR = 256,
		LO_CAPS_LOCK_BS_HL_COLOR,
		LO_CAPS_LOCK_KEY_HL_COLOR,
		LO_FONT,
		LO_FONT_SIZE,
		LO_IND_IDLE_VISIBLE,
		LO_IND_RADIUS,
		LO_IND_X_POSITION,
		LO_IND_Y_POSITION,
		LO_IND_THICKNESS,
		LO_INSIDE_COLOR,
		LO_INSIDE_CLEAR_COLOR,
		LO_INSIDE_CAPS_LOCK_COLOR,
		LO_INSIDE_VER_COLOR,
		LO_INSIDE_WRONG_COLOR,
		LO_KEY_HL_COLOR,
		LO_LAYOUT_TXT_COLOR,
		LO_LAYOUT_BG_COLOR,
		LO_LAYOUT_BORDER_COLOR,
		LO_LINE_COLOR,
		LO_LINE_CLEAR_COLOR,
		LO_LINE_CAPS_LOCK_COLOR,
		LO_LINE_VER_COLOR,
		LO_LINE_WRONG_COLOR,
		LO_RING_COLOR,
		LO_RING_CLEAR_COLOR,
		LO_RING_CAPS_LOCK_COLOR,
		LO_RING_VER_COLOR,
		LO_RING_WRONG_COLOR,
		LO_SEP_COLOR,
		LO_TEXT_COLOR,
		LO_TEXT_CLEAR_COLOR,
		LO_TEXT_CAPS_LOCK_COLOR,
		LO_TEXT_VER_COLOR,
		LO_TEXT_WRONG_COLOR,
		LO_PLUGIN_COMMAND,
		LO_PLUGIN_COMMAND_EACH,
	};

	static struct option long_options[] = {
		{"config", required_argument, NULL, 'C'},
		{"color", required_argument, NULL, 'c'},
		{"debug", no_argument, NULL, 'd'},
		{"ignore-empty-password", no_argument, NULL, 'e'},
		{"daemonize", no_argument, NULL, 'f'},
		{"ready-fd", required_argument, NULL, 'R'},
		{"help", no_argument, NULL, 'h'},
		{"image", required_argument, NULL, 'i'},
		{"disable-caps-lock-text", no_argument, NULL, 'L'},
		{"indicator-caps-lock", no_argument, NULL, 'l'},
		{"line-uses-inside", no_argument, NULL, 'n'},
		{"line-uses-ring", no_argument, NULL, 'r'},
		{"scaling", required_argument, NULL, 's'},
		{"tiling", no_argument, NULL, 't'},
		{"no-unlock-indicator", no_argument, NULL, 'u'},
		{"show-keyboard-layout", no_argument, NULL, 'k'},
		{"hide-keyboard-layout", no_argument, NULL, 'K'},
		{"show-failed-attempts", no_argument, NULL, 'F'},
		{"version", no_argument, NULL, 'v'},
		{"bs-hl-color", required_argument, NULL, LO_BS_HL_COLOR},
		{"caps-lock-bs-hl-color", required_argument, NULL, LO_CAPS_LOCK_BS_HL_COLOR},
		{"caps-lock-key-hl-color", required_argument, NULL, LO_CAPS_LOCK_KEY_HL_COLOR},
		{"font", required_argument, NULL, LO_FONT},
		{"font-size", required_argument, NULL, LO_FONT_SIZE},
		{"indicator-idle-visible", no_argument, NULL, LO_IND_IDLE_VISIBLE},
		{"indicator-radius", required_argument, NULL, LO_IND_RADIUS},
		{"indicator-thickness", required_argument, NULL, LO_IND_THICKNESS},
		{"indicator-x-position", required_argument, NULL, LO_IND_X_POSITION},
		{"indicator-y-position", required_argument, NULL, LO_IND_Y_POSITION},
		{"inside-color", required_argument, NULL, LO_INSIDE_COLOR},
		{"inside-clear-color", required_argument, NULL, LO_INSIDE_CLEAR_COLOR},
		{"inside-caps-lock-color", required_argument, NULL, LO_INSIDE_CAPS_LOCK_COLOR},
		{"inside-ver-color", required_argument, NULL, LO_INSIDE_VER_COLOR},
		{"inside-wrong-color", required_argument, NULL, LO_INSIDE_WRONG_COLOR},
		{"key-hl-color", required_argument, NULL, LO_KEY_HL_COLOR},
		{"layout-bg-color", required_argument, NULL, LO_LAYOUT_BG_COLOR},
		{"layout-border-color", required_argument, NULL, LO_LAYOUT_BORDER_COLOR},
		{"layout-text-color", required_argument, NULL, LO_LAYOUT_TXT_COLOR},
		{"line-color", required_argument, NULL, LO_LINE_COLOR},
		{"line-clear-color", required_argument, NULL, LO_LINE_CLEAR_COLOR},
		{"line-caps-lock-color", required_argument, NULL, LO_LINE_CAPS_LOCK_COLOR},
		{"line-ver-color", required_argument, NULL, LO_LINE_VER_COLOR},
		{"line-wrong-color", required_argument, NULL, LO_LINE_WRONG_COLOR},
		{"ring-color", required_argument, NULL, LO_RING_COLOR},
		{"ring-clear-color", required_argument, NULL, LO_RING_CLEAR_COLOR},
		{"ring-caps-lock-color", required_argument, NULL, LO_RING_CAPS_LOCK_COLOR},
		{"ring-ver-color", required_argument, NULL, LO_RING_VER_COLOR},
		{"ring-wrong-color", required_argument, NULL, LO_RING_WRONG_COLOR},
		{"separator-color", required_argument, NULL, LO_SEP_COLOR},
		{"text-color", required_argument, NULL, LO_TEXT_COLOR},
		{"text-clear-color", required_argument, NULL, LO_TEXT_CLEAR_COLOR},
		{"text-caps-lock-color", required_argument, NULL, LO_TEXT_CAPS_LOCK_COLOR},
		{"text-ver-color", required_argument, NULL, LO_TEXT_VER_COLOR},
		{"text-wrong-color", required_argument, NULL, LO_TEXT_WRONG_COLOR},
		{"command", required_argument, NULL, LO_PLUGIN_COMMAND},
		{"command-each", required_argument, NULL, LO_PLUGIN_COMMAND_EACH},
		{0, 0, 0, 0}
	};

	const char usage[] =
		"Usage: swaylock [options...]\n"
		"\n"
		"  -C, --config <config_file>       "
			"Path to the config file.\n"
		"  -c, --color <color>              "
			"Turn the screen into the given color instead of white.\n"
		"  -d, --debug                      "
			"Enable debugging output.\n"
		"  -e, --ignore-empty-password      "
			"When an empty password is provided, do not validate it.\n"
		"  -F, --show-failed-attempts       "
			"Show current count of failed authentication attempts.\n"
		"  -f, --daemonize                  "
			"Detach from the controlling terminal after locking.\n"
		"  -R, --ready-fd <fd>              "
			"File descriptor to send readiness notifications to.\n"
		"  -h, --help                       "
			"Show help message and quit.\n"
		"  -i, --image [[<output>]:]<path>  "
			"Display the given image, optionally only on the given output.\n"
		"  -k, --show-keyboard-layout       "
			"Display the current xkb layout while typing.\n"
		"  -K, --hide-keyboard-layout       "
			"Hide the current xkb layout while typing.\n"
		"  -L, --disable-caps-lock-text     "
			"Disable the Caps Lock text.\n"
		"  -l, --indicator-caps-lock        "
			"Show the current Caps Lock state also on the indicator.\n"
		"  -s, --scaling <mode>             "
			"Image scaling mode: stretch, fill, fit, center, tile, solid_color.\n"
		"  -t, --tiling                     "
			"Same as --scaling=tile.\n"
		"  -u, --no-unlock-indicator        "
			"Disable the unlock indicator.\n"
		"  -v, --version                    "
			"Show the version number and quit.\n"
		"  --bs-hl-color <color>            "
			"Sets the color of backspace highlight segments.\n"
		"  --caps-lock-bs-hl-color <color>  "
			"Sets the color of backspace highlight segments when Caps Lock "
			"is active.\n"
		"  --caps-lock-key-hl-color <color> "
			"Sets the color of the key press highlight segments when "
			"Caps Lock is active.\n"
		"  --font <font>                    "
			"Sets the font of the text.\n"
		"  --font-size <size>               "
			"Sets a fixed font size for the indicator text.\n"
		"  --indicator-idle-visible         "
			"Sets the indicator to show even if idle.\n"
		"  --indicator-radius <radius>      "
			"Sets the indicator radius.\n"
		"  --indicator-thickness <thick>    "
			"Sets the indicator thickness.\n"
		"  --indicator-x-position <x>       "
			"Sets the horizontal position of the indicator.\n"
		"  --indicator-y-position <y>       "
			"Sets the vertical position of the indicator.\n"
		"  --inside-color <color>           "
			"Sets the color of the inside of the indicator.\n"
		"  --inside-clear-color <color>     "
			"Sets the color of the inside of the indicator when cleared.\n"
		"  --inside-caps-lock-color <color> "
			"Sets the color of the inside of the indicator when Caps Lock "
			"is active.\n"
		"  --inside-ver-color <color>       "
			"Sets the color of the inside of the indicator when verifying.\n"
		"  --inside-wrong-color <color>     "
			"Sets the color of the inside of the indicator when invalid.\n"
		"  --key-hl-color <color>           "
			"Sets the color of the key press highlight segments.\n"
		"  --layout-bg-color <color>        "
			"Sets the background color of the box containing the layout text.\n"
		"  --layout-border-color <color>    "
			"Sets the color of the border of the box containing the layout text.\n"
		"  --layout-text-color <color>      "
			"Sets the color of the layout text.\n"
		"  --line-color <color>             "
			"Sets the color of the line between the inside and ring.\n"
		"  --line-clear-color <color>       "
			"Sets the color of the line between the inside and ring when "
			"cleared.\n"
		"  --line-caps-lock-color <color>   "
			"Sets the color of the line between the inside and ring when "
			"Caps Lock is active.\n"
		"  --line-ver-color <color>         "
			"Sets the color of the line between the inside and ring when "
			"verifying.\n"
		"  --line-wrong-color <color>       "
			"Sets the color of the line between the inside and ring when "
			"invalid.\n"
		"  -n, --line-uses-inside           "
			"Use the inside color for the line between the inside and ring.\n"
		"  -r, --line-uses-ring             "
			"Use the ring color for the line between the inside and ring.\n"
		"  --ring-color <color>             "
			"Sets the color of the ring of the indicator.\n"
		"  --ring-clear-color <color>       "
			"Sets the color of the ring of the indicator when cleared.\n"
		"  --ring-caps-lock-color <color>   "
			"Sets the color of the ring of the indicator when Caps Lock "
			"is active.\n"
		"  --ring-ver-color <color>         "
			"Sets the color of the ring of the indicator when verifying.\n"
		"  --ring-wrong-color <color>       "
			"Sets the color of the ring of the indicator when invalid.\n"
		"  --separator-color <color>        "
			"Sets the color of the lines that separate highlight segments.\n"
		"  --text-color <color>             "
			"Sets the color of the text.\n"
		"  --text-clear-color <color>       "
			"Sets the color of the text when cleared.\n"
		"  --text-caps-lock-color <color>   "
			"Sets the color of the text when Caps Lock is active.\n"
		"  --text-ver-color <color>         "
			"Sets the color of the text when verifying.\n"
		"  --text-wrong-color <color>       "
			"Sets the color of the text when invalid.\n"
		"  --command <cmd>                  "
			"Indicates which program to run to draw backgrounds.\n"
		"  --command-each <cmd>             "
			"Like --command, but program is run once for each output\n"
		"\n"
		"All <color> options are of the form <rrggbb[aa]>.\n";

	int c;
	optind = 1;
	while (1) {
		int opt_idx = 0;
		c = getopt_long(argc, argv, "c:deFfhi:kKLlnrs:tuvC:R:", long_options,
				&opt_idx);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'C':
			if (config_path) {
				*config_path = strdup(optarg);
			}
			break;
		case 'c':
			if (state) {
				state->args.colors.background = parse_color(optarg);
			}
			break;
		case 'd':
			swaylock_log_init(LOG_DEBUG);
			break;
		case 'e':
			if (state) {
				state->args.ignore_empty = true;
			}
			break;
		case 'F':
			if (state) {
				state->args.show_failed_attempts = true;
			}
			break;
		case 'f':
			if (state) {
				state->args.daemonize = true;
			}
			break;
		case 'R':
			if (state) {
				state->args.ready_fd = strtol(optarg, NULL, 10);
			}
			break;
		case 'i':
			if (state) {
				load_image(optarg, state);
			}
			break;
		case 'k':
			if (state) {
				state->args.show_keyboard_layout = true;
			}
			break;
		case 'K':
			if (state) {
				state->args.hide_keyboard_layout = true;
			}
			break;
		case 'L':
			if (state) {
				state->args.show_caps_lock_text = false;
			}
			break;
		case 'l':
			if (state) {
				state->args.show_caps_lock_indicator = true;
			}
			break;
		case 'n':
			if (line_mode) {
				*line_mode = LM_INSIDE;
			}
			break;
		case 'r':
			if (line_mode) {
				*line_mode = LM_RING;
			}
			break;
		case 's':
			if (state) {
				state->args.mode = parse_background_mode(optarg);
				if (state->args.mode == BACKGROUND_MODE_INVALID) {
					return 1;
				}
			}
			break;
		case 't':
			if (state) {
				state->args.mode = BACKGROUND_MODE_TILE;
			}
			break;
		case 'u':
			if (state) {
				state->args.show_indicator = false;
			}
			break;
		case 'v':
			fprintf(stdout, "swaylock version " SWAYLOCK_VERSION "\n");
			exit(EXIT_SUCCESS);
			break;
		case LO_BS_HL_COLOR:
			if (state) {
				state->args.colors.bs_highlight = parse_color(optarg);
			}
			break;
		case LO_CAPS_LOCK_BS_HL_COLOR:
			if (state) {
				state->args.colors.caps_lock_bs_highlight = parse_color(optarg);
			}
			break;
		case LO_CAPS_LOCK_KEY_HL_COLOR:
			if (state) {
				state->args.colors.caps_lock_key_highlight = parse_color(optarg);
			}
			break;
		case LO_FONT:
			if (state) {
				free(state->args.font);
				state->args.font = strdup(optarg);
			}
			break;
		case LO_FONT_SIZE:
			if (state) {
				state->args.font_size = atoi(optarg);
			}
			break;
		case LO_IND_IDLE_VISIBLE:
			if (state) {
				state->args.indicator_idle_visible = true;
			}
			break;
		case LO_IND_RADIUS:
			if (state) {
				state->args.radius = strtol(optarg, NULL, 0);
			}
			break;
		case LO_IND_THICKNESS:
			if (state) {
				state->args.thickness = strtol(optarg, NULL, 0);
			}
			break;
		case LO_IND_X_POSITION:
			if (state) {
				state->args.override_indicator_x_position = true;
				state->args.indicator_x_position = atoi(optarg);
			}
			break;
		case LO_IND_Y_POSITION:
			if (state) {
				state->args.override_indicator_y_position = true;
				state->args.indicator_y_position = atoi(optarg);
			}
			break;
		case LO_INSIDE_COLOR:
			if (state) {
				state->args.colors.inside.input = parse_color(optarg);
			}
			break;
		case LO_INSIDE_CLEAR_COLOR:
			if (state) {
				state->args.colors.inside.cleared = parse_color(optarg);
			}
			break;
		case LO_INSIDE_CAPS_LOCK_COLOR:
			if (state) {
				state->args.colors.inside.caps_lock = parse_color(optarg);
			}
			break;
		case LO_INSIDE_VER_COLOR:
			if (state) {
				state->args.colors.inside.verifying = parse_color(optarg);
			}
			break;
		case LO_INSIDE_WRONG_COLOR:
			if (state) {
				state->args.colors.inside.wrong = parse_color(optarg);
			}
			break;
		case LO_KEY_HL_COLOR:
			if (state) {
				state->args.colors.key_highlight = parse_color(optarg);
			}
			break;
		case LO_LAYOUT_BG_COLOR:
			if (state) {
				state->args.colors.layout_background = parse_color(optarg);
			}
			break;
		case LO_LAYOUT_BORDER_COLOR:
			if (state) {
				state->args.colors.layout_border = parse_color(optarg);
			}
			break;
		case LO_LAYOUT_TXT_COLOR:
			if (state) {
				state->args.colors.layout_text = parse_color(optarg);
			}
			break;
		case LO_LINE_COLOR:
			if (state) {
				state->args.colors.line.input = parse_color(optarg);
			}
			break;
		case LO_LINE_CLEAR_COLOR:
			if (state) {
				state->args.colors.line.cleared = parse_color(optarg);
			}
			break;
		case LO_LINE_CAPS_LOCK_COLOR:
			if (state) {
				state->args.colors.line.caps_lock = parse_color(optarg);
			}
			break;
		case LO_LINE_VER_COLOR:
			if (state) {
				state->args.colors.line.verifying = parse_color(optarg);
			}
			break;
		case LO_LINE_WRONG_COLOR:
			if (state) {
				state->args.colors.line.wrong = parse_color(optarg);
			}
			break;
		case LO_RING_COLOR:
			if (state) {
				state->args.colors.ring.input = parse_color(optarg);
			}
			break;
		case LO_RING_CLEAR_COLOR:
			if (state) {
				state->args.colors.ring.cleared = parse_color(optarg);
			}
			break;
		case LO_RING_CAPS_LOCK_COLOR:
			if (state) {
				state->args.colors.ring.caps_lock = parse_color(optarg);
			}
			break;
		case LO_RING_VER_COLOR:
			if (state) {
				state->args.colors.ring.verifying = parse_color(optarg);
			}
			break;
		case LO_RING_WRONG_COLOR:
			if (state) {
				state->args.colors.ring.wrong = parse_color(optarg);
			}
			break;
		case LO_SEP_COLOR:
			if (state) {
				state->args.colors.separator = parse_color(optarg);
			}
			break;
		case LO_TEXT_COLOR:
			if (state) {
				state->args.colors.text.input = parse_color(optarg);
			}
			break;
		case LO_TEXT_CLEAR_COLOR:
			if (state) {
				state->args.colors.text.cleared = parse_color(optarg);
			}
			break;
		case LO_TEXT_CAPS_LOCK_COLOR:
			if (state) {
				state->args.colors.text.caps_lock = parse_color(optarg);
			}
			break;
		case LO_TEXT_VER_COLOR:
			if (state) {
				state->args.colors.text.verifying = parse_color(optarg);
			}
			break;
		case LO_TEXT_WRONG_COLOR:
			if (state) {
				state->args.colors.text.wrong = parse_color(optarg);
			}
			break;
		case LO_PLUGIN_COMMAND:
			if (state) {
				free(state->args.plugin_command);
				state->args.plugin_command = strdup(optarg);
				state->args.plugin_per_output = false;
			}
			break;
		case LO_PLUGIN_COMMAND_EACH:
			if (state) {
				free(state->args.plugin_command);
				state->args.plugin_command = strdup(optarg);
				state->args.plugin_per_output = true;
			}
			break;
		default:
			fprintf(stderr, "%s", usage);
			return 1;
		}
	}

	return 0;
}

static bool file_exists(const char *path) {
	return path && access(path, R_OK) != -1;
}

static char *get_config_path(void) {
	static const char *config_paths[] = {
		"$HOME/.swaylock/config",
		"$XDG_CONFIG_HOME/swaylock/config",
		SYSCONFDIR "/swaylock/config",
	};

	char *config_home = getenv("XDG_CONFIG_HOME");
	if (!config_home || config_home[0] == '\0') {
		config_paths[1] = "$HOME/.config/swaylock/config";
	}

	wordexp_t p;
	char *path;
	for (size_t i = 0; i < sizeof(config_paths) / sizeof(char *); ++i) {
		if (wordexp(config_paths[i], &p, 0) == 0) {
			path = strdup(p.we_wordv[0]);
			wordfree(&p);
			if (file_exists(path)) {
				return path;
			}
			free(path);
		}
	}

	return NULL;
}

static int load_config(char *path, struct swaylock_state *state,
		enum line_mode *line_mode) {
	FILE *config = fopen(path, "r");
	if (!config) {
		swaylock_log(LOG_ERROR, "Failed to read config. Running without it.");
		return 0;
	}
	char *line = NULL;
	size_t line_size = 0;
	ssize_t nread;
	int line_number = 0;
	int result = 0;
	while ((nread = getline(&line, &line_size, config)) != -1) {
		line_number++;

		if (line[nread - 1] == '\n') {
			line[--nread] = '\0';
		}

		if (!*line || line[0] == '#') {
			continue;
		}

		swaylock_log(LOG_DEBUG, "Config Line #%d: %s", line_number, line);
		char *flag = malloc(nread + 3);
		if (flag == NULL) {
			free(line);
			fclose(config);
			swaylock_log(LOG_ERROR, "Failed to allocate memory");
			return 0;
		}
		sprintf(flag, "--%s", line);
		char *argv[] = {"swaylock", flag};
		result = parse_options(2, argv, state, line_mode, NULL);
		free(flag);
		if (result != 0) {
			break;
		}
	}
	free(line);
	fclose(config);
	return 0;
}

static struct swaylock_state state = {0};

static void display_in(int fd, short mask, void *data) {
	if (wl_display_dispatch(state.display) == -1) {
		state.run_display = false;
	}
}

static void comm_in(int fd, short mask, void *data) {
	if (read_comm_reply()) {
		// Authentication succeeded
		state.run_display = false;
	} else {
		state.auth_state = AUTH_STATE_INVALID;
		schedule_auth_idle(&state);
		++state.failed_attempts;
		damage_state(&state);
	}
}

static void dispatch_nested(int fd, short mask, void *data) {
	wl_event_loop_dispatch(state.server.loop, 0);
	if (state.start_clientless_mode) {
		setup_clientless_mode(&state);
	}
}

static void xdg_output_destroy_func(struct wl_resource *resource) {
	/* remove xdg output resource from surface's list of them */
	wl_list_remove(wl_resource_get_link(resource));
}

static void handle_zxdg_output_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static const struct zxdg_output_v1_interface zxdg_output_impl = {
	.destroy = handle_zxdg_output_destroy,
};

static const struct wl_output_interface wl_output_impl;
static void xdg_output_manager_get_xdg_output(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *output) {
	assert(wl_resource_instance_of(output, &wl_output_interface, &wl_output_impl));
	struct swaylock_surface *surface = wl_resource_get_user_data(output);

	struct wl_resource *output_resource =
		wl_resource_create(client, &zxdg_output_v1_interface,
			wl_resource_get_version(resource), id);
	if (output_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(output_resource, &zxdg_output_impl, surface, xdg_output_destroy_func);

	wl_list_insert(&surface->nested_server_xdg_output_resources, wl_resource_get_link(output_resource));

	zxdg_output_v1_send_logical_position(output_resource, 0, 0);
	// todo: how should scale/etc affect this
	zxdg_output_v1_send_logical_size(output_resource, surface->width, surface->height);
	zxdg_output_v1_send_name(output_resource, surface->output_name);
	zxdg_output_v1_send_description(output_resource, surface->output_description);
	zxdg_output_v1_send_done(output_resource);
}
static void xdg_output_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static const struct zxdg_output_manager_v1_interface xdg_output_manager_impl = {
	.destroy = xdg_output_manager_destroy,
	.get_xdg_output = xdg_output_manager_get_xdg_output,
};
static void bind_xdg_output_manager(struct wl_client *client, void *data,
				    uint32_t version, uint32_t id) {
	struct wl_resource *resource =
			wl_resource_create(client, &zxdg_output_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &xdg_output_manager_impl, NULL, NULL);
}

static void handle_wl_output_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_output_interface wl_output_impl = {
	.release = handle_wl_output_release,
};

static void wl_output_handle_destroy(struct wl_resource *resource) {
	// remove output from the list of objects
	wl_list_remove(wl_resource_get_link(resource));
}
static void bind_wl_output(struct wl_client *client, void *data,
			   uint32_t version, uint32_t id) {
	struct swaylock_surface *surface = data;

	struct wl_resource *resource =
			wl_resource_create(client, &wl_output_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &wl_output_impl, surface, wl_output_handle_destroy);

	wl_list_insert(&surface->nested_server_wl_output_resources, wl_resource_get_link(resource));

	// critically, each wl_output is only advertised when the swaylock_surface
	// is first configured, since that is the size that we want to fill
	wl_output_send_geometry(resource, 0, 0, surface->physical_width,
		surface->physical_height, surface->subpixel,
		"swaylock","swaylock", surface->output_transform);
	wl_output_send_mode(resource, 1, surface->mode_width, surface->mode_height, 0);
	wl_output_send_scale(resource, surface->scale);

	if (version >= 4) {
		wl_output_send_name(resource, surface->output_name);
		wl_output_send_description(resource, surface->output_description);
	}
	wl_output_send_done(resource);
}


static void zwlr_layer_surface_set_size(struct wl_client *client,
		struct wl_resource *resource, uint32_t width, uint32_t height) {
	/* ignore this, will send configure as needed */
	/* or alternatively, check that width=height=0 is sent, if anything ?*/
	if (width != 0 || height != 0) {
		swaylock_log(LOG_ERROR, "Warning, layer surface client requesting specific size -- unlikely to be background type");
		return;
	}
}
static void zwlr_layer_surface_set_anchor(struct wl_client *client,
		struct wl_resource *resource, uint32_t anchor) {
	/* ignore this, will always fill the output */
}
static void zwlr_layer_surface_set_exclusive_zone(struct wl_client *client,
		struct wl_resource *resource, int32_t zone) {
	/* ignore this, there are no other clients */
}
static void zwlr_layer_surface_set_margin(struct wl_client *client,
		struct wl_resource *resource,
		int32_t top, int32_t right, int32_t bottom, int32_t left) {
	/* ignore this, will always fill the output */
}
static void zwlr_layer_surface_set_keyboard_interactivity(struct wl_client *client,
		struct wl_resource *resource, uint32_t keyboard_interactivity) {
	/* ignore this, no input will be sent anyway */
}
static void zwlr_layer_surface_get_popup(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *popup) {
	/* should never be called, as no xdg_popup can be ever be created */
}
static void zwlr_layer_surface_ack_configure(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial) {
	struct swaylock_surface *surface = wl_resource_get_user_data(resource);
	struct forward_surface *plugin_surf = surface->plugin_surface;

	if (serial == plugin_surf->last_used_plugin_serial) {
		/* Repeated ack_configures with the same serial can be dropped;
		 * Furthermore, if the upstream uses ext_session_lock_surface,
		 * calling ack_configure twice with the same serial is an error. */
		return;
	}
	plugin_surf->last_used_plugin_serial = serial;

	uint32_t upstream_serial = -1;
	bool found_serial = false;
	for (size_t i = 0; i < plugin_surf->serial_table_len; i++) {
		// fprintf(stderr, "serial check (%zu): %u -> %u =? %u\n", i, plugin_surf->serial_table[i].upstream_serial,
		//	plugin_surf->serial_table[i].plugin_serial, serial);
		if (plugin_surf->serial_table[i].plugin_serial == serial) {
			struct serial_pair entry = plugin_surf->serial_table[i];
			upstream_serial = entry.upstream_serial;
			found_serial = true;
			bool local_only = entry.local_only;
			plugin_surf->last_acked_width = entry.config_width;
			plugin_surf->last_acked_height = entry.config_height;

			/* once a serial is used, discard both it and serials older than it */
			memmove(plugin_surf->serial_table, plugin_surf->serial_table + (i + 1),
				(plugin_surf->serial_table_len - (i + 1))
					* sizeof(struct serial_pair) );
			plugin_surf->serial_table_len -= (i+1);

			if (local_only) {
				// This serial was sent by us, not in response
				// to an upstream configure, so do not forward it
				return;
			}
			break;
		}
	}
	if (!found_serial) {
		// todo: get right message
		wl_client_post_implementation_error(client, "used ack configure with invalid serial");
		return;
	}

	/* Do not send the ack_configure immediately; this avoids a race condition
	 * where the plugin sends ack_configure, and before it sends the matching
	 * configure with a buffer using the new size, the overlay gets updated
	 * and swaylock injects an extra commit (which is necessary for some
	 * subsurface state changes); this commit would use the old buffer with
	 * the wrong size, which is a protocol error for ext-session-lock. */
	surface->has_pending_ack_conf = true;
	surface->pending_upstream_serial = upstream_serial;
}
static void zwlr_layer_surface_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	/* no resource to clean up */
}

static void zwlr_layer_surface_set_layer(struct wl_client *client,
		struct wl_resource *resource, uint32_t layer) {
	/* ignore this, will always fill the entire output */
}

static void zwlr_layer_surface_set_exclusive_edge(struct wl_client *client,
		struct wl_resource *resource, uint32_t edge) {
	/* ignore this, will always fill the entire output */
}

static const struct zwlr_layer_surface_v1_interface layer_surface_impl = {
	.set_size = zwlr_layer_surface_set_size,
	.set_anchor = zwlr_layer_surface_set_anchor,
	.set_exclusive_zone = zwlr_layer_surface_set_exclusive_zone,
	.set_margin = zwlr_layer_surface_set_margin,
	.set_keyboard_interactivity = zwlr_layer_surface_set_keyboard_interactivity,
	.get_popup = zwlr_layer_surface_get_popup,
	.ack_configure = zwlr_layer_surface_ack_configure,
	.destroy = zwlr_layer_surface_destroy,
	.set_layer = zwlr_layer_surface_set_layer,
	.set_exclusive_edge = zwlr_layer_surface_set_exclusive_edge,
};

void wlr_layer_shell_get_layer_surface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface,
		struct wl_resource *output, uint32_t layer, const char *namespace) {
	struct forward_surface *surf = wl_resource_get_user_data(surface);
	struct swaylock_state *state = wl_resource_get_user_data(resource);

	struct swaylock_surface *sw_surface;
	if ((state->server.main_client && client == state->server.main_client->client) || output) {
		if (!output) {
			swaylock_log(LOG_ERROR, "Main client tried to create a layer surface without specifying an output");
			return;
		}
		assert(wl_resource_instance_of(output, &wl_output_interface, &wl_output_impl));
		sw_surface = wl_resource_get_user_data(output);
	} else {
		// Lookup output for client
		struct swaylock_bg_client *bg_client;
		wl_list_for_each(bg_client, &state->server.clients, link) {
			if (bg_client->client == client) {
				sw_surface = bg_client->unique_output;
			}
		}
		if (!sw_surface) {
			swaylock_log(LOG_ERROR, "Failed to find an output matching client");
			return;
		}
	}

	// todo: replace the old surface instead; this will simplify implementation
	// of plugin command restarting
	if (sw_surface->plugin_surface) {
		wl_client_post_implementation_error(client, "Tried to get a new layer surface for an output that already has one.");
		return;
	}
	if (surf->sway_surface) {
		wl_client_post_implementation_error(client, "Tried to get a new layer surface for a surface that already has one.");
		return;
	}
	/* normal programs will only use the BACKGROUND layer, but there is no reason
	 * not to force everything to work. */
	(void)layer; // todo: validate this is in range
	/* not important */
	(void)namespace;

	sw_surface->plugin_surface = surf;
	surf->sway_surface = sw_surface;

	/* consume a serial, and do not reveal it to the client, for the purpose
	 * of ensuring this value is unique. todo: simpler solution */
	struct swaylock_bg_client *bg_client = sw_surface->client ?
		sw_surface->client : sw_surface->state->server.main_client;
	surf->last_used_plugin_serial = bg_client->serial++;

	/* now, create the object that was asked for */
	struct wl_resource *layer_surface_resource = wl_resource_create(client,
		&zwlr_layer_surface_v1_interface, wl_resource_get_version(resource), id);
	if (layer_surface_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(layer_surface_resource, &layer_surface_impl, sw_surface, NULL);

	surf->layer_surface = layer_surface_resource;

	/* Notify client immediately of surface fractional scale, if possible and it is available */
	if (sw_surface->last_fractional_scale > 0 && surf->fractional_scale) {
		wp_fractional_scale_v1_send_preferred_scale(surf->fractional_scale,
			sw_surface->last_fractional_scale);
	}

	// todo: when plugin surface commits, proceed?
}

static const struct zwlr_layer_shell_v1_interface zwlr_layer_shell_v1_impl = {
	.get_layer_surface = wlr_layer_shell_get_layer_surface,
};

static void bind_wlr_layer_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &zwlr_layer_shell_v1_interface, version, id);
	struct swaylock_state *state = data;
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &zwlr_layer_shell_v1_impl, state, NULL);
}

static void render_fallback_surface(struct swaylock_surface *surface) {
	// create a new buffer each time; this is a fallback path, so efficiency
	// is much less important than correctness. That being said, if wp_viewporter
	// were always available, one could instead make a single-pixel buffer in advance

	struct pool_buffer buffer;
	if (!create_buffer(surface->state->shm, &buffer, surface->width, surface->height,
			WL_SHM_FORMAT_ARGB8888)) {
		swaylock_log(LOG_ERROR,
			     "Failed to create new buffer for frame background.");
		return;
	}
	cairo_t *cairo = buffer.cairo;
	cairo_set_source_rgba(buffer.cairo, 0.73, 0.73, 0.73, 1.0);
	cairo_set_operator(buffer.cairo, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cairo);

	wl_surface_set_buffer_scale(surface->surface, 1);
	wl_surface_attach(surface->surface, buffer.buffer, 0, 0);
	wl_surface_damage_buffer(surface->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface->surface);
	destroy_buffer(&buffer);

	surface->has_buffer = true;
}

static void setup_clientless_mode(struct swaylock_state *state) {
	if (!state->server.display) {
		// Have already done this
		return;
	}

	// First, shutdown nested server, and all resources and clients.
	// todo: any additional clean up necessary?
	loop_remove_fd(state->eventloop, wl_event_loop_get_fd(state->server.loop));
	wl_display_destroy(state->server.display);
	state->server.display = NULL;

	struct swaylock_surface *surface = NULL;
	wl_list_for_each(surface, &state->surfaces, link) {
		bool pre_configure = surface->width <= 0 || surface->height <= 0;
		if (pre_configure) {
			continue;
		}

		if (!surface->has_buffer) {
			/* Reply to most recent configure, if the nested client did not already do so
			 * before it died. */
			if (surface->has_newer_serial) {
				ext_session_lock_surface_v1_ack_configure(surface->ext_session_lock_surface_v1, surface->newest_serial);
				surface->has_newer_serial = false;
			}
			render_fallback_surface(surface);
		}
		render_frame(surface);
	}
}


static void client_timeout(struct swaylock_bg_client *bg_client) {
	wl_list_remove(&bg_client->client_destroy_listener.link);

	struct swaylock_state *state = bg_client->state;

	// Destroying the client will free the bg_client
	wl_client_destroy(bg_client->client);

	setup_clientless_mode(state);
}

static void client_connection_timeout(void *data) {
	struct swaylock_bg_client *bg_client = data;
	// The event loop frees the timer object, so clean up
	bg_client->client_connect_timer = NULL;

	swaylock_log(LOG_ERROR, "Client connection timed out; falling back to a client-less mode");
	client_timeout(bg_client);
}

static void output_redraw_timeout(void *data) {
	struct swaylock_surface *surface = data;
	// The event loop frees the timer object, so clean up
	surface->client_submission_timer = NULL;

	if (!surface->client && !surface->state->server.main_client) {
		swaylock_log(LOG_DEBUG,
			"Original client failed to redraw output %d in time",
			surface->output_global_name);
		return;
	} else {
		swaylock_log(LOG_ERROR,
			"Client failed to redraw output %d in time; falling back to a client-less mode",
			surface->output_global_name);
	}

	client_timeout(surface->client ? surface->client : surface->state->server.main_client);
}

uint32_t posix_spawn_setsid_flag(void);
static bool spawn_command(struct swaylock_state *state, int sock_child,
		int sock_local, const char *output_name, const char *output_desc) {
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attribs;
	char **prog_envp = NULL;
	bool ret = false;

	if (posix_spawn_file_actions_init(&actions) == -1) {
		swaylock_log(LOG_ERROR, "Failed to initialize file actions");
		return false;
	}
	if (posix_spawnattr_init(&attribs) != 0) {
		posix_spawn_file_actions_destroy(&actions);
		swaylock_log(LOG_ERROR, "Failed to init spawn attributes");
		return false;
	}

	if (posix_spawn_file_actions_addclose(&actions, sock_local) == -1) {
		swaylock_log(LOG_ERROR, "Failed to update file actions");
		goto end;
	}
	/* Make child processes their own session leader. This ensures that
	 * they do not have a controlling terminal, and thus should not expect
	 * to interact on swaylock's terminal */
	if (posix_spawnattr_setflags(&attribs, posix_spawn_setsid_flag()) != 0) {
		swaylock_log(LOG_ERROR, "Failed to set spawn flags");
		goto end;
	}

	size_t envlen = 0;
	while (environ[envlen]) {
		envlen++;
	}
	prog_envp = calloc(envlen + 4, sizeof(char *));
	if (!prog_envp) {
		swaylock_log(LOG_ERROR, "Failed to allocate new environ");
		goto end;
	}
	size_t j = 0;
	for (size_t i = 0; i < envlen; i++) {
		// removing WAYLAND_DEBUG avoids confusion between debug logs;
		// alas, the default debug log does not distinguish programs :-(
		const char *skip[] = {
			"WAYLAND_DEBUG",
			"WAYLAND_DISPLAY",
			"WAYLAND_SOCKET",
			"SWAYLOCK_PLUGIN_OUTPUT_NAME",
			"SWAYLOCK_PLUGIN_OUTPUT_DESC",
			"DISPLAY",
		};
		bool drop = false;
		for (size_t k = 0; k < sizeof(skip) / sizeof(skip[0]); k++) {
			if (strncmp(environ[i], skip[k], strlen(skip[k])) == 0) {
				drop = true;
				break;
			}
		}
		if (drop) {
			continue;
		}

		prog_envp[j++] = environ[i];
	}
	char kv_name[256], kv_desc[256];
	char kv_socket[32];
	snprintf(kv_socket, sizeof(kv_socket), "WAYLAND_SOCKET=%d", sock_child);
	prog_envp[j++] = kv_socket;
	if (output_name && output_desc) {
		snprintf(kv_name, sizeof(kv_name), "SWAYLOCK_PLUGIN_OUTPUT_NAME=%s", output_name);
		snprintf(kv_desc, sizeof(kv_desc), "SWAYLOCK_PLUGIN_OUTPUT_DESC=%s", output_desc);
		prog_envp[j++] = kv_name;
		prog_envp[j++] = kv_desc;
	}
	prog_envp[j++] = NULL;

	char *prog_argv[] = {
		"sh", "-c", state->args.plugin_command, NULL
	};

	// Use posix_spawnp to spawn program. This is rather awkward to do,
	// but can be significantly more efficient than fork()+exec()
	pid_t pid;
	if (posix_spawnp(&pid, "sh", &actions, &attribs, prog_argv, prog_envp) == -1) {
		swaylock_log(LOG_ERROR, "Failed to forkspawn background plugin: %s", strerror(errno));
		goto end;
	}
	fprintf(stderr, "Forked background plugin (pid = %d): %s\n", pid, state->args.plugin_command);
	ret = true;

end:
	posix_spawnattr_destroy(&attribs);
	posix_spawn_file_actions_destroy(&actions);
	free(prog_envp);
	return ret;
}

static void client_resource_create(struct wl_listener *listener, void *data) {
	struct swaylock_bg_client *bg_client = wl_container_of(listener, bg_client, client_resource_create_listener);
	struct wl_resource *resource = data;
	if (wl_resource_get_client(resource) != bg_client->client) {
		swaylock_log(LOG_ERROR, "Resource create callback does not match client");
		return;
	}

	// TODO: use a more intelligent system based on the time needed
	// to produce a buffer on request
	bg_client->made_a_registry = true;
	if (bg_client->client_connect_timer) {
		loop_remove_timer(bg_client->state->eventloop, bg_client->client_connect_timer);
		bg_client->client_connect_timer = NULL;
	}

	// Unregister this listener
	wl_list_remove(&listener->link);
	wl_list_init(&listener->link);
}

static void client_destroyed(struct wl_listener *listener, void *data) {
	struct swaylock_bg_client *bg_client = wl_container_of(listener, bg_client, client_destroy_listener);
	struct wl_client *client = data;
	if (client != bg_client->client) {
		swaylock_log(LOG_ERROR, "Client destroy callback does not match actual client");
		return;
	}
	wl_list_remove(&bg_client->link);

	if (bg_client->client_connect_timer) {
		loop_remove_timer(bg_client->state->eventloop, bg_client->client_connect_timer);
	}

	bool made_a_registry = bg_client->made_a_registry;
	struct swaylock_state *state = bg_client->state;
	struct swaylock_surface *output_surface = bg_client->unique_output;

	if (output_surface) {
		output_surface->client = NULL;
	} else {
		state->server.main_client = NULL;
	}

	free(bg_client);

	// Restart the command, ONLY if it successfully did something the last time
	// A one-shot program like wayland-info will still cycle indefinitely, so
	// a better measure appears necessary
	if (!made_a_registry || !run_plugin_command(state, output_surface)) {
		// Cannot call `setup_clientless_mode` inside `wl_event_loop_dispatch`,
		// so mark it to be called immediately afterwards
		state->start_clientless_mode = true;
	}
}

/**
 * Start the plugin command. If `output` is NULL, apply it to all outputs;
 * otherwise only to the one specified by `output`.
 */
static bool run_plugin_command(struct swaylock_state *state, struct swaylock_surface *output_surface) {
	int sockpair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockpair) == -1) {
		swaylock_log(LOG_ERROR, "Failed to create socket pair for background plugin");
		return false;
	}
	if (!set_cloexec(sockpair[1])) {
		close(sockpair[0]);
		close(sockpair[1]);
		swaylock_log(LOG_ERROR, "Failed to set close-on-exec for local socket end");
		return false;
	}

	if (!spawn_command(state, sockpair[0], sockpair[1],
			output_surface ? output_surface->output_name : NULL,
			output_surface ? output_surface->output_description : NULL)) {
		close(sockpair[0]);
		close(sockpair[1]);
		printf("Failed to run command: %s\n", state->args.plugin_command);
		return false;
	}
	close(sockpair[0]);

	struct swaylock_bg_client *bg_client = calloc(1, sizeof(struct swaylock_bg_client));
	if (!bg_client) {
		close(sockpair[1]);
		return false;
	}
	wl_list_insert(&state->server.clients, &bg_client->link);
	bg_client->state = state;
	bg_client->serial = 100000;
	bg_client->client = wl_client_create(state->server.display, sockpair[1]);

	bg_client->made_a_registry = false;

	// Note: the timers added here use CLOCK_MONOTONIC, which on Linux does not
	// count time in a suspended state; the callback will only mark the client
	// as broken/not responding if it spends 10 seconds with the system active
	// not doing anything
	bg_client->client_connect_timer = loop_add_timer(state->eventloop,
		TIMEOUT_CONNECT, client_connection_timeout, bg_client);

	bg_client->client_destroy_listener.notify = client_destroyed;
	bg_client->client_resource_create_listener.notify = client_resource_create;

	// We treat the client as "connected" when it makes a registry
	wl_client_add_resource_created_listener(bg_client->client,
		&bg_client->client_resource_create_listener);

	wl_client_add_destroy_listener(bg_client->client,
		&bg_client->client_destroy_listener);

	if (output_surface) {
		output_surface->client = bg_client;
		bg_client->unique_output = output_surface;
	} else {
		state->server.main_client = bg_client;
	}

	return true;
}

bool global_filter(const struct wl_client *client, const struct wl_global *global,
	void *data) {
	struct swaylock_state *state = data;
	struct swaylock_bg_client *bg_client;
	wl_list_for_each(bg_client, &state->server.clients, link) {
		if (bg_client->client == client) {
			if (bg_client->unique_output && wl_global_get_interface(global) == &wl_output_interface) {
				struct swaylock_surface *surf = wl_global_get_user_data(global);
				if (surf != bg_client->unique_output) {
					return false;
				}
			}
			return true;
		}
	}
	printf("Unidentified client\n");
	return false;
}



static void term_in(int fd, short mask, void *data) {
	state.run_display = false;
}

// Check for --debug 'early' we also apply the correct loglevel
// to the forked child, without having to first proces all of the
// configuration (including from file) before forking and (in the
// case of the shadow backend) dropping privileges
void log_init(int argc, char **argv) {
	static struct option long_options[] = {
		{"debug", no_argument, NULL, 'd'},
		{0, 0, 0, 0}
	};
	int c;
		optind = 1;
	while (1) {
		int opt_idx = 0;
		c = getopt_long(argc, argv, "-:d", long_options, &opt_idx);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'd':
			swaylock_log_init(LOG_DEBUG);
			return;
		}
	}
	swaylock_log_init(LOG_ERROR);
}

int main(int argc, char **argv) {
	log_init(argc, argv);
	initialize_pw_backend(argc, argv);
	srand(time(NULL));

	enum line_mode line_mode = LM_LINE;
	state.failed_attempts = 0;
	state.args = (struct swaylock_args){
		.mode = BACKGROUND_MODE_FILL,
		.font = strdup("sans-serif"),
		.font_size = 0,
		.radius = 50,
		.thickness = 10,
		.indicator_x_position = 0,
		.indicator_y_position = 0,
		.override_indicator_x_position = false,
		.override_indicator_y_position = false,
		.ignore_empty = false,
		.show_indicator = true,
		.show_caps_lock_indicator = false,
		.show_caps_lock_text = true,
		.show_keyboard_layout = false,
		.hide_keyboard_layout = false,
		.show_failed_attempts = false,
		.indicator_idle_visible = false,
		.ready_fd = -1,
		.plugin_command = NULL,
	};
	wl_list_init(&state.images);
	set_default_colors(&state.args.colors);

	char *config_path = NULL;
	int result = parse_options(argc, argv, NULL, NULL, &config_path);
	if (result != 0) {
		free(config_path);
		return result;
	}
	if (!config_path) {
		config_path = get_config_path();
	}

	if (config_path) {
		swaylock_log(LOG_DEBUG, "Found config at %s", config_path);
		int config_status = load_config(config_path, &state, &line_mode);
		free(config_path);
		if (config_status != 0) {
			free(state.args.font);
			return config_status;
		}
	}

	if (argc > 1) {
		swaylock_log(LOG_DEBUG, "Parsing CLI Args");
		int result = parse_options(argc, argv, &state, &line_mode, NULL);
		if (result != 0) {
			free(state.args.font);
			return result;
		}
	}

	if (line_mode == LM_INSIDE) {
		state.args.colors.line = state.args.colors.inside;
	} else if (line_mode == LM_RING) {
		state.args.colors.line = state.args.colors.ring;
	}

	state.password.len = 0;
	state.password.buffer_len = 1024;
	state.password.buffer = password_buffer_create(state.password.buffer_len);
	if (!state.password.buffer) {
		return EXIT_FAILURE;
	}

	if (pipe(sigusr_fds) != 0) {
		swaylock_log(LOG_ERROR, "Failed to pipe");
		return EXIT_FAILURE;
	}
	if (!set_cloexec(sigusr_fds[0]) || !set_cloexec(sigusr_fds[1])) {
		swaylock_log(LOG_ERROR, "Failed to make pipes close-on-exec");
		return EXIT_FAILURE;
	}
	if (fcntl(sigusr_fds[1], F_SETFL, O_NONBLOCK) == -1) {
		swaylock_log(LOG_ERROR, "Failed to make pipe end nonblocking");
		return EXIT_FAILURE;
	}

	// temp: make all backgrounds use some sort of plugin command
	if (!state.args.plugin_command) {
		char command[2048];
		size_t start = 0;
		start += sprintf(command + start, "swaybg -c '#%06x'", state.args.colors.background >> 8);

		struct swaylock_image *image;
		wl_list_for_each(image, &state.images, link) {
			const char *mode = "stretch";
			if (state.args.mode == BACKGROUND_MODE_STRETCH) {
				mode = "stretch";
			} else if (state.args.mode == BACKGROUND_MODE_FILL) {
				mode = "fill";
			} else if (state.args.mode == BACKGROUND_MODE_FIT) {
				mode = "fit";
			} else if (state.args.mode == BACKGROUND_MODE_CENTER) {
				mode = "center";
			} else if (state.args.mode == BACKGROUND_MODE_TILE) {
				mode = "tile";
			} else {
				mode = "solid_color";
			}

			start += sprintf(command + start, " -o '%s' -i '%s' -m %s",
					 image->output_name ? image->output_name : "*", image->path, mode);
		}

		// todo: build more complicated swaybg command
		state.args.plugin_command = strdup(command);
	}

	state.eventloop = loop_create();

	wl_list_init(&state.surfaces);
	state.xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	state.display = wl_display_connect(NULL);
	if (!state.display) {
		free(state.args.font);
		swaylock_log(LOG_ERROR, "Unable to connect to the compositor. "
				"If your compositor is running, check or set the "
				"WAYLAND_DISPLAY environment variable.");
		return EXIT_FAILURE;
	}

	struct wl_registry *registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	state.forward.upstream_display = state.display;
	state.forward.upstream_registry = registry;
	wl_list_init(&state.forward.feedback_instances);
	wl_list_init(&state.stale_wl_output_resources);
	wl_list_init(&state.stale_xdg_output_resources);
	wl_list_init(&state.server.clients);

	// Create the downstream display early, so that per-output plugin commands
	// launched on upstream output receipt have something to connect to.
	// Issue: the globals may be delayed, somewhat
	state.server.display = wl_display_create();
	wl_display_set_global_filter(state.server.display, global_filter, &state);

	if (wl_display_roundtrip(state.display) == -1) {
		swaylock_log(LOG_ERROR, "wl_display_roundtrip() failed");
		return EXIT_FAILURE;
	}

	if (!state.compositor) {
		swaylock_log(LOG_ERROR, "Missing wl_compositor");
		return 1;
	}

	if (!state.subcompositor) {
		swaylock_log(LOG_ERROR, "Missing wl_subcompositor");
		return 1;
	}

	if (!state.shm) {
		swaylock_log(LOG_ERROR, "Missing wl_shm");
		return 1;
	}

	if (!state.ext_session_lock_manager_v1) {
		swaylock_log(LOG_ERROR, "Missing ext-session-lock-v1");
		return 1;
	}

	state.ext_session_lock_v1 = ext_session_lock_manager_v1_lock(state.ext_session_lock_manager_v1);
	ext_session_lock_v1_add_listener(state.ext_session_lock_v1,
			&ext_session_lock_v1_listener, &state);

	if (wl_display_roundtrip(state.display) == -1) {
		free(state.args.font);
		if (state.input_inhibit_manager) {
			swaylock_log(LOG_ERROR, "Exiting - failed to inhibit input:"
					" is another lockscreen already running?");
			return 2;
		}
		return 1;
	}

	state.test_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
	state.test_cairo = cairo_create(state.test_surface);

	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state.surfaces, link) {
		create_surface(surface);
	}

	while (!state.locked) {
		if (wl_display_dispatch(state.display) < 0) {
			swaylock_log(LOG_ERROR, "wl_display_dispatch() failed");
			return 2;
		}
	}

	if (state.args.ready_fd >= 0) {
		if (write(state.args.ready_fd, "\n", 1) != 1) {
			swaylock_log(LOG_ERROR, "Failed to send readiness notification");
			return 2;
		}
		close(state.args.ready_fd);
		state.args.ready_fd = -1;
	}
	if (state.args.daemonize) {
		daemonize();
	}

	/* fill in dmabuf modifier list if empty and upstream provided dmabuf-feedback */
	if (state.forward.linux_dmabuf && zwp_linux_dmabuf_v1_get_version(state.forward.linux_dmabuf) >= 4) {
		size_t npairs = 0;
		for (size_t i = 0; i < state.forward.current.tranches_len; i++) {
			npairs += state.forward.current.tranches[i].indices.size;
		}
		free(state.forward.dmabuf_formats);
		state.forward.dmabuf_formats = calloc(npairs, sizeof(struct dmabuf_modifier_pair));

		void *table = mmap(NULL, state.forward.current.table_fd_size, PROT_READ, MAP_PRIVATE,  state.forward.current.table_fd, 0);
		if (table == MAP_FAILED) {
			swaylock_log(LOG_ERROR, "Failed to map dmabuf feedback table");
			return 1;
		}
		struct feedback_pair *table_data = table;
		size_t j = 0;
		for (size_t i = 0; i < state.forward.current.tranches_len; i++) {
			const struct wl_array *indices = &state.forward.current.tranches[i].indices;
			for (size_t k = 0; k < indices->size / 2; k++) {
				uint16_t index = ((uint16_t*)indices->data)[k];
				state.forward.dmabuf_formats[j].format = table_data[index].format;
				state.forward.dmabuf_formats[j].modifier_hi = table_data[index].modifier_hi;
				state.forward.dmabuf_formats[j].modifier_lo = table_data[index].modifier_lo;
				j++;
			}
		}
		state.forward.dmabuf_formats_len = j;
		munmap(table, state.forward.current.table_fd_size);
		// todo: sort & deduplicate table?
	}

	// Blind forwarding interfaces. TODO: cache data until needed, so
	// as to avoid creating unused buffers or surfaces on the compositor.
	// Also TODO: forwarding linux-dmabuf and (only the device part) of wl-drm
	state.server.compositor = wl_global_create(state.server.display,
		&wl_compositor_interface, 4, &state.forward, bind_wl_compositor);
	state.server.shm = wl_global_create(state.server.display,
		&wl_shm_interface, 1, &state.forward, bind_wl_shm);
	if (state.forward.drm) {
		state.server.drm = wl_global_create(state.server.display,
			&wl_drm_interface, 2, &state.forward, bind_drm);
	}
	if (state.forward.linux_dmabuf) {
		uint32_t version = zwp_linux_dmabuf_v1_get_version(state.forward.linux_dmabuf);
		state.server.zwp_linux_dmabuf = wl_global_create(state.server.display,
			&zwp_linux_dmabuf_v1_interface, version, &state.forward, bind_linux_dmabuf);
	}

	// Fortunately, the _interface_ structs are identical between
	// wayland-client and wayland-server
	state.server.wlr_layer_shell = wl_global_create(state.server.display,
		&zwlr_layer_shell_v1_interface, 5, &state, bind_wlr_layer_shell);
	state.server.xdg_output_manager = wl_global_create(state.server.display,
		&zxdg_output_manager_v1_interface, 2, NULL, bind_xdg_output_manager);
	if (state.forward.fractional_scale) {
		state.server.wp_fractional_scale = wl_global_create(state.server.display,
			&wp_fractional_scale_manager_v1_interface, 1, &state.forward, bind_fractional_scale);
	}
	if (state.forward.viewporter) {
		state.server.wp_viewporter = wl_global_create(state.server.display,
			&wp_viewporter_interface, 1, &state.forward, bind_viewporter);
	}
	state.server.loop = wl_display_get_event_loop(state.server.display);

	// Start the plugin (assuming it applies to all outputs)
	if (!state.args.plugin_per_output) {
		if (!run_plugin_command(&state, NULL)) {
			setup_clientless_mode(&state);
		}
	}

	loop_add_fd(state.eventloop, wl_display_get_fd(state.display), POLLIN,
			display_in, NULL);

	loop_add_fd(state.eventloop, get_comm_reply_fd(), POLLIN, comm_in, NULL);

	loop_add_fd(state.eventloop, wl_event_loop_get_fd(state.server.loop),
		POLLIN, dispatch_nested, NULL);

	loop_add_fd(state.eventloop, sigusr_fds[0], POLLIN, term_in, NULL);

	struct sigaction sa;
	sa.sa_handler = do_sigusr;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL);

	// Ignore SIGCHLD, to make child processes be automatically reaped.
	// (This setting is not inherited to child processes.)
	struct sigaction sa2;
	sa2.sa_handler = SIG_IGN;
	sigemptyset(&sa2.sa_mask);
	sa2.sa_flags = 0;
	sigaction(SIGCHLD, &sa2, NULL);

	state.run_display = true;
	while (state.run_display) {
		errno = 0;
		if (wl_display_flush(state.display) == -1 && errno != EAGAIN) {
			break;
		}
		if (state.server.display) {
			wl_display_flush_clients(state.server.display);
		}

		loop_poll(state.eventloop);
	}

	ext_session_lock_v1_unlock_and_destroy(state.ext_session_lock_v1);
	wl_display_roundtrip(state.display);

	free(state.args.font);
	cairo_destroy(state.test_cairo);
	cairo_surface_destroy(state.test_surface);
	return 0;
}
