#include <string.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>

#include "wifi_provisioning.hpp"
#include "scoped_exit.hpp"

#ifdef __cplusplus
extern "C" {
#endif

static const char *TAG = "GUEST";

using namespace esp32_wifi_util;


void setup()
{
    ESP_LOGI("GUEST", "进入配置阶段");
    printf("Hello, world!\n");

    auto wp = new wifi_provisioning;

    wp->auto_connect([wp](wifi_status status, std::string ssid) mutable
    {
        switch (status)
        {
        case wifi_status::CONNECTED:
            ESP_LOGI("GUEST", "Wi-Fi 连接成功: %s", ssid.c_str());
            break;
        case wifi_status::FAILED:
            ESP_LOGE("GUEST", "Wi-Fi 连接失败: %s", ssid.c_str());
            delete wp;
            // wp->connect_wifi("客厅网络", "20121208");
            break;
        default:
            break;
        }
    });

    // wp.start();
    // wp.scan_wifi();
    // wp.connect_wifi("客厅网络", "20121208");
}

void loop()
{
    ESP_LOGI("GUEST", "Hello, world!");
}

#ifdef __cplusplus
}
#endif
