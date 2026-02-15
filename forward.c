#include "swaylock.h"

#include "log.h"
#include "loop.h"

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <stdlib.h>
#include <assert.h>

#include "color-management-v1-server-protocol.h"
#include "color-representation-v1-server-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "wayland-drm-server-protocol.h"
#include "wlr-layer-shell-unstable-v1-server-protocol.h"


static const struct wl_surface_interface surface_impl;
static const struct wl_buffer_interface buffer_impl;
static const struct wl_shm_pool_interface shm_pool_impl;
static const struct wl_compositor_interface compositor_impl;
static const struct zwp_linux_buffer_params_v1_interface linux_dmabuf_params_impl;
static const struct zwp_linux_dmabuf_feedback_v1_interface linux_dmabuf_feedback_v1_impl;
static const struct wp_viewport_interface viewport_impl;
static const struct wp_fractional_scale_v1_interface fractional_scale_impl;
static const struct wp_image_description_creator_icc_v1_interface desc_creator_icc_impl;
static const struct wp_image_description_creator_params_v1_interface desc_creator_params_impl;
static const struct wp_color_management_surface_v1_interface color_surface_impl;
static const struct wp_image_description_v1_interface image_desc_impl;
static const struct wp_color_representation_surface_v1_interface color_rep_surface_impl;
static void delete_image_desc_if_unreferenced(struct forward_image_desc* desc);

struct forward_params {
	struct zwp_linux_buffer_params_v1* params;
	struct wl_resource *resource;
	int32_t width;
	int32_t height;
};

static bool does_transform_transpose_size(int32_t transform) {
	switch (transform) {
	default:
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		return false;
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		return false;
	}
}

