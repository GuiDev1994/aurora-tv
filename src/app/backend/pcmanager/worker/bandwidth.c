#include "worker.h"
#include "backend/pcmanager/pclist.h"
#include "backend/pcmanager/priv.h"
#include "app.h"
#include "errors.h"

#include <string.h>

int worker_probe_bandwidth(worker_context_t *context) {
    pcmanager_lock(context->manager);
    pclist_t *node = pclist_find_by_uuid(context->manager, &context->uuid);
    PSERVER_DATA server = node != NULL ? serverdata_clone(node->server) : NULL;
    pcmanager_unlock(context->manager);
    if (server == NULL) {
        return GS_ERROR;
    }
    GS_CLIENT client = app_gs_client_new(context->app);
    int ret = gs_probe_bandwidth(client, server, context->out);
    gs_destroy(client);
    serverdata_free(server);
    if (ret != GS_OK) {
        const char *gs_error = NULL;
        gs_get_error(&gs_error);
        context->error = gs_error != NULL ? strdup(gs_error) : NULL;
    }
    return ret;
}
