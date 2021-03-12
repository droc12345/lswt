/*
 * lswt - list Wayland toplevels
 *
 * Copyright (C) 2021 Leon Henrik Plickat
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <wayland-client.h>

#include "xdg-output-unstable-v1.h"
#include "wlr-foreign-toplevel-management-unstable-v1.h"

const char usage[] =
	"Usage: lswt [options...]\n"
	"  -t, --tsv   Output data as tab separated values.\n"
	"  -j, --json  Output data in JSON format.\n"
	"  -h, --help  Print this helpt text and exit.\n";

/* wl_list is not useful for what we want to do, so we need our own little list type. */
typedef struct
{
	size_t capacity, change, length;
	void **items;
} list_t;

static bool list_init (list_t *list, size_t capacity)
{
	list->capacity = capacity;
	list->change   = 5;
	list->length   = 0;
	list->items    = malloc(sizeof(void *) * list->capacity);
	return list->items != NULL;
}

static void list_finish (list_t *list)
{
	if ( list->items != NULL )
		free(list->items);
}

static void list_append_item (list_t *list, void *item)
{
	if ( list->length == list->capacity )
	{
		list->capacity += list->change;
		list->items = realloc(list->items, sizeof(void *) * list->capacity);
		if ( list->items == NULL )
		{
			fputs("ERROR: Failed to re-allocate.\n", stderr);
			return;
		}
	}
	list->items[list->length] = item;
	list->length++;
}

int ret = EXIT_SUCCESS;
bool loop = true;
bool json = false;
bool tsv = false;
struct wl_display *wl_display = NULL;
struct wl_registry *wl_registry = NULL;
struct wl_callback *sync_callback = NULL;
struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager = NULL;
struct zxdg_output_manager_v1 *xdg_output_manager = NULL;
struct Output *no_output = NULL;
list_t outputs;
list_t all_toplevels;
uint32_t buffered_registry_name = 0;
uint32_t buffered_registry_version;

static void noop () {}

struct Toplevel
{
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	uint32_t i;
	char *title;
	char *app_id;
	bool maximized;
	bool minimized;
	bool activated;
	bool fullscreen;
	bool output;
	list_t outputs;
	// TODO list_t children
};

struct Output
{
	struct wl_output *wl_output;
	struct zxdg_output_v1 *xdg_output;
	char *name;
	uint32_t global_name;
	list_t toplevels;
};

static void handle_handle_title (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *title)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	if ( toplevel->title != NULL )
		free(toplevel->title);
	toplevel->title = strdup(title);
}

static void handle_handle_app_id (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *app_id)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	if ( toplevel->app_id != NULL )
		free(toplevel->app_id);
	toplevel->app_id = strdup(app_id);
}

static void handle_handle_output_enter (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		struct wl_output *wl_output)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	toplevel->output = true;
	struct Output *output = wl_output_get_user_data(wl_output);
	if ( output == NULL )
	{
		fputs("ERROR: Toplevel on unadvertised output.\n", stderr);
		return;
	}
	list_append_item(&output->toplevels, (void *)toplevel);
	list_append_item(&toplevel->outputs, (void *)output);
}

static void handle_handle_state (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		struct wl_array *states)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	uint32_t *state;
	wl_array_for_each(state, states)
	{
		switch (*state)
		{
			case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
				toplevel->maximized = true;
				break;

			case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
				toplevel->minimized = true;
				break;

			case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
				toplevel->activated = true;
				break;
			case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
				toplevel->fullscreen = true;
				break;
		}
	}
}

static void handle_handle_done (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	list_append_item(&all_toplevels, (void *)toplevel);

	if (! toplevel->output)
	{
		if ( no_output == NULL )
		{
			no_output = calloc(1, sizeof(struct Toplevel));
			if ( no_output == NULL )
			{
				fputs("ERROR: Failed to allocate.\n", stderr);
				return;
			}

			no_output->wl_output = NULL;
			no_output->xdg_output = NULL;
			no_output->global_name = 0;
			no_output->name = NULL;

			list_init(&no_output->toplevels, 1);
			list_append_item(&outputs, (void *)no_output);
		}

		list_append_item(&no_output->toplevels, (void *)toplevel);
		list_append_item(&toplevel->outputs, (void *)no_output);
	}
}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
	.title        = handle_handle_title,
	.app_id       = handle_handle_app_id,
	.output_enter = handle_handle_output_enter,
	.output_leave = noop,
	.state        = handle_handle_state,
	.done         = handle_handle_done,
	.closed       = noop,
	.parent       = noop
};

