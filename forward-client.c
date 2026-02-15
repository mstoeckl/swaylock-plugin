#include "swaylock.h"

#include "log.h"
#include "assert.h"
#include "color-management-v1-server-protocol.h"

static size_t next_power_of_two(size_t n) {
	assert(n <= SIZE_MAX / 2);
	if (n == 0) {
		return 0;
	}
	size_t p = 1;
	while (p < n) {
		p <<= 1;
	}
	return p;
}

static void add_one_element(void **data, size_t element_size, size_t *current_len) {
	size_t capacity = next_power_of_two(*current_len);
	size_t next_capacity = next_power_of_two(*current_len + 1);
	if (next_capacity != capacity) {
		assert(SIZE_MAX / element_size >= next_capacity);
		void *new_data = realloc(*data, element_size * next_capacity);
		assert(new_data);
		*data = new_data;
	}
	*current_len += 1;
}


static void wl_shm_handle_format(void *data, struct wl_shm *wl_shm, uint32_t format) {
	struct forward_state *forward = data;
	add_one_element((void **)&forward->shm_formats, sizeof(uint32_t), &forward->shm_formats_len);
	forward->shm_formats[forward->shm_formats_len - 1] = format;
}

const struct wl_shm_listener shm_listener = {
	.format = wl_shm_handle_format,
};

static void linux_dmabuf_handle_format(void *data, struct zwp_linux_dmabuf_v1 *linux_dmabuf,
		uint32_t format) {
	/* ignore, can be reconstructed from modifier list */
}

static void linux_dmabuf_handle_modifier(void *data, struct zwp_linux_dmabuf_v1 *linux_dmabuf,
		uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo) {
	struct forward_state *forward = data;
	add_one_element((void**)&forward->dmabuf_formats, sizeof(struct dmabuf_modifier_pair), &forward->dmabuf_formats_len);
	forward->dmabuf_formats[forward->dmabuf_formats_len - 1].format = format;
	forward->dmabuf_formats[forward->dmabuf_formats_len - 1].modifier_lo = modifier_lo;
	forward->dmabuf_formats[forward->dmabuf_formats_len - 1].modifier_hi = modifier_hi;
}

const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_listener = {
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
	add_one_element((void**)&forward->pending.tranches, sizeof(struct feedback_tranche), &forward->pending.tranches_len);

	forward->pending.tranches[forward->pending.tranches_len - 1] = forward->pending_tranche;
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

const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
	.done = dmabuf_feedback_done,
	.format_table = dmabuf_feedback_format_table,
	.main_device = dmabuf_feedback_main_device,
	.tranche_done = dmabuf_feedback_tranche_done,
	.tranche_target_device = dmabuf_feedback_tranche_target_device,
	.tranche_formats = dmabuf_feedback_tranche_formats,
	.tranche_flags = dmabuf_feedback_tranche_flags,
};


static void color_supported_intent(void *data,
		struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t render_intent) {
	struct forward_state *forward = data;
	add_one_element((void**)&forward->supported_intents,
		sizeof(*forward->supported_intents),
		&forward->supported_intents_len);
	forward->supported_intents[forward->supported_intents_len - 1] = render_intent;
}

static void color_supported_feature(void *data,
		struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t feature) {
	struct forward_state *forward = data;
	add_one_element((void**)&forward->supported_features,
		sizeof(*forward->supported_features),
		&forward->supported_features_len);
	forward->supported_features[forward->supported_features_len - 1] = feature;
}

static void color_supported_tf(void *data,
		struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t tf) {
	struct forward_state *forward = data;
	add_one_element((void**)&forward->supported_tfs,
		sizeof(*forward->supported_tfs),
		&forward->supported_tfs_len);
	forward->supported_tfs[forward->supported_tfs_len - 1] = tf;
}

static void color_supported_primaries(void *data,
		struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t primaries) {
	struct forward_state *forward = data;
	add_one_element((void**)&forward->supported_primaries,
		sizeof(*forward->supported_primaries),
		&forward->supported_primaries_len);
	forward->supported_primaries[forward->supported_primaries_len - 1] = primaries;

}

static void color_mgr_done(void *data, struct wp_color_manager_v1 *wp_color_manager_v1) {
	struct forward_state *forward = data;
	forward->color_management_done = true;
}

