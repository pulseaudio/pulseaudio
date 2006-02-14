#ifndef foopolyplibintrospecthfoo
#define foopolyplibintrospecthfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <inttypes.h>

#include <polyp/polyplib-operation.h>
#include <polyp/polyplib-context.h>
#include <polyp/cdecl.h>
#include <polyp/channelmap.h>
#include <polyp/volume.h>

/** \file
 *
 * Routines for daemon introspection. When enumerating all entitites
 * of a certain kind, use the pa_context_xxx_list() functions. The
 * specified callback function is called once for each entry. The
 * enumeration is finished by a call to the callback function with
 * is_last=1 and i=NULL. Strings referenced in pa_xxx_info structures
 * and the structures themselves point to internal memory that may not
 * be modified. That memory is only valid during the call to the
 * callback function. A deep copy is required if you need this data
 * outside the callback functions. An error is signalled by a call to * the callback function with i=NULL and is_last=0.
 *
 * When using the routines that ask fo a single entry only, a callback
 * with the same signature is used. However, no finishing call to the
 * routine is issued. */

PA_C_DECL_BEGIN

/** Stores information about sinks */
typedef struct pa_sink_info {
    const char *name;                  /**< Name of the sink */
    uint32_t index;                    /**< Index of the sink */ 
    const char *description;           /**< Description of this sink */
    pa_sample_spec sample_spec;        /**< Sample spec of this sink */
    pa_channel_map channel_map;        /**< Channel map \since 0.9 */
    uint32_t owner_module;             /**< Index of the owning module of this sink, or PA_INVALID_INDEX */
    pa_cvolume volume;                 /**< Volume of the sink */
    uint32_t monitor_source;           /**< Index of the monitor source connected to this sink */
    const char *monitor_source_name;   /**< The name of the monitor source */
    pa_usec_t latency;                 /**< Length of filled playback buffer of this sink */
    const char *driver;                /**< Driver name. \since 0.9 */
} pa_sink_info;

/** Get information about a sink by its name */
pa_operation* pa_context_get_sink_info_by_name(pa_context *c, const char *name, void (*cb)(pa_context *c, const pa_sink_info *i, int is_last, void *userdata), void *userdata);

/** Get information about a sink by its index */
pa_operation* pa_context_get_sink_info_by_index(pa_context *c, uint32_t id, void (*cb)(pa_context *c, const pa_sink_info *i, int is_last, void *userdata), void *userdata);

/** Get the complete sink list */
pa_operation* pa_context_get_sink_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_sink_info *i, int is_last, void *userdata), void *userdata);

/** Stores information about sources */
typedef struct pa_source_info { 
    const char *name ;                  /**< Name of the source */
    uint32_t index;                     /**< Index of the source */
    const char *description;            /**< Description of this source */
    pa_sample_spec sample_spec;         /**< Sample spec of this source */
    pa_channel_map channel_map;         /**< Channel map \since 0.9 */
    uint32_t owner_module;              /**< Owning module index, or PA_INVALID_INDEX */
    uint32_t monitor_of_sink;           /**< If this is a monitor source the index of the owning sink, otherwise PA_INVALID_INDEX */
    const char *monitor_of_sink_name;   /**< Name of the owning sink, or PA_INVALID_INDEX */
    pa_usec_t latency;                  /**< Length of filled record buffer of this source. \since 0.5 */
    const char *driver;                 /**< Driver name \since 0.9 */
} pa_source_info;

/** Get information about a source by its name */
pa_operation* pa_context_get_source_info_by_name(pa_context *c, const char *name, void (*cb)(pa_context *c, const pa_source_info *i, int is_last, void *userdata), void *userdata);

/** Get information about a source by its index */
pa_operation* pa_context_get_source_info_by_index(pa_context *c, uint32_t id, void (*cb)(pa_context *c, const pa_source_info *i, int is_last, void *userdata), void *userdata);

/** Get the complete source list */
pa_operation* pa_context_get_source_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_source_info *i, int is_last, void *userdata), void *userdata);

/** Server information */
typedef struct pa_server_info {
    const char *user_name;              /**< User name of the daemon process */
    const char *host_name;              /**< Host name the daemon is running on */
    const char *server_version;         /**< Version string of the daemon */
    const char *server_name;            /**< Server package name (usually "polypaudio") */
    pa_sample_spec sample_spec;         /**< Default sample specification */
    const char *default_sink_name;      /**< Name of default sink. \since 0.4 */
    const char *default_source_name;    /**< Name of default sink. \since 0.4*/
    uint32_t cookie;                    /**< A random cookie for identifying this instance of polypaudio. \since 0.8 */
} pa_server_info;

/** Get some information about the server */
pa_operation* pa_context_get_server_info(pa_context *c, void (*cb)(pa_context *c, const pa_server_info*i, void *userdata), void *userdata);

