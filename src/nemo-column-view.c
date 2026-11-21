/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/* nemo-column-view.c - Miller column (Finder style) view of a directory.

   The Gnome Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 51 Franklin Street - Suite 500,
   Boston, MA 02110-1335, USA.
*/

#include <config.h>

#include "nemo-column-view.h"

#include "nemo-list-model.h"
#include "nemo-view-dnd.h"
#include "nemo-view-factory.h"
#include "nemo-window.h"
#include "nemo-window-slot.h"

#include <eel/eel-vfs-extensions.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <libnemo-private/nemo-directory.h>
#include <libnemo-private/nemo-file.h>
#include <libnemo-private/nemo-global-preferences.h>
#include <libnemo-private/nemo-icon-info.h>

#define COLUMN_VIEW_WIDTH 220
#define COLUMN_VIEW_ICON_SIZE 16

enum {
	COLUMN_ICON,
	COLUMN_NAME,
	COLUMN_URI,
	COLUMN_IS_DIRECTORY,
	COLUMN_NUM_COLUMNS
};

enum {
	PENDING_NONE,
	PENDING_CONTEXT_MENU,
	PENDING_OPEN
};

typedef struct {
	NemoColumnView *view;
	GtkWidget *scroller;
	GtkWidget *tree_view;
	GtkListStore *store;
	GtkTreeModelSort *sort;
	NemoDirectory *directory;
	GFile *location;
	char *highlight_uri;
	gboolean show_hidden;
	gboolean dragging;
	GtkEventController *key_controller;
} NemoColumnColumn;

struct NemoColumnViewDetails {
	GtkWidget *hbox;
	GtkWidget *current_scroller;
	GtkTreeView *current_tree_view;
	GList *columns;

	guint scroll_id;

	guint pending_timeout_id;
	gint pending_action;
	char *pending_uri;

	gboolean current_dragging;
	GtkEventController *current_key_controller;
};

G_DEFINE_TYPE (NemoColumnView, nemo_column_view, NEMO_TYPE_LIST_VIEW);

static const GtkTargetEntry drag_targets[] = {
	{ "text/uri-list", 0, 0 },
	{ "x-special/gnome-copied-files", 0, 1 }
};

static const GtkTargetEntry drop_targets[] = {
	{ "text/uri-list", 0, 0 },
	{ "x-special/gnome-copied-files", 0, 1 }
};

static void column_data_free (NemoColumnColumn *column, gboolean destroy_widget);

static gboolean column_button_press_cb (GtkWidget *widget, GdkEventButton *event, gpointer data);
static gboolean column_button_release_cb (GtkWidget *widget, GdkEventButton *event, gpointer data);
static gboolean column_key_pressed_cb (GtkEventControllerKey *controller,
				       guint keyval,
				       guint keycode,
				       GdkModifierType state,
				       gpointer data);
static void column_drag_begin_cb (GtkWidget *widget, GdkDragContext *context, gpointer data);
static void column_drag_end_cb (GtkWidget *widget, GdkDragContext *context, gpointer data);
static void column_drag_data_get_cb (GtkWidget *widget,
				     GdkDragContext *context,
				     GtkSelectionData *selection_data,
				     guint info,
				     guint time,
				     gpointer data);
static void column_drag_data_received_cb (GtkWidget *widget,
					  GdkDragContext *context,
					  gint x,
					  gint y,
					  GtkSelectionData *selection_data,
					  guint info,
					  guint time,
					  gpointer data);

static char *
uri_for_tree_path (GtkTreeView *tree_view, GtkTreePath *path)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	char *uri = NULL;

	model = gtk_tree_view_get_model (tree_view);
	if (path == NULL || !gtk_tree_model_get_iter (model, &iter, path)) {
		return NULL;
	}

	gtk_tree_model_get (model, &iter,
			    COLUMN_URI, &uri,
			    -1);

	return uri;
}

static gboolean
tree_path_is_directory (GtkTreeView *tree_view, GtkTreePath *path)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gboolean is_directory = FALSE;

	model = gtk_tree_view_get_model (tree_view);
	if (path == NULL || !gtk_tree_model_get_iter (model, &iter, path)) {
		return FALSE;
	}

	gtk_tree_model_get (model, &iter,
			    COLUMN_IS_DIRECTORY, &is_directory,
			    -1);

	return is_directory;
}

static GFile *
location_from_uri (const char *uri)
{
	if (uri == NULL) {
		return NULL;
	}
	return g_file_new_for_uri (uri);
}

static void
navigate_to_location (NemoColumnView *view, GFile *location)
{
	NemoWindowSlot *slot;

	if (location == NULL) {
		return;
	}

	slot = nemo_view_get_nemo_window_slot (NEMO_VIEW (view));
	if (slot != NULL) {
		nemo_window_slot_open_location (slot, location, 0);
	}
}

static GdkPixbuf *
icon_for_file (NemoFile *file, GtkWidget *widget)
{
	NemoIconInfo *icon_info;
	GdkPixbuf *pixbuf;
	int scale;

	scale = gtk_widget_get_scale_factor (widget);
	icon_info = nemo_file_get_icon (file, COLUMN_VIEW_ICON_SIZE, 0, scale,
					NEMO_FILE_ICON_FLAGS_NONE);
	if (icon_info == NULL) {
		return NULL;
	}

	pixbuf = nemo_icon_info_get_pixbuf_at_size (icon_info, COLUMN_VIEW_ICON_SIZE);
	nemo_icon_info_unref (icon_info);

	return pixbuf;
}