static void nested_surface_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	// this will also destroy the user_data.
	wl_resource_destroy(resource);
}
static void nested_surface_attach(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *buffer, int32_t x, int32_t y) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);
	struct forward_buffer *f_buffer = NULL;
	if (buffer) {
		assert(wl_resource_instance_of(buffer, &wl_buffer_interface, &buffer_impl));
		f_buffer = wl_resource_get_user_data(buffer);
	}

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

	struct damage_record *new_damage = realloc(surface->old_damage, sizeof(struct damage_record) * (surface->old_damage_len + 1));
	if (new_damage) {
		surface->old_damage = new_damage;
		surface->old_damage[surface->old_damage_len].x = x;
		surface->old_damage[surface->old_damage_len].y = y;
		surface->old_damage[surface->old_damage_len].w = width;
		surface->old_damage[surface->old_damage_len].h = height;
		surface->old_damage_len++;
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

void add_serial_pair(struct forward_surface *surf, uint32_t upstream_serial,
		uint32_t downstream_serial, uint32_t width, uint32_t height, bool local_only) {
	surf->serial_table = realloc(surf->serial_table, sizeof(struct serial_pair) * (surf->serial_table_len + 1));
	assert(surf->serial_table);

	surf->serial_table[surf->serial_table_len] = (struct serial_pair) {
		.plugin_serial = downstream_serial,
		.upstream_serial = upstream_serial,
		.config_width = width,
		.config_height = height,
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
		/* Clients can create and commit to any number of wl_surfaces; however,
		 * these have no impact until the surface is given a role. Ignore these
		 * commits. */
		return;
	}

	if (!surface->has_been_configured) {
		/* send initial configure */
		struct swaylock_bg_client *bg_client = surface->sway_surface->client ?
			surface->sway_surface->client : surface->sway_surface->state->server.main_client;
		uint32_t plugin_serial = bg_client->serial++;

		uint32_t config_width = surface->sway_surface->width, config_height = surface->sway_surface->height;
		if (config_width == 0 || config_height == 0) {
			swaylock_log(LOG_ERROR, "committing nested surface before main surface dimensions known");
		}

		// When committing a plugin surface for the first time,
		// if the upstream surface is also new, then forward the configure;
		// but if the upsteam surface was configured long ago, then
		// keep the configure local
		if (!surface->sway_surface->used_first_configure) {
			add_serial_pair(surface, surface->sway_surface->first_configure_serial, plugin_serial,
				config_width, config_height, false);
			surface->sway_surface->used_first_configure = true;
		} else if (surface->sway_surface->has_newer_serial) {
			/* In this case, the swaylock surface has received
			 * unacknowledged configures that the previous client
			 * for the surface did not acknowledge. Since we are
			 * giving this client an up to date size, acknowledge
			 * the corresponding configure when the client finally
			 * responds. */
			add_serial_pair(surface, surface->sway_surface->newest_serial, plugin_serial,
				config_width, config_height, false);
		} else {
			/* Swallow plugin's configure event -- all upstream configures
			 * were acknowledged by past clients */
			add_serial_pair(surface, 0, plugin_serial, config_width, config_height, true);
		}
		zwlr_layer_surface_v1_send_configure(surface->layer_surface, plugin_serial,
			config_width, config_height);

		surface->has_been_configured = true;

		// todo: handle unmap/remap logic -- is it an error to attach a buffer
		// after unmapping?
		assert(!surface->pending.attachment);
		/* The first commit should not be forwarded, because the main swaylock
		 * process already made such a commit in order to receive its
		 * own configure event. Thus, return here. */
		return;
	}

	if (surface->committed.attachment == NULL && surface->pending.attachment == NULL) {
		/* In this scenario, no buffer has been attached yet; there is no point in making
		 * a second or further commit without a buffer, so don't bother committing anything.
		 * (Note: other than the buffer, the surface state has nothing that risks dangling
		 * if it neglects to commit, and there is no attached buffer.) */
		return;
	}
	if (surface->committed.attachment != NULL && surface->pending.attachment == NULL) {
		/* Good wallpaper clients should never unmap their surfaces. Kill it. */
		wl_resource_post_error(resource, 1000, "The wallpaper program should not unmap any layer shell surface");
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
	if (surface->committed.viewport_dest_width != surface->pending.viewport_dest_width ||
			surface->committed.viewport_dest_height != surface->pending.viewport_dest_height) {
		assert(sw_surf->viewport);
		wp_viewport_set_destination(sw_surf->viewport, surface->pending.viewport_dest_width,
			surface->pending.viewport_dest_height);
		surface->committed.viewport_dest_width = surface->pending.viewport_dest_width;
		surface->committed.viewport_dest_height = surface->pending.viewport_dest_height;
	}
	if (surface->committed.viewport_source_x != surface->pending.viewport_source_x ||
			surface->committed.viewport_source_y != surface->pending.viewport_source_y ||
			surface->committed.viewport_source_w != surface->pending.viewport_source_w ||
			surface->committed.viewport_source_h != surface->pending.viewport_source_h) {
		assert(sw_surf->viewport);
		wp_viewport_set_source(sw_surf->viewport,
			surface->committed.viewport_source_x, surface->committed.viewport_source_y,
			surface->committed.viewport_source_w, surface->committed.viewport_source_h);
		surface->committed.viewport_source_x = surface->pending.viewport_source_x;
		surface->committed.viewport_source_y = surface->pending.viewport_source_y;
		surface->committed.viewport_source_w = surface->pending.viewport_source_w;
		surface->committed.viewport_source_h = surface->pending.viewport_source_h;
	}

	if (surface->committed.has_alpha_mode != surface->pending.has_alpha_mode ||
			surface->committed.alpha_mode != surface->pending.alpha_mode ||
			surface->committed.has_chroma_location != surface->pending.has_chroma_location ||
			surface->committed.chroma_location != surface->pending.chroma_location ||
			surface->committed.has_coef_range != surface->pending.has_coef_range ||
			surface->committed.coefficients != surface->pending.coefficients ||
			surface->committed.range != surface->pending.range) {
		assert(sw_surf->color_rep_surface);
		// There is no way to reset color representation parameters to default
		// other than unsetting and recreating the surface. To simplify the logic,
		// recreate the color rep surface on every change.
		wp_color_representation_surface_v1_destroy(sw_surf->color_rep_surface);
		sw_surf->color_rep_surface = wp_color_representation_manager_v1_get_surface(
			sw_surf->state->forward.color_representation, sw_surf->surface);
		if (surface->pending.has_alpha_mode) {
			wp_color_representation_surface_v1_set_alpha_mode(
				sw_surf->color_rep_surface, surface->pending.alpha_mode);
		}
		if (surface->pending.has_chroma_location) {
			wp_color_representation_surface_v1_set_chroma_location(
				sw_surf->color_rep_surface, surface->pending.chroma_location);
		}
		if (surface->pending.has_coef_range) {
			wp_color_representation_surface_v1_set_coefficients_and_range(
				sw_surf->color_rep_surface, surface->pending.coefficients,
				surface->pending.range);
		}
		surface->committed.has_alpha_mode = surface->pending.has_alpha_mode;
		surface->committed.alpha_mode = surface->pending.alpha_mode;
		surface->committed.has_chroma_location = surface->pending.has_chroma_location;
		surface->committed.chroma_location = surface->pending.chroma_location;
		surface->committed.has_coef_range = surface->pending.has_coef_range;
		surface->committed.coefficients = surface->pending.coefficients;
		surface->committed.range = surface->pending.range;
	}

	if (surface->committed.image_desc != surface->pending.image_desc ||
		surface->committed.render_intent != surface->pending.render_intent) {
		assert(sw_surf->color_surface);
		if (!surface->pending.image_desc) {
			wp_color_management_surface_v1_unset_image_description(
				sw_surf->color_surface);
		} else {
			wp_color_management_surface_v1_set_image_description(
				sw_surf->color_surface, surface->pending.image_desc->description,
				surface->pending.render_intent);
		}
		if (surface->committed.image_desc != surface->pending.image_desc) {
			if (surface->committed.image_desc) {
				wl_list_remove(&surface->committed.image_desc_link);
				delete_image_desc_if_unreferenced(surface->committed.image_desc);
			}
			if (surface->pending.image_desc) {
				surface->committed.image_desc = surface->pending.image_desc;
				wl_list_insert(&surface->pending.image_desc->committed_surfaces,
					&surface->committed.image_desc_link);
			} else {
				surface->committed.image_desc = NULL;
				wl_list_init(&surface->committed.image_desc_link);
			}
		}
		surface->committed.render_intent = surface->pending.render_intent;
	}

	// The protocol does not make this fully explicit, but the buffer should
	// be attached _each time_ that any damage is sent alongside it, even if
	// the buffer is the same. This is also necessary to ensure that the
	// appropriate release events are sent
	if (surface->pending.attachment != BUFFER_COMMITTED) {
		/* unlink the committed attachment */
		if (surface->committed.attachment != NULL && surface->committed.attachment != BUFFER_UNREACHABLE) {
			assert(surface->committed.attachment->resource != NULL);
			wl_list_remove(&surface->committed.attachment_link);
		}

		/* See above: null attachments are either bad wallpaper program behavior or need no commit */
		assert(surface->pending.attachment != NULL);

		struct forward_buffer *upstream_buffer = surface->pending.attachment;
		int32_t offset_x =  wl_resource_get_version(resource) >= 5 ? 0 : surface->pending.offset_x;
		int32_t offset_y =  wl_resource_get_version(resource) >= 5 ? 0 : surface->pending.offset_y;
		wl_surface_attach(background,
			upstream_buffer ? upstream_buffer->buffer : NULL,
			offset_x, offset_y);
		if (wl_resource_get_version(resource) < 5) {
			surface->committed.offset_x = surface->pending.offset_x;
			surface->committed.offset_y = surface->pending.offset_y;
		}
		surface->committed.attachment = surface->pending.attachment;

		surface->committed_buffer_width = upstream_buffer->width;
		surface->committed_buffer_height = upstream_buffer->height;
		wl_list_insert(&upstream_buffer->committed_surfaces, &surface->committed.attachment_link);
	}

	wl_fixed_t n = wl_fixed_from_int(-1);
	bool viewport_dst_on = surface->committed.viewport_dest_width != -1;
	bool viewport_src_on = surface->committed.viewport_source_w != n;
	uint32_t output_width, output_height;
	if (viewport_dst_on) {
		output_width = surface->committed.viewport_dest_width;
		output_height = surface->committed.viewport_dest_height;
	} else if (viewport_src_on) {
		output_width = wl_fixed_to_int(surface->committed.viewport_source_w);
		output_height = wl_fixed_to_int(surface->committed.viewport_source_h);
		if (wl_fixed_from_int(output_width) != surface->committed.viewport_source_w ||
			wl_fixed_from_int(output_height) != surface->committed.viewport_source_h) {
			wl_resource_post_error(surface->viewport, WP_VIEWPORT_ERROR_BAD_SIZE, "width/height not integral");
			return;
		}
		// TODO: technically, should also validate that the viewport dimensions fall inside the
		// (transformed) buffer bounding box
	} else {
		if (surface->committed_buffer_width % surface->committed.buffer_scale != 0 ||
			surface->committed_buffer_height % surface->committed.buffer_scale != 0) {
			wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SIZE, "buffer dimensions not divisible by scale");
			return;
		}
		output_width = surface->committed_buffer_width / surface->committed.buffer_scale;
		output_height = surface->committed_buffer_height / surface->committed.buffer_scale;
		if (does_transform_transpose_size(surface->committed.buffer_transform)) {
			uint32_t tmp = output_width;
			output_width = output_height;
			output_height = tmp;
		}
	}
	if (output_width != surface->last_acked_width || output_height != surface->last_acked_height) {
		swaylock_log(LOG_ERROR, "Wallpaper program committed surface at size %d x %d, which does not exactly match last acknowledged W x H = %d x %d",
			output_width, output_height, surface->last_acked_width, surface->last_acked_height);
		wl_resource_post_error(resource, 1000, "The wallpaper program should exactly match the configure width/height");
		return;
	}

	// TODO: verify that on scale or attachment change, the resulting size exactly matches the output

	/* If there was an offset change, but no buffer value change */
	if ((surface->committed.offset_x != surface->pending.offset_x ||
			surface->committed.offset_y != surface->pending.offset_y) && wl_resource_get_version(resource) >= 5) {
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
		if (sw_surf->pending_upstream_serial == sw_surf->newest_serial) {
			sw_surf->has_newer_serial = false;
		}
	}

	if (sw_surf->client_submission_timer) {
		/* Disarm timer, indicating that plugin have responded on time
		 * for this output. */
		loop_remove_timer(sw_surf->state->eventloop, sw_surf->client_submission_timer);
		sw_surf->client_submission_timer = NULL;
	}

	wl_surface_commit(background);
}

static void nested_surface_set_buffer_transform(struct wl_client *client,
		struct wl_resource *resource, int32_t transform) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
	struct forward_surface *surface = wl_resource_get_user_data(resource);
	surface->pending.buffer_transform = transform;
	// TODO: validate that the transform is valid;
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

	if (fwd_surface->pending.image_desc) {
		wl_list_remove(&fwd_surface->pending.image_desc_link);
		delete_image_desc_if_unreferenced(fwd_surface->pending.image_desc);
	}
	if (fwd_surface->committed.image_desc) {
		wl_list_remove(&fwd_surface->committed.image_desc_link);
		delete_image_desc_if_unreferenced(fwd_surface->committed.image_desc);
	}

	free(fwd_surface->buffer_damage);
	free(fwd_surface->old_damage);
	free(fwd_surface->serial_table);

	if (fwd_surface->viewport) {
		wl_resource_set_user_data(fwd_surface->viewport, NULL);
	}
	if (fwd_surface->fractional_scale) {
		wl_resource_set_user_data(fwd_surface->fractional_scale, NULL);
	}
	if (fwd_surface->color_surface) {
		wl_resource_set_user_data(fwd_surface->color_surface, NULL);
	}
	if (fwd_surface->color_representation) {
		wl_resource_set_user_data(fwd_surface->color_representation, NULL);
	}

	free(fwd_surface);
}

