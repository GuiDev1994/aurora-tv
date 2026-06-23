#include "lvgl/lv_sdl_drv_input.h"

#include <SDL.h>

#include "app.h"
#include "stream/session.h"
#include "stream/session_events.h"
#include "ui/root.h"

static void sdl_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data);

int lv_sdl_init_wheel(lv_indev_drv_t *drv, app_ui_input_t *input) {
    lv_indev_drv_init(drv);
    drv->user_data = input;
    drv->type = LV_INDEV_TYPE_ENCODER;
    drv->read_cb = sdl_input_read;

    return 0;
}

static void sdl_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    app_ui_input_t *input = drv->user_data;
    SDL_Event e;
    data->state = LV_INDEV_STATE_RELEASED;
    data->enc_diff = 0;
    if (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_MOUSEWHEEL, SDL_MOUSEWHEEL)) {
        app_t *app = input->ui->app;
        if (app->session != NULL && !ui_should_block_input()) {
            session_handle_input_event(app->session, &e);
        } else if (e.wheel.y != 0) {
            /* Encoder scroll only: never set PRESSED — that triggers LVGL "encoder button" clicks. */
            data->enc_diff = (e.wheel.y > 0) ? -1 : 1;
        }
    }
    data->continue_reading = false;
}
