#ifndef foopolyplibintrospecthfoo
#define foopolyplibintrospecthfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <inttypes.h>

#include "polyplib-operation.h"
#include "polyplib-context.h"
#include "cdecl.h"

PA_C_DECL_BEGIN

struct pa_sink_info {
    const char *name;
    uint32_t index;
    const char *description;
    struct pa_sample_spec sample_spec;
    uint32_t owner_module;
    uint32_t volume;
    uint32_t monitor_source;
    const char *monitor_source_name;
    uint32_t latency;
};

struct pa_operation* pa_context_get_sink_info_by_name(struct pa_context *c, const char *name, void (*cb)(struct pa_context *c, const struct pa_sink_info *i, int is_last, void *userdata), void *userdata);
struct pa_operation* pa_context_get_sink_info_by_index(struct pa_context *c, uint32_t id, void (*cb)(struct pa_context *c, const struct pa_sink_info *i, int is_last, void *userdata), void *userdata);
struct pa_operation* pa_context_get_sink_info_list(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_sink_info *i, int is_last, void *userdata), void *userdata);

struct pa_source_info {
    const char *name;
    uint32_t index;
    const char *description;
    struct pa_sample_spec sample_spec;
    uint32_t owner_module;
    uint32_t monitor_of_sink;
    const char *monitor_of_sink_name;
};

struct pa_operation* pa_context_get_source_info_by_name(struct pa_context *c, const char *name, void (*cb)(struct pa_context *c, const struct pa_source_info *i, int is_last, void *userdata), void *userdata);
struct pa_operation* pa_context_get_source_info_by_index(struct pa_context *c, uint32_t id, void (*cb)(struct pa_context *c, const struct pa_source_info *i, int is_last, void *userdata), void *userdata);
struct pa_operation* pa_context_get_source_info_list(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_source_info *i, int is_last, void *userdata), void *userdata);

struct pa_server_info {
    const char *user_name;
    const char *host_name;
    const char *server_version;
    const char *server_name;
    struct pa_sample_spec sample_spec;
};

struct pa_operation* pa_context_get_server_info(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_server_info*i, void *userdata), void *userdata);

struct pa_module_info {
    uint32_t index;
    const char*name, *argument;
    uint32_t n_used, auto_unload;
};

struct pa_operation* pa_context_get_module_info(struct pa_context *c, uint32_t index, void (*cb)(struct pa_context *c, const struct pa_module_info*i, int is_last, void *userdata), void *userdata);
struct pa_operation* pa_context_get_module_info_list(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_module_info*i, int is_last, void *userdata), void *userdata);

struct pa_client_info {
    uint32_t index;
    const char *name;
    uint32_t owner_module;
    const char *protocol_name;
};

struct pa_operation* pa_context_get_client_info(struct pa_context *c, uint32_t index, void (*cb)(struct pa_context *c, const struct pa_client_info*i, int is_last, void *userdata), void *userdata);
struct pa_operation* pa_context_get_client_info_list(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_client_info*i, int is_last, void *userdata), void *userdata);

struct pa_sink_input_info {
    uint32_t index;
    const char *name;
    uint32_t owner_module;
    uint32_t owner_client;
    uint32_t sink;
    struct pa_sample_spec sample_spec;
    uint32_t volume;
    uint32_t latency;
};

struct pa_operation* pa_context_get_sink_input_info(struct pa_context *c, uint32_t index, void (*cb)(struct pa_context *c, const struct pa_sink_input_info*i, int is_last, void *userdata), void *userdata);
struct pa_operation* pa_context_get_sink_input_info_list(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_sink_input_info*i, int is_last, void *userdata), void *userdata);

struct pa_source_output_info {
    uint32_t index;
    const char *name;
    uint32_t owner_module;
    uint32_t owner_client;
    uint32_t source;
    struct pa_sample_spec sample_spec;
};

struct pa_operation* pa_context_get_source_output_info(struct pa_context *c, uint32_t index, void (*cb)(struct pa_context *c, const struct pa_source_output_info*i, int is_last, void *userdata), void *userdata);
struct pa_operation* pa_context_get_source_output_info_list(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_source_output_info*i, int is_last, void *userdata), void *userdata);

struct pa_operation* pa_context_set_sink_volume_by_index(struct pa_context *c, uint32_t index, uint32_t volume, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata);
struct pa_operation* pa_context_set_sink_volume_by_name(struct pa_context *c, const char *name, uint32_t volume, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata);
struct pa_operation* pa_context_set_sink_input_volume(struct pa_context *c, uint32_t index, uint32_t volume, void (*cb)(struct pa_context *c, int success, void *userdata), void *userdata);

struct pa_stat_info {
    uint32_t memblock_count;
    uint32_t memblock_total;
};

struct pa_operation* pa_context_stat(struct pa_context *c, void (*cb)(struct pa_context *c, const struct pa_stat_info *i, void *userdata), void *userdata);

PA_C_DECL_END

#endif
