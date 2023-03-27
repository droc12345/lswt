/*
 * lswt - list Wayland toplevels
 *
 * Copyright (C) 2021 - 2023 Leon Henrik Plickat
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
#include <errno.h>
#include <assert.h>
#include <wayland-client.h>

#ifdef __linux__
#include <features.h>
#include <linux/landlock.h>
#include <sys/syscall.h>
#ifdef __GLIBC__
#include<execinfo.h>
#endif
#endif

#include "wlr-foreign-toplevel-management-unstable-v1.h"
#include "ext-foreign-toplevel-list-v1.h"

#define VERSION "1.1.0"

#define DEBUG_LOG(FMT, ...) { if (debug_log) fprintf(stderr, "DEBUG: " FMT "\n" __VA_OPT__(,) __VA_ARGS__); }
#define BOOL_TO_STR(B) (B) ? "true" : "false"

const char usage[] =
	"Usage: lswt [options...]\n"
	"  -h,        --help           Print this helpt text and exit.\n"
	"  -v,        --version        Print version and exit.\n"
	"  -j,        --json           Output data in JSON format.\n"
	"  -c <fmt>, --custom <fmt>    Define a custom line-based output format.\n";

enum Output_format
{
	NORMAL,
	CUSTOM,
	JSON,
};
enum Output_format output_format = NORMAL;
char *custom_output_format = NULL;

/** Used for padding when printing output in NORMAL format. */
size_t longest_app_id = 7; // strlen("app-id:")
const size_t max_app_id_padding = 40;

int ret = EXIT_SUCCESS;
bool loop = true;
bool debug_log = false;

struct wl_display *wl_display = NULL;
struct wl_registry *wl_registry = NULL;
struct wl_callback *sync_callback = NULL;

/* We implement both the new protocol (ext-*) as well as the old one it is based
 * on (zwlr-*), since there likely are compositors still stuck with the legacy
 * one for a while.
 * NOTE: zwlr-foreign-toplevel-management-v1 support will be deprecated eventually!
 */
struct zwlr_foreign_toplevel_manager_v1 *zwlr_toplevel_manager = NULL;
struct ext_foreign_toplevel_list_v1 *ext_toplevel_list = NULL;

struct wl_list toplevels;

static void noop () {}

/**********************
 *                    *
 *    Capabilities    *
 *                    *
 **********************/
bool support_fullscreen = false;
bool support_activated = false;
bool support_maximized = false;
bool support_minimized = false;
bool support_identifier = false;

static void update_capabilities (void)
{
	if ( zwlr_toplevel_manager != NULL )
	{
		support_fullscreen = true;
		support_activated = true;
		support_maximized = true;
		support_minimized = true;
	}
	else if ( ext_toplevel_list != NULL )
	{
		support_identifier = true;
	}
}

/******************
 *                *
 *    Toplevel    *
 *                *
 ******************/
struct Toplevel
{
	struct wl_list link;

	struct zwlr_foreign_toplevel_handle_v1 *zwlr_handle;
	struct ext_foreign_toplevel_handle_v1 *ext_handle;

	char *title;
	char *app_id;

	/**
	 * Optional data. Whether these are supported depends on the bound
	 * protocol(s). See update_capabilities() and related globals.
	 */
	char *identifier;
	bool fullscreen;
	bool activated;
	bool maximized;
	bool minimized;

	/**
	 * True if this toplevel has already been added to the list, false
	 * otherwise. Used to prevent accidentally appending the same toplevel
	 * multiple times if toplevel_handle_done is called more than once.
	 */
	bool listed;
};

/** Allocate a new Toplevel and initialize it. Returns pointer to the Toplevel. */
static struct Toplevel *toplevel_new (void)
{
	struct Toplevel *new = calloc(1, sizeof(struct Toplevel));
	if ( new == NULL )
	{
		fprintf(stderr, "ERROR: calloc: %s\n", strerror(errno));
		return NULL;
	}

	DEBUG_LOG("New toplevel: %p", (void*)new);

