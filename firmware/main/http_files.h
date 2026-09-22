#ifndef RLCD_HTTP_FILES_H
#define RLCD_HTTP_FILES_H
#include <stdbool.h>
#include "esp_http_server.h"
bool http_files_register(httpd_handle_t server);
#endif
