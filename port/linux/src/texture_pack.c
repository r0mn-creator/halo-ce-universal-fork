/*
TEXTURE_PACK.C

The texture pack (texture_pack.h): which replacement stands for a bitmap
being uploaded, and each one's GL texture.

Every replacement is a PNG the size of its bitmap times a whole number (a pack
is made at 2x, or 4x for the smallest), so the game's own texture coordinates
fit it unchanged. A decoded texture is several times the size of the DXT
bitmap it replaces, so how much of the pack is in GL memory at once is
limited: past the limit the least recently used texture is dropped and decoded
again if it is drawn again.
*/

#include "texture_pack.h"
#include "hud_hires.h"
#include "platform.h"
#include "port_config.h"
#include "xgpu.h"

#include "memory/zlib/zlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* what the decoded textures (with their mip levels) may take, in bytes */
#define PACK_MEMORY_LIMIT (384UL * 1024 * 1024)

struct pack_asset
{
	unsigned long crc;
	unsigned long width, height;
	unsigned int texture;
	unsigned long levels;
	unsigned long bytes;
	unsigned long last_used;
	int failed;
};

static struct pack_asset *assets;
static long asset_count;
static int initialized, enabled;
/* -1: follow display.texture_pack; 0/1: set live by control.txt */
static int runtime_enabled = -1;
static unsigned long use_clock, resident_bytes;

static int compare_assets(const void *a, const void *b)
{
	unsigned long x = ((const struct pack_asset *)a)->crc, y = ((const struct pack_asset *)b)->crc;

	return x < y ? -1 : x > y;
}

static void pack_path(char *path, size_t size, const char *name)
{
	snprintf(path, size, "%s/texture_pack/%s", platform_data_root(), name);
}

static void initialize(void)
{
	char path[512], line[64];
	FILE *file;
	long capacity = 0;

	initialized = 1;
	enabled = runtime_enabled >= 0 ? runtime_enabled : config_boolean("display.texture_pack");
	if (!enabled)
		return;
	pack_path(path, sizeof(path), "index.txt");
	file = fopen(path, "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file))
	{
		unsigned long crc;

		if (sscanf(line, "%lx", &crc) != 1)
			continue;
		if (asset_count == capacity)
		{
			capacity = capacity ? capacity * 2 : 1024;
			assets = realloc(assets, capacity * sizeof(*assets));
			if (!assets)
			{
				asset_count = 0;
				break;
			}
		}
		memset(&assets[asset_count], 0, sizeof(assets[0]));
		assets[asset_count++].crc = crc;
	}
	fclose(file);
	if (asset_count)
		qsort(assets, (size_t)asset_count, sizeof(*assets), compare_assets);
	platform_log("texture pack: %ld textures in %s", asset_count, path);
}

long texture_pack_find(unsigned long address, unsigned long width, unsigned long height,
	unsigned long level0_size)
{
	struct pack_asset key, *found;

	if (!initialized)
		initialize();
	if (!asset_count)
		return -1;
	key.crc = crc32(0L, (const Bytef *)address, (uInt)level0_size) & 0xffffffffUL;
	found = bsearch(&key, assets, (size_t)asset_count, sizeof(*assets), compare_assets);
	if (!found || found->failed)
		return -1;
	found->width = width;
	found->height = height;
	return (long)(found - assets);
}

static unsigned long big_endian_long(const unsigned char *bytes)
{
	return ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) |
		((unsigned long)bytes[2] << 8) | bytes[3];
}

/* drops the least recently used texture other than keep; 0 if none is left */
static int drop_oldest(struct pack_asset *keep)
{
	struct pack_asset *oldest = NULL;
	long index;

	for (index = 0; index < asset_count; index++)
	{
		if (assets[index].texture && &assets[index] != keep &&
			(!oldest || assets[index].last_used < oldest->last_used))
			oldest = &assets[index];
	}
	if (!oldest)
		return 0;
	glDeleteTextures(1, &oldest->texture);
	resident_bytes -= oldest->bytes;
	oldest->texture = 0;
	oldest->bytes = 0;
	return 1;
}

