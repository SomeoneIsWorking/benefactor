/* Level and world geometry, and the level-name table.
 *
 * Split out of game_loop.c, which had grown past its structural limit: the
 * frame loop and interrupt delivery are one concern, and "how many levels are
 * in world 3, and what are they called" is another. Nothing here runs per
 * frame.
 */
#ifndef BENEFACTOR_PORT_LEVEL_LAYOUT_H
#define BENEFACTOR_PORT_LEVEL_LAYOUT_H

#include <stdint.h>

int pc_extra_worlds_available(void);
int pc_num_worlds_ui(void);
int pc_num_levels_ui(void);
int pc_levels_in_world(int world);
int pc_world_first_level(int world);
void pc_level_split(int level, int *world_out, int *level_in_world_out);
void pc_preload_all_level_names(void);

/* Which banner is on screen. See the comment on the implementation: the three
 * banner paths share a copper list, and only the title card keeps a countdown
 * at $57FEF6, which is how they are told apart. */
int pc_is_banner_displayed(void);
int pc_is_title_card_displayed(void);
int pc_is_level_card_displayed(void); /* legacy alias of pc_is_banner_displayed */

#endif /* BENEFACTOR_PORT_LEVEL_LAYOUT_H */
