#ifndef fooalsautilhfoo
#define fooalsautilhfoo

#include <asoundlib.h>

#include "sample.h"
#include "mainloop-api.h"

int pa_alsa_set_hw_params(snd_pcm_t *pcm_handle, struct pa_sample_spec *ss, uint32_t *periods, snd_pcm_uframes_t *buffer_size);

int pa_create_io_sources(snd_pcm_t *pcm_handle, struct pa_mainloop_api *m, void ***io_sources, unsigned *n_io_sources, void (*cb)(struct pa_mainloop_api*a, void *id, int fd, enum pa_mainloop_api_io_events events, void *userdata), void *userdata);
void pa_free_io_sources(struct pa_mainloop_api* m, void **io_sources, unsigned n_io_sources);

#endif
