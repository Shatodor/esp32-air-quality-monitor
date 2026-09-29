#ifndef ESP_NETIF_SNTP_H
#define ESP_NETIF_SNTP_H
#include <stdbool.h>
#include <sys/time.h>
#include "esp_err.h"

typedef struct {
    int         num_of_servers;
    const char *server_names[4];
} esp_sntp_servers_t;

typedef struct {
    bool start;
    bool smooth_sync;
    bool wait_for_sync;
    void (*sync_cb)(struct timeval *tv);
    esp_sntp_servers_t servers;
} esp_sntp_config_t;

#define ESP_SNTP_SERVER_LIST(...) { __VA_ARGS__ }
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(n, list) \
    { .start = true, .servers = { .num_of_servers = (n), .server_names = list }, }

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *cfg);
#endif
