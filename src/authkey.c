#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>

#include "authkey.h"
#include "util.h"

#define RANDOM_DEVICE "/dev/urandom"

static int load(const char *fn, void *data, size_t length) {
    int fd = -1, ret = -1;
    ssize_t r;
    
    assert(fn && data && length);

    if ((fd = open(fn, O_RDONLY)) < 0)
        goto finish;

    if ((r = pa_loop_read(fd, data, length)) < 0 || (size_t) r != length) {
        ret = -2;
        goto finish;
    }

    ret = 0;
    
finish:
    if (fd >= 0)
        close(fd);

    return ret;
}

static int generate(const char *fn, void *data, size_t length) {
    int fd = -1, random_fd = -1, ret = -1;
    ssize_t r;
    assert(fn && data && length);

    if ((fd = open(fn, O_WRONLY|O_EXCL|O_CREAT, S_IRUSR | S_IWUSR)) < 0)
        goto finish;
    
    if ((random_fd = open(RANDOM_DEVICE, O_RDONLY)) >= 0) {

        if ((r = pa_loop_read(random_fd, data, length)) < 0 || (size_t) r != length) {
            ret = -2;
            goto finish;
        }
        
    } else {
        uint8_t *p;
        size_t l;
        fprintf(stderr, "WARNING: Failed to open entropy device '"RANDOM_DEVICE"': %s, falling back to unsecure pseudo RNG.\n", strerror(errno));

        srandom(time(NULL));
        
        for (p = data, l = length; l > 0; p++, l--)
            *p = (uint8_t) random();
    }

    if ((r = pa_loop_write(fd, data, length)) < 0 || (size_t) r != length) {
        ret =  -2;
        goto finish;
    }

    ret = 0;

finish:
    if (fd >= 0) {
        if (ret != 0)
            unlink(fn);
        close(fd);
    }
    if (random_fd >= 0)
        close(random_fd);

    return ret;
}

int pa_authkey_load(const char *path, void *data, size_t length) {
    int ret, i;

    assert(path && data && length);
    
    for (i = 0; i < 10; i++) {
        if ((ret = load(path, data, length)) < 0)
            if (ret == -1 && errno == ENOENT)
                if ((ret = generate(path,  data, length)) < 0)
                    if (ret == -1 && errno == EEXIST)
                        continue;
        break;
    }

    if (ret < 0)
        fprintf(stderr, "Failed to load authorization key '%s': %s\n", path, (ret == -1) ? strerror(errno) : "file corrupt");

    return ret;
}

int pa_authkey_load_from_home(const char *fn, void *data, size_t length) {
    char *home;
    char path[PATH_MAX];

    assert(fn && data && length);
    
    if (!(home = getenv("HOME")))
        return -2;
    
    snprintf(path, sizeof(path), "%s/%s", home, fn);

    return pa_authkey_load(path, data, length);
}

int pa_authkey_load_auto(const char *fn, void *data, size_t length) {
    assert(fn && data && length);

    if (*fn == '/')
        return pa_authkey_load(fn, data, length);
    else
        return pa_authkey_load_from_home(fn, data, length);
}
