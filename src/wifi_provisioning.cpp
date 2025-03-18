//
// Copyright (C) 2019 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#include "wifi_provisioning.hpp"
#include "scoped_exit.hpp"

#include <string.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_timer.h>
// #include <esp_http_server.h>

#include <nvs_flash.h>


namespace esp32_wifi_util
{
    static const int WIFI_CONNECTED_BIT = BIT0;
    static const char* TAG = "WIFI";

    static esp_event_handler_instance_t g_instance_any_id = nullptr;
    static esp_event_handler_instance_t g_instance_got_ip = nullptr;
    static esp_timer_handle_t g_timer_handle = nullptr;
    static EventGroupHandle_t g_wifi_event_group = xEventGroupCreate();


    void Wifi_Event_Handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
    {
        // auto self = static_cast<wifi_provisioning*>(arg);
        // self->WifiEventHandler(event_base, event_id, event_data);

        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            esp_wifi_connect();
            ESP_LOGI(TAG, "重试连接到 AP");
        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "获取到 IP: " IPSTR, IP2STR(&event->ip_info.ip));
            xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }

    wifi_provisioning::wifi_provisioning()
    {
        // 初始化网络协议栈
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        // 注册事件处理程序
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &Wifi_Event_Handler,
                                                            this,
                                                            &g_instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &Wifi_Event_Handler,
                                                            this,
                                                            &g_instance_got_ip));
        esp_netif_create_default_wifi_sta();

        // 初始化 Wi-Fi
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }

    void wifi_provisioning::auto_connect(connect_callback_t connect_cb)
    {
        m_connect_cb = connect_cb;

        // 使用 scoped_exit 确保返回时回调失败。
        scoped_exit failed_exit([&] {
            call_connect_cb(wifi_status::FAILED, {});
        });

        // 首先从 NVS 中读取 Wi-Fi 的 SSID 和密码，如果读取成功，则直接连接。
        nvs_handle_t nvs;
        auto err = nvs_open("wifi_settings", NVS_READONLY, &nvs);
        if (err == ESP_OK)
        {
            // 使用 scoped_exit 来确保 nvs_close 能够被调用。
            scoped_exit e([&] { nvs_close(nvs); });

            size_t len = 0;
            char buf[64] = {0};
            err = nvs_get_str(nvs, "ssid", buf, &len);
            if (err == ESP_OK) {
                m_ssid = buf;
            } else {
                ESP_LOGE(TAG, "NVS get ssid failed, start provisioning");
                return;
            }
            len = 0;
            memset(&buf[0], 0, sizeof(buf));
            err = nvs_get_str(nvs, "password", buf, &len);
            if (err == ESP_OK) {
                m_password = buf;
            } else {
                ESP_LOGW(TAG, "NVS get password empty, start provisioning");
            }
        }
        else
        {
            ESP_LOGI(TAG, "NVS open failed, start provisioning");
            return;
        }

        // 连接到 Wi-Fi
        if (!m_ssid.empty())
        {
            if (connect_wifi(m_ssid, m_password))
            {
                // 取消失败回调
                failed_exit.cancel();

                // 回调成功
                call_connect_cb(wifi_status::CONNECTED, m_ssid);
                return;
            }
        }
    }

    void wifi_provisioning::scan_networks(scan_callback_t scan_callback)
    {
        ESP_ERROR_CHECK(esp_wifi_start());

        // 创建定时器用于定时扫描 Wi-Fi
        ESP_ERROR_CHECK(esp_wifi_scan_start(nullptr, false));
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg)
            {
                auto self = static_cast<wifi_provisioning*>(arg);
                self->scan_timer_cb();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "WiFiScanTimer",
            .skip_unhandled_events = true
        };

        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &g_timer_handle));
        ESP_ERROR_CHECK(esp_timer_start_periodic(g_timer_handle, 10000 * 1000));
    }

    void wifi_provisioning::scan_timer_cb()
    {
        uint16_t ap_count = 0;
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
        if (ap_count == 0)
        {
            ESP_LOGW(TAG, "No AP found, retry!!!");
            ESP_ERROR_CHECK(esp_wifi_scan_start(nullptr, false));
            return;
        }

        ESP_LOGI(TAG, "扫描到 %d 个 Wi-Fi 网络", ap_count);
        wifi_ap_record_t* ap_records  = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (!ap_records)
        {
            ESP_LOGE(TAG, "内存分配失败");
            return;
        }

        m_wifi_list.clear();

        // 获取扫描到的接入点信息
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));
        for (int i = 0; i < ap_count; i++)
        {
            wifi_network net;

            net.ssid = (const char*)&ap_records[i].ssid[0];
            net.rssi = ap_records[i].rssi;
            net.auth_mode = ap_records[i].authmode;

            m_wifi_list.push_back(net);

            ESP_LOGI(TAG, "SSID: %-32.32s RSSI: %d Auth: %d",
                     ap_records[i].ssid, ap_records[i].rssi, ap_records[i].authmode);
        }

        // 释放内存
        free(ap_records);

        // 停止扫描
        esp_wifi_scan_stop();

        esp_timer_stop(g_timer_handle);
        esp_timer_delete(g_timer_handle);

        g_timer_handle = nullptr;

        // 回调扫描结果
        call_scan_cb(m_wifi_list);
    }

    bool wifi_provisioning::connect_wifi(const std::string& ssid, const std::string& password)
    {
        // 设置 Wi-Fi 配置并连接到 Wi-Fi
        wifi_config_t wifi_config = {};
        memset(&wifi_config, 0, sizeof(wifi_config_t));

        strcpy((char *)wifi_config.sta.ssid, ssid.c_str());
        strcpy((char *)wifi_config.sta.password, password.c_str());

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Connecting to WiFi %s", ssid.c_str());
        EventBits_t bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to WiFi %s", ssid.c_str());
            return true;
        } else {
            ESP_LOGE(TAG, "Failed to connect to WiFi %s", ssid.c_str());
            return false;
        }
    }

    bool wifi_provisioning::start_config_server(std::string ap_ssid/* = "ESP32"*/, std::string ap_password/* = ""*/)
    {
        return false;
    }

    void wifi_provisioning::call_connect_cb(wifi_status status, const std::string& ssid)
    {
        if (m_connect_cb && !m_abort)
            m_connect_cb(status, ssid);
    }

    void wifi_provisioning::call_scan_cb(const std::vector<wifi_network>& wifi_list)
    {
        if (m_scan_cb && !m_abort)
            m_scan_cb(wifi_list);
    }
}
