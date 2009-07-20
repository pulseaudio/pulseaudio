#include <stdio.h>

#include <pulse/volume.h>
#include <pulse/gccmacro.h>

int main(int argc, char *argv[]) {
    pa_volume_t v;
    pa_cvolume cv;
    float b;
    pa_channel_map map;

    printf("Attenuation of sample 1 against 32767: %g dB\n", 20.0*log10(1.0/32767.0));
    printf("Smallest possible attenutation > 0 applied to 32767: %li\n", lrint(32767.0*pa_sw_volume_to_linear(1)));

    for (v = PA_VOLUME_MUTED; v <= PA_VOLUME_NORM*2; v += 256) {

        double dB = pa_sw_volume_to_dB(v);
        double f = pa_sw_volume_to_linear(v);

        printf("Volume: %3i; percent: %i%%; decibel %0.2f; linear = %0.2f; volume(decibel): %3i; volume(linear): %3i\n",
               v, (v*100)/PA_VOLUME_NORM, dB, f, pa_sw_volume_from_dB(dB), pa_sw_volume_from_linear(f));
    }

    for (v = PA_VOLUME_MUTED; v <= PA_VOLUME_NORM*2; v += 256) {
        char s[PA_CVOLUME_SNPRINT_MAX], t[PA_SW_CVOLUME_SNPRINT_DB_MAX];

        pa_cvolume_set(&cv, 2, v);

        printf("Volume: %3i [%s] [%s]\n",
               v,
               pa_cvolume_snprint(s, sizeof(s), &cv),
               pa_sw_cvolume_snprint_dB(t, sizeof(t), &cv));

    }

    map.channels = cv.channels = 2;
    map.map[0] = PA_CHANNEL_POSITION_LEFT;
    map.map[1] = PA_CHANNEL_POSITION_RIGHT;

    for (cv.values[0] = PA_VOLUME_MUTED; cv.values[0] <= PA_VOLUME_NORM*2; cv.values[0] += 4096)
        for (cv.values[1] = PA_VOLUME_MUTED; cv.values[1] <= PA_VOLUME_NORM*2; cv.values[1] += 4096) {
            char s[PA_CVOLUME_SNPRINT_MAX];

            printf("Volume: [%s]; balance: %2.1f\n", pa_cvolume_snprint(s, sizeof(s), &cv), pa_cvolume_get_balance(&cv, &map));
        }

    for (cv.values[0] = PA_VOLUME_MUTED+4096; cv.values[0] <= PA_VOLUME_NORM*2; cv.values[0] += 4096)
        for (cv.values[1] = PA_VOLUME_MUTED; cv.values[1] <= PA_VOLUME_NORM*2; cv.values[1] += 4096)
            for (b = -1.0f; b <= 1.0f; b += 0.2f) {
                char s[PA_CVOLUME_SNPRINT_MAX];
                pa_cvolume r;
                float k;

                printf("Before: volume: [%s]; balance: %2.1f\n", pa_cvolume_snprint(s, sizeof(s), &cv), pa_cvolume_get_balance(&cv, &map));

                r = cv;
                pa_cvolume_set_balance(&r, &map,b);

                k = pa_cvolume_get_balance(&r, &map);
                printf("After: volume: [%s]; balance: %2.1f (intended: %2.1f) %s\n", pa_cvolume_snprint(s, sizeof(s), &r), k, b, k < b-.05 || k > b+.5 ? "MISMATCH" : "");
            }

    return 0;
}