/** Stores information about modules */
typedef struct pa_module_info {
    uint32_t index;                     /**< Index of the module */
    const char*name,                    /**< Name of the module */
        *argument;                      /**< Argument string of the module */
    uint32_t n_used;                    /**< Usage counter or PA_INVALID_INDEX */
    int auto_unload;                    /**< Non-zero if this is an autoloaded module */
} pa_module_info;

/** Get some information about a module by its index */
pa_operation* pa_context_get_module_info(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_module_info*i, int is_last, void *userdata), void *userdata);

/** Get the complete list of currently loaded modules */
pa_operation* pa_context_get_module_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_module_info*i, int is_last, void *userdata), void *userdata);

/** Stores information about clients */
typedef struct pa_client_info {
    uint32_t index;                      /**< Index of this client */
    const char *name;                    /**< Name of this client */
    uint32_t owner_module;               /**< Index of the owning module, or PA_INVALID_INDEX */
    const char *driver;                  /**< Driver name \since 0.9 */
} pa_client_info;

/** Get information about a client by its index */
pa_operation* pa_context_get_client_info(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_client_info*i, int is_last, void *userdata), void *userdata);

/** Get the complete client list */
pa_operation* pa_context_get_client_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_client_info*i, int is_last, void *userdata), void *userdata);

/** Stores information about sink inputs */
typedef struct pa_sink_input_info {
    uint32_t index;                      /**< Index of the sink input */  
    const char *name;                    /**< Name of the sink input */
    uint32_t owner_module;               /**< Index of the module this sink input belongs to, or PA_INVALID_INDEX when it does not belong to any module */
    uint32_t client;                     /**< Index of the client this sink input belongs to, or PA_INVALID_INDEX when it does not belong to any client */
    uint32_t sink;                       /**< Index of the connected sink */
    pa_sample_spec sample_spec;          /**< The sample specification of the sink input */
    pa_channel_map channel_map;          /**< Channel map */
    pa_cvolume volume;                   /**< The volume of this sink input */
    pa_usec_t buffer_usec;               /**< Latency due to buffering in sink input, see pa_latency_info for details */
    pa_usec_t sink_usec;                 /**< Latency of the sink device, see pa_latency_info for details */
    const char *resample_method;         /**< Thre resampling method used by this sink input. \since 0.7 */
    const char *driver;                  /**< Driver name \since 0.9 */
} pa_sink_input_info;

/** Get some information about a sink input by its index */
pa_operation* pa_context_get_sink_input_info(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_sink_input_info*i, int is_last, void *userdata), void *userdata);

/** Get the complete sink input list */
pa_operation* pa_context_get_sink_input_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_sink_input_info*i, int is_last, void *userdata), void *userdata);

/** Stores information about source outputs */
typedef struct pa_source_output_info {
    uint32_t index;                      /**< Index of the sink input */ 
    const char *name;                    /**< Name of the sink input */
    uint32_t owner_module;               /**< Index of the module this sink input belongs to, or PA_INVALID_INDEX when it does not belong to any module */ 
    uint32_t client;                     /**< Index of the client this sink input belongs to, or PA_INVALID_INDEX when it does not belong to any client */  
    uint32_t source;                     /**< Index of the connected source */ 
    pa_sample_spec sample_spec;          /**< The sample specification of the source output */
    pa_channel_map channel_map;          /**< Channel map */
    pa_usec_t buffer_usec;               /**< Latency due to buffering in the source output, see pa_latency_info for details. \since 0.5 */
    pa_usec_t source_usec;               /**< Latency of the source device, see pa_latency_info for details. \since 0.5 */
    const char *resample_method;         /**< Thre resampling method used by this source output. \since 0.7 */
    const char *driver;                  /**< Driver name \since 0.9 */
} pa_source_output_info;

/** Get information about a source output by its index */
pa_operation* pa_context_get_source_output_info(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_source_output_info*i, int is_last, void *userdata), void *userdata);

/** Get the complete list of source outputs */
pa_operation* pa_context_get_source_output_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_source_output_info*i, int is_last, void *userdata), void *userdata);

/** Set the volume of a sink device specified by its index */
pa_operation* pa_context_set_sink_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, void (*cb)(pa_context *c, int success, void *userdata), void *userdata);

/** Set the volume of a sink device specified by its name */
pa_operation* pa_context_set_sink_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, void (*cb)(pa_context *c, int success, void *userdata), void *userdata);

/** Set the volume of a sink input stream */
pa_operation* pa_context_set_sink_input_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, void (*cb)(pa_context *c, int success, void *userdata), void *userdata);

