#include <stdio.h>
#include <stdlib.h>

#include "polyp-error.h"
#include "protocol-native-spec.h"

static const char* const errortab[PA_ERROR_MAX] = {
    [PA_ERROR_OK] = "OK",
    [PA_ERROR_ACCESS] = "Access denied",
    [PA_ERROR_COMMAND] = "Unknown command",
    [PA_ERROR_INVALID] = "Invalid argument",
    [PA_ERROR_EXIST] = "Entity exists",
    [PA_ERROR_NOENTITY] = "No such entity",
    [PA_ERROR_CONNECTIONREFUSED] = "Connection refused",
    [PA_ERROR_PROTOCOL] = "Protocol corrupt",
    [PA_ERROR_TIMEOUT] = "Timeout",
    [PA_ERROR_AUTHKEY] = "Not authorization key",
    [PA_ERROR_INTERNAL] = "Internal error",
    [PA_ERROR_CONNECTIONTERMINATED] = "Connection terminated",
    [PA_ERROR_KILLED] = "Entity killed",
};

const char*pa_strerror(uint32_t error) {
    if (error >= PA_ERROR_MAX)
        return NULL;

    return errortab[error];
}