const struct wp_color_manager_v1_listener color_manager_listener = {
	.supported_feature = color_supported_feature,
	.supported_intent = color_supported_intent,
	.supported_primaries_named = color_supported_primaries,
	.supported_tf_named = color_supported_tf,
	.done = color_mgr_done,
};


static void color_alpha_mode(void *data,
		struct wp_color_representation_manager_v1 *wp_color_representation_manager_v1,
		uint32_t alpha_mode) {
	struct forward_state *forward = data;
	add_one_element((void**)&forward->alpha_modes,
		sizeof(*forward->alpha_modes),
		&forward->alpha_modes_len);
	forward->alpha_modes[forward->alpha_modes_len - 1] = alpha_mode;
}

static void color_coef_range(void *data,
		struct wp_color_representation_manager_v1 *wp_color_representation_manager_v1,
		uint32_t coefficients, uint32_t range) {
	struct forward_state *forward = data;
	add_one_element((void**)&forward->coef_range_pairs,
		sizeof(*forward->coef_range_pairs),
		&forward->coef_range_pairs_len);
	forward->coef_range_pairs[forward->coef_range_pairs_len - 1] = (struct color_coef_range){
		.coefficients = coefficients,
		.range = range
	};
}

static void color_rep_done(void *data,
		struct wp_color_representation_manager_v1 *wp_color_representation_manager_v1) {
	struct forward_state *forward = data;
	forward->color_representation_done = true;
}

const struct wp_color_representation_manager_v1_listener
	color_representation_manager_listener = {
	.supported_alpha_mode = color_alpha_mode,
	.supported_coefficients_and_ranges = color_coef_range,
	.done = color_rep_done,
};

void unref_image_description_props(struct image_description_properties *s) {
	if (!s) {
		return;
	}
	s->reference_count--;
	if (s->reference_count > 0) {
		return;
	}

	assert(s->description);
	wp_image_description_v1_destroy(s->description);
	s->description = NULL;

	free(s->failure_reason);
	if (s->icc_profile >= 0) {
		close(s->icc_profile);
	}
	free(s);
}
struct image_description_properties *create_image_description_props(void) {
	struct image_description_properties *s = calloc(1, sizeof(*s));
	assert(s);
	memset(s, 0, sizeof(*s));
	s->icc_profile = -1;
	s->reference_count = 1;
	return s;
}

static void request_new_image_desc(struct image_description_state *state) {
	// TODO: the color-management protocol should provide a mechanism to get the
	// output color properties in ONE exchange of messages, instead of two, even
	// if it requires sending some redundant data; roundtrips can be very slow.
	// (Zero roundtrips would be even better, but would likely require server
	// allocated wl_outputs).
	state->dirty = false;
	assert(!state->pending);
	assert(!state->info_request);

	state->pending = create_image_description_props();
	if (state->surface) {
		state->pending->description =
			wp_color_management_output_v1_get_image_description(
				state->surface->color_output);
	} else {
		assert(state->state);
		state->pending->description =
			wp_color_management_surface_feedback_v1_get_preferred_parametric(
				state->state->forward.test_feedback);
	}
	wp_image_description_v1_add_listener(state->pending->description,
		&image_output_desc_listener, state);
}