static void
column_remove_uri (NemoColumnColumn *column, const char *uri)
{
	GtkTreeIter iter;
	gboolean valid;

	valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (column->store), &iter);
	while (valid) {
		char *row_uri = NULL;

		gtk_tree_model_get (GTK_TREE_MODEL (column->store), &iter,
				    COLUMN_URI, &row_uri,
				    -1);

		if (g_strcmp0 (row_uri, uri) == 0) {
			g_free (row_uri);
			gtk_list_store_remove (column->store, &iter);
			return;
		}

		g_free (row_uri);
		valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (column->store), &iter);
	}
}

static void
column_select_highlight (NemoColumnColumn *column)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gboolean valid;

	if (column->highlight_uri == NULL) {
		return;
	}

	model = GTK_TREE_MODEL (column->sort);
	valid = gtk_tree_model_get_iter_first (model, &iter);
	while (valid) {
		char *uri = NULL;

		gtk_tree_model_get (model, &iter,
				    COLUMN_URI, &uri,
				    -1);

		if (g_strcmp0 (uri, column->highlight_uri) == 0) {
			GtkTreePath *path;

			path = gtk_tree_model_get_path (model, &iter);
			gtk_tree_selection_select_iter (
				gtk_tree_view_get_selection (GTK_TREE_VIEW (column->tree_view)),
				&iter);
			gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (column->tree_view),
						      path, NULL, FALSE, 0.0, 0.0);
			gtk_tree_path_free (path);
			g_free (uri);
			return;
		}

		g_free (uri);
		valid = gtk_tree_model_iter_next (model, &iter);
	}
}

static void
column_add_file (NemoColumnColumn *column, NemoFile *file)
{
	GtkTreeIter iter;
	GdkPixbuf *pixbuf;
	char *name;
	char *uri;

	if (nemo_file_is_gone (file)) {
		return;
	}

	if (!column->show_hidden && nemo_file_is_hidden_file (file)) {
		return;
	}

	name = nemo_file_get_display_name (file);
	uri = nemo_file_get_uri (file);
	if (name == NULL || uri == NULL) {
		g_free (name);
		g_free (uri);
		return;
	}

	column_remove_uri (column, uri);

	pixbuf = icon_for_file (file, column->tree_view);

	gtk_list_store_append (column->store, &iter);
	gtk_list_store_set (column->store, &iter,
			    COLUMN_ICON, pixbuf,
			    COLUMN_NAME, name,
			    COLUMN_URI, uri,
			    COLUMN_IS_DIRECTORY, nemo_file_is_directory (file),
			    -1);

	if (pixbuf != NULL) {
		g_object_unref (pixbuf);
	}

	if (g_strcmp0 (uri, column->highlight_uri) == 0) {
		column_select_highlight (column);
	}

	g_free (name);
	g_free (uri);
}

static void
column_files_added_cb (NemoDirectory *directory, GList *files, gpointer data)
{
	NemoColumnColumn *column = data;
	GList *l;

	for (l = files; l != NULL; l = l->next) {
		column_add_file (column, NEMO_FILE (l->data));
	}
}

static void
column_files_changed_cb (NemoDirectory *directory, GList *files, gpointer data)
{
	NemoColumnColumn *column = data;
	GList *l;

	for (l = files; l != NULL; l = l->next) {
		NemoFile *file = NEMO_FILE (l->data);

		if (nemo_file_is_gone (file)) {
			char *uri = nemo_file_get_uri (file);
			if (uri != NULL) {
				column_remove_uri (column, uri);
				g_free (uri);
			}
		} else {
			column_add_file (column, file);
		}
	}
}

static gint
column_sort_func (GtkTreeModel *model,
		  GtkTreeIter *a,
		  GtkTreeIter *b,
		  gpointer user_data)
{
	gboolean a_is_directory, b_is_directory;
	char *a_name, *b_name;
	gint result;

	gtk_tree_model_get (model, a,
			    COLUMN_IS_DIRECTORY, &a_is_directory,
			    COLUMN_NAME, &a_name,
			    -1);
	gtk_tree_model_get (model, b,
			    COLUMN_IS_DIRECTORY, &b_is_directory,
			    COLUMN_NAME, &b_name,
			    -1);

	if (a_is_directory != b_is_directory) {
		result = a_is_directory ? -1 : 1;
	} else {
		result = g_utf8_collate (a_name != NULL ? a_name : "",
					 b_name != NULL ? b_name : "");
	}

	g_free (a_name);
	g_free (b_name);

	return result;
}

static void
column_scroller_destroyed_cb (GtkWidget *widget, gpointer data)
{
	NemoColumnColumn *column = data;

	column->scroller = NULL;
	column->tree_view = NULL;
}

