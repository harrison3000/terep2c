// What the game needs from a host, plus helpers shared by the hosts.
#pragma once
#include "../src/terep.hpp"

// Put the game's state back as at the start of the program (before game_init).
bool host_init_state();

// Free what a game allocated, to start a new one (host_init_state and
// game_init again) in the same process.
void host_free_game();

// Run one display frame of the original timing: two physics ticks
// (120 Hz) per rendered frame (60 Hz).  Returns true when Esc was handled.
bool host_step();

// RGB of palette entry i (the PCX palette, 8 bits per channel).
inline const uint8_t *host_palette(uint8_t i) { return &map_tables.palette[i * 3]; }
