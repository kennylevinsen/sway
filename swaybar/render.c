#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <linux/input-event-codes.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "cairo.h"
#include "pango.h"
#include "pool-buffer.h"
#include "swaybar/bar.h"
#include "swaybar/config.h"
#include "swaybar/i3bar.h"
#include "swaybar/ipc.h"
#include "swaybar/render.h"
#include "swaybar/status_line.h"
#if HAVE_TRAY
#include "swaybar/tray/tray.h"
#endif
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static const int WS_HORIZONTAL_PADDING = 5;
static const double WS_VERTICAL_PADDING = 1.5;
static const double BORDER_WIDTH = 1;

static uint32_t render_status_line_error(cairo_t *cairo,
		struct swaybar_output *output, double *x) {
	const char *error = output->bar->status->text;
	if (!error) {
		return 0;
	}

	uint32_t breadth = output->breadth * output->scale;

	cairo_set_source_u32(cairo, 0xFF0000FF);

	int margin = 3 * output->scale;
	double ws_vertical_padding =
		output->bar->config->status_padding * output->scale;

	char *font = output->bar->config->font;

	uint32_t lr_anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	bool is_horiz = (output->bar->config->position & lr_anchors) == lr_anchors;

	int text_width, text_height;
	get_text_size(cairo, font, &text_width, &text_height, NULL,
			output->scale, !is_horiz, false, "%s", error);

	uint32_t ideal_height = text_height + ws_vertical_padding * 2;
	uint32_t ideal_surface_height = ideal_height / output->scale;
	if (!output->bar->config->breadth &&
			output->breadth < ideal_surface_height) {
		return ideal_surface_height;
	}
	*x -= text_width + margin;

	double text_y = breadth / 2.0 - text_height / 2.0;
	cairo_move_to(cairo, *x, (int)floor(text_y));
	pango_printf(cairo, font, output->scale, !is_horiz, false, "%s", error);
	*x -= margin;
	return 0;
}

static uint32_t render_status_line_text(cairo_t *cairo,
		struct swaybar_output *output, double *x) {
	const char *text = output->bar->status->text;
	if (!text) {
		return 0;
	}

	struct swaybar_config *config = output->bar->config;
	cairo_set_source_u32(cairo, output->focused ?
			config->colors.focused_statusline : config->colors.statusline);

	uint32_t lr_anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	bool is_horiz = (output->bar->config->position & lr_anchors) == lr_anchors;

	int text_width, text_height;
	get_text_size(cairo, config->font, &text_width, &text_height, NULL,
			output->scale, !is_horiz, config->pango_markup, "%s", text);

	double ws_vertical_padding = config->status_padding * output->scale;
	int margin = 3 * output->scale;

	uint32_t ideal_height = text_height + ws_vertical_padding * 2;
	uint32_t ideal_surface_height = ideal_height / output->scale;
	if (!output->bar->config->breadth &&
			output->breadth < ideal_surface_height) {
		return ideal_surface_height;
	}

	*x -= text_width + margin;
	uint32_t height = output->breadth * output->scale;
	double text_y = height / 2.0 - text_height / 2.0;
	cairo_move_to(cairo, *x, (int)floor(text_y));
	pango_printf(cairo, config->font, output->scale,
			!is_horiz, config->pango_markup, "%s", text);
	*x -= margin;
	return 0;
}

static void render_sharp_rectangle(cairo_t *cairo, uint32_t color,
		double x, double y, double width, double height) {
	cairo_save(cairo);
	cairo_set_source_u32(cairo, color);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_NONE);
	cairo_rectangle(cairo, x, y, width, height);
	cairo_fill(cairo);
	cairo_restore(cairo);
}

