Copyright (c) 2014, Feng, Bin Hui
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <mupdf/fitz.h>
#include <gtk/gtk.h>

#define SCROLL_STEP 80
#define SCROLL_PAGE 0.95
#define RESIZE_MIN 0.1
#define RESIZE_MAX 10.0
#define RESIZE_DELTA 0.1

#define CACHE_LENGTH 10

#define BACKGROUND 0xff
#define DPI 96
#define CLEARANCE 2

#define BUF_LEN 16

typedef struct {
	int page;
	float scale;
	float rotation;
	int width;
	int height;
	unsigned char *data;
	cairo_surface_t *surface;
} image_t;

typedef struct {
	char *filename;
	fz_context *ctx;
	fz_document *doc;
	int pages_number;
	/* state variables for display */
	int page;    /* [0 pages_number) */
	float line;  /* [0.0 1.0] of [0 (image->height + CLEARANCE - 1)] */
	float bias;  /* the distance the display window moved from middle of pages toward the right axis */
	/* state variables for rendering */
	float scale; /* [0.1 10.0] for DPI */
	float rotation;
	GList *cache;
	int cache_length; /* [0 CACHE_LENGTH] */
} file_info_t;

file_info_t *file = NULL;
char b[BUF_LEN] = "";
GtkWidget *da;
GtkWidget *entry;
GtkWidget *paned;
GtkWidget *window;

void update_entry(void);

file_info_t *
open_file(char *filename)
{
	file_info_t *file;

	if (filename == NULL) {
		g_print("filename is empty!\n");
		exit(0);
	}
	file = g_slice_new(file_info_t);
	file->filename = filename;
	file->ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
	file->doc = fz_open_document(file->ctx, filename);
	if (file->ctx->error->errcode != 0) {
		g_print("Can't open file: \"%s\"\n", filename);
		exit(-1);
	}
	file->pages_number = fz_count_pages(file->doc);
	g_print("%s %d pages.\n", filename, file->pages_number);
	file->page = 0;
	file->line = 0.0;
	file->bias = 0.0;
	file->scale = 1.0;
	file->rotation = 0.0;
	file->cache = NULL;
	file->cache_length = 0;
	return file;
}

void
set_file_position(file_info_t *file, int page, float line)
{
	if (page < 0)
		file->page = 0;
	else if (page >= file->pages_number)
		file->page = file->pages_number - 1;
	else
		file->page = page;

	if (line < 0.0)
		file->line = 0.0;
	else if (line > 1.0)
		file->line = 1.0;
	else
		file->line = line;
}

void
set_file_bias(file_info_t *file, float bias)
{
	if (bias >= -SCROLL_STEP / 2 && bias <= SCROLL_STEP / 2)
		file->bias = 0.0;
	else
		file->bias = bias;
}

void
set_file_scale(file_info_t *file, float scale)
{
	if (scale < RESIZE_MIN)
		file->scale = RESIZE_MIN;
	else if (scale > RESIZE_MAX)
		file->scale = RESIZE_MAX;
	else
		file->scale = scale;
}

void
set_rotation(file_info_t *file, float rotation)
{
	file->rotation = rotation;
}

void
free_image(image_t *image)
{
	g_slice_free1(4 * image->width * image->height, image->data);
	cairo_surface_destroy(image->surface);
	g_slice_free(image_t, image);
}

void
close_file(file_info_t *file)
{
	g_list_free_full(file->cache, (GDestroyNotify)free_image);
	fz_close_document(file->doc);
	fz_free_context(file->ctx);
	g_slice_free(file_info_t, file);
}

gint
compare_page(image_t *a, gint *b)
{
	if (a->page < *b)
		return -1;
	else if (a->page == *b)
		return 0;
	else
		return 1;
}

