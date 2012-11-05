#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <inttypes.h>
#include <time.h>

#include <alsa/asoundlib.h>

static uint64_t timespec_us(const struct timespec *ts) {
    return
        ts->tv_sec * 1000000LLU +
        ts->tv_nsec / 1000LLU;
}

int main(int argc, char *argv[]) {
    const char *dev;
    int r, cap;
    snd_pcm_hw_params_t *hwparams;
    snd_pcm_sw_params_t *swparams;
    snd_pcm_status_t *status;
    snd_pcm_t *pcm;
    unsigned rate = 44100;
    unsigned periods = 2;
    snd_pcm_uframes_t boundary, buffer_size = 44100/10; /* 100s */
    int dir = 1;
    struct timespec start, last_timestamp = { 0, 0 };
    uint64_t start_us;
    snd_pcm_sframes_t last_avail = 0, last_delay = 0;
    struct pollfd *pollfds;
    int n_pollfd;
    int64_t sample_count = 0;

    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_sw_params_alloca(&swparams);
    snd_pcm_status_alloca(&status);

    r = clock_gettime(CLOCK_MONOTONIC, &start);
    assert(r == 0);

    start_us = timespec_us(&start);

    dev = argc > 1 ? argv[1] : "front:AudioPCI";
    cap = argc > 2 ? atoi(argv[2]) : 0;

    if (cap == 0)
      r = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    else
      r = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    assert(r == 0);

    r = snd_pcm_hw_params_any(pcm, hwparams);
    assert(r == 0);

    r = snd_pcm_hw_params_set_rate_resample(pcm, hwparams, 0);
    assert(r == 0);

    r = snd_pcm_hw_params_set_access(pcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
    assert(r == 0);

    r = snd_pcm_hw_params_set_format(pcm, hwparams, SND_PCM_FORMAT_S16_LE);
    assert(r == 0);

    r = snd_pcm_hw_params_set_rate_near(pcm, hwparams, &rate, NULL);
    assert(r == 0);

    r = snd_pcm_hw_params_set_channels(pcm, hwparams, 2);
    assert(r == 0);

    r = snd_pcm_hw_params_set_periods_integer(pcm, hwparams);
    assert(r == 0);

    r = snd_pcm_hw_params_set_periods_near(pcm, hwparams, &periods, &dir);
    assert(r == 0);

    r = snd_pcm_hw_params_set_buffer_size_near(pcm, hwparams, &buffer_size);
    assert(r == 0);

    r = snd_pcm_hw_params(pcm, hwparams);
    assert(r == 0);

    r = snd_pcm_hw_params_current(pcm, hwparams);
    assert(r == 0);

    r = snd_pcm_sw_params_current(pcm, swparams);
    assert(r == 0);

    if (cap == 0)
      r = snd_pcm_sw_params_set_avail_min(pcm, swparams, 1);
    else
      r = snd_pcm_sw_params_set_avail_min(pcm, swparams, 0);
    assert(r == 0);

    r = snd_pcm_sw_params_set_period_event(pcm, swparams, 0);
    assert(r == 0);

    r = snd_pcm_hw_params_get_buffer_size(hwparams, &buffer_size);
    assert(r == 0);
    r = snd_pcm_sw_params_set_start_threshold(pcm, swparams, buffer_size);
    assert(r == 0);

    r = snd_pcm_sw_params_get_boundary(swparams, &boundary);
    assert(r == 0);
    r = snd_pcm_sw_params_set_stop_threshold(pcm, swparams, boundary);
    assert(r == 0);

    r = snd_pcm_sw_params_set_tstamp_mode(pcm, swparams, SND_PCM_TSTAMP_ENABLE);
    assert(r == 0);

    r = snd_pcm_sw_params(pcm, swparams);
    assert(r == 0);

    r = snd_pcm_prepare(pcm);
    assert(r == 0);

    r = snd_pcm_sw_params_current(pcm, swparams);
    assert(r == 0);

/*     assert(snd_pcm_hw_params_is_monotonic(hwparams) > 0); */

    n_pollfd = snd_pcm_poll_descriptors_count(pcm);
    assert(n_pollfd > 0);

    pollfds = malloc(sizeof(struct pollfd) * n_pollfd);
    assert(pollfds);

    r = snd_pcm_poll_descriptors(pcm, pollfds, n_pollfd);
    assert(r == n_pollfd);

    if (cap) {
      r = snd_pcm_start(pcm);
      assert(r == 0);
    }

    for (;;) {
        snd_pcm_sframes_t avail, delay;
        struct timespec now, timestamp;
        unsigned short revents;
        int handled = 0;
        uint64_t now_us, timestamp_us;
        snd_pcm_state_t state;
        unsigned long long pos;

        r = poll(pollfds, n_pollfd, 0);
        assert(r >= 0);

        r = snd_pcm_poll_descriptors_revents(pcm, pollfds, n_pollfd, &revents);
        assert(r == 0);

        if (cap == 0)
          assert((revents & ~POLLOUT) == 0);
        else
          assert((revents & ~POLLIN) == 0);

        avail = snd_pcm_avail(pcm);
        assert(avail >= 0);

        r = snd_pcm_status(pcm, status);
        assert(r == 0);

        /* This assertion fails from time to time. ALSA seems to be broken */
/*         assert(avail == (snd_pcm_sframes_t) snd_pcm_status_get_avail(status)); */
/*         printf("%lu %lu\n", (unsigned long) avail, (unsigned long) snd_pcm_status_get_avail(status)); */

        snd_pcm_status_get_htstamp(status, &timestamp);
        delay = snd_pcm_status_get_delay(status);
        state = snd_pcm_status_get_state(status);

        r = clock_gettime(CLOCK_MONOTONIC, &now);
        assert(r == 0);

        assert(!revents || avail > 0);

        if ((!cap && avail) || (cap && (unsigned)avail >= buffer_size)) {
            snd_pcm_sframes_t sframes;
            static const uint16_t psamples[2] = { 0, 0 };
            uint16_t csamples[2];

            if (cap == 0)
              sframes = snd_pcm_writei(pcm, psamples, 1);
            else
              sframes = snd_pcm_readi(pcm, csamples, 1);
            assert(sframes == 1);

            handled = 1;
            sample_count++;
        }

        if (!handled &&
            memcmp(&timestamp, &last_timestamp, sizeof(timestamp)) == 0 &&
            avail == last_avail &&
            delay == last_delay) {
            /* This is boring */
            continue;
        }

        now_us = timespec_us(&now);
        timestamp_us = timespec_us(&timestamp);

        if (cap == 0)
            pos = (unsigned long long) ((sample_count - handled - delay) * 1000000LU / 44100);
        else
            pos = (unsigned long long) ((sample_count - handled + delay) * 1000000LU / 44100);

        printf("%llu\t%llu\t%llu\t%llu\t%li\t%li\t%i\t%i\t%i\n",
               (unsigned long long) (now_us - start_us),
               (unsigned long long) (timestamp_us ? timestamp_us - start_us : 0),
               pos,
               (unsigned long long) sample_count,
               (signed long) avail,
               (signed long) delay,
               revents,
               handled,
               state);

        if (cap == 0)
          /** When this assert is hit, most likely something bad
           * happened, i.e. the avail jumped suddenly. */
          assert((unsigned) avail <= buffer_size);

        last_avail = avail;
        last_delay = delay;
        last_timestamp = timestamp;
    }

    return 0;
}