static void image_desc_info_handle_done(void *data,
		struct wp_image_description_info_v1 *info) {
	wp_image_description_info_v1_destroy(info);

	struct image_description_state *state = data;

	// Commit the new state
	unref_image_description_props(state->current);
	state->current = state->pending;
	state->pending = NULL;

	if (state->dirty) {
		request_new_image_desc(state);
	}

	if (state->surface) {
		init_surface_if_ready(state->surface);

		// The list of client color outputs will be empty if `surface` is still
		// incompletely initialized
		struct wl_resource *color_output;
		wl_resource_for_each(color_output, &state->surface->nested_server_color_output_resources) {
			wp_color_management_output_v1_send_image_description_changed(color_output);
		}
	} else {
		assert(state->state);

		struct wl_resource *color_feedback;
		wl_resource_for_each(color_feedback, &state->state->forward.color_feedback_resources) {
			if (wl_resource_get_version(color_feedback) >= 2) {
				wp_color_management_surface_feedback_v1_send_preferred_changed2(
					color_feedback, state->current->color_identity_v2_hi,
					state->current->color_identity_v2_lo);
			} else if (wl_proxy_get_version((struct wl_proxy *)state->current->description) >= 2) {
				wp_color_management_surface_feedback_v1_send_preferred_changed(
					color_feedback, color_identity_v2_to_v1(
						state->current->color_identity_v2_hi,
						state->current->color_identity_v2_lo));
			} else {
				wp_color_management_surface_feedback_v1_send_preferred_changed(
					color_feedback, state->current->color_identity_v1);
			}
		}
	}
}
static void image_desc_info_handle_icc_file(void *data,
		struct wp_image_description_info_v1 *wp_image_description_info_v1,
		int32_t icc, uint32_t icc_size) {
	struct image_description_state *state = data;
	assert(state->pending->icc_profile == -1);
	state->pending->icc_profile = icc;
	state->pending->icc_profile_len = icc_size;
}
static void image_desc_info_handle_primaries(void *data,
		struct wp_image_description_info_v1 *wp_image_description_info_v1,
		int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y,
		int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y) {
	struct image_description_state *state = data;
	state->pending->has_primaries = true;
	state->pending->prx = r_x;
	state->pending->pry = r_y;
	state->pending->pgx = g_x;
	state->pending->pgy = g_y;
	state->pending->pbx = b_x;
	state->pending->pby = b_y;
	state->pending->pwx = w_x;
	state->pending->pwy = w_y;
}
static void image_desc_info_handle_primaries_named(void *data,
		struct wp_image_description_info_v1 *wp_image_description_info_v1,
		uint32_t primaries) {
	struct image_description_state *state = data;
	state->pending->has_primaries_named = true;
	state->pending->primaries = primaries;
}
static void image_desc_info_handle_tf_power(void *data,
		struct wp_image_description_info_v1 *wp_image_description_info_v1,
		uint32_t eexp) {
	struct image_description_state *state = data;
	state->pending->has_eexp = true;
	state->pending->eexp = eexp;
}
static void image_desc_info_handle_tf_named(void *data,
		struct wp_image_description_info_v1 *wp_image_description_info_v1,
		uint32_t tf) {
	struct image_description_state *state = data;
	state->pending->has_tf = true;
	state->pending->tf = tf;
}
static void image_desc_info_handle_luminances(void *data,
		struct wp_image_description_info_v1 *wp_image_description_info_v1,
		uint32_t min_lum, uint32_t max_lum, uint32_t reference_lum) {
	struct image_description_state *state = data;
	state->pending->has_mastering_luminance = true;
	state->pending->mastering_min_lum = min_lum;
	state->pending->mastering_max_lum = max_lum;
}
static void image_desc_info_handle_target_primaries(void *data,
		struct wp_image_description_info_v1 *wp_image_description_info_v1,
		int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y,
		int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y) {
	struct image_description_state *state = data;
	state->pending->has_mastering_display_primaries = true;
	state->pending->mrx = r_x;
	state->pending->mry = r_y;
	state->pending->mgx = g_x;
	state->pending->mgy = g_y;
	state->pending->mbx = b_x;
	state->pending->mby = b_y;
	state->pending->mwx = w_x;
	state->pending->mwy = w_y;
}
static void image_desc_info_handle_target_luminance(void *data,
		struct wp_image_description_info_v1 *wp_image_description_info_v1,
		uint32_t min_lum, uint32_t max_lum) {
	struct image_description_state *state = data;
	state->pending->has_mastering_luminance = true;
	state->pending->mastering_min_lum = min_lum;
	state->pending->mastering_max_lum = max_lum;
}
static void image_desc_info_handle_target_max_cll(void *data,
		struct wp_image_description_info_v1 *wp_image_description_info_v1,
		uint32_t max_cll) {
	struct image_description_state *state = data;
	state->pending->has_max_cll = true;
	state->pending->max_cll = max_cll;
}
static void image_desc_info_handle_target_max_fall(void *data,
		struct wp_image_description_info_v1 *wp_image_description_info_v1,
		uint32_t max_fall) {
	struct image_description_state *state = data;
	state->pending->has_max_fall = true;
	state->pending->max_fall = max_fall;
}
const struct wp_image_description_info_v1_listener image_info_listener = {
	.done = image_desc_info_handle_done,
	.icc_file = image_desc_info_handle_icc_file,
	.primaries = image_desc_info_handle_primaries,
	.primaries_named = image_desc_info_handle_primaries_named,
	.tf_power = image_desc_info_handle_tf_power,
	.tf_named = image_desc_info_handle_tf_named,
	.luminances = image_desc_info_handle_luminances,
	.target_primaries = image_desc_info_handle_target_primaries,
	.target_luminance = image_desc_info_handle_target_luminance,
	.target_max_cll = image_desc_info_handle_target_max_cll,
	.target_max_fall = image_desc_info_handle_target_max_fall,
};