image_t *
render_page(file_info_t *file, int page)
{
	GList *list, *last;
	image_t *image;
	fz_page *fzpage;
	fz_rect bound;
	fz_matrix matrix;
	fz_irect bbox;
	fz_pixmap *pix;
	fz_device *dev;


	list = g_list_find_custom(file->cache, &page, (GCompareFunc)compare_page);
	if (list == NULL) {
		if (file->cache_length == CACHE_LENGTH) {
			last = g_list_last(file->cache);
			file->cache = g_list_remove_link(file->cache, last);
			g_list_free_full(last, (GDestroyNotify)free_image);
			file->cache_length--;
		}
		image = g_slice_new(image_t);
		image->page = page;
		file->cache = g_list_prepend(file->cache, image);
		file->cache_length++;
	} else {
		image = list->data;
		if (list != g_list_first(file->cache)) {
			file->cache = g_list_remove_link(file->cache, list);
			file->cache = g_list_prepend(file->cache, image);
			g_list_free(list);
		}
		if (image->scale == file->scale && image->rotation == file->rotation) {
			return image;
		} else {
			g_slice_free1(4 * image->width * image->height, image->data);
			cairo_surface_destroy(image->surface);
		}
	}
	image->scale = file->scale;
	image->rotation = file->rotation;

	fzpage = fz_load_page(file->doc, page);
	fz_bound_page(file->doc, fzpage, &bound);
	fz_rotate(&matrix, file->rotation);
	fz_pre_scale(&matrix, file->scale * DPI / 72, file->scale * DPI / 72);
	fz_transform_rect(&bound, &matrix);
	fz_round_rect(&bbox, &bound);

	image->width = bbox.x1 - bbox.x0;
	image->height = bbox.y1 - bbox.y0;
	image->data = g_slice_alloc(4 * image->width * image->height);

	pix = fz_new_pixmap_with_bbox_and_data(file->ctx, fz_device_bgr(file->ctx), &bbox, image->data);
	fz_clear_pixmap_with_value(file->ctx, pix, BACKGROUND);
	dev = fz_new_draw_device(file->ctx, pix);
	fz_run_page(file->doc, fzpage, dev, &matrix, NULL);
	fz_free_device(dev);
//	fz_write_png(file->ctx, pix, "out.png", 0);
	fz_drop_pixmap(file->ctx, pix);
	fz_free_page(file->doc, fzpage);

	image->surface = cairo_image_surface_create_for_data(image->data, CAIRO_FORMAT_ARGB32, image->width, image->height, image->width * 4);
	return image;
}

gboolean
draw_cb(GtkWidget *da, cairo_t *cc, file_info_t *file)
{
	int da_width, da_height;
	int pages_number;
	int page;
	int x0, y0;
	float bias;
	image_t *image;

	da_width = gtk_widget_get_allocated_width(da);
	da_height = gtk_widget_get_allocated_height(da);
	pages_number = file->pages_number;
	bias = file->bias;
	page = file->page;
	image = render_page(file, page);
	y0 = -file->line * (image->height + CLEARANCE - 1);
	while (1) {
		x0 = (da_width - image->width) / 2 - bias;
		cairo_set_source_surface(cc, image->surface, x0, y0);
		cairo_paint(cc);
		y0 += image->height + CLEARANCE;
		page++;
		if (y0 >= da_height || page == pages_number)
			break;
		else
			image = render_page(file, page);
	}
	return TRUE;
}

void
scroll_v(file_info_t *file, int step)
{
	int page, line;
	int height;

	page = file->page;
	height = render_page(file, page)->height + CLEARANCE;
	line = file->line * (height - 1) + step;
	while (1) {
		if (line < 0) {
			if (page == 0) {
				file->page = 0;
				file->line = 0.0;
				return;
			}
			page--;
			height = render_page(file, page)->height + CLEARANCE;
			line += height;
		} else if (line < height) {
			file->page = page;
			file->line = line / (height - 1.0);
			return;
		} else {
			if (page == file->pages_number - 1) {
				file->page = page;
				file->line = 1.0;
				return;
			}
			line -= height;
			page++;
			height = render_page(file, page)->height + CLEARANCE;
		}
	}
}

void
scroll_h(file_info_t *file, int step)
{
	set_file_bias(file, file->bias + step);
}

void
resize(file_info_t *file, float delta)
{
	float bias;

//	g_print("before: p:%d l:%f b:%f s:%f\n", file->page, file->line, file->bias, file->scale);
	bias = file->bias / file->scale;
	set_file_scale(file, file->scale + delta);
	set_file_bias(file, bias * file->scale);
//	g_print("after: p:%d l:%f b:%f s:%f\n", file->page, file->line, file->bias, file->scale);
}

