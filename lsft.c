/*
 * lsft - list foreign toplevels
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

#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1.h"

bool loop = true;
struct wl_display *wl_display = NULL;
struct wl_registry *wl_registry = NULL;
struct wl_callback *sync_callback = NULL;
struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager = NULL;

static void noop () {}

static void handle_handle_title (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *title)
{
	fprintf(stdout, " title=\"%s\"", title);
}

static void handle_handle_app_id (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		const char *title)
{
	fprintf(stdout, " app-id=\"%s\"", title);
}

static void handle_handle_done (void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	fputs("\n", stdout);
	zwlr_foreign_toplevel_handle_v1_destroy(handle);
}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
	.title        = handle_handle_title,
	.app_id       = handle_handle_app_id,
	.output_enter = noop,
	.output_leave = noop,
	.state        = noop,
	.done         = handle_handle_done,
	.closed       = noop,
};

static void toplevel_manager_handle_toplevel (void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
		struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, NULL);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
	.toplevel = toplevel_manager_handle_toplevel,
	.finished = noop,
};

static void registry_handle_global (void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	// Bind zwlr_foreign_toplevel_manager_v1
	if (! strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name))
	{
		toplevel_manager = wl_registry_bind(registry, name,
			&zwlr_foreign_toplevel_manager_v1_interface, 2);
		zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager,
				&toplevel_manager_listener, NULL);
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

	static int run = 0;
	if ( run == 0 )
	{
		/* First sync: Registry finished advertising stuff. */
		if ( toplevel_manager == NULL )
		{
			fputs("ERROR: Wayland server does not support foreign-toplevel-management-unstable-v1.\n", stderr);
			loop = false;
		}

		sync_callback = wl_display_sync(wl_display);
		wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);
	}
	else
	{
		/* Second sync: Now we have received all toplevel handles and their events. */
		loop = false;
	}

	run++;
}

int main(int argc, char *argv[])
{
	wl_display = wl_display_connect(NULL);
	if ( wl_display == NULL )
	{
		fputs("ERROR: Can not connect to wayland display.\n", stderr);
		return EXIT_FAILURE;
	}

	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &registry_listener, NULL);

	sync_callback = wl_display_sync(wl_display);
	wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);

	while (loop)
		if (! wl_display_dispatch(wl_display))
			break;

	if ( toplevel_manager != NULL )
		zwlr_foreign_toplevel_manager_v1_destroy(toplevel_manager);
	if ( wl_registry != NULL )
		wl_registry_destroy(wl_registry);
	wl_display_disconnect(wl_display);

	return EXIT_SUCCESS;
}

