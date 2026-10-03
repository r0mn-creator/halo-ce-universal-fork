/*
GAME_MENU.H

The graphics menu: the pad's Back (Select) button slides a panel in from the right
of the screen (a quarter of its width at most) with the renderer's options, drawn
by the host over the finished picture. The pad drives it (D-pad, A to toggle or
step a choice, B or Back to close) and so does touch (a tap on a row, a tap
outside the panel closes it); while it is open the game sees no input.
*/

#ifndef GAME_MENU_H
#define GAME_MENU_H

/* menu_font.c (made by tools/menu_font.py) */
extern const int game_menu_font_size, game_menu_font_cell_width, game_menu_font_cell_height;
extern const int game_menu_font_margin, game_menu_font_columns, game_menu_font_first, game_menu_font_last;
extern const int game_menu_font_atlas_width, game_menu_font_atlas_height;
extern const unsigned long game_menu_font_atlas_size;
extern const int game_menu_font_advances[];
extern const unsigned int game_menu_font_atlas[]; /* the atlas's bytes, as little-endian words */

/* the pad's raw buttons, from the input code once per poll: nonzero while the
menu has the pad, when the game has to be shown it neutral */
int game_menu_pad(int back, int up, int down, int left, int right, int a, int b);
/* a finger touched the screen at this window pixel: nonzero if the menu took it */
int game_menu_touch(float x, float y);
/* draws the menu over the window's picture when it is showing (at present time) */
void game_menu_draw(int window_width, int window_height);

/* what the renderer reads from the menu's settings */
float game_menu_render_scale(void);
int game_menu_anisotropy(void);
/* the last pass of a frame: draws the game's picture (a texture) into the window's
rectangle through anti-aliasing (FXAA); 0 if it is off and the caller copies the picture
as it always did */
int game_menu_post_draw(unsigned int source_texture, int x, int y, int width, int height,
	int window_width, int window_height);

#endif
