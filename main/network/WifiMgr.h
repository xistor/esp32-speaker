#ifndef __WIFI_MGR_H__
#define __WIFI_MGR_H__

#include "esp_event.h"

class WifiMgr {
public:
    WifiMgr(const WifiMgr&) = delete;
    WifiMgr& operator=(const WifiMgr&) = delete;

    static WifiMgr& instance();
    static void eventHandler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);
    void init();
    void deinit();

private:
    static constexpr const char *_TAG_WIFI = "WifiMgr";
    WifiMgr();
    ~WifiMgr();

};

#endif // __WIFI_MGR_H__