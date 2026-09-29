#include "esp_netif.h"
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    static int dummy;
    return (esp_netif_t *)&dummy;
}