	new->zwlr_handle = NULL;
	new->ext_handle = NULL;
	new->title = NULL;
	new->app_id = NULL;
	new->identifier = NULL;
	new->listed = false;

	new->fullscreen = false;
	new->activated = false;
	new->maximized = false;
	new->minimized = false;

	return new;
}

/** Destroys a toplevel and removes it from the list, if it is listed. */
static void toplevel_destroy (struct Toplevel *self)
{
	DEBUG_LOG("Destroying toplevel: %p", (void*)self);

	if ( self->zwlr_handle != NULL )
		zwlr_foreign_toplevel_handle_v1_destroy(self->zwlr_handle);
	if ( self->ext_handle != NULL )
		ext_foreign_toplevel_handle_v1_destroy(self->ext_handle);
	if ( self->title != NULL )
		free(self->title);
	if ( self->app_id != NULL )
		free(self->app_id);
	if ( self->identifier != NULL )
		free(self->identifier);
	if (self->listed)
		wl_list_remove(&self->link);
	free(self);
}

/** Set the title of the toplevel. Called from protocol implementations. */
static void toplevel_set_title (struct Toplevel *self, const char *title)
{
	DEBUG_LOG("Toplevel set title: %p, '%s'", (void*)self, title);

	if ( self->title != NULL )
		free(self->title);
	self->title = strdup(title);
	if ( self->title == NULL )
		fprintf(stderr, "ERROR: strdup(): %s\n", strerror(errno));
}

/** Set the app-id of the toplevel. Called from protocol implementations. */
static size_t real_strlen (const char *str);
static void toplevel_set_app_id (struct Toplevel *self, const char *app_id)
{
	DEBUG_LOG("Toplevel set app-id: %p, '%s'", (void*)self, app_id);

	if ( self->app_id != NULL )
		free(self->app_id);
	self->app_id = strdup(app_id);
	if ( self->app_id == NULL )
	{
		fprintf(stderr, "ERROR: strdup(): %s\n", strerror(errno));
		return;
	}

	/* Used when printing output in the default human readable format. */
	const size_t len = real_strlen(app_id);
	if ( len > longest_app_id && max_app_id_padding > len )
		longest_app_id = len;
}

/** Set the identifier of the toplevel. Called from protocol implementations. */
static void toplevel_set_identifier (struct Toplevel *self, const char *identifier)
{
	DEBUG_LOG("Toplevel set identifier: %p, '%s'", (void*)self, identifier);

	if ( self->identifier != NULL )
	{
		fputs("ERROR: protocol-error: compositor changed identifier of toplevel, which is forbidden by the protocol. Continuing anyway...\n", stderr);
		free(self->identifier);
	}
	self->identifier = strdup(identifier);
	if ( self->identifier == NULL )
		fprintf(stderr, "ERROR: strdup(): %s\n", strerror(errno));
}

static void toplevel_set_fullscreen (struct Toplevel *self, bool fullscreen)
{
	DEBUG_LOG("Toplevel set fullscreen: %p, '%d'", (void*)self, fullscreen);
	self->fullscreen = fullscreen;
}

static void toplevel_set_activated (struct Toplevel *self, bool activated)
{
	DEBUG_LOG("Toplevel set activated: %p, '%d'", (void*)self, activated);
	self->activated = activated;
}

static void toplevel_set_maximized (struct Toplevel *self, bool maximized)
{
	DEBUG_LOG("Toplevel set maximized: %p, '%d'", (void*)self, maximized);
	self->maximized = maximized;
}

static void toplevel_set_minimized (struct Toplevel *self, bool minimized)
{
	DEBUG_LOG("Toplevel set minimized: %p, '%d'", (void*)self, minimized);
	self->minimized = minimized;
}

static void toplevel_done (struct Toplevel *self)
{
	DEBUG_LOG("Toplevel done: %p", (void*)self);

	if (self->listed)
		return;
	self->listed = true;

	wl_list_insert(&toplevels, &self->link);
}

/*****************************************************
 *                                                   *
 *    ext-foreign-toplevel-list-v1 implementation    *
 *                                                   *
 *****************************************************/
