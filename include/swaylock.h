#ifndef _SWAYLOCK_H
#define _SWAYLOCK_H
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include "background-image.h"
#include "cairo.h"
#include "pool-buffer.h"
#include "seat.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wayland-drm-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

// Indicator state: status of authentication attempt
enum auth_state {
	AUTH_STATE_IDLE, // nothing happening
	AUTH_STATE_VALIDATING, // currently validating password
	AUTH_STATE_INVALID, // displaying message: password was wrong
};

// Indicator state: status of password buffer / typing letters
enum input_state {
	INPUT_STATE_IDLE, // nothing happening; other states decay to this after time
	INPUT_STATE_CLEAR, // displaying message: password buffer was cleared
	INPUT_STATE_LETTER, // pressed a key that input a letter
	INPUT_STATE_BACKSPACE, // pressed backspace and removed a letter
	INPUT_STATE_NEUTRAL, // pressed a key (like Ctrl) that did nothing
};

struct swaylock_colorset {
	uint32_t input;
	uint32_t cleared;
	uint32_t caps_lock;
	uint32_t verifying;
	uint32_t wrong;
};

struct swaylock_colors {
	uint32_t background;
	uint32_t bs_highlight;
	uint32_t key_highlight;
	uint32_t caps_lock_bs_highlight;
	uint32_t caps_lock_key_highlight;
	uint32_t separator;
	uint32_t layout_background;
	uint32_t layout_border;
	uint32_t layout_text;
	struct swaylock_colorset inside;
	struct swaylock_colorset line;
	struct swaylock_colorset ring;
	struct swaylock_colorset text;
};

struct swaylock_args {
	struct swaylock_colors colors;
	enum background_mode mode;
	char *font;
	uint32_t font_size;
	uint32_t radius;
	uint32_t thickness;
	uint32_t indicator_x_position;
	uint32_t indicator_y_position;
	bool override_indicator_x_position;
	bool override_indicator_y_position;
	bool ignore_empty;
	bool show_indicator;
	bool show_caps_lock_text;
	bool show_caps_lock_indicator;
	bool show_keyboard_layout;
	bool hide_keyboard_layout;
	bool show_failed_attempts;
	bool daemonize;
	int ready_fd;
	bool indicator_idle_visible;
	char *plugin_command;
	bool plugin_per_output;
	/* negative values = no grace; unit: seconds */
	float grace_time;
	/* max number of pixels/sec mouse motion which will be ignored */
	float grace_pointer_hysteresis;
};

struct swaylock_password {
	size_t len;
	size_t buffer_len;
	char *buffer;
};

struct swaylock_bg_client {
	struct swaylock_state *state;

	/* Provide per-client serials, as serials get remapped anyway */
	uint32_t serial;
	struct wl_client *client;

	/* If NULL, this client applies to all outputs; otherwise, to the
	 * specific output indicated. */
	struct swaylock_surface *unique_output;

	bool made_a_registry; // did client even create the wl_registry resource?
	/* Timer after which to give up on a non-connecting client. It is
	 * important to verify this, as there may not be any outputs */
	struct loop_timer *client_connect_timer;
	struct wl_listener client_resource_create_listener;
	struct wl_listener client_destroy_listener;

	/* For swaylock_bg_server::clients */
	struct wl_list link;
};

// for the plugin-based surface drawing
struct swaylock_bg_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_global *wlr_layer_shell;
	struct wl_global *compositor;
	struct wl_global *shm;
	struct wl_global *xdg_output_manager;
	struct wl_global *zwp_linux_dmabuf;
	struct wl_global *drm;
	struct wl_global *wp_fractional_scale;
	struct wl_global *wp_viewporter;
	struct wl_global *data_device_manager;

	struct wl_list clients;
	/* If not NULL, this client provides buffers for all surfaces */
	struct swaylock_bg_client *main_client;
};

struct dmabuf_modifier_pair {
	uint32_t format;
	uint32_t modifier_hi;
	uint32_t modifier_lo;
};

struct feedback_pair {
	uint32_t format;
	uint32_t unused_padding;
	uint32_t modifier_hi;
	uint32_t modifier_lo;
};

struct feedback_tranche  {
	dev_t tranche_device;
	struct wl_array indices;
	uint32_t flags;
};
struct dmabuf_feedback_state  {
	dev_t main_device;
	int table_fd;
	int table_fd_size;
	struct feedback_tranche *tranches;
	size_t tranches_len;
};

// todo: merge with swaylock_bg_server ?
struct forward_state {
	/* these pointers are copies of those in swaylock_state */
	struct wl_display *upstream_display;
	struct wl_registry *upstream_registry;

