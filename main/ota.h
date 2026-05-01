#pragma once

#include "esp_http_server.h"

void ota_init(void);
void ota_register_handlers(httpd_handle_t server);
