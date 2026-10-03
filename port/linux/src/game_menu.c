/*
GAME_MENU.C

The graphics menu (game_menu.h): its rows and their values, how the pad and touch
move through them, and how it is drawn.

It is drawn with one textured, alpha blended triangle list: a font atlas
(menu_font.c, one 8-bit image) whose first cell is solid, so rectangles and text
share a shader. Everything is laid out in window pixels from the window's height,
so it looks the same on any display.
*/

#include "game_menu.h"
#include "platform.h"
#include "port_config.h"
#include "sdl_platform.h"
#include "texture_pack.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef HALO_ANDROID

/* the desktop builds have no menu yet: the renderer's own settings stand */
int game_menu_pad(int back, int up, int down, int left, int right, int a, int b)
{
	(void)back; (void)up; (void)down; (void)left; (void)right; (void)a; (void)b;
	return 0;
}
int game_menu_touch(float x, float y) { (void)x; (void)y; return 0; }
void game_menu_draw(int window_width, int window_height) { (void)window_width; (void)window_height; }
float game_menu_render_scale(void) { return 1.0f; }
int game_menu_anisotropy(void) { return 1; }
int game_menu_post_draw(unsigned int source_texture, int x, int y, int width, int height, int window_width, int window_height)
{
	(void)source_texture; (void)x; (void)y; (void)width; (void)height; (void)window_width; (void)window_height;
	return 0;
}

#else

#include "xgpu.h"

#include <stdlib.h>

/* ---------- the rows */

enum { _row_toggle, _row_choice, _row_unavailable };

struct row
{
	const char *label;
	int kind;
	const char *const *names;
	int count;
	int value;
};

static const char *const resolution_names[] = { "480p", "720p", "960p", "1080p" };
static const float resolution_scales[] = { 1.0f, 1.5f, 2.0f, 2.25f };
static const char *const anisotropy_names[] = { "Off", "x2", "x4", "x8", "x16" };
static const int anisotropy_levels[] = { 1, 2, 4, 8, 16 };
static const char *const antialiasing_names[] = { "Off", "FXAA" };

enum { ROW_TEXTURE_PACK, ROW_RESOLUTION, ROW_ANISOTROPY, ROW_ANTIALIASING, ROW_COUNT };

static struct row rows[ROW_COUNT] =
{
	{ "Texture pack", _row_toggle, NULL, 2, 0 },
	{ "Resolution", _row_choice, resolution_names, 4, 2 },
	{ "Anisotropic", _row_choice, anisotropy_names, 5, 0 },
	{ "Anti-aliasing", _row_choice, antialiasing_names, 2, 0 },
};

static int initialized;
static int selected;
static int is_open;
static float progress; /* 0 closed .. 1 fully open */
static struct timespec last_time;

static void initialize(void)
{
	double scale = config_real("display.render_scale");
	int index, best = 2;
	float best_distance = 1000.0f;

	initialized = 1;
	/* debug.game_menu_open: shows the menu from the start, for screenshots */
	if (config_boolean("debug.game_menu_open"))
		is_open = 1;
	for (index = 0; index < 4; index++)
	{
		float distance = fabsf(resolution_scales[index] - (float)scale);

		if (distance < best_distance)
		{
			best_distance = distance;
			best = index;
		}
	}
	rows[ROW_RESOLUTION].value = best;
	/* the settings the menu starts from (config.toml's display.*) */
	{
		int wanted = (int)config_integer("display.anisotropy");

		best = 0;
		for (index = 0; index < 5; index++)
		{
			if (anisotropy_levels[index] <= wanted)
				best = index;
		}
		rows[ROW_ANISOTROPY].value = best;
		rows[ROW_ANTIALIASING].value = config_boolean("display.fxaa") ? 1 : 0;
	}
}

float game_menu_render_scale(void)
{
	if (!initialized)
		initialize();
	return resolution_scales[rows[ROW_RESOLUTION].value];
}

