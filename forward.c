#include "swaylock.h"

#include "log.h"

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <stdlib.h>
#include <assert.h>
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "wayland-drm-server-protocol.h"
#include "wlr-layer-shell-unstable-v1-server-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"

static const struct wl_surface_interface surface_impl;
static const struct wl_buffer_interface buffer_impl;
static const struct wl_shm_pool_interface shm_pool_impl;
static const struct wl_compositor_interface compositor_impl;
static const struct zwp_linux_buffer_params_v1_interface linux_dmabuf_params_impl;
static const struct zwp_linux_dmabuf_feedback_v1_interface linux_dmabuf_feedback_v1_impl;

static void nested_surface_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	// this will also destroy the user_data.
	wl_resource_destroy(resource);
}
static void nested_surface_attach(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *buffer, int32_t x, int32_t y) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	assert(wl_resource_instance_of(buffer, &wl_buffer_interface, &buffer_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);
	struct forward_buffer *f_buffer = wl_resource_get_user_data(buffer);

	if (surface->pending.attachment == f_buffer) {
		/* no change */
		return;
	}
	if (surface->pending.attachment != NULL && surface->pending.attachment != BUFFER_COMMITTED) {
		/* Dereference pending buffer */
		struct forward_buffer *old_buf = surface->pending.attachment;
		wl_list_remove(&surface->pending.attachment_link);
		/* Remove old buffer if no links to it left */
		if (old_buf->resource == NULL && wl_list_empty(&old_buf->pending_surfaces)) {
			assert(wl_list_empty(&old_buf->committed_surfaces));
			wl_buffer_destroy(old_buf->buffer);
			free(old_buf);
		}
	}

	wl_list_insert(&f_buffer->pending_surfaces, &surface->pending.attachment_link);
	surface->pending.attachment = f_buffer;
}
static void nested_surface_damage(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y,
		int32_t width, int32_t height) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);


	struct damage_record *new_damage = realloc(surface->buffer_damage, sizeof(struct damage_record) * (surface->buffer_damage_len + 1));
	if (new_damage) {
		surface->buffer_damage = new_damage;
		surface->buffer_damage[surface->buffer_damage_len].x = x;
		surface->buffer_damage[surface->buffer_damage_len].y = y;
		surface->buffer_damage[surface->buffer_damage_len].w = width;
		surface->buffer_damage[surface->buffer_damage_len].h = height;
		surface->buffer_damage_len++;
	}
}

static void frame_callback_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_callback_interface, NULL));

	wl_list_remove(wl_resource_get_link(resource));
}
static void nested_surface_frame(struct wl_client *client,
		struct wl_resource *resource, uint32_t callback) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));

	struct wl_resource *callback_resource = wl_resource_create(client, &wl_callback_interface,
		wl_resource_get_version(resource), callback);
	if (callback_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(callback_resource, NULL,
		NULL, frame_callback_handle_resource_destroy);

	struct forward_surface *surface = wl_resource_get_user_data(resource);
	struct wl_list *link = wl_resource_get_link(callback_resource);
	wl_list_insert(&surface->frame_callbacks, link);
}
static void nested_surface_set_opaque_region(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *region) {
	// do nothing, swaylock doesn't need to know about regions
}
static void nested_surface_set_input_region(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *region) {
	// do nothing, swaylock doesn't need to know about regions
}

void add_serial_pair(struct forward_surface *surf, uint32_t upstream_serial, uint32_t downstream_serial, bool local_only) {
	surf->serial_table = realloc(surf->serial_table, sizeof(struct serial_pair) * (surf->serial_table_len + 1));
	assert(surf->serial_table);

	surf->serial_table[surf->serial_table_len] = (struct serial_pair) {
		.plugin_serial = downstream_serial,
		.upstream_serial = upstream_serial,
		.local_only = local_only,
	};
	surf->serial_table_len++;
}

static void bg_frame_handle_done(void *data, struct wl_callback *callback,
		uint32_t time) {
	(void)time;
	struct forward_surface *surface = data;

	// Trigger all frame callbacks for the background
	struct wl_resource *plugin_cb, *tmp;
	wl_resource_for_each_safe(plugin_cb, tmp, &surface->frame_callbacks) {
		wl_callback_send_done(plugin_cb, 0);
		wl_resource_destroy(plugin_cb);
	}
	wl_callback_destroy(callback);
}

