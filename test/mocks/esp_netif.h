#ifndef ESP_NETIF_H
#define ESP_NETIF_H

#include <stdint.h>
#include "esp_err.h"

typedef struct esp_netif_obj esp_netif_t;

typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

typedef struct {
    esp_netif_t        *esp_netif;
    esp_netif_ip_info_t ip_info;
} ip_event_got_ip_t;

#define IPSTR           "%d.%d.%d.%d"
#define IP2STR(ipaddr)  ((ipaddr)->addr) & 0xff,          \
                        (((ipaddr)->addr) >> 8) & 0xff,   \
                        (((ipaddr)->addr) >> 16) & 0xff,  \
                        (((ipaddr)->addr) >> 24) & 0xff

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);

#endif