static void image_desc_handle_failed(void *data,
		struct wp_image_description_v1 *wp_image_description_v1,
		uint32_t cause, const char *msg) {
	struct image_description_state *state = data;
	state->pending->failed = true;
	state->pending->failure_cause = cause;
	assert(!state->pending->failure_reason);
	state->pending->failure_reason = strdup(msg);
	assert(state->pending->failure_reason);

	unref_image_description_props(state->current);
	state->current = state->pending;
	state->pending = NULL;

	if (state->dirty) {
		request_new_image_desc(state);
	}

	if (state->surface) {
		init_surface_if_ready(state->surface);

		// The list of client color outputs will be empty if `surface` is still
		// incompletely initialized
		struct wl_resource *color_output;
		wl_resource_for_each(color_output, &state->surface->nested_server_color_output_resources) {
			wp_color_management_output_v1_send_image_description_changed(color_output);
		}
	} else {
		assert(state->state);
		// Failures can not easily be recursively advertised, because they do
		// not have an associated identity. Sending a failed recommendation
		// wouldn't be useful; and this case is already unlikely for reasonable
		// compositor implementations.
	}
}

static void image_desc_handle_ready(void *data,
		struct wp_image_description_v1 *wp_image_description_v1,
		uint32_t identity) {
	struct image_description_state *state = data;
	state->pending->color_identity_v1 = identity;

	assert(wp_image_description_v1 == state->pending->description);
	assert(!state->info_request);
	state->info_request =
		wp_image_description_v1_get_information(wp_image_description_v1);
	wp_image_description_info_v1_add_listener(state->info_request,
		&image_info_listener, state);
}

static void image_desc_handle_ready2(void *data,
		struct wp_image_description_v1 *wp_image_description_v1,
		uint32_t identity_hi, uint32_t identity_lo) {
	struct image_description_state *state = data;
	state->pending->color_identity_v2_hi = identity_hi;
	state->pending->color_identity_v2_lo = identity_lo;

	assert(wp_image_description_v1 == state->pending->description);
	assert(!state->info_request);
	state->info_request =
		wp_image_description_v1_get_information(wp_image_description_v1);
	wp_image_description_info_v1_add_listener(state->info_request,
		&image_info_listener, state);
}

const struct wp_image_description_v1_listener image_output_desc_listener = {
	.failed = image_desc_handle_failed,
	.ready = image_desc_handle_ready,
	.ready2 = image_desc_handle_ready2,
};


static void color_feedback_handle_preferred_changed(void *data,
		struct wp_color_management_surface_feedback_v1 *wp_color_management_surface_feedback_v1,
		uint32_t identity) {
	(void)identity;

	struct forward_state *state = data;
	if (state->desc_surface.pending) {
		state->desc_surface.dirty = true;
		return;
	}
	request_new_image_desc(&state->desc_surface);
}
static void color_feedback_handle_preferred_changed2(void *data,
		struct wp_color_management_surface_feedback_v1 *wp_color_management_surface_feedback_v1,
		uint32_t identity_hi, uint32_t identity_lo) {
	(void)identity_hi;
	(void)identity_lo;

	struct forward_state *state = data;
	if (state->desc_surface.pending) {
		state->desc_surface.dirty = true;
		return;
	}
	request_new_image_desc(&state->desc_surface);
}

const struct wp_color_management_surface_feedback_v1_listener color_surface_feedback_listener = {
	.preferred_changed = color_feedback_handle_preferred_changed,
	.preferred_changed2 = color_feedback_handle_preferred_changed2,
};

static void color_output_handle_image_desc_changed(void *data,
		struct wp_color_management_output_v1 *wp_color_management_output_v1) {
	struct image_description_state *state = data;

	if (state->pending) {
		// One request is in progress, wait for the pending slot to become available
		state->dirty = true;
		return;
	}

	request_new_image_desc(state);
}

const struct wp_color_management_output_v1_listener color_output_listener = {
	.image_description_changed = color_output_handle_image_desc_changed,
};