static void render_sharp_line(cairo_t *cairo, uint32_t color,
		double x, double y, double width, double height) {
	if (width > 1 && height > 1) {
		render_sharp_rectangle(cairo, color, x, y, width, height);
	} else {
		cairo_save(cairo);
		cairo_set_source_u32(cairo, color);
		cairo_set_antialias(cairo, CAIRO_ANTIALIAS_NONE);
		if (width == 1) {
			x += 0.5;
			height += y;
			width = x;
		}
		if (height == 1) {
			y += 0.5;
			width += x;
			height = y;
		}
		cairo_move_to(cairo, x, y);
		cairo_set_line_width(cairo, 1.0);
		cairo_line_to(cairo, width, height);
		cairo_stroke(cairo);
		cairo_restore(cairo);
	}
}

static enum hotspot_event_handling block_hotspot_callback(
		struct swaybar_output *output, struct swaybar_hotspot *hotspot,
		int x, int y, uint32_t button, void *data) {
	struct i3bar_block *block = data;
	struct status_line *status = output->bar->status;
	return i3bar_block_send_click(status, block, x, y, x - hotspot->x,
			y - hotspot->y, hotspot->width, hotspot->height, button);
}

static void i3bar_block_unref_callback(void *data) {
	i3bar_block_unref(data);
}