static const struct wl_callback_listener bg_frame_listener = {
	.done = bg_frame_handle_done,
};

static void nested_surface_commit(struct wl_client *client,
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);
	if (surface->inert) {
		return;
	}

	if (!surface->sway_surface) {
		/* todo: is a role required? if not, should only ignore in this case */
		wl_client_post_implementation_error(client, "tried to commit a surface without a role");
		return;
	}

	if (!surface->has_been_configured) {
		/* send initial configure */
		uint32_t plugin_serial = wl_display_next_serial(wl_client_get_display(client));

		if (surface->sway_surface->width == 0 || surface->sway_surface->height == 0) {
			swaylock_log(LOG_ERROR, "committing nested surface before main surface dimensions known");
		}

		// When committing a plugin surface for the first time,
		// if the upstream surface is also new, then forward the configure;
		// but if the upsteam surface was configured long ago, then
		// keep the configure local
		if (surface->sway_surface->used_first_configure) {
			add_serial_pair(surface, 0, plugin_serial, true);
		} else {
			add_serial_pair(surface, surface->sway_surface->first_configure_serial, plugin_serial, false);
			surface->sway_surface->used_first_configure = true;
		}
		zwlr_layer_surface_v1_send_configure(surface->layer_surface, plugin_serial,
			surface->sway_surface->width, surface->sway_surface->height);

		surface->has_been_configured = true;

		// todo: handle unmap/remap logic -- is it an error to attach a buffer
		// after unmapping?
		assert(!surface->pending.attachment);
		/* The first commit should not be forwarded, because the main swaylock
		 * process already made such a commit in order to receive its
		 * own configure event. Thus, return here. */
		return;
	}

	// todo: every buffer needs surface backreferences for auto-cleanup
	// issue: figuring out details of this auto-cleanup
	// (one approach: use a forward_buffer object holding the upstream,
	// with a linked list of downstream users -- the plugin's wl_buffer
	// itself, but also all surfaces linked via commits. Only delete upstream
	// wl_buffer when all references are dead.)

	// integrate details, and commit/send updated data only, here

	struct swaylock_surface *sw_surf = surface->sway_surface;
	struct wl_surface *background = sw_surf->surface;

	/* Apply changes */
	if (surface->committed.buffer_scale != surface->pending.buffer_scale) {
		wl_surface_set_buffer_scale(background, surface->pending.buffer_scale);
		surface->committed.buffer_scale = surface->pending.buffer_scale;
	}
	if (surface->committed.buffer_transform != surface->pending.buffer_transform) {
		wl_surface_set_buffer_transform(background, surface->pending.buffer_transform);
		surface->committed.buffer_transform = surface->pending.buffer_transform;
	}
	if (surface->committed.buffer_scale != surface->pending.buffer_scale) {
		wl_surface_set_buffer_scale(background, surface->pending.buffer_scale);
		surface->committed.buffer_scale = surface->pending.buffer_scale;
	}
	if (surface->committed.attachment != surface->pending.attachment &&
			!(surface->pending.attachment == BUFFER_COMMITTED)) {
		// note: would need '||  surface->pending.attachment' for random_walk_bg due to buffer reuse abuse

		/* unlink the committed attachment */
		if (surface->committed.attachment != NULL && surface->committed.attachment != BUFFER_UNREACHABLE) {
			assert(surface->committed.attachment->resource != NULL);
			wl_list_remove(&surface->committed.attachment_link);
		}

		struct forward_buffer *upstream_buffer = surface->pending.attachment ?
			surface->pending.attachment : NULL;
		wl_surface_attach(background,
			upstream_buffer ? upstream_buffer->buffer : NULL,
			surface->pending.offset_x, surface->pending.offset_y);

		surface->committed.attachment = surface->pending.attachment;
		wl_list_insert(&upstream_buffer->committed_surfaces, &surface->committed.attachment_link);

		surface->committed.offset_x = surface->pending.offset_x;
		surface->committed.offset_y = surface->pending.offset_y;
	}
	/* If there was an offset change, but no buffer value change */
	if (surface->committed.offset_x != surface->pending.offset_x ||
			surface->committed.offset_y != surface->pending.offset_y) {
		wl_surface_offset(background, surface->pending.offset_x, surface->pending.offset_y);
		surface->committed.offset_x = surface->pending.offset_x;
		surface->committed.offset_y = surface->pending.offset_y;
	}

	/* apply and clear damage */
	for (size_t i = 0; i < surface->buffer_damage_len; i++) {
		wl_surface_damage_buffer(background, surface->buffer_damage[i].x,
					 surface->buffer_damage[i].y,
					 surface->buffer_damage[i].w,
					 surface->buffer_damage[i].h);
	}
	for (size_t i = 0; i < surface->old_damage_len; i++) {
		wl_surface_damage(background, surface->old_damage[i].x,
					 surface->old_damage[i].y,
					 surface->old_damage[i].w,
					 surface->old_damage[i].h);
	}

	free(surface->buffer_damage);
	surface->buffer_damage = NULL;
	surface->buffer_damage_len = 0;

	free(surface->old_damage);
	surface->old_damage = NULL;
	surface->old_damage_len = 0;

	/* Finally, commit updates to corresponding upstream background surface */
	if (surface->committed.attachment) {
		// permit subsurface drawing
		surface->sway_surface->has_buffer = true;
	}

	if (!wl_list_empty(&surface->frame_callbacks)) {
		/* plugin has requested frame callbacks, so make a request now */
		struct wl_callback *callback = wl_surface_frame(background);
		wl_callback_add_listener(callback, &bg_frame_listener, surface);
	}

	if (sw_surf->has_pending_ack_conf) {
		/* Submit this right before the commit, to avoid race conditions
		 * between injected commits from the swaylock rendering and
		 * the gap between ack and commit from the plugin */
		ext_session_lock_surface_v1_ack_configure(sw_surf->ext_session_lock_surface_v1, sw_surf->pending_upstream_serial);
		sw_surf->has_pending_ack_conf = false;
	}

	wl_surface_commit(background);
}

