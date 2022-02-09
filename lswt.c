/*
 * lswt - list Wayland toplevels
 *
 * Copyright (C) 2021 - 2022 Leon Henrik Plickat
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <ctype.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#ifdef __linux__
#include <features.h>
#ifdef __GLIBC__
#include<execinfo.h>
#endif
#endif

#include "wlr-foreign-toplevel-management-unstable-v1.h"

#define VERSION "1.0.4"

const char usage[] =
	"Usage: lswt [options...]\n"
	"  -t, --tsv      Output data as tab separated values.\n"
	"  -j, --json     Output data in JSON format.\n"
	"  -h, --help     Print this helpt text and exit.\n"
	"  -v, --version  Print version and exit.\n";

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

	/**
	 * True if this toplevel has already been added to lists, false
	 * otherwise. Used to prevent accidentally appending the same toplevel
	 * multiple times if more than one toplevel_handle.done event for a
	 * toplevel is recevied.
	 */
	bool listed;
};

struct Output
{
	struct wl_output *wl_output;
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
	if (toplevel->listed)
		return;
	toplevel->listed = true;

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

static void wl_output_handle_name (void *data, struct wl_output *wl_output,
		const char *name)
{
	struct Output *output = (struct Output *)data;
	if ( output->name != NULL )
		free(output->name);
	output->name = strdup(name);
}

static const struct wl_output_listener wl_output_listener = {
	.name        = wl_output_handle_name,
	.geometry    = noop,
	.mode        = noop,
	.scale       = noop,
	.description = noop,
	.done        = noop,
};

static void registry_handle_global (void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	if ( strcmp(interface, wl_output_interface.name) == 0 )
	{
		if ( version < 4 )
		{
			fputs("ERROR: The compositor uses an outdated wl_output version.\n"
				"       Please inform the compositor developers so they can update to the latest version.\n", stderr);
			loop = false;
			return;
		}

		struct Output *output = calloc(1, sizeof(struct Output));
		if ( output == NULL )
		{
			fputs("ERROR: Failed to allocate.\n", stderr);
			loop = false;
			return;
		}

		output->wl_output = wl_registry_bind(registry, name, 
				&wl_output_interface, 4);
		output->name = NULL;
		output->global_name = name;
		wl_output_add_listener(output->wl_output, &wl_output_listener, output);

		list_init(&output->toplevels, version);
		list_append_item(&outputs, (void *)output);
	}
	else if ( strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0 )
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
	sync_callback = NULL;

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
		if ( buffered_registry_name == 0 || buffered_registry_version < 3 )
		{
			fputs("ERROR: Wayland server does not support foreign-toplevel-management-unstable-v1 version 3 or higher.\n", stderr);
			ret = EXIT_FAILURE;
			loop = false;
			return;
		}

		toplevel_manager = wl_registry_bind(wl_registry, buffered_registry_name,
			&zwlr_foreign_toplevel_manager_v1_interface, 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager,
				&toplevel_manager_listener, NULL);

		sync++;
		sync_callback = wl_display_sync(wl_display);
		wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);
	}
	else
	{
		/* Second sync: Now we have received all toplevel handles and
		 * their events and all wl_output events. Time to leave the main
		 * loop, print all data and exit.
		 */
		loop = false;
	}
}

static void quoted_fputs (char *str, FILE *restrict f)
{
	if ( str == NULL )
	{
		fputs("\"<NULL>\"", f);
		return;
	}

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
	if ( str == NULL )
		fputs("<NULL>", f);
	else if (string_should_be_quoted(str))
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

static void free_data (void)
{
	for (size_t i = 0; i < outputs.length; i++)
	{
		struct Output *output = (struct Output *)outputs.items[i];
		destroy_output(output);
	}
	list_finish(&outputs);

	for (size_t i = 0; i < all_toplevels.length; i++)
	{
		struct Toplevel *toplevel = (struct Toplevel *)all_toplevels.items[i];
		destroy_toplevel(toplevel);
	}
	list_finish(&all_toplevels);
}

/**
 * Intercept error signals (like SIGSEGV and SIGFPE) so that we can try to
 * print a fancy error message and a backtracke before letting the system kill us.
 */
static void handle_error (int signum)
{
	const char *msg =
		"\n"
		"┌──────────────────────────────────────────┐\n"
		"│                                          │\n"
		"│           lswt has crashed.              │\n"
		"│                                          │\n"
		"│    This is likely a bug, so please       │\n"
		"│    report this to the mailing list.      │\n"
		"│                                          │\n"
		"│  ~leon_plickat/public-inbox@lists.sr.ht  │\n"
		"│                                          │\n"
		"└──────────────────────────────────────────┘\n"
		"\n";
	fputs(msg, stderr);

#ifdef __linux__
#ifdef __GLIBC__
	fputs("Attempting to get backtrace:\n", stderr);

	/* In some rare cases, getting a backtrace can also cause a segfault.
	 * There is nothing we can or should do about that. All hope is lost at
	 * that point.
	 */
	void *buffer[255];
	const int calls = backtrace(buffer, sizeof(buffer) / sizeof(void *));
	backtrace_symbols_fd(buffer, calls, fileno(stderr));
	fputs("\n", stderr);
#endif
#endif

	/* Let the default handlers deal with the rest. */
	signal(signum, SIG_DFL);
	kill(getpid(), signum);
}

/**
 * Set up signal handlers.
 */
static void init_signals (void)
{
	signal(SIGSEGV, handle_error);
	signal(SIGFPE, handle_error);

}

int main(int argc, char *argv[])
{
	init_signals();

	if ( argc > 0 ) for (int i = 1; i < argc; i++)
	{
		if ( strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0 )
			json = true;
		else if ( strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--tsv") == 0 )
			tsv = true;
		else if ( strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0 )
		{
			fputs("lswt version " VERSION "\n", stderr);
			return EXIT_SUCCESS;
		}
		else if ( strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 )
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
	list_init(&all_toplevels, 5);

	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &registry_listener, NULL);

	sync_callback = wl_display_sync(wl_display);
	wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);

	while ( loop && wl_display_dispatch(wl_display) > 0 );

	/* If nothing went wrong in the main loop we can print and free all data,
	 * otherwise just free it.
	 */
	if ( ret == EXIT_SUCCESS )
		dump_and_free_data();
	else
		free_data();

	if ( sync_callback != NULL )
		wl_callback_destroy(sync_callback);
	if ( toplevel_manager != NULL )
		zwlr_foreign_toplevel_manager_v1_destroy(toplevel_manager);
	if ( wl_registry != NULL )
		wl_registry_destroy(wl_registry);
	wl_display_disconnect(wl_display);

	return ret;
}