static NemoColumnColumn *
column_new (NemoColumnView *view, GFile *location, char *highlight_uri)
{
	NemoColumnColumn *column;
	NemoColumnViewDetails *details;
	GtkTreeViewColumn *tree_column;
	GtkCellRenderer *renderer;
	GtkWidget *scroller;
	GtkWidget *tree_view;
	GtkTreeSelection *selection;
	GtkEventController *controller;
	gboolean show_hidden;

	details = view->details;

	column = g_new0 (NemoColumnColumn, 1);
	column->view = view;
	column->location = g_object_ref (location);
	column->highlight_uri = highlight_uri;

	scroller = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
					GTK_POLICY_NEVER,
					GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroller),
					     GTK_SHADOW_IN);
	gtk_widget_set_size_request (scroller, COLUMN_VIEW_WIDTH, -1);

	column->store = gtk_list_store_new (COLUMN_NUM_COLUMNS,
					    GDK_TYPE_PIXBUF,
					    G_TYPE_STRING,
					    G_TYPE_STRING,
					    G_TYPE_BOOLEAN);

	column->sort = GTK_TREE_MODEL_SORT (
		gtk_tree_model_sort_new_with_model (GTK_TREE_MODEL (column->store)));
	gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (column->sort), 0,
					 column_sort_func, NULL, NULL);
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (column->sort), 0,
					      GTK_SORT_ASCENDING);

	tree_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (column->sort));
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (tree_view), FALSE);
	gtk_tree_view_set_enable_search (GTK_TREE_VIEW (tree_view), FALSE);
	gtk_tree_view_set_rubber_banding (GTK_TREE_VIEW (tree_view), FALSE);

	tree_column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_expand (tree_column, TRUE);
	gtk_tree_view_column_set_spacing (tree_column, 4);

	renderer = gtk_cell_renderer_pixbuf_new ();
	g_object_set (renderer, "xpad", 1, NULL);
	gtk_tree_view_column_pack_start (tree_column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (tree_column, renderer,
					     "pixbuf", COLUMN_ICON,
					     NULL);

	renderer = gtk_cell_renderer_text_new ();
	g_object_set (renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	gtk_tree_view_column_pack_start (tree_column, renderer, TRUE);
	gtk_tree_view_column_set_attributes (tree_column, renderer,
					     "text", COLUMN_NAME,
					     NULL);

	gtk_tree_view_append_column (GTK_TREE_VIEW (tree_view), tree_column);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (tree_view));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);

	gtk_container_add (GTK_CONTAINER (scroller), tree_view);
	gtk_box_pack_start (GTK_BOX (details->hbox), scroller, FALSE, FALSE, 0);
	gtk_box_reorder_child (GTK_BOX (details->hbox), scroller,
			       g_list_length (details->columns));

	column->scroller = scroller;
	column->tree_view = tree_view;

	g_signal_connect (scroller, "destroy",
			  G_CALLBACK (column_scroller_destroyed_cb), column);

	g_signal_connect (tree_view, "button-press-event",
			  G_CALLBACK (column_button_press_cb), column);
	g_signal_connect (tree_view, "button-release-event",
			  G_CALLBACK (column_button_release_cb), column);

	controller = gtk_event_controller_key_new (tree_view);
	gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
	g_signal_connect (controller, "key-pressed",
			  G_CALLBACK (column_key_pressed_cb), column);
	column->key_controller = controller;

	gtk_drag_source_set (tree_view, GDK_BUTTON1_MASK,
			     drag_targets, G_N_ELEMENTS (drag_targets),
			     GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
	g_signal_connect (tree_view, "drag-begin",
			  G_CALLBACK (column_drag_begin_cb), column);
	g_signal_connect (tree_view, "drag-end",
			  G_CALLBACK (column_drag_end_cb), column);
	g_signal_connect (tree_view, "drag-data-get",
			  G_CALLBACK (column_drag_data_get_cb), column);

	gtk_drag_dest_set (tree_view, GTK_DEST_DEFAULT_ALL,
			   drop_targets, G_N_ELEMENTS (drop_targets),
			   GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
	g_signal_connect (tree_view, "drag-data-received",
			  G_CALLBACK (column_drag_data_received_cb), column);

	gtk_widget_show_all (scroller);

	column->directory = nemo_directory_get (location);
	g_signal_connect (column->directory, "files_added",
			  G_CALLBACK (column_files_added_cb), column);
	g_signal_connect (column->directory, "files_changed",
			  G_CALLBACK (column_files_changed_cb), column);

	show_hidden = g_settings_get_boolean (nemo_preferences,
					      NEMO_PREFERENCES_SHOW_HIDDEN_FILES);
	column->show_hidden = show_hidden;

	nemo_directory_file_monitor_add (column->directory, column, show_hidden,
					 NEMO_FILE_ATTRIBUTES_FOR_ICON |
					 NEMO_FILE_ATTRIBUTE_INFO,
					 column_files_added_cb, column);

	details->columns = g_list_append (details->columns, column);

	return column;
}

static void
column_data_free (NemoColumnColumn *column, gboolean destroy_widget)
{
	if (column->directory != NULL) {
		nemo_directory_file_monitor_remove (column->directory, column);
		g_signal_handlers_disconnect_by_data (column->directory, column);
		nemo_directory_unref (column->directory);
		column->directory = NULL;
	}

	/* GTK3 event controllers are not owned by their widget */
	g_clear_object (&column->key_controller);

	if (destroy_widget && column->scroller != NULL) {
		gtk_widget_destroy (column->scroller);
	}

	if (column->sort != NULL) {
		g_object_unref (column->sort);
		column->sort = NULL;
	}
	if (column->store != NULL) {
		g_object_unref (column->store);
		column->store = NULL;
	}

	g_clear_object (&column->location);
	g_free (column->highlight_uri);
	g_free (column);
}

static void
clear_ancestor_columns (NemoColumnView *view)
{
	NemoColumnViewDetails *details = view->details;
	GList *l;

	for (l = details->columns; l != NULL; l = l->next) {
		column_data_free (l->data, TRUE);
	}
	g_list_free (details->columns);
	details->columns = NULL;
}

static void
scroll_to_end (NemoColumnView *view)
{
	GtkAdjustment *adjustment;
	gdouble upper, page_size;

	adjustment = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (view));
	if (adjustment == NULL) {
		return;
	}

	upper = gtk_adjustment_get_upper (adjustment);
	page_size = gtk_adjustment_get_page_size (adjustment);
	gtk_adjustment_set_value (adjustment, MAX (upper - page_size, 0.0));
}