static void nested_surface_set_buffer_transform(struct wl_client *client,
		struct wl_resource *resource, int32_t transform) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);
	surface->pending.buffer_transform = transform;
}
static void nested_surface_set_buffer_scale(struct wl_client *client,
		struct wl_resource *resource, int32_t scale) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);
	surface->pending.buffer_scale = scale;
}
static void nested_surface_damage_buffer(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y,
		int32_t width, int32_t height) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);

	struct damage_record *new_damage = realloc(surface->buffer_damage, sizeof(struct damage_record) * (surface->buffer_damage_len + 1));
	if (new_damage) {
		surface->buffer_damage = new_damage;
		surface->buffer_damage[surface->buffer_damage_len].x = x;
		surface->buffer_damage[surface->buffer_damage_len].y = y;
		surface->buffer_damage[surface->buffer_damage_len].w = width;
		surface->buffer_damage[surface->buffer_damage_len].h = height;
		surface->buffer_damage_len++;
	}
}

static const struct wl_surface_interface surface_impl = {
	.destroy = nested_surface_destroy,
	.attach = nested_surface_attach,
	.damage = nested_surface_damage,
	.frame = nested_surface_frame,
	.set_opaque_region = nested_surface_set_opaque_region,
	.set_input_region = nested_surface_set_input_region,
	.commit = nested_surface_commit,
	.set_buffer_transform = nested_surface_set_buffer_transform,
	.set_buffer_scale = nested_surface_set_buffer_scale,
	.damage_buffer = nested_surface_damage_buffer,
};

static void surface_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if (fwd_surface->sway_surface) {
		fwd_surface->sway_surface->plugin_surface = NULL;
	}

	struct wl_resource *cb_resource, *tmp;
	wl_resource_for_each_safe(cb_resource, tmp, &fwd_surface->frame_callbacks) {
		// the callback resource, on destruction, will try to remove itself,
		// so set it up with an empty list (on which _remove() is safe)
		wl_list_remove(wl_resource_get_link(cb_resource));
		wl_list_init(wl_resource_get_link(cb_resource));
	}
	if (fwd_surface->pending.attachment
			&& fwd_surface->pending.attachment != BUFFER_UNREACHABLE
			&& fwd_surface->pending.attachment != BUFFER_COMMITTED) {
		assert(fwd_surface->pending.attachment->resource != NULL);
		wl_list_remove(&fwd_surface->pending.attachment_link);
	}
	if (fwd_surface->committed.attachment
			&& fwd_surface->committed.attachment != BUFFER_UNREACHABLE
			&& fwd_surface->committed.attachment != BUFFER_COMMITTED) {
		assert(fwd_surface->committed.attachment->resource != NULL);
		wl_list_remove(&fwd_surface->committed.attachment_link);
	}

	free(fwd_surface->buffer_damage);
	free(fwd_surface->old_damage);
	free(fwd_surface->serial_table);

	free(fwd_surface);
}

