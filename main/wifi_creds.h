#ifndef WIFI_CREDS_H
#define WIFI_CREDS_H

/*
 * WiFi credentials storage (NVS namespace "wifi_db").
 *
 * Key derivation: CRC32 of the SSID, rendered as 8 hex chars — fits
 * within the NVS 15-char key limit. Collisions across SSIDs are
 * theoretically possible but practically irrelevant for a home device.
 *
 * Cross-task safe: each call opens NVS independently, no shared state.
 */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Retrieve a saved password for the given SSID.
   Returns true and writes a NUL-terminated string on success.
   Returns false if no password is saved or on NVS error. */
bool wifi_creds_get(const char *ssid, char *out_password, size_t max_len);

/* Persist a password for the given SSID. No-op if an identical
   password is already stored. Silently logs and returns on NVS error. */
void wifi_creds_save(const char *ssid, const char *password);

/* Erase a stored password. Returns ESP_OK if erased or if there was
   nothing to erase. Returns ESP_ERR_INVALID_ARG on bad input, or an
   NVS error on failure. */
esp_err_t wifi_creds_forget(const char *ssid);

#endif // WIFI_CREDS_H