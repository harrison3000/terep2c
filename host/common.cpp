/*
 * Memory and files for the standalone hosts.
 *
 * The game's state is global (state_reset); the game allocates the rest
 * (game_init, game_free).
 */
#include <cstdlib>
#include "host.hpp"

void host_free_game()
{
    game_free();
}

bool host_init_state()
{
    state_reset();
    return true;
}

bool host_step()
{
    physics_tick();
    physics_tick();
    return game_frame() != 0;
}

#ifdef _WIN32
FILE *data_fopen(const char *name)
{
    return fopen(name, "rb");           /* the file system ignores case */
}
#else
#include <dirent.h>
#include <strings.h>
/* Map files come with any mix of upper and lower case. */
FILE *data_fopen(const char *name)
{
    FILE *f = fopen(name, "rb");
    if (f)
        return f;
    DIR *d = opendir(".");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strcasecmp(e->d_name, name) == 0) {
            f = fopen(e->d_name, "rb");
            break;
        }
    }
    if (d)
        closedir(d);
    return f;
}
#endif
