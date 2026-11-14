#include "worker.h"
#include "backend/pcmanager/pclist.h"
#include "backend/pcmanager/priv.h"
#include "app.h"
#include "errors.h"

#include <SDL.h>
#include <string.h>

/* The host asks Windows once its 202 has left; a refusal shows in GET /power within seconds. */
static int await_refusal(GS_CLIENT client, const SERVER_DATA *server, pcmanager_host_power_t *out) {
    for (int i = 0; i < 3; i++) {
        SDL_Delay(1000);
        GS_HOST_POWER power;
        GS_NATIVE_STATUS status;
        if (gs_get_host_power(client, server, &power, &status) != GS_OK) {
            return GS_OK;
        }
        if (power.last_refusal != NULL) {
            gs_host_power_clear(&out->power);
            out->power = power;
            return GS_FAILED;
        }
        gs_host_power_clear(&power);
    }
    return GS_OK;
}

int worker_host_power(worker_context_t *context) {
    pcmanager_host_power_t *out = context->out;
    pcmanager_lock(context->manager);
    pclist_t *node = pclist_find_by_uuid(context->manager, &context->uuid);
    PSERVER_DATA server = node != NULL ? serverdata_clone(node->server) : NULL;
    pcmanager_unlock(context->manager);
    if (server == NULL) {
        return GS_ERROR;
    }
    GS_CLIENT client = app_gs_client_new(context->app);
    int ret = gs_get_host_power(client, server, &out->power, &out->status);
    if (ret == GS_OK && out->step != PCMANAGER_POWER_QUERY) {
        GS_POWER_KIND kind = out->step == PCMANAGER_POWER_RESTART ? GS_POWER_RESTART : GS_POWER_SHUTDOWN;
        const GS_POWER_ACTION *action = kind == GS_POWER_RESTART ? &out->power.restart : &out->power.shutdown;
        if (!action->available || out->power.token == NULL) {
            ret = GS_FAILED;
        } else {
            ret = gs_host_power_action(client, server, kind, out->power.token, &out->status);
            if (ret == GS_OK) {
                out->accepted = true;
                ret = await_refusal(client, server, out);
            }
        }
    }
    gs_destroy(client);
    serverdata_free(server);
    if (ret != GS_OK) {
        const char *gs_error = NULL;
        gs_get_error(&gs_error);
        context->error = gs_error != NULL ? strdup(gs_error) : NULL;
    }
    return ret;
}