static void compositor_create_surface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	assert(wl_resource_instance_of(resource, &wl_compositor_interface, &compositor_impl));

	struct wl_resource *surf_resource = wl_resource_create(client, &wl_surface_interface,
		wl_resource_get_version(resource), id);
	if (surf_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_surface *fwd_surface = calloc(1, sizeof(struct forward_surface));
	if (!fwd_surface) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&fwd_surface->frame_callbacks);
	struct wl_display *display = wl_client_get_display(client);
	/* consume a serial, and do not reveal it to the client, for the purpose
	 * of ensuring this value is unique. todo: simpler solution */
	fwd_surface->last_used_plugin_serial = wl_display_next_serial(display);

	wl_resource_set_implementation(surf_resource, &surface_impl,
		fwd_surface, surface_handle_resource_destroy);

	// do not listen for events, because the plugin has no input anyway
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	// do nothing, swaylock doesn't need to know about regions
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	// do nothing, swaylock doesn't need to know about regions
}

static void region_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static const struct wl_region_interface region_impl = {
	.destroy = region_destroy,
	.add = region_add,
	.subtract = region_subtract,
};

static void compositor_create_region(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	/* for nested clients, regions are ignored entirely */
	struct wl_resource *region_resource = wl_resource_create(client,
		&wl_region_interface, wl_resource_get_version(resource), id);
	if (region_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(region_resource, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = compositor_create_surface,
	.create_region = compositor_create_region,
};
void bind_wl_compositor(struct wl_client *client, void *data,
				    uint32_t version, uint32_t id) {
	struct wl_resource *resource =
			wl_resource_create(client, &wl_compositor_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &compositor_impl, data, NULL);
}


static void nested_buffer_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void handle_buffer_release(void *data, struct wl_buffer *wl_buffer) {
	struct forward_buffer *buffer = data;
	if (buffer->resource) {
		wl_buffer_send_release(buffer->resource);
	}
}
static const struct wl_buffer_interface buffer_impl = {
	.destroy = nested_buffer_destroy,
};
static const struct wl_buffer_listener buffer_listener = {
	.release = handle_buffer_release
};
static void buffer_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_buffer_interface, &buffer_impl));
	struct forward_buffer* buffer = wl_resource_get_user_data(resource);
	/* The plugin can not longer attach the buffer, so clean up all
	 * places where it is committed. */
	struct forward_surface *surface, *tmp;
	wl_list_for_each_safe(surface, tmp, &buffer->committed_surfaces, committed.attachment_link) {
		if (surface->pending.attachment == surface->committed.attachment) {
			surface->pending.attachment = BUFFER_COMMITTED;
			wl_list_remove(&surface->pending.attachment_link);
		}
		surface->committed.attachment = BUFFER_UNREACHABLE;
		wl_list_remove(&surface->committed.attachment_link);
	}

	if (wl_list_empty(&buffer->pending_surfaces)) {
		wl_buffer_destroy(buffer->buffer);
		free(buffer);
	} else {
		buffer->resource = NULL;
	}
}
static void nested_shm_pool_create_buffer(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		int32_t offset, int32_t width, int32_t height,
		int32_t stride, uint32_t format) {
	assert(wl_resource_instance_of(resource, &wl_shm_pool_interface, &shm_pool_impl));
	struct wl_shm_pool *shm_pool = wl_resource_get_user_data(resource);

	struct wl_resource *buf_resource = wl_resource_create(client, &wl_buffer_interface,
		wl_resource_get_version(resource), id);
	if (buf_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_buffer *buffer = calloc(1, sizeof(struct forward_buffer));
	if (!buffer) {
		wl_client_post_no_memory(client);
		return;
	}
	buffer->resource = buf_resource;
	wl_list_init(&buffer->pending_surfaces);
	wl_list_init(&buffer->committed_surfaces);

	buffer->buffer = wl_shm_pool_create_buffer(shm_pool,
		offset, width, height, stride, format);
	if (!buffer->buffer) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(buf_resource, &buffer_impl,
		buffer, buffer_handle_resource_destroy);

	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
}
static void nested_shm_pool_destroy(struct wl_client *client,
		 struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void nested_shm_pool_resize(struct wl_client *client,
		struct wl_resource *resource, int32_t size) {
	assert(wl_resource_instance_of(resource, &wl_shm_pool_interface, &shm_pool_impl));
	struct wl_shm_pool* shm_pool = wl_resource_get_user_data(resource);
	wl_shm_pool_resize(shm_pool, size);
}

static const struct wl_shm_pool_interface shm_pool_impl = {
	.create_buffer = nested_shm_pool_create_buffer,
	.destroy = nested_shm_pool_destroy,
	.resize = nested_shm_pool_resize,
};

static void shm_pool_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_shm_pool_interface, &shm_pool_impl));
	struct wl_shm_pool* shm_pool = wl_resource_get_user_data(resource);
	wl_shm_pool_destroy(shm_pool);
}
static void shm_create_pool(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, int32_t fd, int32_t size) {
	struct wl_resource *pool_resource = wl_resource_create(client, &wl_shm_pool_interface,
		wl_resource_get_version(resource), id);
	if (pool_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_state *server = wl_resource_get_user_data(resource);
	struct wl_shm *shm = server->shm;
	struct wl_shm_pool *shm_pool = wl_shm_create_pool(shm, fd, size);

	wl_resource_set_implementation(pool_resource, &shm_pool_impl,
		shm_pool, shm_pool_handle_resource_destroy);
}

static const struct wl_shm_interface shm_impl = {
	.create_pool = shm_create_pool
};

void bind_wl_shm(struct wl_client *client, void *data,
				    uint32_t version, uint32_t id) {
	struct wl_resource *resource =
			wl_resource_create(client, &wl_shm_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	struct forward_state *forward = data;
	for (size_t i = 0; i < forward->shm_formats_len; i++) {
		wl_shm_send_format(resource, forward->shm_formats[i]);
	}
	wl_resource_set_implementation(resource, &shm_impl, forward, NULL);
}


static void nested_dmabuf_params_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void nested_dmabuf_params_add(struct wl_client *client,
		struct wl_resource *resource, int32_t fd, uint32_t plane_idx,
		uint32_t offset, uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo) {
	assert(wl_resource_instance_of(resource, &zwp_linux_buffer_params_v1_interface, &linux_dmabuf_params_impl));
	struct zwp_linux_buffer_params_v1* params = wl_resource_get_user_data(resource);
	zwp_linux_buffer_params_v1_add(params, fd, plane_idx, offset, stride, modifier_hi, modifier_lo);
}
static void nested_dmabuf_params_create(struct wl_client *client,
		struct wl_resource *resource, int32_t width, int32_t height,
		uint32_t format, uint32_t flags) {
	wl_client_post_implementation_error(client, "TODO IMPLEMENT CREATE");
}
static void nested_dmabuf_params_create_immed(struct wl_client *client,
		struct wl_resource *resource, uint32_t buffer_id, int32_t width,
		int32_t height, uint32_t format, uint32_t flags) {
	struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface,
		wl_resource_get_version(resource), buffer_id);
	if (buffer_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct zwp_linux_buffer_params_v1 *params = wl_resource_get_user_data(resource);


	struct forward_buffer *buffer = calloc(1, sizeof(struct forward_buffer));
	if (!buffer) {
		wl_client_post_no_memory(client);
		return;
	}
	buffer->resource = buffer_resource;
	wl_list_init(&buffer->pending_surfaces);
	wl_list_init(&buffer->committed_surfaces);

	buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, format, flags);
	if (!buffer->buffer) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(buffer_resource, &buffer_impl,
		buffer, buffer_handle_resource_destroy);

	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
}


static const struct zwp_linux_buffer_params_v1_interface linux_dmabuf_params_impl = {
	.destroy = nested_dmabuf_params_destroy,
	.add = nested_dmabuf_params_add,
	.create = nested_dmabuf_params_create,
	.create_immed = nested_dmabuf_params_create_immed,
};

static void linux_dmabuf_params_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_linux_buffer_params_v1_interface, &linux_dmabuf_params_impl));
	struct zwp_linux_buffer_params_v1* params = wl_resource_get_user_data(resource);
	zwp_linux_buffer_params_v1_destroy(params);
}

