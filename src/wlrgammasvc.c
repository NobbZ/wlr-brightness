#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <glib.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include "gen/wlr-gamma-control-unstable-v1-client-protocol.h"
#include "gen/wlrgammasvcbus.h"
#include "color_math.h"

struct output {
	struct wl_output *wl_output;
	struct zwlr_gamma_control_v1 *gamma_control;
	uint32_t ramp_size;
	int table_fd;
	uint16_t *table;
	struct wl_list link;
};

static struct wl_list outputs;
static struct zwlr_gamma_control_manager_v1 *gamma_control_manager = NULL;
static struct wl_display *display;
static double current_temp = 6500;
static double current_brightness = 1.0;

static int create_anonymous_file(off_t size) {
	char template[] = "/tmp/wlroots-shared-XXXXXX";
	int fd = mkstemp(template);
	if (fd < 0) {
		return -1;
	}

	int ret;
	do {
		errno = 0;
		ret = ftruncate(fd, size);
	} while (errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}

	unlink(template);
	return fd;
}

static int create_gamma_table(uint32_t ramp_size, uint16_t **table) {
	size_t table_size = ramp_size * 3 * sizeof(uint16_t);
	int fd = create_anonymous_file(table_size);
	if (fd < 0) {
		fprintf(stderr, "failed to create anonymous file\n");
		return -1;
	}

	void *data =
		mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "failed to mmap()\n");
		close(fd);
		return -1;
	}

	*table = data;
	return fd;
}

static void gamma_control_handle_gamma_size(void *data,
		struct zwlr_gamma_control_v1 *gamma_control, uint32_t ramp_size) {
	struct output *output = data;
	output->ramp_size = ramp_size;
	output->table_fd = create_gamma_table(ramp_size, &output->table);
	if (output->table_fd < 0) {
		exit(EXIT_FAILURE);
	}
}

static void gamma_control_handle_failed(void *data,
		struct zwlr_gamma_control_v1 *gamma_control) {
	fprintf(stderr, "failed to set gamma table\n");
	exit(EXIT_FAILURE);
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
	.gamma_size = gamma_control_handle_gamma_size,
	.failed = gamma_control_handle_failed,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *output = calloc(1, sizeof(struct output));
		output->wl_output = wl_registry_bind(registry, name,
			&wl_output_interface, 1);
		wl_list_insert(&outputs, &output->link);
	} else if (strcmp(interface,
			zwlr_gamma_control_manager_v1_interface.name) == 0) {
		gamma_control_manager = wl_registry_bind(registry, name,
			&zwlr_gamma_control_manager_v1_interface, 1);
	}
}

