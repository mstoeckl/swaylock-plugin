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
	struct wl_buffer* u_buffer = wl_resource_get_user_data(buffer);
	wl_surface_attach(surface->upstream, u_buffer, x, y);
}
static void nested_surface_damage(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y,
		int32_t width, int32_t height) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);
	wl_surface_damage(surface->upstream, x, y, width, height);
}

static void nested_frame_callback_done(void *data, struct wl_callback *wl_callback,
	     uint32_t callback_data) {
	struct wl_resource *callback_resource = data;
	// the data field here doesn't matter, so no point in forwarding it
	wl_callback_send_done(callback_resource, 0);
}
static const struct wl_callback_listener frame_callback_listener = {
	.done = nested_frame_callback_done,
};
static void frame_callback_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_callback_interface, NULL));
	struct wl_callback* callback = wl_resource_get_user_data(resource);
	// delete the callback so that wl_callback.done event is not received
	// with stale void *data
	wl_callback_destroy(callback);
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

	struct forward_surface *surface = wl_resource_get_user_data(resource);
	struct wl_callback *u_callback = wl_surface_frame(surface->upstream);
	wl_resource_set_implementation(callback_resource, NULL,
		u_callback, frame_callback_handle_resource_destroy);
	wl_callback_add_listener(u_callback, &frame_callback_listener, callback_resource);

}
static void nested_surface_set_opaque_region(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *region) {
	// do nothing, swaylock doesn't need to know about regions
}
static void nested_surface_set_input_region(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *region) {
	// do nothing, swaylock doesn't need to know about regions
}
static void nested_surface_commit(struct wl_client *client,
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);

	if (!surface->sway_surface) {
		/* todo: is a role required? if not, should only ignore in this case */
		wl_client_post_implementation_error(client, "tried to commit a surface without a role");
		return;
	}

	/* commit the root surface to which this is a subsurface.
	 * The two surfaces are synchronized, so this should apply changes
	 * to the current surface */
	wl_surface_commit(surface->upstream); // note: may be able to skip this, and instead trigger the local surface's callbacks when the sway_surface's frame callback is triggered
	wl_surface_commit(surface->sway_surface->surface);

	if (!surface->has_been_configured) {
		/* send initial configure */
		uint32_t serial = wl_display_next_serial(wl_client_get_display(client));

		if (surface->sway_surface->width == 0 || surface->sway_surface->height == 0) {
			swaylock_log(LOG_ERROR, "committing nested surface before main surface dimensions known");
		}

		zwlr_layer_surface_v1_send_configure(surface->layer_surface, serial,
			surface->sway_surface->width, surface->sway_surface->height);

		surface->has_been_configured = true;


		// todo: handle unmap/remap logic -- is it an error to attach a buffer
		// after unmapping?
	}
}
static void nested_surface_set_buffer_transform(struct wl_client *client,
		struct wl_resource *resource, int32_t transform) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);
	wl_surface_set_buffer_transform(surface->upstream, transform);
}
static void nested_surface_set_buffer_scale(struct wl_client *client,
		struct wl_resource *resource, int32_t scale) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);
	wl_surface_set_buffer_scale(surface->upstream, scale);
}
static void nested_surface_damage_buffer(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y,
		int32_t width, int32_t height) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);
	wl_surface_damage_buffer(surface->upstream, x, y, width, height);
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
	wl_surface_destroy(fwd_surface->upstream);
	/* todo: proper cleanup */
	if (fwd_surface->sway_surface) {
		fwd_surface->sway_surface->plugin_child = NULL;
	}
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

	struct forward_state *server = wl_resource_get_user_data(resource);
	struct wl_compositor *compositor = server->compositor;

	fwd_surface->upstream = wl_compositor_create_surface(compositor);

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
	struct wl_resource *resource = data;
	wl_buffer_send_release(resource);
}
static const struct wl_buffer_interface buffer_impl = {
	.destroy = nested_buffer_destroy,
};
static const struct wl_buffer_listener buffer_listener = {
	.release = handle_buffer_release
};
static void buffer_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_buffer_interface, &buffer_impl));
	struct wl_buffer* buffer = wl_resource_get_user_data(resource);
	wl_buffer_destroy(buffer);
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

	struct wl_buffer *buffer = wl_shm_pool_create_buffer(shm_pool,
		offset, width, height, stride, format);

	wl_resource_set_implementation(buf_resource, &buffer_impl,
		buffer, buffer_handle_resource_destroy);

	wl_buffer_add_listener(buffer, &buffer_listener, buf_resource);
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
	       struct wl_resource *resource,
	       int32_t width,
	       int32_t height,
	       uint32_t format,
	       uint32_t flags) {
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
	struct wl_buffer *buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, format, flags);

	wl_resource_set_implementation(buffer_resource, &buffer_impl,
		buffer, buffer_handle_resource_destroy);

	wl_buffer_add_listener(buffer, &buffer_listener, buffer_resource);
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
	// ignore, or forward?
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