static gboolean
scroll_to_end_idle (gpointer data)
{
	NemoColumnView *view = data;

	view->details->scroll_id = 0;
	scroll_to_end (view);
	gtk_widget_queue_draw (view->details->hbox);

	return G_SOURCE_REMOVE;
}

static void
schedule_scroll_to_end (NemoColumnView *view)
{
	if (view->details->scroll_id != 0) {
		return;
	}

	view->details->scroll_id = g_idle_add_full (G_PRIORITY_DEFAULT,
						    scroll_to_end_idle,
						    g_object_ref (view),
						    (GDestroyNotify) g_object_unref);
}

static GList *
build_location_chain (GFile *location, gboolean *contains_home, GFile **home_out)
{
	GList *chain = NULL;
	GFile *home;
	GFile *current;
	GFile *parent;

	home = g_file_new_for_path (g_get_home_dir ());
	*contains_home = FALSE;
	*home_out = home;

	current = g_object_ref (location);
	chain = g_list_prepend (chain, current);

	while ((parent = g_file_get_parent (current)) != NULL) {
		chain = g_list_prepend (chain, parent);
		current = parent;
	}

	for (GList *l = chain; l != NULL; l = l->next) {
		if (g_file_equal (G_FILE (l->data), home)) {
			*contains_home = TRUE;
			break;
		}
	}

	return chain;
}

static void
rebuild_ancestor_columns (NemoColumnView *view)
{
	NemoDirectory *model;
	GFile *location;
	GFile *home;
	GList *chain;
	GList *start = NULL;
	gboolean contains_home;

	clear_ancestor_columns (view);

	model = nemo_view_get_model (NEMO_VIEW (view));
	if (model == NULL) {
		return;
	}

	location = nemo_directory_get_location (model);
	if (location == NULL) {
		return;
	}

	chain = build_location_chain (location, &contains_home, &home);

	if (contains_home) {
		for (GList *l = chain; l != NULL; l = l->next) {
			if (g_file_equal (G_FILE (l->data), home)) {
				start = l;
				break;
			}
		}
	}

	if (start == NULL) {
		start = chain;
	}

	for (GList *l = start; l != NULL && l->next != NULL; l = l->next) {
		GFile *directory = G_FILE (l->data);
		GFile *child = G_FILE (l->next->data);
		char *child_uri;

		child_uri = g_file_get_uri (child);
		column_new (view, directory, child_uri);
	}

	gtk_widget_queue_resize (view->details->hbox);
	gtk_widget_queue_resize (GTK_WIDGET (view));
	gtk_widget_queue_draw (view->details->hbox);
	gtk_widget_queue_draw (GTK_WIDGET (view));

	g_list_free_full (chain, g_object_unref);
	g_object_unref (home);
	g_object_unref (location);
}

static void
configure_current_tree_view (NemoColumnView *view)
{
	NemoColumnViewDetails *details = view->details;
	GList *columns;
	GList *l;
	GtkTreeViewColumn *name_column = NULL;

	if (details->current_tree_view == NULL) {
		return;
	}

	gtk_tree_view_set_headers_visible (details->current_tree_view, FALSE);

	columns = gtk_tree_view_get_columns (details->current_tree_view);
	for (l = columns; l != NULL; l = l->next) {
		GtkTreeViewColumn *column = GTK_TREE_VIEW_COLUMN (l->data);

		if (g_strcmp0 (gtk_tree_view_column_get_title (column), _("Name")) == 0) {
			name_column = column;
			break;
		}
	}

	if (name_column == NULL && columns != NULL) {
		name_column = GTK_TREE_VIEW_COLUMN (columns->data);
	}

	for (l = columns; l != NULL; l = l->next) {
		gtk_tree_view_column_set_visible (GTK_TREE_VIEW_COLUMN (l->data),
						  GTK_TREE_VIEW_COLUMN (l->data) == name_column);
	}

	if (name_column != NULL) {
		gtk_tree_view_column_set_expand (name_column, TRUE);
	}

	g_list_free (columns);
}

static void
select_pending_file (NemoColumnView *view)
{
	NemoColumnViewDetails *details = view->details;
	NemoFile *file;
	GList *selection;

	if (details->pending_uri == NULL) {
		return;
	}

	file = nemo_file_get_by_uri (details->pending_uri);
	if (file == NULL) {
		return;
	}

	selection = g_list_append (NULL, file);
	nemo_view_set_selection (NEMO_VIEW (view), selection);
	g_list_free (selection);
	nemo_file_unref (file);
}