static void default_surface_state(struct surface_state *state) {
	wl_fixed_t n = wl_fixed_from_int(-1);
	state->viewport_dest_height = -1;
	state->viewport_dest_width = -1;
	state->viewport_source_x = n;
	state->viewport_source_y = n;
	state->viewport_source_w = n;
	state->viewport_source_h = n;
	state->buffer_scale = 1;
	state->buffer_transform = WL_OUTPUT_TRANSFORM_NORMAL;
	state->offset_x = 0;
	state->offset_y = 0;
	state->attachment = NULL;
	// state->attachment_link is only used when attachment is not NULL
}

static void compositor_create_surface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	assert(wl_resource_instance_of(resource, &wl_compositor_interface, &compositor_impl));
	struct forward_state *state = wl_resource_get_user_data(resource);

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
	fwd_surface->state = state;
	wl_list_init(&fwd_surface->frame_callbacks);
	default_surface_state(&fwd_surface->pending);
	default_surface_state(&fwd_surface->committed);

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
	buffer->width = width;
	buffer->height = height;

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
		close(fd);
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_state *server = wl_resource_get_user_data(resource);
	struct wl_shm *shm = server->shm;
	struct wl_shm_pool *shm_pool = wl_shm_create_pool(shm, fd, size);
	close(fd);

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
	struct forward_params* params = wl_resource_get_user_data(resource);
	zwp_linux_buffer_params_v1_add(params->params, fd, plane_idx, offset, stride, modifier_hi, modifier_lo);
	close(fd);
}
static void nested_dmabuf_params_create(struct wl_client *client,
		struct wl_resource *resource, int32_t width, int32_t height,
		uint32_t format, uint32_t flags) {
	assert(wl_resource_instance_of(resource, &zwp_linux_buffer_params_v1_interface, &linux_dmabuf_params_impl));
	struct forward_params *params = wl_resource_get_user_data(resource);
	params->width = width;
	params->height = height;
	zwp_linux_buffer_params_v1_create(params->params, width, height, format, flags);
}
static struct forward_buffer *make_buffer(int width, int height) {
	struct forward_buffer *buffer = calloc(1, sizeof(struct forward_buffer));
	if (!buffer) {
		return NULL;
	}
	wl_list_init(&buffer->pending_surfaces);
	wl_list_init(&buffer->committed_surfaces);
	buffer->width = width;
	buffer->height = height;
	return buffer;
}
static void nested_dmabuf_params_create_immed(struct wl_client *client,
		struct wl_resource *resource, uint32_t buffer_id, int32_t width,
		int32_t height, uint32_t format, uint32_t flags) {
	assert(wl_resource_instance_of(resource, &zwp_linux_buffer_params_v1_interface, &linux_dmabuf_params_impl));
	struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface,
		wl_resource_get_version(resource), buffer_id);
	if (buffer_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_params *params = wl_resource_get_user_data(resource);

	struct forward_buffer *buffer = make_buffer(width, height);
	if (!buffer) {
		wl_client_post_no_memory(client);
		return;
	}
	buffer->resource = buffer_resource;
	buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params->params, width, height, format, flags);
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
	struct forward_params* params = wl_resource_get_user_data(resource);
	zwp_linux_buffer_params_v1_destroy(params->params);
	free(params);
}

static void nested_linux_dmabuf_destroy(struct wl_client *client,
		struct wl_resource *resource){
	wl_resource_destroy(resource);
}

void handle_dmabuf_params_created(void *data,
		struct zwp_linux_buffer_params_v1 *zwp_linux_buffer_params_v1,
		struct wl_buffer *wl_buffer) {
	struct forward_params *params = data;

	struct wl_client *client = wl_resource_get_client(params->resource);
	struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface,
		wl_resource_get_version(params->resource), 0);
	if (buffer_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_buffer *buffer = make_buffer(params->width, params->height);
	if (!buffer) {
		wl_client_post_no_memory(client);
		return;
	}
	buffer->resource = buffer_resource;
	buffer->buffer = wl_buffer;
	wl_resource_set_implementation(buffer_resource, &buffer_impl,
		buffer, buffer_handle_resource_destroy);
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
	zwp_linux_buffer_params_v1_send_created(params->resource, buffer_resource);
}

static void handle_dmabuf_params_failed(void *data,
		struct zwp_linux_buffer_params_v1 *zwp_linux_buffer_params_v1)  {
	struct forward_params *params = data;
	zwp_linux_buffer_params_v1_send_failed(params->resource);
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
	.created = handle_dmabuf_params_created,
	.failed = handle_dmabuf_params_failed,
};

