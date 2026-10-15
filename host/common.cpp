/*
 * Memory and files for the standalone hosts.
 *
 * The data segment (struct DataSeg) is allocated here and filled by
 * dataseg_init; the game allocates the rest (game_init, game_free).
 */
#include <cstdlib>
#include "host.hpp"

void host_free_game()
{
    game_free();
    free(G);
    G = nullptr;
}

bool host_init_dataseg()
{
    G = static_cast<DataSeg *>(calloc(1, sizeof(DataSeg)));
    if (!G) {
        perror("data segment");
        return false;
    }
    dataseg_init();
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
