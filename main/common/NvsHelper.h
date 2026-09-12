#ifndef __NVS_HELPER_H__
#define __NVS_HELPER_H__

#include <cstddef>
#include <cstdint>

/**
 * Thin wrapper around the ESP-IDF NVS blob API so callers don't have to
 * repeat the open/set/commit/close dance (and the equivalent on the read
 * path). Lives outside SpeakerApp because several subsystems persist
 * independent settings — e.g. the BT stack stores its last-paired MAC while
 * the UI persists the selected streaming protocol.
 */
namespace NvsHelper {

// Write `len` bytes from `data` under (namespace, key). Silently no-ops on
// open failure; check the return value of `load` to detect missing keys.
void save(const char *ns, const char *key, const uint8_t *data, size_t len);

// Read up to `len` bytes into `data`. Returns true on success; false if the
// namespace/key is missing or the read fails.
bool load(const char *ns, const char *key, uint8_t *data, size_t len);

} // namespace NvsHelper

#endif // __NVS_HELPER_H__