static void nested_linux_dmabuf_create_params(struct wl_client *client,
		struct wl_resource *resource, uint32_t params_id) {
	struct forward_params *params = calloc(1, sizeof(*params));
	if (!params) {
		wl_client_post_no_memory(client);
		return;
	}

	struct wl_resource *params_resource = wl_resource_create(client,
		&zwp_linux_buffer_params_v1_interface,
		wl_resource_get_version(resource), params_id);
	if (params_resource == NULL) {
		free(params);
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_state *forward = wl_resource_get_user_data(resource);
	params->resource = params_resource;
	params->params = zwp_linux_dmabuf_v1_create_params(forward->linux_dmabuf);
	params->width = 0;
	params->height = 0;
	zwp_linux_buffer_params_v1_add_listener(params->params, &params_listener,
		params);
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
		struct wl_array tranche_device;
		tranche_device.data = (void*)&state->tranches[i].tranche_device;
		tranche_device.alloc = 0;
		tranche_device.size = sizeof(dev_t);
		zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &tranche_device);
		zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback, state->tranches[i].flags);
		zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback, &state->tranches[i].indices);
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

static void viewport_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_viewport_interface, &viewport_impl));
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if (fwd_surface) {
		fwd_surface->viewport = NULL;
	}
}

static void nested_viewport_destroy(struct wl_client *client,
	struct wl_resource *resource) {
	/* `viewport_handle_resource_destroy` will be invoked */
	wl_resource_destroy(resource);
}

static void nested_viewport_set_source(struct wl_client *client, struct wl_resource *resource,
		wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height) {
	assert(wl_resource_instance_of(resource, &wp_viewport_interface, &viewport_impl));
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	wl_fixed_t n = wl_fixed_from_int(-1);
	bool no_source = x == n && y == n && width == n && height == n;
	if ((x < 0 || y < 0 || width <= 0 || height <= 0) && !no_source) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid x/y/width/height for set_source");
	} else {
		fwd_surface->pending.viewport_source_x = x;
		fwd_surface->pending.viewport_source_y = y;
		fwd_surface->pending.viewport_source_w = width;
		fwd_surface->pending.viewport_source_h = height;
	}
}

static void nested_viewport_set_destination(struct wl_client *client, struct wl_resource *resource,
		int32_t width, int32_t height) {
	assert(wl_resource_instance_of(resource, &wp_viewport_interface, &viewport_impl));
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if ((width <= 0 || height <= 0) && !(width == -1 && height == -1)) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid width/height pair for set_destination");
	} else {
		fwd_surface->pending.viewport_dest_width = width;
		fwd_surface->pending.viewport_dest_height = height;
	}
}

static const struct wp_viewport_interface viewport_impl = {
	.destroy = nested_viewport_destroy,
	.set_source = nested_viewport_set_source,
	.set_destination = nested_viewport_set_destination,
};

static void nested_viewporter_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void nested_viewporter_get_viewport(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface) {
	struct forward_surface *forward_surf = wl_resource_get_user_data(surface);
	/* Each surface has at most one wp_viewport associated */
	if (forward_surf->viewport) {
		wl_resource_post_error(resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS, "viewport already exists");
	}

	struct wl_resource *viewport_resource = wl_resource_create(client, &wp_viewport_interface,
		wl_resource_get_version(resource), id);
	if (viewport_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	forward_surf->viewport = viewport_resource;
	wl_resource_set_implementation(viewport_resource, &viewport_impl,
		forward_surf, viewport_handle_resource_destroy);
}

static const struct wp_viewporter_interface viewporter_impl = {
	.destroy = nested_viewporter_destroy,
	.get_viewport = nested_viewporter_get_viewport,
};

void bind_viewporter(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wp_viewporter_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	struct forward_state *forward = data;
	wl_resource_set_implementation(resource, &viewporter_impl, forward, NULL);
}

static void fractional_scale_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_fractional_scale_v1_interface, &fractional_scale_impl));
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if (fwd_surface) {
		fwd_surface->fractional_scale = NULL;
	}
}
static void nested_fractional_scale_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	/* `fractional_scale_handle_resource_destroy` will be invoked */
	wl_resource_destroy(resource);
}
static const struct wp_fractional_scale_v1_interface fractional_scale_impl = {
	.destroy = nested_fractional_scale_destroy,
};

static void nested_fractional_scale_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	/* nothing to do */
}
static void nested_fractional_scale_manager_get_fractional_scale(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface) {
	struct forward_surface *forward_surf = wl_resource_get_user_data(surface);
	/* Each surface has at most one wp_fractional_scale associated */
	if (forward_surf->fractional_scale) {
		wl_resource_post_error(resource, WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS, "fractional scale already exists");
	}

	struct wl_resource *scale_resource = wl_resource_create(client, &wp_fractional_scale_v1_interface,
		wl_resource_get_version(resource), id);
	if (scale_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	forward_surf->fractional_scale = scale_resource;
	if (forward_surf->sway_surface && forward_surf->sway_surface->last_fractional_scale > 0) {
		wp_fractional_scale_v1_send_preferred_scale(scale_resource,
			forward_surf->sway_surface->last_fractional_scale);
	}
	wl_resource_set_implementation(scale_resource, &fractional_scale_impl,
		forward_surf, fractional_scale_handle_resource_destroy);
}

static const struct wp_fractional_scale_manager_v1_interface fractional_scale_manager_impl = {
	.destroy = nested_fractional_scale_manager_destroy,
	.get_fractional_scale = nested_fractional_scale_manager_get_fractional_scale,
};

void bind_fractional_scale(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wp_fractional_scale_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	struct forward_state *forward = data;
	wl_resource_set_implementation(resource, &fractional_scale_manager_impl, forward, NULL);
}


static void color_surface_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_color_management_surface_v1_interface, &color_surface_impl));
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if (fwd_surface) {
		assert(fwd_surface->color_surface == resource);
		fwd_surface->color_surface = NULL;
	}
}
static void nested_color_surface_unset_image_desc(struct wl_client *client,
		struct wl_resource *resource);
static void nested_color_surface_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	// Destroying the surface also unsets the image description
	nested_color_surface_unset_image_desc(client, resource);
	wl_resource_destroy(resource);
}
static void nested_color_surface_set_image_desc(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *image_description,
		uint32_t render_intent) {
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if (!fwd_surface) {
		return;
	}
	if (fwd_surface->pending.image_desc) {
		wl_list_remove(&fwd_surface->pending.image_desc_link);
		delete_image_desc_if_unreferenced(fwd_surface->pending.image_desc);
	}
	struct forward_image_desc *fwd_desc = wl_resource_get_user_data(image_description);
	fwd_surface->pending.render_intent = render_intent;
	fwd_surface->pending.image_desc = fwd_desc;
	wl_list_insert(&fwd_desc->pending_surfaces, &fwd_surface->pending.image_desc_link);
}
static void nested_color_surface_unset_image_desc(struct wl_client *client,
		struct wl_resource *resource) {
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if (!fwd_surface) {
		return;
	}
	if (fwd_surface->pending.image_desc) {
		wl_list_remove(&fwd_surface->pending.image_desc_link);
		delete_image_desc_if_unreferenced(fwd_surface->pending.image_desc);
	}
	fwd_surface->pending.render_intent = 0;
	fwd_surface->pending.image_desc = NULL;
	wl_list_init(&fwd_surface->pending.image_desc_link);
}
static const struct wp_color_management_surface_v1_interface color_surface_impl = {
	.destroy = nested_color_surface_destroy,
	.set_image_description = nested_color_surface_set_image_desc,
	.unset_image_description =nested_color_surface_unset_image_desc,
};