static uint32_t render_status_block(cairo_t *cairo,
		struct swaybar_output *output, struct i3bar_block *block, double *l,
		bool edge) {
	if (!block->full_text || !*block->full_text) {
		return 0;
	}

	struct swaybar_config *config = output->bar->config;

	uint32_t lr_anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	bool is_horiz = (output->bar->config->position & lr_anchors) == lr_anchors;

	int text_width, text_height;
	get_text_size(cairo, config->font, &text_width, &text_height, NULL,
			output->scale, !is_horiz, block->markup, "%s", block->full_text);

	int margin = 3 * output->scale;
	double ws_vertical_padding = config->status_padding * output->scale;

	int width = text_width;
	if (block->min_width_str) {
		int w;
		get_text_size(cairo, config->font, &w, NULL, NULL,
				output->scale, !is_horiz, block->markup, "%s", block->min_width_str);
		block->min_width = w;
	}
	if (width < block->min_width) {
		width = block->min_width;
	}

	double block_width = width;
	uint32_t ideal_height = text_height + ws_vertical_padding * 2;
	uint32_t ideal_surface_height = ideal_height / output->scale;
	if (!output->bar->config->breadth &&
			output->breadth < ideal_surface_height) {
		return ideal_surface_height;
	}

	*l -= width;
	if ((block->border || block->urgent) && block->border_left > 0) {
		*l -= (block->border_left * output->scale + margin);
		block_width += block->border_left * output->scale + margin;
	}
	if ((block->border || block->urgent) && block->border_right > 0) {
		*l -= (block->border_right * output->scale + margin);
		block_width += block->border_right * output->scale + margin;
	}

	int sep_width, sep_height;
	int sep_block_width = block->separator_block_width;
	if (!edge) {
		if (config->sep_symbol) {
			get_text_size(cairo, config->font, &sep_width, &sep_height, NULL,
					output->scale, !is_horiz, false, "%s", config->sep_symbol);
			uint32_t _ideal_height = sep_height + ws_vertical_padding * 2;
			uint32_t _ideal_surface_height = _ideal_height / output->scale;
			if (!output->bar->config->breadth &&
					output->breadth < _ideal_surface_height) {
				return _ideal_surface_height;
			}
			if (sep_width > sep_block_width) {
				sep_block_width = sep_width + margin * 2;
			}
		}
		*l -= sep_block_width;
	} else if (config->status_edge_padding) {
		*l -= config->status_edge_padding * output->scale;
	}

	uint32_t height = output->breadth * output->scale;
	if (output->bar->status->click_events) {
		struct swaybar_hotspot *hotspot = calloc(1, sizeof(struct swaybar_hotspot));

		hotspot->x = *l;
		hotspot->y = 0;
		hotspot->width = width;
		hotspot->height = height;
		hotspot->callback = block_hotspot_callback;
		hotspot->destroy = i3bar_block_unref_callback;
		hotspot->data = block;
		block->ref_count++;
		wl_list_insert(&output->hotspots, &hotspot->link);
	}

	double x_pos = *l;
	double y_pos = ws_vertical_padding;
	double render_height = height - ws_vertical_padding * 2;

	uint32_t bg_color = block->urgent
		? config->colors.urgent_workspace.background : block->background;
	if (bg_color) {
		render_sharp_rectangle(cairo, bg_color, x_pos, y_pos,
				block_width, render_height);
	}

	uint32_t border_color = block->urgent
		? config->colors.urgent_workspace.border : block->border;
	if (border_color && block->border_top > 0) {
		render_sharp_line(cairo, border_color, x_pos, y_pos,
				block_width, block->border_top * output->scale);
	}
	if (border_color && block->border_bottom > 0) {
		render_sharp_line(cairo, border_color, x_pos,
				y_pos + render_height - block->border_bottom * output->scale,
				block_width, block->border_bottom * output->scale);
	}
	if (border_color && block->border_left > 0) {
		render_sharp_line(cairo, border_color, x_pos, y_pos,
				block->border_left * output->scale, render_height);
		x_pos += block->border_left * output->scale + margin;
	}

	double offset = 0;
	if (strncmp(block->align, "left", 5) == 0) {
		offset = x_pos;
	} else if (strncmp(block->align, "right", 5) == 0) {
		offset = x_pos + width - text_width;
	} else if (strncmp(block->align, "center", 6) == 0) {
		offset = x_pos + (width - text_width) / 2;
	}
	cairo_move_to(cairo, offset, height / 2.0 - text_height / 2.0);
	uint32_t color = block->color ?  *block->color : config->colors.statusline;
	color = block->urgent ? config->colors.urgent_workspace.text : color;
	cairo_set_source_u32(cairo, color);
	pango_printf(cairo, config->font, output->scale,
			!is_horiz, block->markup, "%s", block->full_text);
	x_pos += width;

	if (block->border && block->border_right > 0) {
		x_pos += margin;
		render_sharp_line(cairo, border_color, x_pos, y_pos,
				block->border_right * output->scale, render_height);
		x_pos += block->border_right * output->scale;
	}

	if (!edge && block->separator) {
		if (output->focused) {
			cairo_set_source_u32(cairo, config->colors.focused_separator);
		} else {
			cairo_set_source_u32(cairo, config->colors.separator);
		}
		if (config->sep_symbol) {
			offset = x_pos + (sep_block_width - sep_width) / 2;
			cairo_move_to(cairo, offset, height / 2.0 - sep_height / 2.0);
			pango_printf(cairo, config->font, output->scale, !is_horiz, false,
					"%s", config->sep_symbol);
		} else {
			cairo_set_line_width(cairo, 1);
			cairo_move_to(cairo, x_pos + sep_block_width / 2, margin);
			cairo_line_to(cairo, x_pos + sep_block_width / 2, height - margin);
			cairo_stroke(cairo);
		}
	}
	return 0;
}

static uint32_t render_status_line_i3bar(cairo_t *cairo,
		struct swaybar_output *output, double *l) {
	uint32_t max_breadth = 0;
	bool edge = *l == output->breadth * output->scale;
	struct i3bar_block *block;
	wl_list_for_each(block, &output->bar->status->blocks, link) {
		uint32_t b = render_status_block(cairo, output, block, l, edge);
		max_breadth = b > max_breadth ? b : max_breadth;
		edge = false;
	}
	return max_breadth;
}

static uint32_t render_status_line(cairo_t *cairo,
		struct swaybar_output *output, double *l) {
	struct status_line *status = output->bar->status;
	switch (status->protocol) {
	case PROTOCOL_ERROR:
		return render_status_line_error(cairo, output, l);
	case PROTOCOL_TEXT:
		return render_status_line_text(cairo, output, l);
	case PROTOCOL_I3BAR:
		return render_status_line_i3bar(cairo, output, l);
	case PROTOCOL_UNDEF:
		return 0;
	}
	return 0;
}

