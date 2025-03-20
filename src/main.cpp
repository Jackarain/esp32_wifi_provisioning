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


static const char *TAG = "GUEST";

using namespace esp32_wifi_util;

wifi_provisioning* g_wifi_provisioning = nullptr;

extern "C" void setup()
{
    ESP_LOGI("GUEST", "进入配置阶段");

    g_wifi_provisioning = new wifi_provisioning;

#if 0
    g_wifi_provisioning->scan_networks([](std::vector<wifi_network> wifi_list)
    {
        ESP_LOGI("GUEST", "扫描到 %d 个 Wi-Fi 网络", wifi_list.size());
        for (auto &network : wifi_list)
        {
            ESP_LOGI("GUEST", "SSID: %s, RSSI: %d, Auth: %d", network.ssid.c_str(), network.rssi, network.auth_mode);
        }
    });

    ESP_LOGI("GUEST", "开始 Wi-Fi 配置");

    g_wifi_provisioning->start_config_server("ESP32-XXXX", "20121208");

    ESP_LOGI("GUEST", "Wi-Fi SSID: %s", g_wifi_provisioning->get_connected_ssid().c_str());
    ESP_LOGI("GUEST", "IP 地址: %s", g_wifi_provisioning->get_connected_ip().c_str());

#else
g_wifi_provisioning->auto_connect([](wifi_status status, std::string ssid) mutable
{
    switch (status)
    {
    case wifi_status::CONNECTED:
        ESP_LOGI("GUEST", "Wi-Fi 连接成功: %s", ssid.c_str());
        g_wifi_provisioning->stop();
        delete g_wifi_provisioning;
        g_wifi_provisioning = nullptr;
        break;
    case wifi_status::FAILED:
        ESP_LOGE("GUEST", "Wi-Fi 连接失败: %s", ssid.c_str());
        // g_wifi_provisioning->connect_wifi("客厅网络", "20121208");
        g_wifi_provisioning->start_config_server("ESP32-XXXX", "20121208");
        break;
    default:
        break;
    }
});
#endif
}

extern "C" void loop()
{
    ESP_LOGI("GUEST", "IP 地址: %s", g_wifi_provisioning->get_connected_ip().c_str());
    static int count = 0;
    ESP_LOGI("GUEST", "Count: %d", count++);

    if (++count == 2000 && g_wifi_provisioning)
    {
        g_wifi_provisioning->stop();
        delete g_wifi_provisioning;
        g_wifi_provisioning = nullptr;
        ESP_LOGI("GUEST", "停止 Wi-Fi 配置");
    }
}