static void delete_image_desc_if_unreferenced(struct forward_image_desc* desc) {
	if (desc->resource) {
		// Client still can refer to object
		return;
	}
	if (!wl_list_empty(&desc->committed_surfaces) || !wl_list_empty(&desc->pending_surfaces)) {
		return;
	}
	assert(desc->description);
	if (desc->properties) {
		unref_image_description_props(desc->properties);
	} else {
		wp_image_description_v1_destroy(desc->description);
	}
	desc->description = NULL;
	free(desc);
}
static void image_desc_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_image_description_v1_interface, &image_desc_impl));
	struct forward_image_desc* fwd_desc =
		wl_resource_get_user_data(resource);
	assert(fwd_desc->resource == resource);
	fwd_desc->resource = NULL;
	delete_image_desc_if_unreferenced(fwd_desc);
}
void nested_image_desc_destroy(struct wl_client *client,
			struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
void nested_image_desc_get_information(struct wl_client *client,
		struct wl_resource *resource, uint32_t information) {
	struct forward_image_desc* fwd_desc =
		wl_resource_get_user_data(resource);
	if (fwd_desc->properties) {
		struct wl_resource *info = wl_resource_create(client,
			&wp_image_description_info_v1_interface, wl_resource_get_version(resource),
			information);
		if (info == NULL) {
			wl_client_post_no_memory(client);
			return;
		}

		struct image_description_properties *s = fwd_desc->properties;
		if (s->icc_profile >= 0) {
			wp_image_description_info_v1_send_icc_file(info,
				s->icc_profile, s->icc_profile_len);
		}

		if (s->has_tf) {
			wp_image_description_info_v1_send_tf_named(info, s->tf);
		}
		if (s->has_eexp) {
			wp_image_description_info_v1_send_tf_power(info, s->eexp);
		}
		if (s->has_primaries_named) {
			wp_image_description_info_v1_send_primaries_named(info, s->primaries);
		}
		if (s->has_primaries) {
			wp_image_description_info_v1_send_primaries(info, s->prx, s->pry,
				s->pgx, s->pgy, s->pbx, s->pby, s->pwx, s->pwy);
		}
		if (s->has_luminances) {
			wp_image_description_info_v1_send_luminances(info,
				s->min_lum, s->max_lum, s->reference_lum);
		}
		if (s->has_mastering_display_primaries) {
			wp_image_description_info_v1_send_target_primaries(info,
				s->mrx, s->mry, s->mgx, s->mgy, s->mbx, s->mby, s->mwx, s->mwy);
		}
		if (s->has_mastering_luminance) {
			wp_image_description_info_v1_send_target_luminance(info,
				s->mastering_min_lum, s->mastering_max_lum);
		}
		if (s->has_max_cll) {
			wp_image_description_info_v1_send_target_max_cll(info, s->max_cll);
		}
		if (s->has_max_fall) {
			wp_image_description_info_v1_send_target_max_fall(info, s->max_fall);
		}

		wp_image_description_info_v1_send_done(info);
		wl_resource_destroy(info);
	} else {
		// Client created image description objects do not support get_information
		wl_resource_post_error(resource, WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION,
			"client controlled image description");
	}
}
static const struct wp_image_description_v1_interface image_desc_impl = {
	.destroy = nested_image_desc_destroy,
	.get_information = nested_image_desc_get_information,
};

static void image_desc_handle_failed(void *data,
		struct wp_image_description_v1 *wp_image_description_v1, uint32_t cause,
		const char *msg) {
	struct forward_image_desc* fwd_desc = data;
	if (fwd_desc->resource) {
		wp_image_description_v1_send_failed(fwd_desc->resource, cause, msg);
	}
}
static void image_desc_handle_ready(void *data,
		struct wp_image_description_v1 *wp_image_description_v1, uint32_t identity) {
	// ID numbers are server allocated, and swaylock's image descriptions
	// outlive the plugin clients's, so these will remain unique even if there
	// are multiple clients or clients are replaced
	struct forward_image_desc* fwd_desc = data;
	if (fwd_desc->resource) {
		wp_image_description_v1_send_ready(fwd_desc->resource, identity);
	}
}
uint32_t color_identity_v2_to_v1(uint32_t identity_hi, uint32_t identity_lo) {
	// In practice, v2 identities are densely packed sequential IDs and
	// are extremely unlikely to wrap around a u32. In the unlikely event that
	// both a) a compositor doesn't use this allocation method b) a client
	// cares, it is possible to maintain a map from u64 ids to the
	// associated live image descriptions and current preferred ids of
	// the upstream color surfaces.
	return identity_lo;
}
static void image_desc_handle_ready2(void *data,
		struct wp_image_description_v1 *wp_image_description_v1, uint32_t identity_hi,
		uint32_t identity_lo) {
	struct forward_image_desc* fwd_desc = data;
	if (fwd_desc->resource) {
		if (wl_resource_get_version(fwd_desc->resource) >= 2) {
			wp_image_description_v1_send_ready2(fwd_desc->resource, identity_hi, identity_lo);
		} else {
			wp_image_description_v1_send_ready(fwd_desc->resource,
				color_identity_v2_to_v1(identity_hi, identity_lo));
		}
	}
}
const struct wp_image_description_v1_listener image_desc_listener = {
	.failed = image_desc_handle_failed,
	.ready = image_desc_handle_ready,
	.ready2 = image_desc_handle_ready2,
};

static void create_forward_image_desc(
		struct wl_resource *parent, struct wp_image_description_v1 *desc, uint32_t desc_id) {
	struct wl_client *client = wl_resource_get_client(parent);
	struct wl_resource *desc_resource = wl_resource_create(client,
		&wp_image_description_v1_interface, wl_resource_get_version(parent),
		desc_id);
	if (desc_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_image_desc *fwd_desc = calloc(1, sizeof(*fwd_desc));
	if (!fwd_desc) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&fwd_desc->committed_surfaces);
	wl_list_init(&fwd_desc->pending_surfaces);
	fwd_desc->resource = desc_resource;
	fwd_desc->description = desc;

	wp_image_description_v1_add_listener(fwd_desc->description,
		&image_desc_listener, fwd_desc);
	wl_resource_set_implementation(desc_resource, &image_desc_impl,
		fwd_desc, image_desc_handle_resource_destroy);
}
static void create_output_image_desc(
		struct wl_resource *parent, struct image_description_properties *state, uint32_t desc_id) {
	struct wl_client *client = wl_resource_get_client(parent);
	struct wl_resource *desc_resource = wl_resource_create(client,
		&wp_image_description_v1_interface, wl_resource_get_version(parent),
		desc_id);
	if (desc_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_image_desc *fwd_desc = calloc(1, sizeof(*fwd_desc));
	if (!fwd_desc) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&fwd_desc->committed_surfaces);
	wl_list_init(&fwd_desc->pending_surfaces);
	fwd_desc->resource = desc_resource;
	fwd_desc->description = state->description;
	fwd_desc->properties = state;
	state->reference_count++;

	wl_resource_set_implementation(desc_resource, &image_desc_impl,
		fwd_desc, image_desc_handle_resource_destroy);

	if (state->failed) {
		wp_image_description_v1_send_failed(desc_resource, state->failure_cause, state->failure_reason);
	} else if (wl_resource_get_version(parent) >= 2) {
		assert(wl_proxy_get_version((struct wl_proxy *)state->description) >= 2);
		wp_image_description_v1_send_ready2(desc_resource,
			state->color_identity_v2_hi, state->color_identity_v2_lo);
	} else {
		if (wl_proxy_get_version((struct wl_proxy *)state->description) >= 2) {
			wp_image_description_v1_send_ready(desc_resource,
				color_identity_v2_to_v1(state->color_identity_v2_hi,
					state->color_identity_v2_lo));
		} else {
			wp_image_description_v1_send_ready(desc_resource,
				state->color_identity_v1);
		}
	}
}

static void desc_creator_icc_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_image_description_creator_icc_v1_interface, &desc_creator_icc_impl));
	struct wp_image_description_creator_icc_v1* creator =
		wl_resource_get_user_data(resource);
	if (creator) {
		wp_image_description_creator_icc_v1_destroy(creator);
	}
}
static void nested_desc_creator_icc_create(struct wl_client *client,
		struct wl_resource *resource, uint32_t image_description) {
	struct wp_image_description_creator_icc_v1* creator =
		wl_resource_get_user_data(resource);
	create_forward_image_desc(resource,
		wp_image_description_creator_icc_v1_create(creator),
		image_description);
	// ::create destroys the image creator
	wl_resource_set_user_data(resource, NULL);
}
static void nested_desc_creator_icc_set_icc_file(struct wl_client *client,
		struct wl_resource *resource, int32_t icc_profile, uint32_t offset, uint32_t length) {
	struct wp_image_description_creator_icc_v1* creator =
		wl_resource_get_user_data(resource);
	assert(icc_profile >= 0);
	wp_image_description_creator_icc_v1_set_icc_file(creator, icc_profile, offset, length);
	close(icc_profile);
}
static const struct wp_image_description_creator_icc_v1_interface desc_creator_icc_impl = {
	.create = nested_desc_creator_icc_create,
	.set_icc_file = nested_desc_creator_icc_set_icc_file,
};