static GdkEvent *
create_popup_event (NemoColumnView *view)
{
	GdkDisplay *display;
	GdkSeat *seat;
	GdkDevice *pointer;
	GdkWindow *window;
	GdkEvent *event;
	gint x, y;

	display = gtk_widget_get_display (GTK_WIDGET (view));
	window = gtk_widget_get_window (gtk_widget_get_toplevel (GTK_WIDGET (view)));
	if (window == NULL) {
		return NULL;
	}

	seat = gdk_display_get_default_seat (display);
	pointer = seat != NULL ? gdk_seat_get_pointer (seat) : NULL;
	if (pointer != NULL) {
		gdk_device_get_position (pointer, NULL, &x, &y);
	} else {
		x = 0;
		y = 0;
	}

	event = gdk_event_new (GDK_BUTTON_PRESS);
	if (pointer != NULL) {
		gdk_event_set_device (event, pointer);
	}
	event->button.window = g_object_ref (window);
	event->button.x = x;
	event->button.y = y;
	event->button.x_root = x;
	event->button.y_root = y;
	event->button.button = 3;
	event->button.time = GDK_CURRENT_TIME;

	return event;
}

static gboolean
perform_pending_action (gpointer data)
{
	NemoColumnView *view = data;
	NemoColumnViewDetails *details = view->details;
	gint action;

	details->pending_timeout_id = 0;
	action = details->pending_action;

	if (action != PENDING_NONE) {
		details->pending_action = PENDING_NONE;

		if (action == PENDING_CONTEXT_MENU) {
			GdkEvent *event = create_popup_event (view);

			if (details->pending_uri != NULL) {
				select_pending_file (view);
			}

			if (event != NULL) {
				if (details->pending_uri != NULL) {
					nemo_view_pop_up_selection_context_menu (NEMO_VIEW (view),
										 &event->button);
				} else {
					nemo_view_pop_up_background_context_menu (NEMO_VIEW (view),
										  &event->button);
				}
				gdk_event_free (event);
			}
		} else if (action == PENDING_OPEN) {
			NemoFile *file = details->pending_uri != NULL ?
				nemo_file_get_by_uri (details->pending_uri) : NULL;
			if (file != NULL) {
				nemo_view_activate_file (NEMO_VIEW (view), file, 0);
				nemo_file_unref (file);
			}
		}
	}

	g_free (details->pending_uri);
	details->pending_uri = NULL;

	return G_SOURCE_REMOVE;
}

static void
schedule_pending_action (NemoColumnView *view,
			 gint action,
			 const char *uri)
{
	NemoColumnViewDetails *details = view->details;

	if (details->pending_timeout_id != 0) {
		g_source_remove (details->pending_timeout_id);
		details->pending_timeout_id = 0;
	}

	g_free (details->pending_uri);
	details->pending_uri = g_strdup (uri);
	details->pending_action = action;

	details->pending_timeout_id =
		g_timeout_add_full (G_PRIORITY_DEFAULT, 1500,
				    perform_pending_action,
				    g_object_ref (view),
				    (GDestroyNotify) g_object_unref);
}

static void
queue_context_menu (NemoColumnColumn *column, GdkEventButton *event, gboolean has_selection)
{
	char *uri = NULL;

	if (has_selection) {
		GtkTreePath *path = NULL;

		gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (column->tree_view),
					       event->x, event->y,
					       &path, NULL, NULL, NULL);
		if (path != NULL) {
			uri = uri_for_tree_path (GTK_TREE_VIEW (column->tree_view), path);
			gtk_tree_path_free (path);
		}
	}

	schedule_pending_action (column->view,
				 PENDING_CONTEXT_MENU,
				 uri);
	g_free (uri);

	navigate_to_location (column->view, column->location);
}

static void
queue_open_file (NemoColumnColumn *column, const char *uri)
{
	if (uri == NULL) {
		return;
	}

	schedule_pending_action (column->view, PENDING_OPEN, uri);
	navigate_to_location (column->view, column->location);
}

static gboolean
column_button_press_cb (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	NemoColumnColumn *column = data;
	GtkTreePath *path = NULL;

	if (event->type != GDK_BUTTON_PRESS && event->type != GDK_2BUTTON_PRESS) {
		return FALSE;
	}

	gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (widget),
				       event->x, event->y,
				       &path, NULL, NULL, NULL);

	if (event->button == 3) {
		queue_context_menu (column, event, path != NULL);
		if (path != NULL) {
			gtk_tree_path_free (path);
		}
		return TRUE;
	}

	if (event->button == 1 && event->type == GDK_2BUTTON_PRESS && path != NULL) {
		if (!tree_path_is_directory (GTK_TREE_VIEW (widget), path)) {
			char *uri = uri_for_tree_path (GTK_TREE_VIEW (widget), path);
			queue_open_file (column, uri);
			g_free (uri);
			gtk_tree_path_free (path);
			return TRUE;
		}
	}

	if (path != NULL) {
		gtk_tree_path_free (path);
	}

	return FALSE;
}

