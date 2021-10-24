/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "message-handler.h"

/* Check if a path string starts with a / and only contains valid characters.
 * Also reject double slashes. */
static bool object_path_is_valid(const char *test_string) {
    uint32_t i;

    if (!test_string)
        return false;

    /* Make sure the string starts with a / */
    if (test_string[0] != '/')
        return false;

    for (i = 0; test_string[i]; i++) {

        if ((test_string[i] >= 'a' && test_string[i] <= 'z') ||
            (test_string[i] >= 'A' && test_string[i] <= 'Z') ||
            (test_string[i] >= '0' && test_string[i] <= '9') ||
            test_string[i] == '.' ||
            test_string[i] == '_' ||
            test_string[i] == '-' ||
            (test_string[i] == '/' && test_string[i + 1] != '/'))
            continue;

        return false;
    }

    /* Make sure the string does not end with a / */
    if (test_string[i - 1] == '/')
        return false;

    return true;
}

/* Message handler functions */

/* Register message handler for the specified object. object_path must be a unique name starting with "/". */
void pa_message_handler_register(pa_core *c, const char *object_path, const char *description, pa_message_handler_cb_t cb, void *userdata) {
    struct pa_message_handler *handler;

    pa_assert(c);
    pa_assert(object_path);
    pa_assert(cb);
    pa_assert(userdata);

    /* Ensure that object path is valid */
    pa_assert(object_path_is_valid(object_path));

    handler = pa_xnew0(struct pa_message_handler, 1);
    handler->userdata = userdata;
    handler->callback = cb;
    handler->object_path = pa_xstrdup(object_path);
    handler->description = pa_xstrdup(description);

    pa_assert_se(pa_hashmap_put(c->message_handlers, handler->object_path, handler) == 0);
}

/* Unregister a message handler */
void pa_message_handler_unregister(pa_core *c, const char *object_path) {
    struct pa_message_handler *handler;

    pa_assert(c);
    pa_assert(object_path);

    pa_assert_se(handler = pa_hashmap_remove(c->message_handlers, object_path));

    pa_xfree(handler->object_path);
    pa_xfree(handler->description);
    pa_xfree(handler);
}

/* Send a message to an object identified by object_path */
int pa_message_handler_send_message(pa_core *c, const char *object_path, const char *message, const char *message_parameters, char **response) {
    struct pa_message_handler *handler;
    int ret;
    char *path_copy;
    pa_json_object *parameters = NULL;

    pa_assert(c);
    pa_assert(object_path);
    pa_assert(message);
    pa_assert(response);

    *response = NULL;

    path_copy = pa_xstrdup(object_path);

    /* Remove trailing / from path name if present */
    if (path_copy[strlen(path_copy) - 1] == '/')
        path_copy[strlen(path_copy) - 1] = 0;

    if (!(handler = pa_hashmap_get(c->message_handlers, path_copy))) {
        pa_xfree(path_copy);
        return -PA_ERR_NOENTITY;
    }

    pa_xfree(path_copy);

    if (message_parameters) {
        parameters = pa_json_parse(message_parameters);

        if (!parameters) {
            char *wrapped_message_parameters;

            /* Message parameters is not a valid JSON
             *
             * Wrap message parameters into JSON string and try again.
             * User might have missed double-quotes and passed ARGSTRING instead of proper JSON "ARGSTRING"
             */
            pa_log_warn("Message parameters is not a valid JSON, wrapping into JSON string '\"%s\"'", message_parameters);

            wrapped_message_parameters = pa_sprintf_malloc("\"%s\"", message_parameters);
            parameters = pa_json_parse(wrapped_message_parameters);
            pa_xfree(wrapped_message_parameters);

            if (!parameters) {
                pa_log_error("Message parameters is not a valid JSON object. Tried both '%s' and '\"%s\"'",
                        message_parameters, message_parameters);
                return -PA_ERR_INVALID;
            }
        }
    }

    /* The handler is expected to return an error code and may also
       return an error string in response */
    ret = handler->callback(handler->object_path, message, parameters, response, handler->userdata);

    if (parameters)
        pa_json_object_free(parameters);
    return ret;
}

/* Set handler description */
int pa_message_handler_set_description(pa_core *c, const char *object_path, const char *description) {
    struct pa_message_handler *handler;

    pa_assert(c);
    pa_assert(object_path);

    if (!(handler = pa_hashmap_get(c->message_handlers, object_path)))
        return -PA_ERR_NOENTITY;

    pa_xfree(handler->description);
    handler->description = pa_xstrdup(description);

    return PA_OK;
}