static void desc_creator_params_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_image_description_creator_params_v1_interface, &desc_creator_params_impl));
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	if (creator) {
		wp_image_description_creator_params_v1_destroy(creator);
	}
}
static void nested_desc_creator_params_create(struct wl_client *client,
		struct wl_resource *resource, uint32_t image_description) {
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	create_forward_image_desc(resource,
		wp_image_description_creator_params_v1_create(creator),
		image_description);
	// ::create destroys the image creator
	wl_resource_set_user_data(resource, NULL);
}
static void nested_desc_creator_params_set_tf_named(struct wl_client *client,
		struct wl_resource *resource, uint32_t tf) {
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	wp_image_description_creator_params_v1_set_tf_named(creator, tf);
}
static void nested_desc_creator_params_set_tf_power(struct wl_client *client,
		struct wl_resource *resource, uint32_t eexp) {
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	wp_image_description_creator_params_v1_set_tf_power(creator, eexp);
}
static void nested_desc_creator_params_set_primaries_named(struct wl_client *client,
		struct wl_resource *resource, uint32_t primaries) {
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	wp_image_description_creator_params_v1_set_primaries_named(creator, primaries);
}
static void nested_desc_creator_params_set_primaries(struct wl_client *client,
		struct wl_resource *resource, int32_t r_x, int32_t r_y, int32_t g_x,
		int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y) {
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	wp_image_description_creator_params_v1_set_primaries(creator,
		r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y);
}
static void nested_desc_creator_params_set_luminances(struct wl_client *client,
		struct wl_resource *resource, uint32_t min_lum, uint32_t max_lum,
		uint32_t reference_lum) {
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	wp_image_description_creator_params_v1_set_luminances(creator,
		min_lum, max_lum, reference_lum);
}
static void nested_desc_creator_params_set_mastering_display_primaries(struct wl_client *client,
		struct wl_resource *resource, int32_t r_x, int32_t r_y, int32_t g_x,
		int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y) {
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	wp_image_description_creator_params_v1_set_mastering_display_primaries(creator,
		r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y);
}
static void nested_desc_creator_params_set_mastering_luminance(struct wl_client *client,
		struct wl_resource *resource, uint32_t min_lum, uint32_t max_lum) {
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	wp_image_description_creator_params_v1_set_mastering_luminance(creator,
		min_lum, max_lum);

}
static void nested_desc_creator_params_set_max_cll(struct wl_client *client,
		struct wl_resource *resource, uint32_t max_cll) {
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	wp_image_description_creator_params_v1_set_max_cll(creator, max_cll);
}
static void nested_desc_creator_params_set_max_fall(struct wl_client *client,
		struct wl_resource *resource, uint32_t max_fall) {
	struct wp_image_description_creator_params_v1* creator =
		wl_resource_get_user_data(resource);
	wp_image_description_creator_params_v1_set_max_fall(creator, max_fall);

}
static const struct wp_image_description_creator_params_v1_interface desc_creator_params_impl = {
	.create = nested_desc_creator_params_create,
	.set_tf_named = nested_desc_creator_params_set_tf_named,
	.set_tf_power = nested_desc_creator_params_set_tf_power,
	.set_primaries_named = nested_desc_creator_params_set_primaries_named,
	.set_primaries = nested_desc_creator_params_set_primaries,
	.set_luminances = nested_desc_creator_params_set_luminances,
	.set_mastering_display_primaries = nested_desc_creator_params_set_mastering_display_primaries,
	.set_mastering_luminance = nested_desc_creator_params_set_mastering_luminance,
	.set_max_cll = nested_desc_creator_params_set_max_fall,
	.set_max_fall = nested_desc_creator_params_set_max_cll,
};