static gboolean
column_button_release_cb (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	NemoColumnColumn *column = data;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;

	if (event->button != 1 || column->dragging) {
		return FALSE;
	}

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
	if (!gtk_tree_selection_get_selected (selection, &model, &iter)) {
		return FALSE;
	}

	{
		GtkTreePath *path = gtk_tree_model_get_path (model, &iter);
		gboolean is_directory = tree_path_is_directory (GTK_TREE_VIEW (widget), path);

		gtk_tree_path_free (path);

		if (is_directory) {
			char *uri = NULL;
			GFile *location;

			gtk_tree_model_get (model, &iter, COLUMN_URI, &uri, -1);
			location = location_from_uri (uri);
			if (location != NULL) {
				navigate_to_location (column->view, location);
				g_object_unref (location);
			}
			g_free (uri);
		}
	}

	return FALSE;
}

static void
column_focus_previous (NemoColumnColumn *column)
{
	NemoColumnViewDetails *details = column->view->details;
	NemoColumnColumn *previous;
	gint index;

	index = g_list_index (details->columns, column);
	if (index <= 0) {
		return;
	}

	previous = g_list_nth_data (details->columns, index - 1);
	if (previous == NULL || previous->tree_view == NULL) {
		return;
	}

	gtk_widget_grab_focus (previous->tree_view);
	column_select_highlight (previous);
}

static gboolean
column_key_pressed_cb (GtkEventControllerKey *controller,
		       guint keyval,
		       guint keycode,
		       GdkModifierType state,
		       gpointer data)
{
	NemoColumnColumn *column = data;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;

	if (state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK)) {
		return FALSE;
	}

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (column->tree_view));

	if (!gtk_tree_selection_get_selected (selection, &model, &iter)) {
		return FALSE;
	}

	if (keyval == GDK_KEY_Right || keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
		gboolean is_directory = FALSE;
		char *uri = NULL;

		gtk_tree_model_get (model, &iter,
				    COLUMN_URI, &uri,
				    COLUMN_IS_DIRECTORY, &is_directory,
				    -1);

		if (is_directory && uri != NULL) {
			GFile *location = location_from_uri (uri);
			if (location != NULL) {
				navigate_to_location (column->view, location);
				g_object_unref (location);
			}
			g_free (uri);
			return TRUE;
		}

		if (keyval != GDK_KEY_Right && uri != NULL) {
			queue_open_file (column, uri);
			g_free (uri);
			return TRUE;
		}

		g_free (uri);
		return keyval == GDK_KEY_Right;
	}

	if (keyval == GDK_KEY_Left) {
		column_focus_previous (column);
		return TRUE;
	}

	return FALSE;
}

static gboolean
current_key_pressed_cb (GtkEventControllerKey *controller,
			guint keyval,
			guint keycode,
			GdkModifierType state,
			gpointer data)
{
	NemoColumnView *view = data;
	NemoColumnViewDetails *details = view->details;

	if (state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK)) {
		return FALSE;
	}

	if (keyval == GDK_KEY_Right) {
		GList *selection = nemo_view_get_selection (NEMO_VIEW (view));

		if (selection != NULL) {
			NemoFile *file = NEMO_FILE (selection->data);
			if (nemo_file_is_directory (file)) {
				nemo_view_activate_file (NEMO_VIEW (view), file, 0);
			}
			nemo_file_list_free (selection);
		}
		return TRUE;
	}

	if (keyval == GDK_KEY_Left) {
		if (details->columns != NULL) {
			GtkTreeView *parent_tree;
			NemoColumnColumn *parent_column;

			parent_column = g_list_last (details->columns)->data;
			parent_tree = GTK_TREE_VIEW (parent_column->tree_view);
			gtk_widget_grab_focus (GTK_WIDGET (parent_tree));
			column_select_highlight (parent_column);
		}
		return TRUE;
	}

	return FALSE;
}

static void
column_drag_begin_cb (GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	NemoColumnColumn *column = data;
	column->dragging = TRUE;
}

static void
column_drag_end_cb (GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	NemoColumnColumn *column = data;
	column->dragging = FALSE;
}

static void
column_drag_data_get_cb (GtkWidget *widget,
			 GdkDragContext *context,
			 GtkSelectionData *selection_data,
			 guint info,
			 guint time,
			 gpointer data)
{
	GtkTreeSelection *selection;
	GList *paths;
	GList *l;
	GString *uris;
	GString *gnome_uris;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
	paths = gtk_tree_selection_get_selected_rows (selection, NULL);

	uris = g_string_new (NULL);
	gnome_uris = g_string_new ("copy\n");

	for (l = paths; l != NULL; l = l->next) {
		char *uri = uri_for_tree_path (GTK_TREE_VIEW (widget), (GtkTreePath *) l->data);

		if (uri != NULL) {
			g_string_append (uris, uri);
			g_string_append (uris, "\r\n");
			g_string_append (gnome_uris, uri);
			g_string_append_c (gnome_uris, '\n');
			g_free (uri);
		}
	}

	if (info == 0) {
		gtk_selection_data_set (selection_data,
					gtk_selection_data_get_target (selection_data), 8,
					(const guchar *) uris->str, uris->len);
	} else {
		gtk_selection_data_set (selection_data,
					gtk_selection_data_get_target (selection_data), 8,
					(const guchar *) gnome_uris->str,
					gnome_uris->len);
	}

	g_string_free (uris, TRUE);
	g_string_free (gnome_uris, TRUE);
	g_list_free_full (paths, (GDestroyNotify) gtk_tree_path_free);
}

