//
// Copyright (C) 2019 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#include <string.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>

#include "wifi_provisioning.hpp"
#include "scoped_exit.hpp"

using namespace esp32_wifi_util;

static const char *TAG = "GUEST";
static wifi_provisioning* g_wifi_provisioning = nullptr;

extern "C" void setup()
{
    ESP_LOGI(TAG, "进入配置阶段");

    g_wifi_provisioning = new wifi_provisioning;

    g_wifi_provisioning->auto_connect([](wifi_status status, std::string ssid) mutable
    {
        switch (status)
        {
        case wifi_status::CONNECTED:
            ESP_LOGI(TAG, "Wi-Fi 连接成功: %s", ssid.c_str());

            // 连接成功释放资源.
            g_wifi_provisioning->stop();
            delete g_wifi_provisioning;
            g_wifi_provisioning = nullptr;
            break;
        case wifi_status::FAILED:
            ESP_LOGE(TAG, "Wi-Fi 连接失败: %s", ssid.c_str());
            g_wifi_provisioning->start_config_server("ESP32-XXXX", "20121208");
            break;
        default:
            break;
        }
    });

}

extern "C" void loop()
{
    static int count = 0;

    ESP_LOGI(TAG, "Count: %d", count++);

    if (g_wifi_provisioning)
    {
        ESP_LOGI(TAG, "IP 地址: %s", g_wifi_provisioning->get_connected_ip().c_str());

        if (++count == 2000)
        {
            g_wifi_provisioning->stop();
            delete g_wifi_provisioning;
            g_wifi_provisioning = nullptr;
            ESP_LOGI(TAG, "停止 Wi-Fi 配置");
        }
    }
}
