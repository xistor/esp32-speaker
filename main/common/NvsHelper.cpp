#include "NvsHelper.h"
#include "nvs_flash.h"

namespace NvsHelper {

esp_err_t save(const char *ns, const char *key, const uint8_t *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_blob(handle, key, data, len);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t load(const char *ns, const char *key, uint8_t *data, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_get_blob(handle, key, data, &len);
    nvs_close(handle);
    return err;
}

} // namespace NvsHelper