int game_menu_anisotropy(void)
{
	return anisotropy_levels[rows[ROW_ANISOTROPY].value];
}

static void change(int row, int step, int wrap)
{
	struct row *entry = &rows[row];

	if (entry->kind == _row_unavailable)
		return;
	if (entry->kind == _row_toggle)
	{
		/* the pack flips at the start of the next frame; the row follows it */
		texture_pack_request_toggle();
		return;
	}
	entry->value += step;
	if (entry->value < 0)
		entry->value = wrap ? entry->count - 1 : 0;
	if (entry->value >= entry->count)
		entry->value = wrap ? 0 : entry->count - 1;
}

/* ---------- input */

static void set_open(int open)
{
	if (open && !is_open)
	{
		if (!initialized)
			initialize();
		selected = 0;
	}
	is_open = open;
}

int game_menu_pad(int back, int up, int down, int left, int right, int a, int b)
{
	static int was_back, was_up, was_down, was_left, was_right, was_a, was_b;

	if (back && !was_back)
		set_open(!is_open);
	else if (is_open)
	{
		if (b && !was_b)
			set_open(0);
		if (up && !was_up)
			selected = (selected + ROW_COUNT - 1) % ROW_COUNT;
		if (down && !was_down)
			selected = (selected + 1) % ROW_COUNT;
		if (left && !was_left)
			change(selected, -1, 0);
		if (right && !was_right)
			change(selected, +1, 0);
		if (a && !was_a)
			change(selected, +1, 1);
	}
	was_back = back; was_up = up; was_down = down; was_left = left; was_right = right;
	was_a = a; was_b = b;
	return is_open || progress > 0.0f;
}

/* ---------- layout (window pixels) */

struct layout
{
	float panel_x, panel_width;
	float title_y, title_size;
	float row_y, row_height, row_size;
	float margin;
	float safe; /* the height the system bars take at the top and the bottom */
};

static float ease(float t)
{
	return t * t * (3.0f - 2.0f * t);
}

static void layout_get(int width, int height, struct layout *l)
{
	l->panel_width = (float)width * 0.25f;
	l->panel_x = (float)width - l->panel_width * ease(progress);
	l->row_height = (float)height * 0.085f;
	l->row_size = l->row_height * 0.38f;
	l->title_size = l->row_height * 0.52f;
	l->margin = l->panel_width * 0.06f;
	l->safe = (float)height * 0.058f;
	l->title_y = l->safe + (float)height * 0.012f;
	l->row_y = l->title_y + l->title_size * 1.9f;
}

int game_menu_touch(float x, float y)
{
	int width, height, row;
	struct layout l;

	if (!is_open)
		return 0;
	platform_video_drawable_size(&width, &height);
	layout_get(width, height, &l);
	if (x < l.panel_x)
	{
		set_open(0);
		return 1;
	}
	row = (int)floorf((y - l.row_y) / l.row_height);
	if (row >= 0 && row < ROW_COUNT)
	{
		selected = row;
		change(row, +1, 1);
	}
	return 1;
}

/* ---------- drawing */

static GLuint program, vertex_array, vertex_buffer, atlas_texture;
static GLint size_uniform, atlas_uniform;
static int failed;

#define MAXIMUM_QUADS 512
static float vertices[MAXIMUM_QUADS * 6 * 8];
static int vertex_count;