static void registry_handle_global_remove(void *data,
		struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static void fill_gamma_table(uint16_t *table, uint32_t ramp_size,
		double brightness, int temp) {
	double rw, gw, bw;
	calc_whitepoint(temp, &rw, &gw, &bw);
	uint16_t *r = table;
	uint16_t *g = table + ramp_size;
	uint16_t *b = table + 2 * ramp_size;
	for (uint32_t i = 0; i < ramp_size; ++i) {
		double val = (double)i / (ramp_size - 1);

		// We never want to get brighter, but we also want to dim bright colors a
		// little bit more than dark colors.
		val = fmin(pow(val, 0.9) * brightness, val);
		if (val > 1.0) {
			val = 1.0;
		} else if (val < 0.0) {
			val = 0.0;
		}
		r[i] = (uint16_t)(UINT16_MAX * val * rw);
		g[i] = (uint16_t)(UINT16_MAX * val * gw);
		b[i] = (uint16_t)(UINT16_MAX * val * bw);
	}
}

static void
set_gamma(double new_brightness, int new_temp)
{
  if(new_brightness > 1.0)
  {
    new_brightness = 1.0;
  }
  if(new_brightness < 0.0)
  {
    new_brightness = 0.0;
  }

  if(new_temp > 16000)
  {
    new_temp = 16000;
  }
  if(new_temp < 1600)
  {
    new_temp = 1600;
  }

	  struct output *output;
    wl_list_for_each(output, &outputs, link) {
      output->table_fd = create_gamma_table(output->ramp_size, &output->table);
      fill_gamma_table(output->table, output->ramp_size,
         new_brightness, new_temp);
      zwlr_gamma_control_v1_set_gamma(output->gamma_control,
        output->table_fd);
    }
    wl_display_flush(display);
  current_brightness = new_brightness;
  current_temp = new_temp;
}

static gboolean
on_handle_increase_brightness (WlrGammaSvcBusBrightness *interface, GDBusMethodInvocation *invocation,
					const gdouble value, gpointer user_data)
{
  set_gamma(current_brightness + value, current_temp);
	wlr_gamma_svc_bus_brightness_complete_increase (interface, invocation, current_brightness);
	return TRUE;
}

static gboolean
on_handle_decrease_brightness (WlrGammaSvcBusBrightness *interface, GDBusMethodInvocation *invocation,
					const gdouble value, gpointer user_data)
{
  set_gamma(current_brightness - value, current_temp);
	wlr_gamma_svc_bus_brightness_complete_decrease (interface, invocation, current_brightness);
	return TRUE;
}

static gboolean
on_handle_set_brightness (WlrGammaSvcBusBrightness *interface, GDBusMethodInvocation *invocation,
					const gdouble value, gpointer user_data)
{
  set_gamma(value, current_temp);
	wlr_gamma_svc_bus_brightness_complete_set (interface, invocation, current_brightness);
	return TRUE;
}

static gboolean
on_handle_get_brightness (WlrGammaSvcBusBrightness *interface, GDBusMethodInvocation *invocation,	gpointer user_data)
{
	wlr_gamma_svc_bus_brightness_complete_get (interface, invocation, current_brightness);
	return TRUE;
}

static gboolean
on_handle_increase_temperature (WlrGammaSvcBusTemperature *interface, GDBusMethodInvocation *invocation,
                                        const gint value, gpointer user_data)
{
  set_gamma(current_brightness, current_temp + value);
        wlr_gamma_svc_bus_temperature_complete_increase (interface, invocation, current_temp);
        return TRUE;
}

static gboolean
on_handle_decrease_temperature (WlrGammaSvcBusTemperature *interface, GDBusMethodInvocation *invocation,
                                        const gint value, gpointer user_data)
{
  set_gamma(current_brightness, current_temp - value);
        wlr_gamma_svc_bus_temperature_complete_decrease (interface, invocation, current_temp);
        return TRUE;
}

static gboolean
on_handle_set_temperature (WlrGammaSvcBusTemperature *interface, GDBusMethodInvocation *
invocation,
                                        const gint value, gpointer user_data)
{
  set_gamma(current_brightness, value);
        wlr_gamma_svc_bus_temperature_complete_set (interface, invocation, current_temp);
        return TRUE;
}

static gboolean
on_handle_get_temperature (WlrGammaSvcBusTemperature *interface, GDBusMethodInvocation *
invocation,  gpointer user_data)
{
        wlr_gamma_svc_bus_temperature_complete_get (interface, invocation, current_temp);
        return TRUE;
}

static void
on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	WlrGammaSvcBusBrightness *brightness_interface;
	WlrGammaSvcBusTemperature *temperature_interface;
	GError *error;

	brightness_interface = wlr_gamma_svc_bus_brightness_skeleton_new();
	g_signal_connect (brightness_interface, "handle-get", G_CALLBACK (on_handle_get_brightness), NULL);
	g_signal_connect (brightness_interface, "handle-set", G_CALLBACK (on_handle_set_brightness), NULL);
	g_signal_connect (brightness_interface, "handle-increase", G_CALLBACK (on_handle_increase_brightness), NULL);
	g_signal_connect (brightness_interface, "handle-decrease", G_CALLBACK (on_handle_decrease_brightness), NULL);
	error = NULL;
	!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (brightness_interface), connection, "/net/zoidplex/wlr_gamma_service", &error);

	temperature_interface = wlr_gamma_svc_bus_temperature_skeleton_new();
	g_signal_connect (temperature_interface, "handle-get", G_CALLBACK (on_handle_get_temperature), NULL);
        g_signal_connect (temperature_interface, "handle-set", G_CALLBACK (on_handle_set_temperature), NULL);
        g_signal_connect (temperature_interface, "handle-increase", G_CALLBACK (on_handle_increase_temperature), NULL);
        g_signal_connect (temperature_interface, "handle-decrease", G_CALLBACK (on_handle_decrease_temperature), NULL);
	error = NULL;
	!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (temperature_interface), connection, "/net/zoidplex/wlr_gamma_service", &error);
}

int main(int argc, char *argv[]) {
	 wl_list_init(&outputs);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display\n");
		return -1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_dispatch(display);
	wl_display_roundtrip(display);

	if (gamma_control_manager == NULL) {
		fprintf(stderr,
			"compositor doesn't support wlr-gamma-control-unstable-v1\n");
		return EXIT_FAILURE;
	}

	struct output *output;
	wl_list_for_each(output, &outputs, link) {
		output->gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(
			gamma_control_manager, output->wl_output);
		zwlr_gamma_control_v1_add_listener(output->gamma_control,
			&gamma_control_listener, output);
	}
	wl_display_roundtrip(display);


	GMainLoop *loop;
	loop = g_main_loop_new (NULL, FALSE);

	g_bus_own_name(G_BUS_TYPE_SESSION, "net.zoidplex.wlr_gamma_service", G_BUS_NAME_OWNER_FLAGS_NONE, NULL,
				on_name_acquired, NULL, NULL, NULL);

	g_main_loop_run (loop);

	return EXIT_SUCCESS;
}
