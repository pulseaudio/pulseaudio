#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/prioq.h>
#include <pulsecore/macro.h>

#define N 1024

int main(int argc, char *argv[]) {
    pa_prioq *q;
    unsigned i;

    srand(0);

    q = pa_prioq_new(pa_idxset_trivial_compare_func);

    /* Fill in 1024 */
    for (i = 0; i < N; i++)
        pa_prioq_put(q, PA_UINT_TO_PTR((unsigned) rand()));

    /* Remove half of it again */
    for (i = 0; i < N/2; i++){
        unsigned u = PA_PTR_TO_UINT(pa_prioq_pop(q));
        pa_log("%16u", u);
    }

    pa_log("Refilling");

    /* Fill in another 1024 */
    for (i = 0; i < N; i++)
        pa_prioq_put(q, PA_UINT_TO_PTR((unsigned) rand()));


    /* Remove everything */
    while (!pa_prioq_isempty(q)) {
        unsigned u = PA_PTR_TO_UINT(pa_prioq_pop(q));
        pa_log("%16u", u);
    }

    pa_prioq_free(q, NULL, NULL);

    return 0;
}
