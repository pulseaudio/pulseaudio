/***
  This file is part of PulseAudio.

  Copyright 2009 Tanu Kaskinen

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core-util.h>

#include "iface-stream.h"

#define PLAYBACK_OBJECT_NAME "playback_stream"
#define RECORD_OBJECT_NAME "record_stream"

enum stream_type {
    STREAM_TYPE_PLAYBACK,
    STREAM_TYPE_RECORD
};

struct pa_dbusiface_stream {
    union {
        pa_sink_input *sink_input;
        pa_source_output *source_output;
    };
    enum stream_type type;
    char *path;
};

pa_dbusiface_stream *pa_dbusiface_stream_new_playback(pa_sink_input *sink_input, const char *path_prefix) {
    pa_dbusiface_stream *s;

    pa_assert(sink_input);
    pa_assert(path_prefix);

    s = pa_xnew(pa_dbusiface_stream, 1);
    s->sink_input = pa_sink_input_ref(sink_input);
    s->type = STREAM_TYPE_PLAYBACK;
    s->path = pa_sprintf_malloc("%s/%s%u", path_prefix, PLAYBACK_OBJECT_NAME, sink_input->index);

    return s;
}

pa_dbusiface_stream *pa_dbusiface_stream_new_record(pa_source_output *source_output, const char *path_prefix) {
    pa_dbusiface_stream *s;

    pa_assert(source_output);
    pa_assert(path_prefix);

    s = pa_xnew(pa_dbusiface_stream, 1);
    s->source_output = pa_source_output_ref(source_output);
    s->type = STREAM_TYPE_RECORD;
    s->path = pa_sprintf_malloc("%s/%s%u", path_prefix, RECORD_OBJECT_NAME, source_output->index);

    return s;
}

void pa_dbusiface_stream_free(pa_dbusiface_stream *s) {
    pa_assert(s);

    if (s->type == STREAM_TYPE_PLAYBACK)
        pa_sink_input_unref(s->sink_input);
    else
        pa_source_output_unref(s->source_output);

    pa_xfree(s->path);
    pa_xfree(s);
}

const char *pa_dbusiface_stream_get_path(pa_dbusiface_stream *s) {
    pa_assert(s);

    return s->path;
}