static void ext_foreign_handle_handle_identifier (void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *identifier)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	toplevel_set_identifier(toplevel, identifier);
}

static void ext_foreign_handle_handle_title (void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *title)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	toplevel_set_title(toplevel, title);
}

static void ext_foreign_handle_handle_app_id (void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *app_id)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	toplevel_set_app_id(toplevel, app_id);
}

static void ext_foreign_handle_handle_done (void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	toplevel_done(toplevel);
}

static const struct ext_foreign_toplevel_handle_v1_listener ext_handle_listener = {
	.app_id       = ext_foreign_handle_handle_app_id,
	.closed       = noop,
	.done         = ext_foreign_handle_handle_done,
	.identifier   = ext_foreign_handle_handle_identifier,
	.title        = ext_foreign_handle_handle_title,
};

static void ext_toplevel_list_handle_toplevel (void *data,
		struct ext_foreign_toplevel_list_v1 *list,
		struct ext_foreign_toplevel_handle_v1 *handle)
{
	struct Toplevel *toplevel = toplevel_new();
	if ( toplevel == NULL )
		return;
	toplevel->ext_handle = handle;
	ext_foreign_toplevel_handle_v1_add_listener(handle, &ext_handle_listener, toplevel);
}

static const struct ext_foreign_toplevel_list_v1_listener ext_toplevel_list_listener = {
	.toplevel = ext_toplevel_list_handle_toplevel,
	.finished = noop,
};

/************************************************************
 *                                                          *
 *    zwlr-foreign-toplevel-management-v1 implementation    *
 *                                                          *
 ************************************************************/
static void zwlr_foreign_handle_handle_title (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *title)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	toplevel_set_title(toplevel, title);
}

static void zwlr_foreign_handle_handle_app_id (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *app_id)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	toplevel_set_app_id(toplevel, app_id);
}

static void zwlr_foreign_handle_handle_state (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		struct wl_array *states)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;

	bool fullscreen = false;
	bool activated = false;
	bool minimized = false;
	bool maximized = false;

	uint32_t *state;
	wl_array_for_each(state, states) switch (*state)
	{
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED: maximized = true; break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED: minimized = true; break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED: activated = true; break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN: fullscreen = true; break;
	}

	toplevel_set_fullscreen(toplevel, fullscreen);
	toplevel_set_activated(toplevel, activated);
	toplevel_set_minimized(toplevel, minimized);
	toplevel_set_maximized(toplevel, maximized);
}

static void zwlr_foreign_handle_handle_done (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct Toplevel *toplevel = (struct Toplevel *)data;
	toplevel_done(toplevel);
}

static const struct zwlr_foreign_toplevel_handle_v1_listener zwlr_handle_listener = {
	.app_id       = zwlr_foreign_handle_handle_app_id,
	.closed       = noop,
	.done         = zwlr_foreign_handle_handle_done,
	.output_enter = noop,
	.output_leave = noop,
	.parent       = noop,
	.state        = zwlr_foreign_handle_handle_state,
	.title        = zwlr_foreign_handle_handle_title,
};