	struct wl_drm *drm;
	struct wl_shm *shm;
	/* this instance is used just for forwarding */
	struct zwp_linux_dmabuf_v1 *linux_dmabuf;
	/* list of wl_resources corresponding to (default/surface) feedback instances
	 * that should get updated when the upstream feedback is updated */
	struct wl_list feedback_instances;
	/* We only let the background generator create surfaces, but not
	 * subsurfaces, because those are much trickier to implement correctly,
	 * and a well designed background shouldn't need them anyway. */
	struct wl_compositor *compositor;

	struct wp_viewporter *viewporter;
	struct wp_fractional_scale_manager_v1 *fractional_scale;

	uint32_t *shm_formats;
	uint32_t shm_formats_len;

	struct dmabuf_modifier_pair *dmabuf_formats;
	uint32_t dmabuf_formats_len;

	struct dmabuf_feedback_state current, pending;
	struct feedback_tranche pending_tranche;
};

struct damage_record {
	int32_t x,y,w,h;
};

struct forward_buffer {
	/* may be null if plugin program deleted it */
	struct wl_resource *resource;
	/* upstream buffer */
	struct wl_buffer *buffer;
	/* list of surfaces where buffer is pending */
	struct wl_list pending_surfaces;
	/* list of surfaces where buffer is committed */
	struct wl_list committed_surfaces;
	/* dimensions of the buffer */
	uint32_t width, height;
};
/* BUFFER_UNREACHABLE is used for the committed buffer it it was been deleted
 * downstream
 *
 * BUFFER_COMMITTED is used for the pending buffer if it was deleted downstream
 * and matches whatever was already committed.
 */
#define BUFFER_UNREACHABLE (struct forward_buffer *)(-1)
#define BUFFER_COMMITTED (struct forward_buffer *)(-2)

struct surface_state {
	/* wl_buffer, invoke get_resource for upstream */
	struct forward_buffer *attachment;
	struct wl_list attachment_link;
	int32_t offset_x, offset_y;
	int32_t buffer_scale;
	int32_t buffer_transform;

	/* Viewport state */
	wl_fixed_t viewport_source_x;
	wl_fixed_t viewport_source_y;
	wl_fixed_t viewport_source_w;
	wl_fixed_t viewport_source_h;
	int32_t viewport_dest_width;
	int32_t viewport_dest_height;
};

struct serial_pair {
	uint32_t plugin_serial;
	uint32_t upstream_serial;
	/* The width and height corresponding to the configure matching `plugin_serial`.
	 * Used to verify the client submits buffers with dimensions actually matching its
	 * configures. */
	uint32_t config_width;
	uint32_t config_height;
	/* if true, plugin serial was not generated in response to an
	 * upstream configure event; so do not forward acknowledgements. */
	bool local_only;
};

/* this is a resource associated to a downstream wl_surface */
struct forward_surface {
	bool has_been_configured;
	struct wl_resource *layer_surface; // downstream only

	/* is null until get_layer_surface is called and initializes this */
	struct swaylock_surface *sway_surface;
	// set after layer surface is destroyed
	bool inert;

	/* list of callbacks for wl_surface::frame */
	struct wl_list frame_callbacks;

	// double-buffered state
	struct surface_state pending;
	struct surface_state committed;
	// copy of buffer size, to retain even in case attached buffer is destroyed after commit
	uint32_t committed_buffer_width;
	uint32_t committed_buffer_height;

	/* damage is not, strictly speaking, double buffered */
	struct damage_record *buffer_damage;
	size_t buffer_damage_len;
	struct damage_record *old_damage;
	size_t old_damage_len;

	uint32_t last_used_plugin_serial;
	uint32_t last_acked_width, last_acked_height;
	struct serial_pair *serial_table;
	size_t serial_table_len;

	/* The unique viewport resource attached to the surface, if any */
	struct wl_resource *viewport;

	/* The unique fractional_scale resource attached to the surface, if any */
	struct wl_resource *fractional_scale;
};

struct swaylock_state {
	struct loop *eventloop;
	struct loop_timer *input_idle_timer; // timer to reset input state to IDLE
	struct loop_timer *auth_idle_timer; // timer to stop displaying AUTH_STATE_INVALID
	struct loop_timer *clear_password_timer;  // clears the password buffer
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct zwlr_input_inhibit_manager_v1 *input_inhibit_manager;
	struct wl_shm *shm;
	struct zwp_linux_dmabuf_feedback_v1 *dmabuf_default_feedback;
	struct wl_list surfaces;
	struct wl_list images;
	struct swaylock_args args;
	struct swaylock_password password;
	struct swaylock_xkb xkb;
	cairo_surface_t *test_surface;
	cairo_t *test_cairo; // used to estimate font/text sizes
	enum auth_state auth_state; // state of the authentication attempt
	enum input_state input_state; // state of the password buffer and key inputs
	uint32_t highlight_start; // position of highlight; 2048 = 1 full turn
	int failed_attempts;
	bool run_display, locked;
	struct ext_session_lock_manager_v1 *ext_session_lock_manager_v1;
	struct ext_session_lock_v1 *ext_session_lock_v1;
	struct zxdg_output_manager_v1 *zxdg_output_manager;
	struct forward_state forward;
	struct swaylock_bg_server server;
	bool start_clientless_mode;
	struct loop_timer *grace_timer; // timer for grace period to end
	int sleep_comm_r, sleep_comm_w;

