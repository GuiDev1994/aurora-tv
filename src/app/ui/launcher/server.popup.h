#pragma once

#include "launcher.controller.h"

/**
 * Open the modal "Select server" popup.
 *
 * Lists every paired/discovered host known to the pcmanager, with the same
 * status icons used previously by the side-nav PC list. Selecting an entry
 * closes the popup and invokes launcher_select_server() with that UUID.
 * Long-pressing an entry opens the existing server context menu (rename /
 * pair / unpair / etc.).
 */
void server_popup_open(launcher_fragment_t *controller);