static void nested_linux_dmabuf_destroy(struct wl_client *client,
		struct wl_resource *resource){
	wl_resource_destroy(resource);
}
static void nested_linux_dmabuf_create_params(struct wl_client *client,
		struct wl_resource *resource, uint32_t params_id) {
	struct wl_resource *params_resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
		wl_resource_get_version(resource), params_id);
	if (params_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	struct forward_state *forward = wl_resource_get_user_data(resource);
	struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(forward->linux_dmabuf);

	wl_resource_set_implementation(params_resource, &linux_dmabuf_params_impl,
		params, linux_dmabuf_params_handle_resource_destroy);
}

static void nested_dmabuf_feedback_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static const struct zwp_linux_dmabuf_feedback_v1_interface linux_dmabuf_feedback_v1_impl = {
	.destroy = nested_dmabuf_feedback_destroy,
};

static void linux_dmabuf_feedback_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

void send_dmabuf_feedback_data(struct wl_resource *feedback, const struct dmabuf_feedback_state *state) {
	assert(wl_resource_instance_of(feedback, &zwp_linux_dmabuf_feedback_v1_interface, &linux_dmabuf_feedback_v1_impl));

	struct wl_array main_device;
	main_device.data = (void*)&state->main_device;
	main_device.alloc = 0;
	main_device.size = sizeof(dev_t);
	zwp_linux_dmabuf_feedback_v1_send_main_device(feedback, &main_device);
	if (state->table_fd == -1) {
		swaylock_log(LOG_ERROR, "table fd was -1");
	}
	zwp_linux_dmabuf_feedback_v1_send_format_table(feedback, state->table_fd, state->table_fd_size);
	for (size_t i = 0; i < state->tranches_len; i++) {
		zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback, &state->tranches[i].indices);
		dev_t alt = {0};
		if (memcmp(&state->tranches[i].tranche_device, &alt, sizeof(dev_t))) {
			struct wl_array tranche_device;
			tranche_device.data = (void*)&state->tranches[i].tranche_device;
			tranche_device.alloc = 0;
			tranche_device.size = sizeof(dev_t);
			zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &tranche_device);
		}
		zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback, state->tranches[i].flags);
		zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);
	}
	zwp_linux_dmabuf_feedback_v1_send_done(feedback);
}