static uint32_t render_binding_mode_indicator(cairo_t *cairo,
		struct swaybar_output *output, double l) {
	const char *mode = output->bar->mode;
	if (!mode) {
		return 0;
	}

	struct swaybar_config *config = output->bar->config;

	uint32_t lr_anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	bool is_horiz = (output->bar->config->position & lr_anchors) == lr_anchors;

	int text_width, text_height;
	get_text_size(cairo, config->font, &text_width, &text_height, NULL,
			output->scale, !is_horiz, output->bar->mode_pango_markup,
			"%s", mode);

	int ws_vertical_padding = WS_VERTICAL_PADDING * output->scale;
	int ws_horizontal_padding = WS_HORIZONTAL_PADDING * output->scale;
	int border_width = BORDER_WIDTH * output->scale;

	uint32_t ideal_height = text_height + ws_vertical_padding * 2
		+ border_width * 2;
	uint32_t ideal_surface_height = ideal_height / output->scale;
	if (!output->bar->config->breadth &&
			output->breadth < ideal_surface_height) {
		return ideal_surface_height;
	}
	uint32_t width = text_width + ws_horizontal_padding * 2 + border_width * 2;

	uint32_t height = output->breadth * output->scale;

	cairo_set_source_u32(cairo, config->colors.binding_mode.background);
	cairo_rectangle(cairo, l, 0, width, height);
	cairo_fill(cairo);

	cairo_set_source_u32(cairo, config->colors.binding_mode.border);
	cairo_rectangle(cairo, l, 0, width, border_width);
	cairo_fill(cairo);
	cairo_rectangle(cairo, l, 0, border_width, height);
	cairo_fill(cairo);
	cairo_rectangle(cairo, l + width - border_width, 0, border_width, height);
	cairo_fill(cairo);
	cairo_rectangle(cairo, l, height - border_width, width, border_width);
	cairo_fill(cairo);

	double text_y = height / 2.0 - text_height / 2.0;
	cairo_set_source_u32(cairo, config->colors.binding_mode.text);
	cairo_move_to(cairo, l + width / 2 - text_width / 2, (int)floor(text_y));
	pango_printf(cairo, config->font, output->scale,
			!is_horiz, output->bar->mode_pango_markup, "%s", mode);

	return 0;
}

static enum hotspot_event_handling workspace_hotspot_callback(
		struct swaybar_output *output, struct swaybar_hotspot *hotspot,
		int x, int y, uint32_t button, void *data) {
	if (button != BTN_LEFT) {
		return HOTSPOT_PROCESS;
	}
	ipc_send_workspace_command(output->bar, (const char *)data);
	return HOTSPOT_IGNORE;
}