static GLuint compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[512];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("game menu: shader: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static int gl_initialize(void)
{
	static const char *vertex_source =
		"#version 300 es\n"
		"layout(location = 0) in vec2 a_position;\n"
		"layout(location = 1) in vec2 a_uv;\n"
		"layout(location = 2) in vec4 a_color;\n"
		"uniform vec2 u_size;\n"
		"out vec2 v_uv;\n"
		"out vec4 v_color;\n"
		"void main() {\n"
		"    vec2 c = a_position / u_size;\n"
		"    gl_Position = vec4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);\n"
		"    v_uv = a_uv;\n"
		"    v_color = a_color;\n"
		"}\n";
	static const char *fragment_source =
		"#version 300 es\n"
		"precision highp float;\n"
		"in vec2 v_uv;\n"
		"in vec4 v_color;\n"
		"uniform sampler2D u_atlas;\n"
		"out vec4 o_color;\n"
		"void main() {\n"
		"    o_color = vec4(v_color.rgb, v_color.a * texture(u_atlas, v_uv).a);\n"
		"}\n";
	GLuint vertex_shader, fragment_shader;
	unsigned char *rgba;
	unsigned long index;

	vertex_shader = compile(GL_VERTEX_SHADER, vertex_source);
	fragment_shader = compile(GL_FRAGMENT_SHADER, fragment_source);
	if (!vertex_shader || !fragment_shader)
		return 0;
	program = glCreateProgram();
	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);
	glLinkProgram(program);
	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);
	size_uniform = glGetUniformLocation(program, "u_size");
	atlas_uniform = glGetUniformLocation(program, "u_atlas");

	rgba = malloc(game_menu_font_atlas_size * 4);
	if (!rgba)
	{
		platform_log("game menu: out of memory for the font");
		return 0;
	}
	for (index = 0; index < game_menu_font_atlas_size; index++)
	{
		rgba[index * 4 + 0] = rgba[index * 4 + 1] = rgba[index * 4 + 2] = 255;
		rgba[index * 4 + 3] = ((const unsigned char *)game_menu_font_atlas)[index];
	}
	glGenTextures(1, &atlas_texture);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, atlas_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, game_menu_font_atlas_width, game_menu_font_atlas_height, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free(rgba);

	glGenVertexArrays(1, &vertex_array);
	glGenBuffers(1, &vertex_buffer);
	return 1;
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const float *color)
{
	float *v;
	int corner;
	static const int order[6][2] = { {0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1} };

	if (vertex_count + 6 > MAXIMUM_QUADS * 6)
		return;
	v = vertices + vertex_count * 8;
	for (corner = 0; corner < 6; corner++)
	{
		int right = order[corner][0], down = order[corner][1];

		*v++ = right ? x1 : x0;
		*v++ = down ? y1 : y0;
		*v++ = right ? u1 : u0;
		*v++ = down ? v1 : v0;
		memcpy(v, color, sizeof(float) * 4);
		v += 4;
	}
	vertex_count += 6;
}

static void rectangle(float x0, float y0, float x1, float y1, const float *color)
{
	/* the middle of the solid cell (cell 0), away from its edges */
	float u = (float)game_menu_font_cell_width * 0.5f / (float)game_menu_font_atlas_width;
	float v = (float)game_menu_font_cell_height * 0.5f / (float)game_menu_font_atlas_height;

	quad(x0, y0, x1, y1, u, v, u, v, color);
}

static float text_width(const char *text, float size)
{
	float scale = size / (float)game_menu_font_size, width = 0.0f;

	for (; *text; text++)
	{
		if (*text >= game_menu_font_first && *text <= game_menu_font_last)
			width += game_menu_font_advances[*text - game_menu_font_first] * scale;
	}
	return width;
}

static void text(float x, float y, float size, const char *string, const float *color)
{
	float scale = size / (float)game_menu_font_size;

	for (; *string; string++)
	{
		int cell;
		float cx, cy, u0, v0;

		if (*string < game_menu_font_first || *string > game_menu_font_last)
			continue;
		cell = *string - game_menu_font_first + 1;
		cx = (float)((cell % game_menu_font_columns) * game_menu_font_cell_width);
		cy = (float)((cell / game_menu_font_columns) * game_menu_font_cell_height);
		u0 = cx / (float)game_menu_font_atlas_width;
		v0 = cy / (float)game_menu_font_atlas_height;
		if (*string != ' ')
		{
			quad(x - (float)game_menu_font_margin * scale, y - (float)game_menu_font_margin * scale,
				x - (float)game_menu_font_margin * scale + game_menu_font_cell_width * scale, y - (float)game_menu_font_margin * scale + game_menu_font_cell_height * scale,
				u0, v0, u0 + (float)game_menu_font_cell_width / (float)game_menu_font_atlas_width,
				v0 + (float)game_menu_font_cell_height / (float)game_menu_font_atlas_height, color);
		}
		x += game_menu_font_advances[*string - game_menu_font_first] * scale;
	}
}