/** Memory block statistics */
typedef struct pa_stat_info {
    uint32_t memblock_total;           /**< Currently allocated memory blocks */
    uint32_t memblock_total_size;      /**< Currentl total size of allocated memory blocks */
    uint32_t memblock_allocated;       /**< Allocated memory blocks during the whole lifetime of the daemon */
    uint32_t memblock_allocated_size;  /**< Total size of all memory blocks allocated during the whole lifetime of the daemon */
    uint32_t scache_size;              /**< Total size of all sample cache entries. \since 0.4 */ 
} pa_stat_info;

/** Get daemon memory block statistics */
pa_operation* pa_context_stat(pa_context *c, void (*cb)(pa_context *c, const pa_stat_info *i, void *userdata), void *userdata);

/** Stores information about sample cache entries */
typedef struct pa_sample_info {
    uint32_t index;                       /**< Index of this entry */
    const char *name;                     /**< Name of this entry */
    pa_cvolume volume;                    /**< Default volume of this entry */
    pa_sample_spec sample_spec;           /**< Sample specification of the sample */
    pa_channel_map channel_map;           /**< The channel map */
    pa_usec_t duration;                   /**< Duration of this entry */
    uint32_t bytes;                       /**< Length of this sample in bytes. \since 0.4 */
    int lazy;                             /**< Non-zero when this is a lazy cache entry. \since 0.5 */
    const char *filename;                 /**< In case this is a lazy cache entry, the filename for the sound file to be loaded on demand. \since 0.5 */
} pa_sample_info;

/** Get information about a sample by its name */
pa_operation* pa_context_get_sample_info_by_name(pa_context *c, const char *name, void (*cb)(pa_context *c, const pa_sample_info *i, int is_last, void *userdata), void *userdata);

/** Get information about a sample by its index */
pa_operation* pa_context_get_sample_info_by_index(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_sample_info *i, int is_last, void *userdata), void *userdata);

/** Get the complete list of samples stored in the daemon. */
pa_operation* pa_context_get_sample_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_sample_info *i, int is_last, void *userdata), void *userdata);

/** Kill a client. \since 0.5 */
pa_operation* pa_context_kill_client(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void *userdata);
                                            
/** Kill a sink input. \since 0.5 */
pa_operation* pa_context_kill_sink_input(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void *userdata);

/** Kill a source output. \since 0.5 */
pa_operation* pa_context_kill_source_output(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void *userdata);

/** Load a module. \since 0.5 */
pa_operation* pa_context_load_module(pa_context *c, const char*name, const char *argument, void (*cb)(pa_context *c, uint32_t idx, void *userdata), void *userdata);

/** Unload a module. \since 0.5 */
pa_operation* pa_context_unload_module(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void *userdata);

/** Type of an autoload entry. \since 0.5 */
typedef enum pa_autoload_type {
    PA_AUTOLOAD_SINK = 0,
    PA_AUTOLOAD_SOURCE = 1
} pa_autoload_type_t;

/** Stores information about autoload entries. \since 0.5 */
typedef struct pa_autoload_info {
    uint32_t index;               /**< Index of this autoload entry */
    const char *name;             /**< Name of the sink or source */
    pa_autoload_type_t type;   /**< Type of the autoload entry */
    const char *module;           /**< Module name to load */
    const char *argument;         /**< Argument string for module */
} pa_autoload_info;

/** Get info about a specific autoload entry. \since 0.6 */
pa_operation* pa_context_get_autoload_info_by_name(pa_context *c, const char *name, pa_autoload_type_t type, void (*cb)(pa_context *c, const pa_autoload_info *i, int is_last, void *userdata), void *userdata);

/** Get info about a specific autoload entry. \since 0.6 */
pa_operation* pa_context_get_autoload_info_by_index(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, const pa_autoload_info *i, int is_last, void *userdata), void *userdata);

/** Get the complete list of autoload entries. \since 0.5 */
pa_operation* pa_context_get_autoload_info_list(pa_context *c, void (*cb)(pa_context *c, const pa_autoload_info *i, int is_last, void *userdata), void *userdata);

/** Add a new autoload entry. \since 0.5 */
pa_operation* pa_context_add_autoload(pa_context *c, const char *name, pa_autoload_type_t type, const char *module, const char*argument, void (*cb)(pa_context *c, int idx, void *userdata), void* userdata);

/** Remove an autoload entry. \since 0.6 */
pa_operation* pa_context_remove_autoload_by_name(pa_context *c, const char *name, pa_autoload_type_t type, void (*cb)(pa_context *c, int success, void *userdata), void* userdata);

/** Remove an autoload entry. \since 0.6 */
pa_operation* pa_context_remove_autoload_by_index(pa_context *c, uint32_t idx, void (*cb)(pa_context *c, int success, void *userdata), void* userdata);


PA_C_DECL_END

#endif