static uint32_t render_workspace_button(cairo_t *cairo,
		struct swaybar_output *output,
		struct swaybar_workspace *ws, double *l) {
	struct swaybar_config *config = output->bar->config;
	struct box_colors box_colors;
	if (ws->urgent) {
		box_colors = config->colors.urgent_workspace;
	} else if (ws->focused) {
		box_colors = config->colors.focused_workspace;
	} else if (ws->visible) {
		box_colors = config->colors.active_workspace;
	} else {
		box_colors = config->colors.inactive_workspace;
	}

	uint32_t breadth = output->breadth * output->scale;

	uint32_t lr_anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	bool is_horiz = (output->bar->config->position & lr_anchors) == lr_anchors;

	int text_width, text_height;
	get_text_size(cairo, config->font, &text_width, &text_height, NULL,
			output->scale, !is_horiz, config->pango_markup, "%s", ws->label);

	int ws_vertical_padding = WS_VERTICAL_PADDING * output->scale;
	int ws_horizontal_padding = WS_HORIZONTAL_PADDING * output->scale;
	int border_width = BORDER_WIDTH * output->scale;

	uint32_t ideal_height = ws_vertical_padding * 2 + text_height
		+ border_width * 2;
	uint32_t ideal_surface_breadth = ideal_height / output->scale;
	if (!output->bar->config->breadth &&
			output->breadth < ideal_surface_breadth) {
		return ideal_surface_breadth;
	}

	uint32_t length = ws_horizontal_padding * 2 + text_width + border_width * 2;

	cairo_set_source_u32(cairo, box_colors.background);
	cairo_rectangle(cairo, *l, 0, length, breadth);
	cairo_fill(cairo);
	cairo_set_source_u32(cairo, box_colors.border);
	cairo_rectangle(cairo, *l, 0, length, border_width);
	cairo_fill(cairo);
	cairo_rectangle(cairo, *l, 0, border_width, breadth);
	cairo_fill(cairo);
	cairo_rectangle(cairo, *l + length - border_width, 0, border_width, breadth);
	cairo_fill(cairo);
	cairo_rectangle(cairo, *l, breadth - border_width, length, border_width);
	cairo_fill(cairo);

	double text_y = breadth / 2.0 - text_height / 2.0;
	cairo_set_source_u32(cairo, box_colors.text);
	cairo_move_to(cairo, *l + length / 2 - text_width / 2, (int)floor(text_y));

	pango_printf(cairo, config->font, output->scale, !is_horiz, config->pango_markup,
			"%s", ws->label);

	struct swaybar_hotspot *hotspot = calloc(1, sizeof(struct swaybar_hotspot));
	hotspot->x = *l;
	hotspot->y = 0;
	hotspot->width = length;
	hotspot->height = breadth;
	hotspot->callback = workspace_hotspot_callback;
	hotspot->destroy = free;
	hotspot->data = strdup(ws->name);
	wl_list_insert(&output->hotspots, &hotspot->link);

	*l += length;
	return 0;
}

static uint32_t render_to_cairo(cairo_t *cairo, struct swaybar_output *output) {
	struct swaybar *bar = output->bar;
	struct swaybar_config *config = bar->config;
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	if (output->focused) {
		cairo_set_source_u32(cairo, config->colors.focused_background);
	} else {
		cairo_set_source_u32(cairo, config->colors.background);
	}
	cairo_paint(cairo);

	uint32_t lr_anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	bool is_horiz = (output->bar->config->position & lr_anchors) == lr_anchors;

	int th;
	get_text_size(cairo, config->font, NULL, &th, NULL, output->scale, !is_horiz, false, "");
	uint32_t max_breadth = (th + WS_VERTICAL_PADDING * 4) / output->scale;
	/*
	 * Each render_* function takes the actual height of the bar, and returns
	 * the ideal height. If the actual height is too short, the render function
	 * can do whatever it wants - the buffer won't be committed. If the actual
	 * height is too tall, the render function should adapt its drawing to
	 * utilize the available space.
	 */
	double l = output->length * output->scale;
#if HAVE_TRAY
	if (bar->tray) {
		uint32_t b = render_tray(cairo, output, &l);
		max_breadth = b > max_breadth ? b : max_breadth;
	}
#endif
	if (bar->status) {
		uint32_t b = render_status_line(cairo, output, &l);
		max_breadth = b > max_breadth ? b : max_breadth;
	}
	l = 0;
	if (config->workspace_buttons) {
		struct swaybar_workspace *ws;
		wl_list_for_each(ws, &output->workspaces, link) {
			uint32_t b = render_workspace_button(cairo, output, ws, &l);
			max_breadth = b > max_breadth ? b : max_breadth;
		}
	}
	if (config->binding_mode_indicator) {
		uint32_t b = render_binding_mode_indicator(cairo, output, l);
		max_breadth = b > max_breadth ? b : max_breadth;
	}

	return max_breadth > output->breadth ? max_breadth : output->breadth;
}