gboolean
scroll_cb(GtkWidget *da, GdkEventScroll *event, file_info_t *file)
{
//	g_print("%d, %d, %d\n", event->type, event->state, event->direction);
	switch (event->direction) {
		case GDK_SCROLL_UP:
			switch (event->state) {
				case 0:
					scroll_v(file, -SCROLL_STEP);
					break;
				case GDK_SHIFT_MASK:
					scroll_h(file, -SCROLL_STEP);
					break;
				case GDK_CONTROL_MASK:
					resize(file, RESIZE_DELTA);
					break;
				default:
					return FALSE;
			}
			break;
		case GDK_SCROLL_DOWN:
			switch (event->state) {
				case 0:
					scroll_v(file, SCROLL_STEP);
					break;
				case GDK_SHIFT_MASK:
					scroll_h(file, SCROLL_STEP);
					break;
				case GDK_CONTROL_MASK:
					resize(file, -RESIZE_DELTA);
					break;
				default:
					return FALSE;
			}
			break;
		default:
			return FALSE;
	}
	gtk_widget_queue_draw(da);
	update_entry();
	return TRUE;
}

gboolean
key_press_cb(GtkWidget *da, GdkEventKey *event, file_info_t *file)
{
	static int page = 0;

//	g_print("%d, %d, %x, %d, %d, %d\n", event->type, event->state, event->keyval, event->hardware_keycode, event->group, event->is_modifier);
	switch (event->keyval) {
		case GDK_KEY_Up:
			scroll_v(file, -SCROLL_STEP);
			break;
		case GDK_KEY_Down:
			scroll_v(file, SCROLL_STEP);
			break;
		case GDK_KEY_Left:
			scroll_h(file, -SCROLL_STEP);
			break;
		case GDK_KEY_Right:
			scroll_h(file, SCROLL_STEP);
			break;
		case GDK_KEY_Page_Up:
			scroll_v(file, -SCROLL_PAGE * gtk_widget_get_allocated_height(da));
			break;
		case GDK_KEY_Page_Down:
			scroll_v(file, SCROLL_PAGE * gtk_widget_get_allocated_height(da));
			break;
		case GDK_KEY_equal:
			if (event->state == GDK_CONTROL_MASK)
				resize(file, RESIZE_DELTA);
			else
				return FALSE;
			break;
		case GDK_KEY_minus:
			if (event->state == GDK_CONTROL_MASK)
				resize(file, -RESIZE_DELTA);
			else
				return FALSE;
			break;
		case GDK_KEY_G:
			set_file_position(file, file->pages_number - 1, 0.0);
			break;
		case GDK_KEY_g:
			set_file_position(file, page - 1, 0.0);
			break;
		case GDK_KEY_0: case GDK_KEY_1: case GDK_KEY_2: case GDK_KEY_3: case GDK_KEY_4:
		case GDK_KEY_5: case GDK_KEY_6: case GDK_KEY_7: case GDK_KEY_8: case GDK_KEY_9:
			page = page * 10 + event->keyval - GDK_KEY_0;
			return FALSE;
		default:
			return FALSE;
	}
	gtk_widget_queue_draw(da);
	update_entry();
	page = 0;
	return TRUE;
}

gboolean
enter_notify_cb(GtkWidget *da, GdkEventCrossing *event, gpointer p)
{
	gtk_widget_grab_focus(da);
	return TRUE;
}

gboolean
button_cb(GtkWidget *da, GdkEventButton *event, file_info_t *file)
{
//	g_print("%d, %d, %d\n", event->type, event->state, event->button);
	return TRUE;
}

void
tb_cb(GtkToggleButton *tb, GtkWidget *frame)
{
	if (gtk_toggle_button_get_active(tb) == TRUE) {
		if (gtk_paned_get_position(GTK_PANED(paned)) == 0) {
			gtk_paned_set_position(GTK_PANED(paned), gtk_widget_get_allocated_width(window) / 3);
		}
		gtk_widget_show(frame);
	} else {
		gtk_widget_hide(frame);
	}
}

void
update_entry(void)
{
	snprintf(b, BUF_LEN, "%d / %d", file->page + 1, file->pages_number);
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), b);
}

void
entry_cb(GtkEntry *entry, file_info_t *file)
{
	const gchar *s;
	long page;
	char *p;

	s = gtk_entry_get_text(entry);
	if (*s == '\0')
		return;
	page = strtol(s, &p, 10);
	if (*p != '\0')
		return;
	set_file_position(file, page - 1, 0.0);

	gtk_widget_queue_draw(da);
	update_entry();
	gtk_entry_set_text(entry, "");
}