void nested_linux_dmabuf_get_default_feedback(struct wl_client *client,
			struct wl_resource *resource, uint32_t id) {
	struct wl_resource *feedback_resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
		wl_resource_get_version(resource), id);
	if (feedback_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	/* The linux-dmabuf protocol docs guarantee that the initial set of
	 * parameters will be provided _before_ the next roundtrip/wl_display_sync
	 * returns. This is hard to implement for a nested compositor.
	 *
	 * We have two ways to handle this:
	 * => Override wl_display.sync, to ensure it only returns after we run
	 *   a sync on the upstream. This is fairly awkward to do, because
	 *   wl_display has no 'get_implementation', and we thus can't override
	 *   just wl_display.sync without redoing the other display and
	 *   registry code. (This may be unavoidable if we ever would like for a
	 *   useful 'get_surface_feedback', that we can't easily emulate.)
	 * => Store all the `get_default_feedback` data received by upstream,
	 *   and replay its values downstream immediately. (This lets us treat
	 *   get_surface_feedback and get_default_feedback identically, and
	 *   gives lower latencies. We can optionally replace the update source
	 *   using get_surface_feedback.)
	 *
	 * Currently, the second option is used.
	 */

	struct forward_state *forward = wl_resource_get_user_data(resource);

	wl_resource_set_implementation(feedback_resource, &linux_dmabuf_feedback_v1_impl,
		NULL, linux_dmabuf_feedback_handle_resource_destroy);

	send_dmabuf_feedback_data(feedback_resource, &forward->current);

	/* register to listen to future changes */
	wl_list_insert(&forward->feedback_instances, wl_resource_get_link(feedback_resource));
}

