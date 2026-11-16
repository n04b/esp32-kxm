/* Веб-интерфейс: страница, REST API, WebSocket для управления, OTA. */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t web_ui_start(bool setup_mode);
