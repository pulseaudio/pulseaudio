#include <stdio.h>

#include <polyp/sample.h>

int main() {
    int p;
    for (p = 0; p <= 200; p++) {
        pa_volume_t v = pa_volume_from_user((double) p/100);
        double dB = pa_volume_to_dB(v);
        printf("%3i%% = %u = %0.2f dB = %u = %3i%%\n", p, v, dB, pa_volume_from_dB(dB), (int) (pa_volume_to_user(v)*100));
    }
}