static const float colour_panel[4] = { 0.045f, 0.060f, 0.095f, 0.90f };
static const float colour_accent[4] = { 0.30f, 0.72f, 1.00f, 1.00f };
static const float colour_selected[4] = { 0.20f, 0.45f, 0.78f, 0.38f };
static const float colour_text[4] = { 0.93f, 0.95f, 1.00f, 1.00f };
static const float colour_dim[4] = { 0.50f, 0.55f, 0.63f, 1.00f };
static const float colour_on[4] = { 0.40f, 0.88f, 0.52f, 1.00f };
static const float colour_off[4] = { 0.36f, 0.39f, 0.46f, 1.00f };
static const float colour_knob[4] = { 0.96f, 0.97f, 1.00f, 1.00f };

static void advance_animation(void)
{
	struct timespec now;
	float seconds;

	clock_gettime(CLOCK_MONOTONIC, &now);
	seconds = (float)(now.tv_sec - last_time.tv_sec) + (float)(now.tv_nsec - last_time.tv_nsec) * 1e-9f;
	last_time = now;
	if (seconds < 0.0f || seconds > 0.25f)
		seconds = 0.0f;
	progress += (is_open ? 1.0f : -1.0f) * seconds / 0.20f;
	if (progress < 0.0f)
		progress = 0.0f;
	if (progress > 1.0f)
		progress = 1.0f;
}