static void
column_drag_data_received_cb (GtkWidget *widget,
			      GdkDragContext *context,
			      gint x,
			      gint y,
			      GtkSelectionData *selection_data,
			      guint info,
			      guint time,
			      gpointer data)
{
	NemoColumnColumn *column = data;
	GtkTreePath *path = NULL;
	char *target_uri = NULL;
	char *text;
	char *payload;
	GdkDragAction action;

	gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (widget), x, y,
				       &path, NULL, NULL, NULL);

	if (path != NULL && tree_path_is_directory (GTK_TREE_VIEW (widget), path)) {
		target_uri = uri_for_tree_path (GTK_TREE_VIEW (widget), path);
	}

	if (target_uri == NULL) {
		target_uri = g_file_get_uri (column->location);
	}

	text = (char *) gtk_selection_data_get_data (selection_data);
	payload = text != NULL ? g_strdup (text) : NULL;

	if (info == 1 && payload != NULL) {
		char *newline = strchr (payload, '\n');
		if (newline != NULL) {
			char *stripped = g_strdup (newline + 1);
			g_free (payload);
			payload = stripped;
		}
	}

	action = gdk_drag_context_get_selected_action (context);
	if (action == 0 || action == GDK_ACTION_DEFAULT) {
		action = GDK_ACTION_COPY;
	}

	if (payload != NULL && target_uri != NULL) {
		nemo_view_handle_uri_list_drop (NEMO_VIEW (column->view),
						payload, target_uri, action, x, y);
	}

	gtk_drag_finish (context, TRUE, FALSE, time);

	g_free (payload);
	g_free (target_uri);
	if (path != NULL) {
		gtk_tree_path_free (path);
	}

	g_signal_stop_emission_by_name (widget, "drag_data_received");
}

static void
current_drag_begin_cb (GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	NemoColumnView *view = data;

	view->details->current_dragging = TRUE;
}

static void
current_drag_end_cb (GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	NemoColumnView *view = data;

	view->details->current_dragging = FALSE;
}

static gboolean
current_button_release_cb (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	NemoColumnView *view = data;
	NemoColumnViewDetails *details = view->details;
	GtkTreePath *path = NULL;

	if (event->button != 1 || details->current_dragging) {
		return FALSE;
	}

	if (event->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK)) {
		return FALSE;
	}

	if (g_settings_get_enum (nemo_preferences, NEMO_PREFERENCES_CLICK_POLICY) !=
	    NEMO_CLICK_POLICY_DOUBLE) {
		return FALSE;
	}

	gtk_tree_view_get_path_at_pos (details->current_tree_view,
				       event->x, event->y,
				       &path, NULL, NULL, NULL);

	if (path != NULL) {
		GtkTreeModel *model = gtk_tree_view_get_model (details->current_tree_view);

		if (NEMO_IS_LIST_MODEL (model)) {
			NemoFile *file = nemo_list_model_file_for_path (NEMO_LIST_MODEL (model),
									path);

			if (file != NULL) {
				if (nemo_file_is_directory (file)) {
					char *uri = nemo_file_get_uri (file);
					GFile *location = location_from_uri (uri);

					if (location != NULL) {
						navigate_to_location (view, location);
						g_object_unref (location);
					}
					g_free (uri);
					nemo_file_unref (file);
					gtk_tree_path_free (path);
					return TRUE;
				}
				nemo_file_unref (file);
			}
		}

		gtk_tree_path_free (path);
	}

	return FALSE;
}

static void
nemo_column_view_begin_loading (NemoView *view)
{
	NEMO_VIEW_CLASS (nemo_column_view_parent_class)->begin_loading (view);

	configure_current_tree_view (NEMO_COLUMN_VIEW (view));
	rebuild_ancestor_columns (NEMO_COLUMN_VIEW (view));
	schedule_scroll_to_end (NEMO_COLUMN_VIEW (view));
}

static void
nemo_column_view_end_loading (NemoView *view, gboolean all_files_seen)
{
	NemoColumnView *column_view = NEMO_COLUMN_VIEW (view);
	NemoColumnViewDetails *details = column_view->details;

	NEMO_VIEW_CLASS (nemo_column_view_parent_class)->end_loading (view, all_files_seen);

	if (details->pending_action != PENDING_NONE &&
	    details->pending_timeout_id != 0) {
		g_source_remove (details->pending_timeout_id);
		details->pending_timeout_id = 0;
		g_idle_add_full (G_PRIORITY_DEFAULT,
				 perform_pending_action,
				 g_object_ref (column_view),
				 (GDestroyNotify) g_object_unref);
	}

	schedule_scroll_to_end (column_view);
}

static const char *
nemo_column_view_get_id (NemoView *view)
{
	return NEMO_COLUMN_VIEW_ID;
}

