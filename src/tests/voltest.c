#include <stdio.h>

#include <pulse/volume.h>
#include <pulse/gccmacro.h>

int main(int argc, char *argv[]) {
    pa_volume_t v;
    pa_cvolume cv;

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

    return 0;
}