static void zwlr_toplevel_manager_handle_toplevel (void *data,
		struct zwlr_foreign_toplevel_manager_v1 *manager,
		struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct Toplevel *toplevel = toplevel_new();
	if ( toplevel == NULL )
		return;
	toplevel->zwlr_handle = handle;
	zwlr_foreign_toplevel_handle_v1_add_listener(handle, &zwlr_handle_listener, toplevel);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener zwlr_toplevel_manager_listener = {
	.toplevel = zwlr_toplevel_manager_handle_toplevel,
	.finished = noop,
};

/************************
 *                      *
 *    Command output    *
 *                      *
 ************************/
static bool string_needs_quotes (char *str)
{
	for (; *str != '\0'; str++)
		if ( isspace(*str) || *str == '"' || *str == '\'' || !isascii(*str) )
			return true;
	return false;
}

static void quoted_fputs (size_t *len, char *str, FILE *restrict f)
{
	if ( str == NULL )
	{
		if ( len != NULL )
			*len = 0;
		return;
	}

	size_t l = 2; // Two bytes for the two mandatory quotes.

	fputc('"', f);
	for (; *str != '\0'; str++)
	{
		if ( *str == '"' )
		{
			l += 2;
			fputs("\\\"", f);
		}
		else if ( *str == '\n' )
		{
			l += 2;
			fputs("\\n", f);
		}
		else if ( *str == '\t' )
		{
			l += 2;
			fputs("\\t", f);
		}
		else
		{
			l += 1;
			fputc(*str, f);
		}
	}
	fputc('"', f);

	if ( len != NULL )
		*len = l;
}

static void write_padding (size_t used_len, size_t padding, FILE *restrict f)
{
	if ( padding > used_len )
		for (size_t i = padding - used_len; i > 0; i--)
			fputc(' ', f);
}

static void write_padded (size_t padding, char *str, FILE *restrict f)
{
	size_t len = 0;
	if ( str == NULL )
	{
		fputs("<NULL>", f);
		len = strlen("<NULL>");
	}
	else
	{
		len = strlen(str);
		fputs(str, f);
	}
	write_padding(len, padding, f);
}

static void write_padded_maybe_quoted (size_t padding, char *str, FILE *restrict f)
{
	size_t len = 0;
	if ( str == NULL )
	{
		fputs("<NULL>", f);
		len = strlen("<NULL>");
	}
	else  if (string_needs_quotes(str))
		quoted_fputs(&len, str, f);
	else
	{
		len = strlen(str);
		fputs(str, f);
	}
	write_padding(len, padding, f);
}

static void write_maybe_quoted (char *str, FILE *restrict f)
{
	if ( str == NULL )
		fputs("<NULL>", f);
	else  if (string_needs_quotes(str))
		quoted_fputs(NULL, str, f);
	else
		fputs(str, f);
}

/** Always quote strings, except if they are NULL. */
static void write_json (char *str, FILE *restrict f)
{
	if ( str == NULL )
		fputs("null", f);
	else
		quoted_fputs(NULL, str, f);
}

/** Never quote strings, print "<NULL>" on NULL. */
static void write_custom (char *str, FILE *restrict f)
{
	if ( str == NULL )
		fputs("<NULL>", f);
	else
		fputs(str, f);
}

static void write_custom_optional (bool supported, char *str, FILE *restrict f)
{
	if (supported)
		write_custom(str, f);
	else
		fputs("unsupported", f);
}

static void write_custom_optional_bool (bool supported, bool b, FILE *restrict f)
{
	if (supported)
		fputs(b ? "true" : "false", f);
	else
		fputs("unsupported", f);
}

/** Return the amount of bytes printed when printing the given string. */
static size_t real_strlen (const char *str)
{
	size_t i = 0;
	bool has_space = false;
	for(; *str != '\0'; str++)
	{
		switch (*str)
		{
			case '"':
			case '\\':
			case '\n':
			case '\t':
				i += 2;
				break;
			
			default:
				if (isspace(*str))
					has_space = true;
				i++;
				break;
		}
	}
	if (has_space)
		i += 2;
	return i;
}

/**
 * Checks whether a custom output format is valid. Prints error messages
 * accordingly.
 */
static bool out_check_custom_format (const char *fmt)
{
	assert(fmt != NULL);
	const size_t len = strlen(fmt);
	if ( len == 1 || len == 0 )
	{
		fputs("ERROR: Invalid custom format: Requires at least delimiter and one field.\n", stderr);
		return false;
	}
	if (!isascii(*fmt))
	{
		fputs("ERROR: Invalid custom format: Delimiter must be an ASCII character.\n", stderr);
		return false;
	}
	fmt++;
	for (; *fmt != '\0'; fmt++)
	{
		switch (*fmt)
		{
			case 't': // Title.
			case 'a': // App-Id.
			case 'i': // Identifier.
			case 'A': // Activated.
			case 'f': // Fullscreen.
			case 'm': // Minimized.
			case 'M': // Maximized.
				continue;

			default:
				fprintf(stderr, "ERROR: Invalid custom format: Unknown field name: '%c'.\n", *fmt);
				return false;
		}
	}

	return true;
}

static void out_write_toplevel (struct Toplevel *toplevel)
{
	static bool json_prev = false;
	switch (output_format)
	{
		case NORMAL:
			write_padded_maybe_quoted(longest_app_id, toplevel->app_id, stdout);
			fputs("   ", stdout);
			write_maybe_quoted(toplevel->title, stdout);
			fputc('\n', stdout);
			break;

		case JSON:
			if (json_prev)
				fputs(",\n", stdout);
			else
				json_prev = true;
			fputs("        {\n", stdout);

			if (support_activated)
				fprintf(stdout, "            \"activated\": %s,\n", BOOL_TO_STR(toplevel->activated));
			if (support_fullscreen)
				fprintf(stdout, "            \"fullscreen\": %s,\n", BOOL_TO_STR(toplevel->fullscreen));
			if (support_minimized)
				fprintf(stdout, "            \"minimized\": %s,\n", BOOL_TO_STR(toplevel->minimized));
			if (support_maximized)
				fprintf(stdout, "            \"maximized\": %s,\n", BOOL_TO_STR(toplevel->maximized));
			if (support_identifier)
				fprintf(stdout, "            \"identifier\": %s,\n", toplevel->identifier);

			/* Whoever designed JSON made the incredibly weird
			 * mistake of enforcing that there is no comma on the
			 * last item. Luckily, there are two fields we know
			 * will always be printed. So by putting them last,
			 * we can easiely implement that. :)
			 */
			fputs("            \"title\": ", stdout);
			write_json(toplevel->title, stdout);
			fputs(",\n            \"app-id\": ", stdout);
			write_json(toplevel->app_id, stdout);
			fputs("\n        }", stdout);
			break;

		case CUSTOM:
			assert(custom_output_format != NULL);
			assert(strlen(custom_output_format) > 1);
			bool need_delim = false;
			char *fmt = custom_output_format + 1;
			for (; *fmt != '\0'; fmt++)
			{
				if (need_delim)
					fputc(custom_output_format[0], stdout);
				else
					need_delim = true;
				switch (*fmt)
				{
					case 't': write_custom(toplevel->title, stdout); break;
					case 'a': write_custom(toplevel->app_id, stdout); break;
					case 'i': write_custom_optional(support_identifier, toplevel->identifier, stdout); break;
					case 'A': write_custom_optional_bool(support_activated, toplevel->activated, stdout); break;
					case 'f': write_custom_optional_bool(support_fullscreen, toplevel->fullscreen, stdout); break;
					case 'm': write_custom_optional_bool(support_minimized, toplevel->minimized, stdout); break;
					case 'M': write_custom_optional_bool(support_maximized, toplevel->maximized, stdout); break;
					default: assert(false); break;
				}
			}
			fputs("\n", stdout);
			break;
	}
}

static void out_start (void)
{
	switch (output_format)
	{
		case NORMAL:
			if (isatty(fileno(stdout))) fputs("\x1b[0;1m", stdout);
			write_padded(longest_app_id, "app-id:", stdout);
			fputs("   ", stdout);
			fputs("title:", stdout);
			fputc('\n', stdout);
			if (isatty(fileno(stdout))) fputs("\x1b[0m", stdout);
			return;

		case JSON:
			fprintf(stdout,
					"{\n"
					"    \"supported-data\": {\n"
					"        \"title\": true,\n"
					"        \"app-id\": true,\n"
					"        \"identifier\": %s,\n"
					"        \"fullscreen\": %s,\n"
					"        \"activated\": %s,\n"
					"        \"minimized\": %s,\n"
					"        \"maximized\": %s\n"
					"    },\n"
					"    \"toplevels\": [\n",
					BOOL_TO_STR(support_identifier),
					BOOL_TO_STR(support_fullscreen),
					BOOL_TO_STR(support_activated),
					BOOL_TO_STR(support_minimized),
					BOOL_TO_STR(support_maximized));
			break;

		case CUSTOM:
			break;
	}
}

static void out_finish (void)
{
	switch (output_format)
	{
		case NORMAL:
			return;

		case JSON:
			fputs("\n    ]\n}\n", stdout);
			break;

		case CUSTOM:
			break;
	}
}

/********************************
 *                              *
 *    main and Wayland logic    *
 *                              *
 ********************************/
static void registry_handle_global (void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	if ( strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0 )
	{
		/* No need to bind the zwlr interface if we already have the ext one. */
		if ( ext_toplevel_list != NULL )
			return;
		if ( version < 3 )
			return;
		DEBUG_LOG("Binding zwlr-foreign-toplevel-manager-v1.");
		zwlr_toplevel_manager = wl_registry_bind(wl_registry, name,
			&zwlr_foreign_toplevel_manager_v1_interface, 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(zwlr_toplevel_manager,
				&zwlr_toplevel_manager_listener, NULL);
	}
	else if ( strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0 )
	{
		/* No need to bind the ext interface if we already have the zwlr one. */
		if ( zwlr_toplevel_manager != NULL )
			return;
		DEBUG_LOG("Binding ext-foreign-toplevel-list-v1.");
		ext_toplevel_list = wl_registry_bind(wl_registry, name,
			&ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(ext_toplevel_list,
				&ext_toplevel_list_listener, NULL);
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
	static int sync = 0;
	DEBUG_LOG("sync callback %d.", sync);

	wl_callback_destroy(wl_callback);
	sync_callback = NULL;

	if ( sync == 0 )
	{
		/* First sync: The registry finished advertising globals.
		 * Now we can check whether we have everything we need.
		 */
		if ( zwlr_toplevel_manager == NULL && ext_toplevel_list == NULL )
		{
			const char *err_message =
				"ERROR: Wayland server supports none of the protocol extensions required for getting toplevel information:\n"
				"    -> zwlr-foreign-toplevel-management-unstable-v1, version 3 or higher\n"
				"    -> ext-foreign-toplevel-list-v1, version 1 or higher\n"
				"\n";
			fputs(err_message, stderr);
			ret = EXIT_FAILURE;
			loop = false;
			return;
		}

		sync++;
		sync_callback = wl_display_sync(wl_display);
		wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);

		// TODO if there are extension protocol for ext_foreign_toplevel_list
		//      to get extra information, we may need one additional sync.
		//      So check if any of those are bound and then add one step
		//      if necessary.
	}
	else
	{
		/* Second sync: Now we have received all toplevel handles and
		 * their events. Time to leave the main loop, print all data and
		 * exit.
		 */
		update_capabilities();
		loop = false;
	}
}

static void dump_and_free_data (void)
{
	out_start();
	struct Toplevel *t, *tmp;
	wl_list_for_each_reverse_safe(t, tmp, &toplevels, link)
	{
		out_write_toplevel(t);
		toplevel_destroy(t);
	}
	out_finish();
}

static void free_data (void)
{
	struct Toplevel *t, *tmp;
	wl_list_for_each_safe(t, tmp, &toplevels, link)
		toplevel_destroy(t);
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
		"│             lswt has crashed.            │\n"
		"│                                          │\n"
		"│   This is most likely a bug, so please   │\n"
		"│     report this to the mailing list.     │\n"
		"│                                          │\n"
		"│  ~leon_plickat/public-inbox@lists.sr.ht  │\n"
		"│                                          │\n"
		"└──────────────────────────────────────────┘\n"
		"\n";
	fputs(msg, stderr);

	/* Set up the default handlers to deal with the rest. We do this before
	 * attempting to get a backtrace, because sometimes that could also
	 * cause a SEGFAULT and we don't want a funny signal loop to happen.
	 */
	signal(signum, SIG_DFL);

#ifdef __linux__
#ifdef __GLIBC__
	fputs("Attempting to get backtrace:\n", stderr);
	void *buffer[255];
	const int calls = backtrace(buffer, sizeof(buffer) / sizeof(void *));
	backtrace_symbols_fd(buffer, calls, fileno(stderr));
	fputs("\n", stderr);
#endif
#endif

	/* Easiest way of calling the default signal handler. */
	kill(getpid(), signum);
}

// TODO replace this once landlock lands in libc.
#ifdef __linux__
#ifndef landlock_create_ruleset
static inline long int landlock_create_ruleset(const struct landlock_ruleset_attr *const attr,
		const size_t size, const __u32 flags)
{
	return syscall(__NR_landlock_create_ruleset, attr, size, flags);
}
#endif
#endif

static void init_landlock (void)
{
#ifdef __linux__
	const long int abi_version = landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
	if (abi_version < 0)
		return;
#endif
}

int main(int argc, char *argv[])
{
	signal(SIGSEGV, handle_error);
	signal(SIGFPE, handle_error);
	init_landlock();

	if ( argc > 0 ) for (int i = 1; i < argc; i++)
	{
		if ( strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0 )
		{
			if ( output_format != NORMAL )
			{
				fputs("ERROR: output format may only be specified once.", stderr);
				ret = EXIT_FAILURE;
				goto cleanup;
			}
			output_format = JSON;
		}
		else if ( strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--custom") == 0 )
		{
			if ( output_format != NORMAL )
			{
				fputs("ERROR: output format may only be specified once.", stderr);
				ret = EXIT_FAILURE;
				goto cleanup;
			}
			if ( argc == i + 1 )
			{
				fprintf(stderr, "ERROR: flag '%s' requires a parameter.", argv[i]);
				ret = EXIT_FAILURE;
				goto cleanup;
			}
			if (!out_check_custom_format(argv[i+1]))
			{
				ret = EXIT_FAILURE;
				goto cleanup;
			}
			output_format = CUSTOM;
			custom_output_format = strdup(argv[i+1]);
			i++;
		}
		else if ( strcmp(argv[i], "--debug") == 0 )
			debug_log = true;
		else if ( strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0 )
		{
			fputs("lswt version " VERSION "\n", stderr);
			ret = EXIT_SUCCESS;
			goto cleanup;
		}
		else if ( strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 )
		{
			fputs(usage, stderr);
			ret = EXIT_SUCCESS;
			goto cleanup;
		}
		else
		{
			fprintf(stderr, "Invalid option: %s\n", argv[i]);
			fputs(usage, stderr);
			ret = EXIT_FAILURE;
			goto cleanup;
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
		ret = EXIT_FAILURE;
		goto cleanup;
	}
	DEBUG_LOG("Trying to connect to display '%s'.", display_name);

	/* Behold: If this succeeds, we may no longer goto cleanup, because
	 * Wayland magic happens, which can cause Toplevels to be allocated.
	 */
	wl_display = wl_display_connect(display_name);
	if ( wl_display == NULL )
	{
		fputs("ERROR: Can not connect to wayland display.\n", stderr);
		ret = EXIT_FAILURE;
		goto cleanup;
	}

	wl_list_init(&toplevels);
	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &registry_listener, NULL);

	sync_callback = wl_display_sync(wl_display);
	wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);

	DEBUG_LOG("Entering main loop.");
	while ( loop && wl_display_dispatch(wl_display) > 0 );

	/* If nothing went wrong in the main loop we can print and free all data,
	 * otherwise just free it.
	 */
	if ( ret == EXIT_SUCCESS )
		dump_and_free_data();
	else
		free_data();

	DEBUG_LOG("Cleaning up Wayland interfaces.");
	if ( sync_callback != NULL )
		wl_callback_destroy(sync_callback);
	if ( zwlr_toplevel_manager != NULL )
		zwlr_foreign_toplevel_manager_v1_destroy(zwlr_toplevel_manager);
	if ( ext_toplevel_list != NULL )
		ext_foreign_toplevel_list_v1_destroy(ext_toplevel_list);
	if ( wl_registry != NULL )
		wl_registry_destroy(wl_registry);
	wl_display_disconnect(wl_display);

cleanup:
	if ( custom_output_format != NULL )
		free(custom_output_format);

	return ret;
}

