#include <time.h>
#include "util.h"

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#include <unistd.h>
#include "stb_image.h"   /* declarations only — the implementation is compiled in main.c */

uint8_t *load_image_tmp(const char *tmp_path, int *w, int *h)
{
    int channels = 0;
    uint8_t *px = stbi_load(tmp_path, w, h, &channels, 4);
    unlink(tmp_path);
    return px;
}

uint8_t *load_image_keep(const char *path, int *w, int *h)
{
    int channels = 0;
    return stbi_load(path, w, h, &channels, 4);
}

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int cache_sweep_superseded(const char *dir, const char *ext, const char *keep_substr)
{
    if (!dir || !ext || !keep_substr || !*ext || !*keep_substr) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;                      /* no cache yet — nothing to do */

    size_t extlen = strlen(ext);
    int removed = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len <= extlen) continue;                       /* also skips "." / ".." */
        if (strcmp(n + len - extlen, ext) != 0) continue;  /* wrong extension */
        if (strstr(n, keep_substr)) continue;              /* current key format */

        char path[512];
        if (snprintf(path, sizeof(path), "%s/%s", dir, n) >= (int)sizeof(path))
            continue;                      /* wouldn't be a name we wrote */

        /* stat rather than d_type: exFAT reports DT_UNKNOWN, and unlinking a
         * directory would merely fail rather than be caught. Only paid on the
         * pass that finds candidates — later runs match nothing. */
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (unlink(path) == 0) removed++;
    }
    closedir(d);
    return removed;
}
