/* $Id$ */

#include <stdio.h>
#include <assert.h>

#include <polyp/channelmap.h>
#include <polypcore/gccmacro.h>

int main(PA_GCC_UNUSED int argc, PA_GCC_UNUSED char *argv[]) {
    char cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    pa_channel_map map, map2;

    pa_channel_map_init_auto(&map, 5);
    
    fprintf(stderr, "map: <%s>\n", pa_channel_map_snprint(cm, sizeof(cm), &map));

    pa_channel_map_parse(&map2, cm);

    assert(pa_channel_map_equal(&map, &map2));

    pa_channel_map_parse(&map2, "left,test");

    
    return 0;
}