void game_menu_draw(int window_width, int window_height)
{
	struct layout l;
	int index;
	float y, height_size;

	if (!initialized)
		initialize();
	advance_animation();
	/* set up on the first frame, not on the first press, so a problem shows at once */
	if (!program && !failed && !gl_initialize())
		failed = 1;
	if (failed || progress <= 0.0f)
		return;
	if (!initialized)
		initialize();
	layout_get(window_width, window_height, &l);
	vertex_count = 0;

	/* the panel, with a bright edge */
	rectangle(l.panel_x, 0.0f, (float)window_width, (float)window_height, colour_panel);
	rectangle(l.panel_x, 0.0f, l.panel_x + (float)window_height * 0.004f, (float)window_height, colour_accent);
	text(l.panel_x + l.margin, l.title_y, l.title_size, "GRAPHICS", colour_text);
	rectangle(l.panel_x + l.margin, l.row_y - l.row_height * 0.22f, (float)window_width - l.margin,
		l.row_y - l.row_height * 0.22f + (float)window_height * 0.002f, colour_accent);

	for (index = 0; index < ROW_COUNT; index++)
	{
		const struct row *entry = &rows[index];
		const float *label_colour = entry->kind == _row_unavailable ? colour_dim : colour_text;
		char value_text[32];
		float row_top, text_y, value_x, value_width, size = l.row_size;
		int toggle_on = index == ROW_TEXTURE_PACK ? texture_pack_is_enabled() : 0;

		row_top = l.row_y + l.row_height * (float)index;
		text_y = row_top + (l.row_height - l.row_size * 1.25f) * 0.5f;
		if (index == selected)
			rectangle(l.panel_x + l.margin * 0.5f, row_top, (float)window_width - l.margin * 0.5f,
				row_top + l.row_height * 0.92f, colour_selected);
		/* a row whose label and value together are wider than the panel shrinks to fit */
		if (entry->kind != _row_toggle)
		{
			float room = l.panel_width - 2.0f * l.margin, gap = l.row_size, need;

			snprintf(value_text, sizeof(value_text), "< %s >", entry->names[entry->value]);
			need = text_width(entry->label, l.row_size) + gap + text_width(value_text, l.row_size);
			if (need > room)
				size = l.row_size * room / need;
		}
		text_y = row_top + (l.row_height * 0.92f - size * 1.25f) * 0.5f;
		text(l.panel_x + l.margin, text_y, size, entry->label, label_colour);

		if (entry->kind == _row_toggle)
		{
			float track_w = l.row_size * 2.1f, track_h = l.row_size * 1.1f;
			float track_x = (float)window_width - l.margin - track_w;
			float track_y = row_top + (l.row_height * 0.92f - track_h) * 0.5f;
			float knob = track_h * 0.78f;
			float knob_x = toggle_on ? track_x + track_w - knob - track_h * 0.11f : track_x + track_h * 0.11f;

			rectangle(track_x, track_y, track_x + track_w, track_y + track_h, toggle_on ? colour_on : colour_off);
			rectangle(knob_x, track_y + (track_h - knob) * 0.5f, knob_x + knob, track_y + (track_h + knob) * 0.5f, colour_knob);
		}
		else
		{
			if (entry->kind == _row_choice && index == selected)
				snprintf(value_text, sizeof(value_text), "< %s >", entry->names[entry->value]);
			else
				snprintf(value_text, sizeof(value_text), "%s", entry->names[entry->value]);
			value_width = text_width(value_text, size);
			value_x = (float)window_width - l.margin - value_width;
			text(value_x, text_y, size, value_text,
				entry->kind == _row_unavailable ? colour_dim : (index == selected ? colour_accent : colour_text));
		}
	}

	{
		static const char *const hints[] = { "D-pad  move / change", "A  toggle / next", "B or Back  close" };
		float room = l.panel_width - 2.0f * l.margin, widest = 0.0f;
		int hint;

		height_size = l.row_size * 0.70f;
		for (hint = 0; hint < 3; hint++)
		{
			float width = text_width(hints[hint], height_size);

			if (width > widest)
				widest = width;
		}
		if (widest > room)
			height_size *= room / widest;
		y = (float)window_height - l.safe - height_size * 1.55f * 3.4f;
		for (hint = 0; hint < 3; hint++)
			text(l.panel_x + l.margin, y + height_size * 1.55f * (float)hint, height_size, hints[hint], colour_dim);
	}

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glViewport(0, 0, window_width, window_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(program);
	glUniform2f(size_uniform, (float)window_width, (float)window_height);
	glUniform1i(atlas_uniform, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, atlas_texture);
	glBindSampler(0, 0);
	glBindVertexArray(vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertex_count * 8 * sizeof(float)), vertices, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (const void *)(2 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (const void *)(4 * sizeof(float)));
	glDrawArrays(GL_TRIANGLES, 0, vertex_count);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDisable(GL_BLEND);
	xgpu_gl_state_invalidate();
}

/* ---------- anti-aliasing: the last pass of a frame */

static GLuint post_program;
static GLint post_size_uniform, post_source_uniform;
static int post_failed;

static int post_initialize(void)
{
	static const char *vertex_source =
		"#version 300 es\n"
		"layout(location = 0) in vec2 a_position;\n"
		"layout(location = 1) in vec2 a_uv;\n"
		"uniform vec2 u_size;\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    vec2 c = a_position / u_size;\n"
		"    gl_Position = vec4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);\n"
		"    v_uv = a_uv;\n"
		"}\n";
	/* FXAA (Lottes' first version): the edge's direction is read from the luma of the
	pixel's four diagonal neighbours, and the pixel is averaged along it; a step that
	would leave the neighbourhood's range falls back to the narrower blend */
	static const char *fragment_source =
		"#version 300 es\n"
		"precision highp float;\n"
		"in vec2 v_uv;\n"
		"uniform sampler2D u_source;\n"
		"out vec4 o_color;\n"
		"const vec3 LUMA = vec3(0.299, 0.587, 0.114);\n"
		"void main() {\n"
		"    vec2 texel = 1.0 / vec2(textureSize(u_source, 0));\n"
		"    vec3 nw = texture(u_source, v_uv + vec2(-1.0, -1.0) * texel).rgb;\n"
		"    vec3 ne = texture(u_source, v_uv + vec2( 1.0, -1.0) * texel).rgb;\n"
		"    vec3 sw = texture(u_source, v_uv + vec2(-1.0,  1.0) * texel).rgb;\n"
		"    vec3 se = texture(u_source, v_uv + vec2( 1.0,  1.0) * texel).rgb;\n"
		"    vec3 m = texture(u_source, v_uv).rgb;\n"
		"    float lnw = dot(nw, LUMA), lne = dot(ne, LUMA), lsw = dot(sw, LUMA), lse = dot(se, LUMA), lm = dot(m, LUMA);\n"
		"    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));\n"
		"    float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
		"    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));\n"
		"    float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);\n"
		"    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
		"    dir = clamp(dir * scale, vec2(-8.0), vec2(8.0)) * texel;\n"
		"    vec3 a = 0.5 * (texture(u_source, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(u_source, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
		"    vec3 b = a * 0.5 + 0.25 * (texture(u_source, v_uv + dir * -0.5).rgb + texture(u_source, v_uv + dir * 0.5).rgb);\n"
		"    float lb = dot(b, LUMA);\n"
		"    o_color = vec4((lb < lmin || lb > lmax) ? a : b, 1.0);\n"
		"}\n";
	GLuint vertex_shader = compile(GL_VERTEX_SHADER, vertex_source);
	GLuint fragment_shader = compile(GL_FRAGMENT_SHADER, fragment_source);

	if (!vertex_shader || !fragment_shader)
		return 0;
	post_program = glCreateProgram();
	glAttachShader(post_program, vertex_shader);
	glAttachShader(post_program, fragment_shader);
	glLinkProgram(post_program);
	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);
	post_size_uniform = glGetUniformLocation(post_program, "u_size");
	post_source_uniform = glGetUniformLocation(post_program, "u_source");
	if (!vertex_array)
		glGenVertexArrays(1, &vertex_array);
	if (!vertex_buffer)
		glGenBuffers(1, &vertex_buffer);
	return 1;
}

