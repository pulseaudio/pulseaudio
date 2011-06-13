#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>

#include <pulse/channelmap.h>

int main(int argc, char *argv[]) {
    char cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    pa_channel_map map, map2;

    pa_channel_map_init_auto(&map, 6, PA_CHANNEL_MAP_AIFF);

    fprintf(stderr, "map: <%s>\n", pa_channel_map_snprint(cm, sizeof(cm), &map));

    pa_channel_map_init_auto(&map, 6, PA_CHANNEL_MAP_AUX);

    fprintf(stderr, "map: <%s>\n", pa_channel_map_snprint(cm, sizeof(cm), &map));

    pa_channel_map_init_auto(&map, 6, PA_CHANNEL_MAP_ALSA);

    fprintf(stderr, "map: <%s>\n", pa_channel_map_snprint(cm, sizeof(cm), &map));

    pa_channel_map_init_extend(&map, 14, PA_CHANNEL_MAP_ALSA);

    fprintf(stderr, "map: <%s>\n", pa_channel_map_snprint(cm, sizeof(cm), &map));

    pa_channel_map_parse(&map2, cm);

    assert(pa_channel_map_equal(&map, &map2));

    pa_channel_map_parse(&map2, "left,test");

    return 0;
}