static void toplevel_manager_handle_toplevel (void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
		struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct Toplevel *toplevel = calloc(1, sizeof(struct Toplevel));
	if ( toplevel == NULL )
	{
		fputs("ERROR: Failed to allocate.\n", stderr);
		return;
	}

	static uint32_t i = 0;

	toplevel->handle     = handle;
	toplevel->i          = i++;
	toplevel->title      = NULL;
	toplevel->app_id     = NULL;
	toplevel->maximized  = false;
	toplevel->minimized  = false;
	toplevel->activated  = false;
	toplevel->fullscreen = false;
	toplevel->output     = false;

	list_init(&toplevel->outputs, 1);

	zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, toplevel);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
	.toplevel = toplevel_manager_handle_toplevel,
	.finished = noop,
};

static void xdg_output_handle_name (void *data, struct zxdg_output_v1 *xdg_output,
		const char *name)
{
	struct Output *output = (struct Output *)data;
	if ( output->name != NULL )
		free(output->name);
	output->name = strdup(name);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_size     = noop,
	.name             = xdg_output_handle_name,
	.logical_position = noop,
	.description      = noop,
	.done             = noop, /* Deprecated since version 3. */
};

static void registry_handle_global (void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	if (! strcmp(interface, wl_output_interface.name))
	{
		struct Output *output = calloc(1, sizeof(struct Output));
		if ( output == NULL )
		{
			fputs("ERROR: Failed to allocate.\n", stderr);
			return;
		}

		output->wl_output = wl_registry_bind(registry, name, 
				&wl_output_interface, version);
		output->xdg_output = NULL;
		output->name = NULL;
		output->global_name = name;

		wl_output_set_user_data(output->wl_output, output);
		list_init(&output->toplevels, version);
		list_append_item(&outputs, (void *)output);
	}
	else if (! strcmp(interface, zxdg_output_manager_v1_interface.name))
		xdg_output_manager = wl_registry_bind(registry, name,
				&zxdg_output_manager_v1_interface, version);
	else if (! strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name))
	{
		buffered_registry_name    = name;
		buffered_registry_version = version;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global        = registry_handle_global,
	.global_remove = noop,
};

static void sync_handle_done (void *data, struct wl_callback *wl_callback, uint32_t other_data);
static const struct wl_callback_listener sync_callback_listener = {
	.done = sync_handle_done,
};

static void sync_handle_done (void *data, struct wl_callback *wl_callback, uint32_t other_data)
{
	wl_callback_destroy(wl_callback);

	static int sync = 0;
	if ( sync == 0 )
	{
		/* First sync: The registry finished advertising globals and we
		 * have bound all outputs. Now we can use the buffered registry
		 * values to bind the foreign-toplevel-manager. We do this now
		 * instead of directly in the registry listener, as otherwise
		 * there is a chance that it gets bound before the outputs,
		 * which causes the server to not send the output_enter events.
		 * See: https://github.com/swaywm/wlroots/issues/1567
		 */
		if ( buffered_registry_name == 0 )
		{
			fputs("ERROR: Wayland server does not support foreign-toplevel-management-unstable-v1.\n", stderr);
			ret = EXIT_FAILURE;
			loop = false;
			return;
		}

		toplevel_manager = wl_registry_bind(wl_registry, buffered_registry_name,
			&zwlr_foreign_toplevel_manager_v1_interface, buffered_registry_version);
		zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager,
				&toplevel_manager_listener, NULL);

		if ( xdg_output_manager != NULL )
			for (size_t i = 0; i < outputs.length; i++)
			{
				struct Output *output = (struct Output *)outputs.items[i];
				output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
						xdg_output_manager, output->wl_output);
				zxdg_output_v1_add_listener(output->xdg_output,
						&xdg_output_listener, output);
			}

		sync++;
		sync_callback = wl_display_sync(wl_display);
		wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);
	}
	else
	{
		/* Second sync: Now we have received all toplevel handles and
		 * their events and (if available) all XDG-Output events.
		 * Time to leave the main loop, print all data and exit.
		 */
		loop = false;
	}
}