static void color_output_handle_resource_destroy(struct wl_resource *resource) {
	/* remove resource from swaylock_surface's list of them (or if that has
	 * been destroyed, from the stale list */
	wl_list_remove(wl_resource_get_link(resource));
}
static void nested_color_output_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void nested_color_output_get_image_description(struct wl_client *client,
		struct wl_resource *resource, uint32_t image_description) {
	struct swaylock_surface *surface = wl_resource_get_user_data(resource);
	assert(surface->output_desc.current);
	create_output_image_desc(resource, surface->output_desc.current, image_description);
}
static const struct wp_color_management_output_v1_interface color_output_impl = {
	.destroy = nested_color_output_destroy,
	.get_image_description = nested_color_output_get_image_description,
};


static void color_feedback_handle_resource_destroy(struct wl_resource *resource) {
	/* remove resource from swaylock_surface's list of them (or from
	 * the stale list, or from the forward_state list if this has no output */
	wl_list_remove(wl_resource_get_link(resource));
}
static void nested_color_feedback_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void nested_color_feedback_get_preferred(struct wl_client *client,
		struct wl_resource *resource, uint32_t image_description) {
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	struct swaylock_surface *surface = fwd_surface->sway_surface;

	// If the surface has an associated output, use the output's color
	// description; otherwise use the default parametric description
	struct image_description_properties *props =
		fwd_surface->state->desc_surface.current;
	if (surface) {
		assert(surface->output_desc.current);
		if (!surface->output_desc.current->failed) {
			props = surface->output_desc.current;
		}
	}

	create_output_image_desc(resource, props, image_description);
}
static void nested_color_feedback_get_preferred_parameteric(struct wl_client *client,
		struct wl_resource *resource, uint32_t image_description) {
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	struct swaylock_surface *surface = fwd_surface->sway_surface;
	// If the surface has an associated output and it has a parametric
	// description, use the output's color description; otherwise use
	// the default parametric description
	struct image_description_properties *props =
		fwd_surface->state->desc_surface.current;
	if (surface) {
		assert(surface->output_desc.current);
		if (!surface->output_desc.current->failed &&
				surface->output_desc.current->icc_profile == -1) {
			props = surface->output_desc.current;
		}
	}

	create_output_image_desc(resource, props, image_description);

}
static const struct wp_color_management_surface_feedback_v1_interface color_feedback_impl = {
	.destroy = nested_color_feedback_destroy,
	.get_preferred = nested_color_feedback_get_preferred,
	.get_preferred_parametric = nested_color_feedback_get_preferred_parameteric,
};


static void nested_color_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void nested_color_get_output(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *output) {
	struct swaylock_surface *surface = wl_resource_get_user_data(output);

	struct wl_resource *output_resource = wl_resource_create(client,
		 &wp_color_management_output_v1_interface, wl_resource_get_version(resource), id);
	if (output_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(output_resource, &color_output_impl,
		surface, color_output_handle_resource_destroy);
	wl_list_insert(&surface->nested_server_color_output_resources,
		wl_resource_get_link(output_resource));
}
static void nested_color_get_surface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface) {
	struct forward_surface *forward_surf = wl_resource_get_user_data(surface);
	/* Each surface has at most one wp_color_representation_surface_v1 associated */
	if (forward_surf->color_surface) {
		wl_resource_post_error(resource, WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"color management surface already exists");
	}

	struct wl_resource *surface_resource = wl_resource_create(client,
		 &wp_color_management_surface_v1_interface, wl_resource_get_version(resource), id);
	if (surface_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	struct forward_surface *surf = wl_resource_get_user_data(surface);
	surf->color_surface = surface_resource;

	wl_resource_set_implementation(surface_resource, &color_surface_impl,
		surf, color_surface_handle_resource_destroy);
}
static void nested_color_get_surface_feedback(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface) {
	struct forward_state *state = wl_resource_get_user_data(resource);
	struct forward_surface *fwd_surface = wl_resource_get_user_data(surface);

	struct wl_resource *feedback_resource = wl_resource_create(client,
		&wp_color_management_surface_feedback_v1_interface,
		wl_resource_get_version(resource), id);
	if (feedback_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(feedback_resource, &color_feedback_impl,
		fwd_surface, color_feedback_handle_resource_destroy);
	wl_list_insert(&state->color_feedback_resources,
		wl_resource_get_link(feedback_resource));
}
static void nested_color_create_icc_creator(struct wl_client *client,
		struct wl_resource *resource, uint32_t obj) {
	struct wl_resource *creator_resource = wl_resource_create(client,
		&wp_image_description_creator_icc_v1_interface,
		wl_resource_get_version(resource), obj);
	if (creator_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_state *state = wl_resource_get_user_data(resource);
	struct wp_image_description_creator_icc_v1 *creator =
		wp_color_manager_v1_create_icc_creator(state->color_management);

	wl_resource_set_implementation(creator_resource, &desc_creator_icc_impl,
		creator, desc_creator_icc_handle_resource_destroy);
}
static void nested_color_create_parametric_creator(struct wl_client *client,
		struct wl_resource *resource, uint32_t obj) {
	struct wl_resource *creator_resource = wl_resource_create(client,
		&wp_image_description_creator_params_v1_interface,
		wl_resource_get_version(resource), obj);
	if (creator_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct forward_state *state = wl_resource_get_user_data(resource);
	struct wp_image_description_creator_params_v1 *creator =
		wp_color_manager_v1_create_parametric_creator(state->color_management);

	wl_resource_set_implementation(creator_resource, &desc_creator_params_impl,
		creator, desc_creator_params_handle_resource_destroy);
}
static void nested_color_create_windows_scrgb(struct wl_client *client,
		struct wl_resource *resource, uint32_t image_description) {
	struct forward_state *state = wl_resource_get_user_data(resource);
	create_forward_image_desc(resource,
		wp_color_manager_v1_create_windows_scrgb(state->color_management),
		image_description);
}
static void nested_color_get_image_description(struct wl_client *client,
		struct wl_resource *resource, uint32_t image_description,
		struct wl_resource *reference) {
	// Currently no protocols are supported that could produce
	// image description references
	wl_resource_post_error(resource, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
		"no legitimate source of references for get_image_description");
}
static const struct wp_color_manager_v1_interface color_mgr_impl = {
	.create_icc_creator = nested_color_create_icc_creator,
	.create_parametric_creator = nested_color_create_parametric_creator,
	.create_windows_scrgb = nested_color_create_windows_scrgb,
	.destroy = nested_color_destroy,
	.get_image_description = nested_color_get_image_description,
	.get_output = nested_color_get_output,
	.get_surface = nested_color_get_surface,
	.get_surface_feedback = nested_color_get_surface_feedback,
};

void bind_color_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id) {
	struct wl_resource *resource =
			wl_resource_create(client, &wp_color_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &color_mgr_impl, data, NULL);
	struct forward_state *state = data;
	for (size_t i = 0; i < state->supported_intents_len; i++) {
		if (!(version >= WP_COLOR_MANAGER_V1_RENDER_INTENT_ABSOLUTE_NO_ADAPTATION_SINCE_VERSION)
				&& state->supported_intents[i] == WP_COLOR_MANAGER_V1_RENDER_INTENT_ABSOLUTE_NO_ADAPTATION) {
			return;
		}
		wp_color_manager_v1_send_supported_intent(
			resource, state->supported_intents[i]);
	}
	for (size_t i = 0; i < state->supported_features_len; i++) {
		wp_color_manager_v1_send_supported_feature(
			resource, state->supported_features[i]);
	}
	for (size_t i = 0; i < state->supported_tfs_len; i++) {
		uint32_t tf = state->supported_tfs[i];
		if (!(version >= WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4_SINCE_VERSION)
				&& state->supported_tfs[i] == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4) {
			tf = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;
		}
		wp_color_manager_v1_send_supported_tf_named(
			resource, tf);
	}
	for (size_t i = 0; i < state->supported_primaries_len; i++) {
		wp_color_manager_v1_send_supported_primaries_named(
			resource, state->supported_primaries[i]);
	}
	wp_color_manager_v1_send_done(resource);
}

static void color_rep_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_color_representation_surface_v1_interface, &color_rep_surface_impl));
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if (fwd_surface) {
		// Destroying the surface also resets the pending color representation state
		fwd_surface->pending.has_alpha_mode = false;
		fwd_surface->pending.alpha_mode = 0;
		fwd_surface->pending.has_coef_range = false;
		fwd_surface->pending.coefficients = 0;
		fwd_surface->pending.range = 0;
		fwd_surface->pending.has_chroma_location = false;
		fwd_surface->pending.chroma_location = 0;
		assert(fwd_surface->color_representation == resource);
		fwd_surface->color_representation = NULL;
	}
}
static void nested_color_rep_surface_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void nested_color_rep_surface_set_alpha_mode(struct wl_client *client,
		struct wl_resource *resource, uint32_t alpha_mode) {
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if (!fwd_surface) {
		return;
	}
	fwd_surface->pending.has_alpha_mode = true;
	fwd_surface->pending.alpha_mode = alpha_mode;
}
static void nested_color_rep_surface_set_coefficients_and_range(struct wl_client *client,
		struct wl_resource *resource, uint32_t coefficients, uint32_t range) {
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if (!fwd_surface) {
		return;
	}
	fwd_surface->pending.has_coef_range = true;
	fwd_surface->pending.coefficients = coefficients;
	fwd_surface->pending.range = range;
}
static void nested_color_rep_surface_set_chroma_location(struct wl_client *client,
		struct wl_resource *resource, uint32_t chroma_location) {
	struct forward_surface *fwd_surface = wl_resource_get_user_data(resource);
	if (!fwd_surface) {
		return;
	}
	fwd_surface->pending.has_chroma_location = true;
	fwd_surface->pending.chroma_location = chroma_location;
}
static const struct wp_color_representation_surface_v1_interface color_rep_surface_impl = {
	.destroy = nested_color_rep_surface_destroy,
	.set_alpha_mode = nested_color_rep_surface_set_alpha_mode,
	.set_chroma_location = nested_color_rep_surface_set_chroma_location,
	.set_coefficients_and_range = nested_color_rep_surface_set_coefficients_and_range,
};

