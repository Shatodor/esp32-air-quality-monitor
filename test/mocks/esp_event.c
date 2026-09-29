#include "esp_event.h"
#include <string.h>
#include <stdlib.h>

ESP_EVENT_DEFINE_BASE(WIFI_EVENT);
ESP_EVENT_DEFINE_BASE(IP_EVENT);

#define MAX_EVENTS 32

typedef struct {
    esp_event_base_t base;
    int32_t          id;
    size_t           size;
    void            *data;
} evt_t;

static evt_t s_events[MAX_EVENTS];
static int   s_count;

void mock_event_reset(void)
{
    for (int i = 0; i < s_count; i++) free(s_events[i].data);
    memset(s_events, 0, sizeof(s_events));
    s_count = 0;
}

int mock_event_count(void) { return s_count; }

esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }

esp_err_t esp_event_post(esp_event_base_t base, int32_t id,
                         const void *data, size_t data_size,
                         uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (s_count >= MAX_EVENTS) return ESP_ERR_NO_MEM;
    s_events[s_count].base = base;
    s_events[s_count].id = id;
    s_events[s_count].size = data_size;
    if (data && data_size > 0) {
        s_events[s_count].data = malloc(data_size);
        if (s_events[s_count].data)
            memcpy(s_events[s_count].data, data, data_size);
    }
    s_count++;
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_register(esp_event_base_t base,
                                              int32_t id,
                                              esp_event_handler_t handler,
                                              void *arg,
                                              void **instance)
{
    (void)base; (void)id; (void)handler; (void)arg;
    if (instance) *instance = NULL;
    return ESP_OK;
}
