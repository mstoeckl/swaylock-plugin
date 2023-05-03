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
};

struct swaylock_password {
	size_t len;
	size_t buffer_len;
	char *buffer;
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

	struct wl_client *surf_client;
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
};

struct serial_pair {
	uint32_t plugin_serial;
	uint32_t upstream_serial;
};

/* this is a resource associated to a downstream wl_surface */
struct forward_surface {
	bool has_been_configured;
	struct wl_resource *layer_surface; // downstream only

	/* is null until get_layer_surface is called and initializes this */
	struct swaylock_surface *sway_surface;

	/* list of callbacks for wl_surface::frame */
	struct wl_list frame_callbacks;

	// double-buffered state
	struct surface_state pending;
	struct surface_state committed;

	/* damage is not, strictly speaking, double buffered */
	struct damage_record *buffer_damage;
	size_t buffer_damage_len;
	struct damage_record *old_damage;
	size_t old_damage_len;

	uint32_t last_used_plugin_serial;
	struct serial_pair *serial_table;
	size_t serial_table_len;
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
	struct wl_listener client_destroy_listener;
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
	struct pool_buffer indicator_buffers[2];
	bool frame_pending, dirty;
	uint32_t width, height;
	int32_t scale;
	enum wl_output_subpixel subpixel;
	char *output_name;
	char *output_description;
	struct wl_list link;

	struct wl_global *nested_server_output;
	// todo: list of associated resources
	struct wl_list nested_server_wl_output_resources;
	struct wl_list nested_server_xdg_output_resources;

	/* the serial of the configure which first established the size of the
	 * surface; will be needed when plugin surface is set up and needs to link
	 * its first configure to the first configure of the swaylock_surface */
	uint32_t first_configure_serial;

	/* needed to delay ack configures from plugin until just before matching commit */
	bool has_pending_ack_conf;
	uint32_t pending_upstream_serial;

	/* has a buffer been attached and committed */
	bool has_buffer;
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
void send_dmabuf_feedback_data(struct wl_resource *feedback, const struct dmabuf_feedback_state *state);

/* use this to record that in response to the configure event with upstream_serial,
 * a configure event with downstream_serial was sent to the plugin surface */
void add_serial_pair(struct forward_surface *plugin_surface, uint32_t upstream_serial, uint32_t downstream_serial);

// There is exactly one swaylock_image for each -i argument
struct swaylock_image {
	char *path;
	char *output_name;
	cairo_surface_t *cairo_surface;
	struct wl_list link;
};

void swaylock_handle_key(struct swaylock_state *state,
		xkb_keysym_t keysym, uint32_t codepoint);
void render_frame_background(struct swaylock_surface *surface);
void render_frame(struct swaylock_surface *surface);
void damage_surface(struct swaylock_surface *surface);
void damage_state(struct swaylock_state *state);
void clear_password_buffer(struct swaylock_password *pw);
void schedule_auth_idle(struct swaylock_state *state);

void initialize_pw_backend(int argc, char **argv);
void run_pw_backend_child(void);
void clear_buffer(char *buf, size_t size);

#endif