static void nested_color_rep_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void nested_color_rep_get_surface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface) {
	struct forward_surface *forward_surf = wl_resource_get_user_data(surface);
	/* Each surface has at most one wp_color_representation_surface_v1 associated */
	if (forward_surf->color_representation) {
		wl_resource_post_error(resource, WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"color representation already exists");
	}

	struct wl_resource *surface_resource = wl_resource_create(client,
		 &wp_color_representation_surface_v1_interface, wl_resource_get_version(resource), id);
	if (surface_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	struct forward_surface *surf = wl_resource_get_user_data(surface);
	surf->color_representation = surface_resource;

	wl_resource_set_implementation(surface_resource, &color_rep_surface_impl,
		surf, color_rep_handle_resource_destroy);
}
static const struct wp_color_representation_manager_v1_interface color_rep_impl = {
	.destroy = nested_color_rep_destroy,
	.get_surface = nested_color_rep_get_surface,
};

void bind_color_representation_manager(struct wl_client *client, void *data,
	uint32_t version, uint32_t id) {
	struct wl_resource *resource =
			wl_resource_create(client, &wp_color_representation_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &color_rep_impl, data, NULL);
	struct forward_state *state = data;
	for (size_t i = 0; i < state->alpha_modes_len; i++) {
		wp_color_representation_manager_v1_send_supported_alpha_mode(
			resource, state->alpha_modes[i]);
	}
	for (size_t i = 0; i < state->coef_range_pairs_len; i++) {
		wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
			resource, state->coef_range_pairs[i].coefficients,
			state->coef_range_pairs[i].range);
	}
	wp_color_representation_manager_v1_send_done(resource);
}



static void nested_data_source_offer(struct wl_client *client, struct wl_resource *resource,
		const char *mime_type) {
}
static void nested_data_source_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void nested_data_source_set_actions(struct wl_client *client, struct wl_resource *resource,
		uint32_t dnd_actions) {
}
static const struct wl_data_source_interface data_source_impl = {
	.destroy = nested_data_source_destroy,
	.offer = nested_data_source_offer,
	.set_actions = nested_data_source_set_actions,
};
static void data_source_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_data_source_interface, &data_source_impl));
}
static void nested_ddm_create_data_source(struct wl_client *client, struct wl_resource *resource,
		uint32_t id) {
	struct wl_resource *source_resource = wl_resource_create(client, &wl_data_source_interface,
			wl_resource_get_version(resource), id);
	if (source_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(source_resource, &data_source_impl,
		NULL, data_source_handle_resource_destroy);
}

static void nested_data_device_start_drag(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *source, struct wl_resource *origin,struct wl_resource *icon, uint32_t serial) {
}
static void nested_data_device_set_selection(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *source, uint32_t serial) {
}
static void nested_data_device_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static const struct wl_data_device_interface data_device_impl = {
	.release = nested_data_device_release,
	.set_selection = nested_data_device_set_selection,
	.start_drag = nested_data_device_start_drag,
};
static void data_device_handle_resource_destroy(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_data_device_interface, &data_device_impl));
}
static void nested_ddm_get_data_device(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *seat) {
	struct wl_resource *device_resource = wl_resource_create(client, &wl_data_device_interface,
		wl_resource_get_version(resource), id);
	if (device_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(device_resource, &data_device_impl,
		NULL, data_device_handle_resource_destroy);
}

static const struct wl_data_device_manager_interface data_device_manager_impl = {
	.get_data_device = nested_ddm_get_data_device,
	.create_data_source = nested_ddm_create_data_source,
};

void bind_wl_data_device_manager(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wl_data_device_manager_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &data_device_manager_impl, NULL, NULL);
}
