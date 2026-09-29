#ifndef ESP_EVENT_H
#define ESP_EVENT_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef const char *esp_event_base_t;

#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t id
#define ESP_EVENT_DEFINE_BASE(id)  esp_event_base_t id = #id

ESP_EVENT_DECLARE_BASE(WIFI_EVENT);
ESP_EVENT_DECLARE_BASE(IP_EVENT);

#define ESP_EVENT_ANY_ID (-1)

typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base,
                                    int32_t id, void *data);

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_post(esp_event_base_t base, int32_t id,
                         const void *data, size_t data_size,
                         uint32_t timeout_ms);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base,
                                              int32_t id,
                                              esp_event_handler_t handler,
                                              void *arg,
                                              void **instance);

void mock_event_reset(void);
int  mock_event_count(void);

#endif