gboolean
expand_cb(GtkEventBox *ebox, GdkEventButton *event, GtkWidget *grid)
{
	GtkArrowType type;
	GtkWidget *arrow;

	//g_print("%d, %d, %d\n", event->type, event->state, event->button);
	arrow = gtk_bin_get_child(GTK_BIN(ebox));
	g_object_get(G_OBJECT(arrow), "arrow-type", &type, NULL);
	if (type == GTK_ARROW_RIGHT) {
		gtk_widget_show(grid);
		gtk_arrow_set(GTK_ARROW(arrow), GTK_ARROW_DOWN, GTK_SHADOW_NONE);
	} else {
		gtk_widget_hide(grid);
		gtk_arrow_set(GTK_ARROW(arrow), GTK_ARROW_RIGHT, GTK_SHADOW_NONE);
	}
	return TRUE;
}

gboolean
gotopage_cb(GtkLabel *label1, GdkEventButton *event, GtkLabel *label2)
{
	int page;

	page = strtol(gtk_label_get_text(label2), NULL, 10);
	set_file_position(file, page - 1, 0.0);
	gtk_widget_queue_draw(da);
	update_entry();
	return TRUE;
}

void
destroy_cb(GtkWidget *widget, file_info_t *file)
{
	close_file(file);
	gtk_main_quit();
}

void
attach_outline(GtkWidget *grid, fz_outline *outline, int row, int depth)
{
	GtkWidget *grid1, *grid2, *grid3, *label1, *label2, *arrow, *ebox;

	grid1 = gtk_grid_new();

	int i = 0, d = depth;
	while (d-- > 0) {
		gtk_grid_attach(GTK_GRID(grid1), gtk_arrow_new(GTK_ARROW_NONE, GTK_SHADOW_NONE), i++, 0, 1, 1);
	}

	label1 = gtk_label_new(outline->title);
	gtk_widget_set_hexpand(label1, TRUE);
	gtk_widget_set_halign(label1, GTK_ALIGN_START);
	gtk_label_set_selectable(GTK_LABEL(label1), TRUE);
	gtk_grid_attach(GTK_GRID(grid1), label1, i + 1, 0, 1, 1);

	snprintf(b, BUF_LEN, "%d", 1 + outline->dest.ld.gotor.page);
	label2 = gtk_label_new(b);
	gtk_grid_attach(GTK_GRID(grid1), label2, i + 2, 0, 1, 1);

	//gtk_widget_set_can_focus(label1, TRUE);
	gtk_widget_add_events(label1, GDK_SCROLL_MASK |  GDK_BUTTON_PRESS_MASK);
	g_signal_connect(G_OBJECT(label1), "button-press-event", G_CALLBACK(gotopage_cb), label2);

	if (outline->down == NULL) {
		arrow = gtk_arrow_new(GTK_ARROW_NONE, GTK_SHADOW_NONE);
		gtk_grid_attach(GTK_GRID(grid1), arrow, i, 0, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), grid1, 0, row, 1, 1);
	} else {
		arrow = gtk_arrow_new(GTK_ARROW_RIGHT, GTK_SHADOW_NONE);
		ebox= gtk_event_box_new();
		gtk_container_add(GTK_CONTAINER(ebox), arrow);
		gtk_grid_attach(GTK_GRID(grid1), ebox, i, 0, 1, 1);

		grid2 = gtk_grid_new();
		attach_outline(grid2, outline->down, 0, depth + 1);

		grid3 = gtk_grid_new();
		gtk_grid_attach(GTK_GRID(grid3), grid1, 0, 0, 1, 1);
		gtk_grid_attach(GTK_GRID(grid3), grid2, 0, 1, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), grid3, 0, row, 1, 1);

		gtk_widget_hide(grid2);
		gtk_widget_show(grid3);

		gtk_widget_add_events(ebox, GDK_SCROLL_MASK |  GDK_BUTTON_PRESS_MASK);
		g_signal_connect(G_OBJECT(ebox), "button-press-event", G_CALLBACK(expand_cb), grid2);
	}

	gtk_widget_show_all(grid1);

	if ((outline = outline->next) == NULL) {
		return;
	} else {
		attach_outline(grid, outline, row + 1, depth);
	}
}