static unsigned char *load_png(const struct pack_asset *asset, unsigned long *width, unsigned long *height)
{
	char name[32], path[512];
	unsigned char *data, *pixels;
	unsigned long size;
	FILE *file;

	snprintf(name, sizeof(name), "%08lx.png", asset->crc);
	pack_path(path, sizeof(path), name);
	file = fopen(path, "rb");
	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = (unsigned long)ftell(file);
	fseek(file, 0, SEEK_SET);
	data = size >= 33 ? malloc(size) : NULL;
	if (!data || fread(data, 1, size, file) != size)
	{
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	if (memcmp(data, "\x89PNG\r\n\x1a\n", 8) || memcmp(data + 12, "IHDR", 4))
	{
		free(data);
		return NULL;
	}
	*width = big_endian_long(data + 16);
	*height = big_endian_long(data + 20);
	/* a whole multiple of the bitmap, the same both ways (so its texture
	coordinates are the bitmap's), and a size every GL can take */
	if (!asset->width || !asset->height || *width % asset->width || *height % asset->height ||
		*width / asset->width != *height / asset->height || *width / asset->width < 2 ||
		*width > 4096 || *height > 4096)
	{
		free(data);
		return NULL;
	}
	pixels = hud_hires_png_decode(data, size, *width, *height);
	free(data);
	return pixels;
}

unsigned int texture_pack_texture(long asset_index, unsigned long *levels)
{
	struct pack_asset *asset;
	unsigned char *pixels;
	unsigned long width, height, largest;
	GLuint texture;

	if (asset_index < 0 || asset_index >= asset_count)
		return 0;
	asset = &assets[asset_index];
	asset->last_used = ++use_clock;
	if (asset->failed)
		return 0;
	if (asset->texture)
	{
		*levels = asset->levels;
		return asset->texture;
	}
	pixels = load_png(asset, &width, &height);
	if (!pixels)
	{
		platform_log("texture pack: could not use %08lx.png for the %lux%lu bitmap", asset->crc,
			asset->width, asset->height);
		asset->failed = 1;
		return 0;
	}
	asset->levels = 1;
	for (largest = width > height ? width : height; largest > 1; largest >>= 1)
		asset->levels++;
	asset->bytes = width * height * 4 + width * height * 4 / 3;
	while (resident_bytes + asset->bytes > PACK_MEMORY_LIMIT && drop_oldest(asset))
		;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	xgpu_gl_state_invalidate();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)asset->levels - 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glGenerateMipmap(GL_TEXTURE_2D);
	xgpu_gl_state_invalidate();
	free(pixels);
	asset->texture = texture;
	resident_bytes += asset->bytes;
	platform_log("texture pack: %08lx.png drawn for a %lux%lu bitmap as %lux%lu (%lu MB resident)", asset->crc,
		asset->width, asset->height, width, height, resident_bytes >> 20);
	*levels = asset->levels;
	return texture;
}

/* ---------- live control

texture_pack/control.txt, looked at every half second or so: its first word
"off" draws the maps' own bitmaps, "on" the pack, and anything after it is
only there to make the file different (write "on 2" after "on 1") so that
"on" also reads the pack's folder again, which is how new textures are tried
without a rebuild or a restart. */

static void forget_everything(void)
{
	long index;

	for (index = 0; index < asset_count; index++)
	{
		if (assets[index].texture)
			glDeleteTextures(1, &assets[index].texture);
	}
	xgpu_gl_state_invalidate();
	free(assets);
	assets = NULL;
	asset_count = 0;
	resident_bytes = 0;
	initialized = 0;
}

/* the pad's Back button asks for this from the input code, which is not
necessarily the thread that owns the GL context: the flip itself happens at
the start of the next frame */
static volatile int toggle_requested;

void texture_pack_request_toggle(void)
{
	toggle_requested = 1;
}

int texture_pack_control_poll(void)
{
	static unsigned long calls;
	static char last[64];
	char path[512], content[64];
	size_t length;
	FILE *file;

	if (toggle_requested)
	{
		int now = runtime_enabled >= 0 ? runtime_enabled : config_boolean("display.texture_pack");

		toggle_requested = 0;
		runtime_enabled = !now;
		platform_log("texture pack: switched %s with the pad's Back button", runtime_enabled ? "on" : "off");
		forget_everything();
		return 1;
	}
	if (++calls % 30)
		return 0;
	pack_path(path, sizeof(path), "control.txt");
	file = fopen(path, "r");
	if (!file)
		return 0;
	length = fread(content, 1, sizeof(content) - 1, file);
	fclose(file);
	content[length] = '\0';
	if (!strcmp(content, last))
		return 0;
	strcpy(last, content);
	if (!strncmp(content, "off", 3))
		runtime_enabled = 0;
	else if (!strncmp(content, "on", 2))
		runtime_enabled = 1;
	else
		return 0;
	platform_log("texture pack: switched %s from %s", runtime_enabled ? "on" : "off", path);
	forget_everything();
	return 1;
}
