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
#include <esp_mac.h>

#include <esp_http_server.h>
#include <cJSON.h> // 引入 cJSON 库


#include <nvs_flash.h>

namespace esp32_wifi_util
{
    static const int WIFI_DONE_BIT = BIT0;
    static const char *TAG = "WIFI";

    static wifi_mode_t g_wifi_mode = WIFI_MODE_NULL;
    static esp_event_handler_instance_t g_instance_any_id = nullptr;
    static esp_event_handler_instance_t g_instance_got_ip = nullptr;
    static EventGroupHandle_t g_wifi_event_group = nullptr;

    static httpd_handle_t g_httpd_server = nullptr;

    void Wifi_Event_Handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
    {
        auto self = static_cast<wifi_provisioning *>(arg);
        self->wifi_event_handler(event_base, event_id, event_data);
    }

    void wifi_provisioning::wifi_event_handler(const char *event_base, int32_t event_id, void *event_data)
    {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
        {
            ESP_LOGI(TAG, "STATION 模式，开始连接到 Wi-Fi");
            if (g_wifi_mode == WIFI_MODE_STA)
                esp_wifi_connect();
        }
        else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            ESP_LOGI(TAG, "STATION 模式，重新连接到 Wi-Fi");
            if (g_wifi_mode == WIFI_MODE_STA)
                esp_wifi_connect();
        }
        else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "获取到 IP: " IPSTR, IP2STR(&event->ip_info.ip));
            xEventGroupSetBits(g_wifi_event_group, WIFI_DONE_BIT);
        }
        else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE)
        {
            ESP_LOGI(TAG, "Wi-Fi 扫描完成");
            xEventGroupSetBits(g_wifi_event_group, WIFI_DONE_BIT);
        }

        if (event_id == WIFI_EVENT_AP_STACONNECTED)
        {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "客户端 " MACSTR " 已连接, AID=%d",
                     MAC2STR(event->mac), event->aid);
        }
        else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
        {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "客户端 " MACSTR " 已离开, AID=%d",
                     MAC2STR(event->mac), event->aid);
        }
    }

    wifi_provisioning::wifi_provisioning()
    {
        g_wifi_event_group = xEventGroupCreate();

        // 初始化网络协议栈
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        esp_netif_create_default_wifi_sta();
        esp_netif_create_default_wifi_ap();

        // 初始化 Wi-Fi
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

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

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }

    void wifi_provisioning::auto_connect(connect_callback_t connect_cb)
    {
        ESP_LOGI(TAG, "开始自动连接 Wi-Fi ...");

        m_connect_cb = connect_cb;

        // 使用 scoped_exit 确保返回时回调失败。
        scoped_exit failed_exit([&]
                                { call_connect_cb(wifi_status::FAILED, {}); });

        std::string ap_password;

        // 首先从 NVS 中读取 Wi-Fi 的 SSID 和密码，如果读取成功，则直接连接。
        nvs_handle_t nvs;
        auto err = nvs_open("wifi_settings", NVS_READONLY, &nvs);
        if (err == ESP_OK)
        {
            // 使用 scoped_exit 来确保 nvs_close 能够被调用。
            scoped_exit e([&]
                          { nvs_close(nvs); });

            size_t len = 0;
            char buf[64] = {0};
            err = nvs_get_str(nvs, "ssid", buf, &len);
            if (err == ESP_OK)
            {
                m_ssid = buf;
            }
            else
            {
                ESP_LOGE(TAG, "NVS get ssid failed, start provisioning");
                return;
            }
            len = 0;
            memset(&buf[0], 0, sizeof(buf));
            err = nvs_get_str(nvs, "password", buf, &len);
            if (err == ESP_OK)
            {
                ap_password = buf;
            }
            else
            {
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
            if (connect_wifi(m_ssid, ap_password))
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
        ESP_LOGI(TAG, "开始扫描 Wi-Fi 网络 ...");

        m_scan_cb = scan_callback;

        ESP_ERROR_CHECK(esp_wifi_start());

        // 扫描 Wi-Fi
        ESP_ERROR_CHECK(esp_wifi_scan_start(nullptr, false));

        auto bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_DONE_BIT, false, true, portMAX_DELAY);
        xEventGroupClearBits(g_wifi_event_group, WIFI_DONE_BIT);
        if (!(bits & WIFI_DONE_BIT))
        {
            ESP_LOGE(TAG, "Wi-Fi 扫描失败");
            return;
        }

        uint16_t ap_count = 0;
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
        if (ap_count == 0)
        {
            ESP_LOGW(TAG, "没有扫描到 Wi-Fi 网络");
            return;
        }

        ESP_LOGI(TAG, "扫描到 %d 个 Wi-Fi 网络", ap_count);
        wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
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

            net.ssid = (const char *)&ap_records[i].ssid[0];
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

        // 回调扫描结果
        call_scan_cb(m_wifi_list);
    }

    bool wifi_provisioning::connect_wifi(const std::string &ssid, const std::string &password)
    {
        // 设置 Wi-Fi 配置并连接到 Wi-Fi
        wifi_config_t wifi_config = {};
        memset(&wifi_config, 0, sizeof(wifi_config_t));

        m_ssid = ssid;

        // 保存 Wi-Fi 模式
        g_wifi_mode = WIFI_MODE_STA;

        strcpy((char *)wifi_config.sta.ssid, ssid.c_str());
        strcpy((char *)wifi_config.sta.password, password.c_str());

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Connecting to WiFi %s", ssid.c_str());
        EventBits_t bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_DONE_BIT, false, true, portMAX_DELAY);
        xEventGroupClearBits(g_wifi_event_group, WIFI_DONE_BIT);
        if (bits & WIFI_DONE_BIT)
        {
            ESP_LOGI(TAG, "Connected to WiFi %s", ssid.c_str());
            return true;
        }
        else
        {
            ESP_LOGE(TAG, "Failed to connect to WiFi %s", ssid.c_str());
            return false;
        }
    }

    bool wifi_provisioning::create_ap(const std::string &ssid, const std::string &password)
    {
        if (ssid.empty())
        {
            ESP_LOGW(TAG, "SSID is empty");
            return false;
        }

        m_ssid = ssid;

        // 保存 Wi-Fi 模式
        g_wifi_mode = WIFI_MODE_AP;

        wifi_config_t wifi_config = {0};

        strcpy((char *)wifi_config.ap.ssid, ssid.c_str());
        if (!password.empty())
        {
            strcpy((char *)wifi_config.ap.password, password.c_str());
            wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        }
        else
        {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        wifi_config.ap.ssid_len = ssid.size();
        wifi_config.ap.channel = 1;
        wifi_config.ap.max_connection = 4;
        wifi_config.ap.beacon_interval = 100;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "WiFi 已经启动, SSID: %s", ssid.c_str());

        return true;
    }

    bool wifi_provisioning::start_config_server(std::string ap_ssid /* = "ESP32"*/, std::string ap_password /* = ""*/, int port /* = 80*/)
    {
        // 创建 Wi-Fi 热点
        if (!create_ap(ap_ssid, ap_password))
            return false;

        // 创建 HTTP 服务器
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = port;

        if (httpd_start(&g_httpd_server, &config) == ESP_OK)
        {
            // 注册 URI 处理程序
            httpd_uri_t root = {
                .uri = "/",         // 根路径
                .method = HTTP_GET, // 处理 GET 请求
                .handler = [](httpd_req_t *req) -> esp_err_t
                {
                    auto self = (wifi_provisioning *)req->user_ctx;
                    return self->http_test_handler((void *)req);
                },
                .user_ctx = (void *)this // 用户上下文（可选）
            };
            httpd_register_uri_handler(g_httpd_server, &root);

            httpd_uri_t http_test = {
                .uri = "/test",
                .method = HTTP_GET, // 处理 GET 请求
                .handler = [](httpd_req_t *req) -> esp_err_t
                {
                    auto self = (wifi_provisioning *)req->user_ctx;
                    return self->http_test_handler((void *)req);
                },
                .user_ctx = (void *)this // 用户上下文（可选）
            };
            httpd_register_uri_handler(g_httpd_server, &http_test);

            httpd_uri_t http_wifi_list = {
                .uri = "/wl",
                .method = HTTP_GET,
                .handler = [](httpd_req_t *req) -> esp_err_t
                {
                    auto self = (wifi_provisioning *)req->user_ctx;
                    return self->http_wifi_list_handler((void *)req);
                },
                .user_ctx = (void *)this // 用户上下文（可选）
            };
            httpd_register_uri_handler(g_httpd_server, &http_wifi_list);

            httpd_uri_t http_wifi_config = {
                .uri = "/wc",
                .method = HTTP_POST,
                .handler = [](httpd_req_t *req) -> esp_err_t
                {
                    auto self = (wifi_provisioning *)req->user_ctx;
                    return self->http_wifi_config_handler((void *)req);
                },
                .user_ctx = (void *)this // 用户上下文（可选）
            };
            httpd_register_uri_handler(g_httpd_server, &http_wifi_config);

            ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to start HTTP server");
        }

        return true;
    }

    void wifi_provisioning::stop()
    {
        m_abort = true;
        g_wifi_mode = WIFI_MODE_NULL;

        if (g_httpd_server)
        {
            httpd_stop(g_httpd_server);
            g_httpd_server = nullptr;
        }

        if (g_instance_any_id)
        {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, g_instance_any_id);
            g_instance_any_id = nullptr;
        }

        if (g_instance_got_ip)
        {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, g_instance_got_ip);
            g_instance_got_ip = nullptr;
        }

        if (g_wifi_event_group)
        {
            vEventGroupDelete(g_wifi_event_group);
            g_wifi_event_group = nullptr;
        }
    }

    void wifi_provisioning::call_connect_cb(wifi_status status, const std::string &ssid)
    {
        if (m_connect_cb && !m_abort)
            m_connect_cb(status, ssid);
    }

    void wifi_provisioning::call_scan_cb(const std::vector<wifi_network> &wifi_list)
    {
        if (m_scan_cb && !m_abort)
            m_scan_cb(wifi_list);
    }

    int wifi_provisioning::http_test_handler(void *arg)
    {
        auto req = (httpd_req_t *)arg;

        ESP_LOGI(TAG, "处理 test HTTP 请求");

        // 处理请求
        httpd_resp_send(req, "Hello, World!", -1);

        return ESP_OK;
    }

    int wifi_provisioning::http_wifi_list_handler(void *arg)
    {
        auto req = (httpd_req_t *)arg;

        ESP_LOGI(TAG, "处理 wifi list HTTP 请求!!!");

        scan_networks([req](std::vector<wifi_network> wifi_list)
        {
            // 创建 JSON 数组
            cJSON *root = cJSON_CreateArray();
            if (!root)
            {
                httpd_resp_send_500(req);
                return;
            }

            // 填充 JSON 数据
            for (const auto &network : wifi_list)
            {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "ssid", network.ssid.c_str());
                cJSON_AddNumberToObject(item, "rssi", network.rssi);
                cJSON_AddNumberToObject(item, "auth_mode", network.auth_mode);
                cJSON_AddItemToArray(root, item);
            }

            // 将 JSON 对象转换为字符串
            char *json_str = cJSON_PrintUnformatted(root);
            if (!json_str)
            {
                cJSON_Delete(root);
                httpd_resp_send_500(req);
                return;
            }

            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_str, -1);
        });

        return ESP_OK;
    }

    int wifi_provisioning::http_wifi_config_handler(void *arg)
    {
        auto req = (httpd_req_t *)arg;

        ESP_LOGI(TAG, "处理 wifi config HTTP 请求");

        char buf[100]; // 用于存储POST数据的缓冲区
        int ret;

        // 获取POST数据
        ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret <= 0)
        {   // 如果没有数据可用，或者发生错误，则返回错误
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                httpd_resp_send_408(req);
            return ESP_FAIL;
        }

        // 确保缓冲区以null结尾
        buf[ret] = '\0';
        ESP_LOGI(TAG, "POST数据: %s", buf);

        // 解析 JSON 数据，获取 SSID 和密码
        cJSON *root = cJSON_Parse(buf);
        if (!root)
        {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
        cJSON *password = cJSON_GetObjectItem(root, "password");

        if (!ssid || !password)
        {
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "SSID: %s, Password: %s", ssid->valuestring, password->valuestring);

        // 存储 Wi-Fi 配置到 NVS
        nvs_handle_t nvs;
        auto err = nvs_open("wifi_settings", NVS_READWRITE, &nvs);
        if (err == ESP_OK)
        {
            err = nvs_set_str(nvs, "ssid", ssid->valuestring);
            if (err == ESP_OK)
                err = nvs_set_str(nvs, "password", password->valuestring);
            nvs_close(nvs);
        }

        cJSON_Delete(root);

        // 连接到 Wi-Fi
        if (connect_wifi(ssid->valuestring, password->valuestring))
        {
            httpd_resp_send(req, "OK", -1);
        }
        else
        {
            httpd_resp_send_500(req);
        }

        return ESP_OK;
    }

    std::string wifi_provisioning::get_connected_ssid() const
    {
        return m_ssid;
    }

    std::string wifi_provisioning::get_connected_ip() const
    {
        esp_netif_ip_info_t ip_info;

        switch (g_wifi_mode)
        {
        case WIFI_MODE_STA:
            if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK)
            {
                char ip[16] = {0};
                sprintf(ip, IPSTR, IP2STR(&ip_info.ip));
                return ip;
            }
            break;
        case WIFI_MODE_AP:
            if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info) == ESP_OK)
            {
                char ip[16] = {0};
                sprintf(ip, IPSTR, IP2STR(&ip_info.ip));
                return ip;
            }
            break;
        default:
            break;
        }

        return {};
    }
}