int game_menu_post_draw(unsigned int source_texture, int x, int y, int width, int height,
	int window_width, int window_height)
{
	float top, left = (float)x, right = (float)(x + width), bottom, quad_vertices[6 * 4];
	static const float corner[6][2] = { {0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1} };
	int corner_index;

	if (rows[ROW_ANTIALIASING].value != 1 || post_failed)
		return 0;
	if (!post_program && !post_initialize())
	{
		post_failed = 1;
		return 0;
	}
	/* the window's rows count from the bottom, the menu's from the top */
	top = (float)(window_height - (y + height));
	bottom = top + (float)height;
	for (corner_index = 0; corner_index < 6; corner_index++)
	{
		quad_vertices[corner_index * 4 + 0] = corner[corner_index][0] ? right : left;
		quad_vertices[corner_index * 4 + 1] = corner[corner_index][1] ? bottom : top;
		/* row 0 of the game's render target is the top of the picture */
		quad_vertices[corner_index * 4 + 2] = corner[corner_index][0];
		quad_vertices[corner_index * 4 + 3] = corner[corner_index][1];
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glViewport(0, 0, window_width, window_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glUseProgram(post_program);
	glUniform2f(post_size_uniform, (float)window_width, (float)window_height);
	glUniform1i(post_source_uniform, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glBindTexture(GL_TEXTURE_2D, source_texture);
	/* the game's own samplers set this texture's filtering when it is drawn with; here it is
	read with the texture's own, which must be a plain linear one with no mip levels */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindVertexArray(vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad_vertices), quad_vertices, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)(2 * sizeof(float)));
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	xgpu_gl_state_invalidate();
	return 1;
}

#endif
