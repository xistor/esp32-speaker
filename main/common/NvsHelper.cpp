#include "NvsHelper.h"
#include "nvs_flash.h"

namespace NvsHelper {

void save(const char *ns, const char *key, const uint8_t *data, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_blob(handle, key, data, len);
    nvs_commit(handle);
    nvs_close(handle);
}

bool load(const char *ns, const char *key, uint8_t *data, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_blob(handle, key, data, &len);
    nvs_close(handle);
    return err == ESP_OK;
}

} // namespace NvsHelper