static void output_frame_handle_done(void *data, struct wl_callback *callback,
		uint32_t time) {
	wl_callback_destroy(callback);
	struct swaybar_output *output = data;
	output->frame_scheduled = false;
	if (output->dirty) {
		render_frame(output);
		output->dirty = false;
	}
}

static const struct wl_callback_listener output_frame_listener = {
	.done = output_frame_handle_done
};

void render_frame(struct swaybar_output *output) {
	assert(output->surface != NULL);
	if (!output->layer_surface) {
		return;
	}

	free_hotspots(&output->hotspots);

	uint32_t lr_anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	bool is_horiz = (output->bar->config->position & lr_anchors) == lr_anchors;

	cairo_surface_t *recorder = cairo_recording_surface_create(
			CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(recorder);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	cairo_font_options_set_subpixel_order(fo, to_cairo_subpixel_order(output->subpixel));
	cairo_set_font_options(cairo, fo);
	cairo_font_options_destroy(fo);
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);
	if (!is_horiz) {
		cairo_matrix_t mtx;
		mtx.x0 = output->width;
		mtx.y0 = 0.;
		mtx.xx = 0.;
		mtx.yy = 0.;
		mtx.xy = -1.0;
		mtx.yx = 1.0;
		cairo_transform(cairo, &mtx);
	}
	output->length = is_horiz ? output->width : output->height;
	output->breadth = is_horiz ? output->height : output->width;
	uint32_t breadth = render_to_cairo(cairo, output);

	if (!is_horiz) {
		struct swaybar_hotspot *hotspot;
		wl_list_for_each(hotspot, &output->hotspots, link) {
			uint32_t t;
			t = hotspot->height;
			hotspot->height = hotspot->width;
			hotspot->width = t;

			t = hotspot->x;
			hotspot->x = hotspot->y;
			hotspot->y = t;
		}
	}

	int config_breadth = output->bar->config->breadth;
	if (config_breadth > 0) {
		breadth = config_breadth;
	}

	bool reconf = false;
	if (is_horiz) {
		reconf = breadth != output->height || output->width == 0;
	} else {
		reconf = breadth != output->width || output->height == 0;
	}
	if (reconf) {
		// Reconfigure surface
		if (is_horiz) {
			zwlr_layer_surface_v1_set_size(output->layer_surface, 0, breadth);
		} else {
			zwlr_layer_surface_v1_set_size(output->layer_surface, breadth, 0);
		}
		zwlr_layer_surface_v1_set_margin(output->layer_surface,
				output->bar->config->gaps.top,
				output->bar->config->gaps.right,
				output->bar->config->gaps.bottom,
				output->bar->config->gaps.left);
		if (strcmp(output->bar->config->mode, "dock") == 0) {
			zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, breadth);
		}
		// TODO: this could infinite loop if the compositor assigns us a
		// different height than what we asked for
		wl_surface_commit(output->surface);
	} else if (breadth > 0) {
		// Replay recording into shm and send it off
		output->current_buffer = get_next_buffer(output->bar->shm,
				output->buffers,
				output->width * output->scale,
				output->height * output->scale);
		if (!output->current_buffer) {
			cairo_surface_destroy(recorder);
			cairo_destroy(cairo);
			return;
		}
		cairo_t *shm = output->current_buffer->cairo;

		cairo_save(shm);
		cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
		cairo_paint(shm);
		cairo_restore(shm);

		cairo_set_source_surface(shm, recorder, 0.0, 0.0);
		cairo_paint(shm);

		wl_surface_set_buffer_scale(output->surface, output->scale);
		wl_surface_attach(output->surface,
				output->current_buffer->buffer, 0, 0);
		wl_surface_damage(output->surface, 0, 0,
				output->width, output->height);

		struct wl_callback *frame_callback = wl_surface_frame(output->surface);
		wl_callback_add_listener(frame_callback, &output_frame_listener, output);
		output->frame_scheduled = true;

		wl_surface_commit(output->surface);
	}
	cairo_surface_destroy(recorder);
	cairo_destroy(cairo);
}
