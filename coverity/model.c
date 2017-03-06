/* Coverity Scan model
 * Copyright (C) 2017 Peter Meerwald-Stadler <pmeerw@pmeerw.net>
 *
 * This is a modeling file for Coverity Scan which helps to avoid false
 * positives and increase scanning accuracy by explaining code Coverity
 * can't see (out of tree libraries); the model file must be uploaded by
 * an admin to:
 * https://scan.coverity.com/projects/pulseaudio?tab=analysis_settings
 */

void fail(void) {
    __coverity_panic__();
}

void fail_unless(int x) {
    if (!x)
        __coverity_panic__();
}