int main(int argc, char **argv)
{
	char *filename;
	int page;
	float line;
	float bias;
	float scale;
	float rotation;

	gtk_init(&argc, &argv);

	/*
	 * open file
	 */
	filename = argc > 1 ? argv[1] : NULL;
	file = open_file(filename);
	page = argc > 2 ? strtol(argv[2], NULL, 10) - 1 : 0;
	line = argc > 3 ? strtof(argv[3], NULL) : 0.0;
	bias = argc > 4 ? strtof(argv[4], NULL) : 0.0;
	scale = argc > 5 ? strtof(argv[5], NULL) : 1.0;
	rotation = argc > 6 ? strtof(argv[6], NULL) : 0.0;
	set_file_position(file, page, line);
	set_file_bias(file, bias);
	set_file_scale(file, scale);
	set_rotation(file, rotation);

	/*
	 * drawing area
	 */
	da = gtk_drawing_area_new();
	g_object_set(G_OBJECT(da), "expand", TRUE, NULL);
	gtk_widget_set_can_focus(da, TRUE);
	gtk_widget_add_events(da, GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK | GDK_ENTER_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
	g_signal_connect(G_OBJECT(da), "draw", G_CALLBACK(draw_cb), file);
	g_signal_connect(G_OBJECT(da), "scroll-event", G_CALLBACK(scroll_cb), file);
	g_signal_connect(G_OBJECT(da), "key-press-event", G_CALLBACK(key_press_cb), file);
	g_signal_connect(G_OBJECT(da), "enter-notify-event", G_CALLBACK(enter_notify_cb), NULL);
	g_signal_connect(G_OBJECT(da), "button-press-event", G_CALLBACK(button_cb), file);
	g_signal_connect(G_OBJECT(da), "button-release-event", G_CALLBACK(button_cb), file);

	/*
	 * bookmark window
	 */
	GtkWidget *scwin, *scwin_grid;
	fz_outline *outline;

	outline = fz_load_outline(file->doc);

	scwin = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scwin), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	scwin_grid = gtk_grid_new();
	if (outline != NULL) {
		attach_outline(scwin_grid, outline, 0, 0);
	}
	gtk_container_add(GTK_CONTAINER(scwin), scwin_grid);

	fz_free_outline(file->ctx, outline);

	/*
	 * paned window
	 */
	GtkWidget *frame1, *frame2;

	frame1 = gtk_frame_new (NULL);
	frame2 = gtk_frame_new (NULL);
	gtk_container_add(GTK_CONTAINER(frame1), scwin);
	gtk_container_add(GTK_CONTAINER(frame2), da);
	paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(paned), frame1, TRUE, TRUE);
	gtk_paned_pack2(GTK_PANED(paned), frame2, TRUE, TRUE);
	g_object_set(G_OBJECT(paned), "position-set", TRUE, NULL);
	gtk_paned_set_position(GTK_PANED(paned), 0);

	/*
	 * togglebutton: show/hide bookmark
	 */
	GtkWidget *tb;

	tb = gtk_toggle_button_new_with_mnemonic("_Bookmark");
	g_signal_connect(G_OBJECT(tb), "toggled", G_CALLBACK(tb_cb), frame1);

	/*
	 * entry: enter/show page number
	 */
	entry = gtk_entry_new();
	g_signal_connect(G_OBJECT(entry), "activate", G_CALLBACK(entry_cb), file);
	snprintf(b, BUF_LEN, "%d", file->pages_number);
	int l = strnlen(b, BUF_LEN);
	gtk_entry_set_max_length(GTK_ENTRY(entry), l);
	gtk_entry_set_width_chars(GTK_ENTRY(entry), l * 2 + 3);
	update_entry();

	/*
	 * grid
	 */
	GtkWidget *grid1, *grid2;

	grid2 = gtk_grid_new();
	gtk_grid_attach(GTK_GRID(grid2), tb, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid2), entry, 1, 0, 1, 1);
	gtk_grid_set_column_spacing(GTK_GRID(grid2), 4);

	grid1 = gtk_grid_new();
	gtk_grid_attach(GTK_GRID(grid1), grid2, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid1), paned, 0, 1, 1, 1);
	gtk_grid_set_row_spacing(GTK_GRID(grid1), 4);

	/*
	 * main window
	 */
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
	gtk_window_maximize(GTK_WINDOW(window));
	g_signal_connect(G_OBJECT(window), "destroy", G_CALLBACK(destroy_cb), file);
	gtk_container_add(GTK_CONTAINER(window), grid1);

	gtk_widget_show(scwin_grid);
	gtk_widget_show(scwin);
	gtk_widget_hide(frame1);
	gtk_widget_show_all(frame2);
	gtk_widget_show(paned);
	gtk_widget_show_all(grid2);
	gtk_widget_show(grid1);
	gtk_widget_show(window);

	gtk_main();

	return 0;
}