static void
nemo_column_view_init (NemoColumnView *view)
{
	NemoColumnViewDetails *details;
	GtkWidget *hbox;
	GtkWidget *current_scroller;
	GtkTreeView *tree_view;
	GtkEventController *controller;

	details = G_TYPE_INSTANCE_GET_PRIVATE (view, NEMO_TYPE_COLUMN_VIEW,
					       NemoColumnViewDetails);
	view->details = details;

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (view),
					GTK_POLICY_AUTOMATIC,
					GTK_POLICY_NEVER);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (view),
					     GTK_SHADOW_NONE);

	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	details->hbox = hbox;

	tree_view = nemo_list_view_get_tree_view (NEMO_LIST_VIEW (view));
	details->current_tree_view = tree_view;

	g_object_ref (tree_view);
	gtk_container_remove (GTK_CONTAINER (view), GTK_WIDGET (tree_view));

	current_scroller = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (current_scroller),
					GTK_POLICY_NEVER,
					GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (current_scroller),
					     GTK_SHADOW_IN);
	gtk_widget_set_size_request (current_scroller, COLUMN_VIEW_WIDTH, -1);
	details->current_scroller = current_scroller;

	gtk_container_add (GTK_CONTAINER (current_scroller), GTK_WIDGET (tree_view));
	g_object_unref (tree_view);
	gtk_box_pack_start (GTK_BOX (hbox), current_scroller, FALSE, FALSE, 0);
	gtk_container_add (GTK_CONTAINER (view), hbox);
	gtk_widget_show_all (hbox);

	controller = gtk_event_controller_key_new (GTK_WIDGET (tree_view));
	gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
	g_signal_connect (controller, "key-pressed",
			  G_CALLBACK (current_key_pressed_cb), view);
	details->current_key_controller = controller;

	g_signal_connect (tree_view, "button-release-event",
			  G_CALLBACK (current_button_release_cb), view);
	g_signal_connect (tree_view, "drag-begin",
			  G_CALLBACK (current_drag_begin_cb), view);
	g_signal_connect (tree_view, "drag-end",
			  G_CALLBACK (current_drag_end_cb), view);

	configure_current_tree_view (view);
}

static void
nemo_column_view_dispose (GObject *object)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (object);
	NemoColumnViewDetails *details = view->details;

	if (details->scroll_id != 0) {
		g_source_remove (details->scroll_id);
		details->scroll_id = 0;
	}

	if (details->pending_timeout_id != 0) {
		g_source_remove (details->pending_timeout_id);
		details->pending_timeout_id = 0;
	}
	details->pending_action = PENDING_NONE;
	g_clear_pointer (&details->pending_uri, g_free);
	details->current_dragging = FALSE;

	clear_ancestor_columns (view);
	g_clear_object (&details->current_key_controller);

	G_OBJECT_CLASS (nemo_column_view_parent_class)->dispose (object);
}

static void
nemo_column_view_finalize (GObject *object)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (object);

	g_free (view->details->pending_uri);
	view->details->pending_uri = NULL;

	G_OBJECT_CLASS (nemo_column_view_parent_class)->finalize (object);
}

static void
nemo_column_view_class_init (NemoColumnViewClass *class)
{
	NemoViewClass *view_class;
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (class);
	view_class = NEMO_VIEW_CLASS (class);

	g_type_class_add_private (class, sizeof (NemoColumnViewDetails));

	object_class->dispose = nemo_column_view_dispose;
	object_class->finalize = nemo_column_view_finalize;

	view_class->begin_loading = nemo_column_view_begin_loading;
	view_class->end_loading = nemo_column_view_end_loading;
	view_class->get_view_id = nemo_column_view_get_id;
}

static NemoView *
nemo_column_view_create (NemoWindowSlot *slot)
{
	NemoColumnView *view;

	view = g_object_new (NEMO_TYPE_COLUMN_VIEW,
			     "window-slot", slot,
			     NULL);

	return NEMO_VIEW (view);
}

static gboolean
nemo_column_view_supports_uri (const char *uri,
			       GFileType file_type,
			       const char *mime_type)
{
	if (file_type == G_FILE_TYPE_DIRECTORY) {
		return TRUE;
	}
	if (g_str_has_prefix (uri, "trash:")) {
		return TRUE;
	}
	if (g_str_has_prefix (uri, "recent:")) {
		return TRUE;
	}
	if (g_str_has_prefix (uri, "favorites:")) {
		return TRUE;
	}
	if (g_str_has_prefix (uri, EEL_SEARCH_URI)) {
		return TRUE;
	}

	return FALSE;
}

static NemoViewInfo nemo_column_view = {
	(char *) NEMO_COLUMN_VIEW_ID,
	/* translators: this is used in the view selection dropdown
	 * of navigation windows and in the preferences dialog */
	(char *) N_("Column View"),
	/* translators: this is used in the view menu */
	(char *) N_("_Columns"),
	(char *) N_("The column view encountered an error."),
	(char *) N_("The column view encountered an error while starting up."),
	(char *) N_("Display this location with the column view."),
	nemo_column_view_create,
	nemo_column_view_supports_uri
};

void
nemo_column_view_register (void)
{
	nemo_column_view.view_combo_label = _(nemo_column_view.view_combo_label);
	nemo_column_view.view_menu_label_with_mnemonic = _(nemo_column_view.view_menu_label_with_mnemonic);
	nemo_column_view.error_label = _(nemo_column_view.error_label);
	nemo_column_view.startup_error_label = _(nemo_column_view.startup_error_label);
	nemo_column_view.display_location_label = _(nemo_column_view.display_location_label);

	nemo_view_factory_register (&nemo_column_view);
}