static void quoted_fputs (char *str, FILE *restrict f)
{
	fputc('"', f);
	for (; *str != '\0'; str++)
	{
		if ( *str == '"' )
			fputs("\\\"", f);
		else
			fputc(*str, f);
	}
	fputc('"', f);
}

static bool string_should_be_quoted (char *str)
{
	for (; *str != '\0'; str++)
		if ( isspace(*str) || *str == '"' || *str == '\'' || ! isascii(*str) )
			return true;
	return false;
}

static void human_fputs (char *str, FILE *restrict f)
{
	if (string_should_be_quoted(str))
		quoted_fputs(str, f);
	else
		fputs(str, f);
}

static void fputb (bool bl, FILE *restrict f)
{
	fputs(bl ? "true" : "false", f);
}

static void print_toplevel (struct Toplevel *toplevel)
{
	if (json)
	{
		static bool json_prev = false;
		if (json_prev)
			fputs(",\n", stdout);
		else
			json_prev = true;

		fputs("    {\n        \"title\": ", stdout);
		quoted_fputs(toplevel->title, stdout);
		fputs(",\n        \"app_id\": ", stdout);
		quoted_fputs(toplevel->app_id, stdout);
		fputs(",\n        \"maximized\": ", stdout);
		fputb(toplevel->maximized, stdout);
		fputs(",\n        \"minimized\": ", stdout);
		fputb(toplevel->minimized, stdout);
		fputs(",\n        \"activated\": ", stdout);
		fputb(toplevel->activated, stdout);
		fputs(",\n        \"fullscreen\": ", stdout);
		fputb(toplevel->fullscreen, stdout);
		fputs(",\n        \"outputs\": [", stdout);

		bool prev = false;
		for (size_t i = 0; i < toplevel->outputs.length; i++)
		{
			struct Output *output = (struct Output *)toplevel->outputs.items[i];

			if ( output == no_output )
				break;

			if (prev)
				fputc(',', stdout);
			else 
				prev = true;

			fputc(' ', stdout);

			if ( output->name == NULL )
				fprintf(stdout, "\"%d\"", output->global_name);
			else
				quoted_fputs(output->name, stdout);
		}

		fputs(" ]\n    }", stdout);
	}
	else if (tsv)
	{
		quoted_fputs(toplevel->title, stdout);
		fputc('\t', stdout);
		quoted_fputs(toplevel->app_id, stdout);
		fputc('\t', stdout);
		fputb(toplevel->maximized, stdout);
		fputc('\t', stdout);
		fputb(toplevel->minimized, stdout);
		fputc('\t', stdout);
		fputb(toplevel->activated, stdout);
		fputc('\t', stdout);
		fputb(toplevel->fullscreen, stdout);
		fputc('\t', stdout);

		if ( toplevel->outputs.length == 0 )
			fputs("none", stdout);
		else
		{
			bool prev = false;
			for (size_t i = 0; i < toplevel->outputs.length; i++)
			{
				struct Output *output = (struct Output *)toplevel->outputs.items[i];

				if ( output == no_output )
				{
					fputs("none", stdout);
					break;
				}

				if (prev)
					fputs(",", stdout);
				else 
					prev = true;

				if ( output->name == NULL )
					fprintf(stdout, "\"%d\"", output->global_name);
				else
					quoted_fputs(output->name, stdout);
			}
		}

		fputc('\n', stdout);
	}
	else
	{
		fprintf(stdout, "%d: %s%s%s%s ",
				toplevel->i,
				toplevel->maximized  ? "m" : "-",
				toplevel->minimized  ? "m" : "-",
				toplevel->activated  ? "a" : "-",
				toplevel->fullscreen ? "f" : "-");
		human_fputs(toplevel->title, stdout);
		fputc(' ', stdout);
		human_fputs(toplevel->app_id, stdout);
		fputc('\n', stdout);
	}
}