	// for nested server, output was destroyed
	struct wl_list stale_wl_output_resources;
	struct wl_list stale_xdg_output_resources;
};

struct swaylock_surface {
	cairo_surface_t *image;
	struct swaylock_state *state;
	struct wl_output *output;
	uint32_t output_global_name;
	struct wl_surface *surface; // surface for background
	struct wl_surface *child; // indicator surface made into subsurface
	struct wl_subsurface *subsurface;

	struct forward_surface *plugin_surface;

	struct ext_session_lock_surface_v1 *ext_session_lock_surface_v1;
	struct wp_viewport *viewport;
	struct wp_fractional_scale_v1* fractional_scale;
	uint32_t last_fractional_scale; /* is zero if nothing received yet */
	struct pool_buffer indicator_buffers[2];
	bool created;
	bool dirty;
	uint32_t width, height;
	int32_t scale;
	enum wl_output_subpixel subpixel;
	char *output_name;
	char *output_description;
	int32_t physical_width, physical_height, output_transform;
	int32_t mode_width, mode_height;
	struct wl_list link;
	struct wl_callback *frame;

	struct wl_global *nested_server_output;
	// lists of associated resources
	struct wl_list nested_server_wl_output_resources;
	struct wl_list nested_server_xdg_output_resources;

	/* the serial of the configure which first established the size of the
	 * surface; will be needed when plugin surface is set up and needs to link
	 * its first configure to the first configure of the swaylock_surface */
	uint32_t first_configure_serial;
	bool used_first_configure;

	/* needed to delay ack configures from plugin until just before matching commit */
	bool has_pending_ack_conf;
	uint32_t pending_upstream_serial;

	/* Does this surface have a newer configure that it did not yet acknowledge?
	 * Tracking this is useful when the client is replaced. */
	bool has_newer_serial;
	uint32_t newest_serial;

	/* has a buffer been attached and committed */
	bool has_buffer;

	/* If not NULL, the client which provides surfaces for this surface.
	 * If NULL, server.main_client will do so */
	struct swaylock_bg_client *client;

	/* Timer to verify if the client submits surfaces promptly.
	 * (To be fully accurate, it would be better to launch a unique timer
	 * every time the compositor resizes this surface, and then have the
	 * client defuse all timers preceding the serial of its last
	 * acknowledgement with associated valid surface submission.
	 */
	struct loop_timer *client_submission_timer;
};

/* Forwarding interface. These create various resources which maintain an
 * exactly corresponding resource on the server side. (With exceptions:
 * wl_regions do not need to be forwarded, so such wl_region-type wl_resources
 * lack user data.
 *
 * This solution is only good for a prototype, because blind forwarding lets
 * a bad plugin process directly overload the compositor, instead of overloading
 * swaylock. The correct thing to do is to fully maintain local buffer/surface
 * state, and only upload buffer data or send damage as needed; it is better
 * to crash swaylock than to crash the compositor.
 */
void bind_wl_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void bind_wl_shm(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void bind_linux_dmabuf(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void bind_drm(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void bind_viewporter(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void bind_fractional_scale(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void send_dmabuf_feedback_data(struct wl_resource *feedback, const struct dmabuf_feedback_state *state);
/* No-op interfaces; do the minimum required to implement the interface but have no effect;
 * used when clients unnecessarily require specific interfaces to run. */
void bind_wl_data_device_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id);

/* use this to record that in response to the configure event with upstream_serial,
 * a configure event with downstream_serial was sent to the plugin surface.
 * If local_only=true, mark that the downstream serial does _not_ need forwarding. */
void add_serial_pair(struct forward_surface *surf, uint32_t upstream_serial,
	uint32_t downstream_serial, uint32_t width, uint32_t height, bool local_only);

// There is exactly one swaylock_image for each -i argument
struct swaylock_image {
	char *path;
	char *output_name;
	cairo_surface_t *cairo_surface;
	struct wl_list link;
};

void swaylock_handle_key(struct swaylock_state *state,
		xkb_keysym_t keysym, uint32_t codepoint);

void render(struct swaylock_surface *surface);
void damage_state(struct swaylock_state *state);
void clear_password_buffer(struct swaylock_password *pw);
void schedule_auth_idle(struct swaylock_state *state);

void initialize_pw_backend(int argc, char **argv);
void run_pw_backend_child(void);
void clear_buffer(char *buf, size_t size);

/* Returns false if it fails to set the close-on-exec flag for `fd` */
bool set_cloexec(int fd);

#endif
