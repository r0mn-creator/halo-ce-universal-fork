/*
TEXTURE_PACK.H

A texture pack: replacement textures for the maps' bitmaps, made offline by
tools/texture_pack.py (the maps' bitmaps upscaled), and kept in the data
folder's texture_pack/ folder as <crc>.png, with an index.txt listing the
crcs. A bitmap is found by the CRC-32 of its first mip level's bytes as the
map holds them, so a pack works with any map that has the same bitmap, with
no tag names, and a bitmap the pack does not hold is drawn as it is.

The texture cache (xbox_textures.c) asks as a bitmap's pixels are uploaded,
and draws the replacement in the bitmap's place: the game still sizes and
places the bitmap by its tag, so nothing else changes.
*/

#ifndef TEXTURE_PACK_H
#define TEXTURE_PACK_H

/* the pack's texture standing for the bitmap whose pixels are at address
(guest virtual), with this size, its first mip level being level0_size
bytes; or -1: none, display.texture_pack off, or no pack */
long texture_pack_find(unsigned long address, unsigned long width, unsigned long height,
	unsigned long level0_size);
/* its GL texture (decoded and uploaded, mipmapped, when first used or when
used again after being dropped for memory; 0 if it could not be) and the
number of its mip levels */
unsigned int texture_pack_texture(long asset, unsigned long *levels);

/* looks at the pack's control.txt (see texture_pack.c) now and then; nonzero
when the pack was switched or reloaded, and every bitmap has to be looked up
again */
int texture_pack_control_poll(void);

/* the pad's Back button: switches between the pack and the maps' own bitmaps
at the start of the next frame (safe to call from any thread) */
void texture_pack_request_toggle(void);

#endif