static void destroy_toplevel (struct Toplevel *toplevel)
{
	zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);
	if ( toplevel->title != NULL )
		free(toplevel->title);
	if ( toplevel->app_id != NULL )
		free(toplevel->app_id);
	list_finish(&toplevel->outputs);
	free(toplevel);
}

static void print_and_destroy_toplevels (list_t *list)
{
	for (size_t i = 0; i < list->length; i++)
	{
		struct Toplevel *toplevel = (struct Toplevel *)list->items[i];
		print_toplevel(toplevel);
		destroy_toplevel(toplevel);
	}
}

static void destroy_output (struct Output *output)
{
	list_finish(&output->toplevels);
	if ( output->wl_output != NULL )
		wl_output_destroy(output->wl_output);
	if ( output->xdg_output != NULL )
		zxdg_output_v1_destroy(output->xdg_output);
	if ( output->name != NULL )
		free(output->name);
	free(output);
}

static void dump_and_free_data (void)
{
	if ( json || tsv || outputs.length == 1 )
	{
		if (json)
			fputs("[\n", stdout);

		print_and_destroy_toplevels(&all_toplevels);

		if (json)
			fputs("\n]\n", stdout);

		for (size_t i = 0; i < outputs.length; i++)
		{
			struct Output *output = (struct Output *)outputs.items[i];
			destroy_output(output);
		}
	}
	else
	{
		bool prev = false;
		for (size_t i = 0; i < outputs.length; i++)
		{
			struct Output *output = (struct Output *)outputs.items[i];

			if (prev)
				fputc('\n', stdout);
			else
				prev = true;

			if ( output == no_output )
				fputs("Toplevels not on any output:\n", stdout);
			else if ( output->name == NULL )
				fprintf(stdout, "Output %d (global-name):\n", output->global_name);
			else
			{
				human_fputs(output->name, stdout);
				fputs(":\n", stdout);
			}

			if ( output->toplevels.length == 0 )
				fputs("[none]\n", stdout);

			print_and_destroy_toplevels(&output->toplevels);
			destroy_output(output);
		}
	}

	list_finish(&outputs);
	list_finish(&all_toplevels);
}

int main(int argc, char *argv[])
{
	if ( argc > 0 ) for (int i = 1; i < argc; i++)
	{
		if ( ! strcmp(argv[i], "-j") || ! strcmp(argv[i], "--json") )
			json = true;
		else if ( ! strcmp(argv[i], "-t") || ! strcmp(argv[i], "--tsv") )
			tsv = true;
		else if ( ! strcmp(argv[i], "-h") || ! strcmp(argv[i], "--help") )
		{
			fputs(usage, stderr);
			return EXIT_SUCCESS;
		}
		else
		{
			fprintf(stderr, "Invalid option: %s\n", argv[i]);
			fputs(usage, stderr);
			return EXIT_FAILURE;
		}
	}

	/* We query the display name here instead of letting wl_display_connect()
	 * figure it out itself, because libwayland (for legacy reasons) falls
	 * back to using "wayland-0" when $WAYLAND_DISPLAY is not set, which is
	 * generally not desirable.
	 */
	const char *display_name = getenv("WAYLAND_DISPLAY");
	if ( display_name == NULL )
	{
		fputs("ERROR: WAYLAND_DISPLAY is not set.\n", stderr);
		return EXIT_FAILURE;
	}

	wl_display = wl_display_connect(display_name);
	if ( wl_display == NULL )
	{
		fputs("ERROR: Can not connect to wayland display.\n", stderr);
		return EXIT_FAILURE;
	}

	list_init(&outputs, 1);
	list_init(&all_toplevels, 1);

	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &registry_listener, NULL);

	sync_callback = wl_display_sync(wl_display);
	wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);

	while (loop)
		if (! wl_display_dispatch(wl_display))
			break;

	dump_and_free_data();

	if ( toplevel_manager != NULL )
		zwlr_foreign_toplevel_manager_v1_destroy(toplevel_manager);
	if ( xdg_output_manager != NULL )
		zxdg_output_manager_v1_destroy(xdg_output_manager);
	if ( wl_registry != NULL )
		wl_registry_destroy(wl_registry);
	wl_display_disconnect(wl_display);

	return ret;
}