void nested_linux_dmabuf_get_surface_feedback(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t id,
			     struct wl_resource *surface) {
	struct wl_resource *feedback_resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
		wl_resource_get_version(resource), id);
	if (feedback_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_state *forward = wl_resource_get_user_data(resource);

	wl_resource_set_implementation(feedback_resource, &linux_dmabuf_feedback_v1_impl,
		NULL, linux_dmabuf_feedback_handle_resource_destroy);

	send_dmabuf_feedback_data(feedback_resource, &forward->current);

	/* register to listen to future changes */
	wl_list_insert(&forward->feedback_instances, wl_resource_get_link(feedback_resource));

	/* alternative: instead of subscribing to general changes, ask for feedback from
	 * upstream.
	 *
	struct forward_surface *fwd_surface = wl_resource_get_user_data(surface);
	struct zwp_linux_dmabuf_feedback_v1 *feedback =
		zwp_linux_dmabuf_v1_get_surface_feedback(forward->linux_dmabuf, fwd_surface->upstream);
	*/

}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_impl = {
	.destroy = nested_linux_dmabuf_destroy,
	.create_params = nested_linux_dmabuf_create_params,
	.get_default_feedback = nested_linux_dmabuf_get_default_feedback,
	.get_surface_feedback = nested_linux_dmabuf_get_surface_feedback,
};

void bind_linux_dmabuf(struct wl_client *client, void *data,
				    uint32_t version, uint32_t id) {
	struct wl_resource *resource =
			wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_state *forward = data;
	if (version <= 3) {
		/* warning; this weakly relies on formats being in sorted order */
		uint32_t last_fmt = (uint32_t)-1;
		for (size_t i = 0; i < forward->dmabuf_formats_len; i++) {
			if (forward->dmabuf_formats[i].format != last_fmt) {
				zwp_linux_dmabuf_v1_send_format(resource, forward->dmabuf_formats[i].format);
			}

			last_fmt = forward->dmabuf_formats[i].format;
		}
	}
	if (version == 3) {
		for (size_t i = 0; i < forward->dmabuf_formats_len; i++) {
			zwp_linux_dmabuf_v1_send_modifier(resource, forward->dmabuf_formats[i].format,  forward->dmabuf_formats[i].modifier_lo,  forward->dmabuf_formats[i].modifier_hi);
		}
	}

	wl_resource_set_implementation(resource, &linux_dmabuf_impl, data, NULL);
}


static void nested_drm_authenticate(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	wl_client_post_implementation_error(client, "wl_drm.authenticate not supported");
}
static void nested_drm_create_buffer(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		uint32_t name, int32_t width,
		int32_t height, uint32_t stride, uint32_t format) {
	wl_client_post_implementation_error(client, "wl_drm.create_buffer not supported");
}
static void nested_drm_create_planar_buffer(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		uint32_t name, int32_t width, int32_t height,
		uint32_t format, int32_t offset0, int32_t stride0,
		int32_t offset1, int32_t stride1, int32_t offset2, int32_t stride2) {
	wl_client_post_implementation_error(client, "wl_drm.create_planar_buffer not supported");
}
static void nested_drm_create_prime_buffer(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, int32_t name,
		int32_t width, int32_t height, uint32_t format, int32_t offset0,
		int32_t stride0, int32_t offset1,int32_t stride1, int32_t offset2,
		int32_t stride2) {
	wl_client_post_implementation_error(client, "wl_drm.create_prime_buffer not supported");

}
static const struct wl_drm_interface wl_drm_impl = {
	.authenticate = nested_drm_authenticate,
	.create_buffer = nested_drm_create_buffer,
	.create_planar_buffer = nested_drm_create_planar_buffer,
	.create_prime_buffer = nested_drm_create_prime_buffer,
};
void bind_drm(struct wl_client *client, void *data,
				    uint32_t version, uint32_t id) {
	struct wl_resource *resource =
			wl_resource_create(client, &wl_drm_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	// TODO: look this up from the upstream copy
	wl_drm_send_device(resource, "/dev/dri/renderD128");
	wl_drm_send_capabilities(resource, 1);

	wl_resource_set_implementation(resource, &wl_drm_impl, data, NULL);
}


