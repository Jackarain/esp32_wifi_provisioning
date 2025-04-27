//
// Copyright (C) 2019 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#include "wifi_provisioning.hpp"
#include "scoped_exit.hpp"

#include <string.h>
#include <sys/socket.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_mac.h>

#include <esp_http_server.h>
#include <cJSON.h> // 引入 cJSON 库


#include <nvs_flash.h>

#include "freertos/event_groups.h"


namespace esp32_wifi_util
{
    static const int WIFI_DONE_BIT = BIT0;
    static const int WIFI_FAIL_BIT = BIT1;

    static const char *TAG = "WIFI_PROVISIONING";
    static const char *wifi_settings = "wifi_settings";

    static wifi_mode_t g_wifi_mode = WIFI_MODE_NULL;
    static esp_event_handler_instance_t g_instance_any_id = nullptr;
    static esp_event_handler_instance_t g_instance_got_ip = nullptr;
    static EventGroupHandle_t g_wifi_event_group = nullptr;

    static httpd_handle_t g_httpd_server = nullptr;
    static bool g_wifi_start = false;

    void Wifi_Event_Handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
    {
        auto self = static_cast<wifi_provisioning *>(arg);
        self->wifi_event_handler(event_base, event_id, event_data);
    }

    void wifi_provisioning::reset_event_handler()
    {
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
    }

    void wifi_provisioning::wifi_event_handler(const char *event_base, int32_t event_id, void *event_data)
    {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
        {
            ESP_LOGI(TAG, "STATION 模式，已经连接到 Wi-Fi ");
            xEventGroupSetBits(g_wifi_event_group, WIFI_DONE_BIT);
            return;
        }
        else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
        {
            ESP_LOGI(TAG, "STATION 模式，开始连接到 Wi-Fi");
            if (g_wifi_mode == WIFI_MODE_STA)
                esp_wifi_connect();
        }
        else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            if (g_wifi_mode == WIFI_MODE_STA)
            {
                ESP_LOGI(TAG, "STATION 模式，Wi-Fi 连接失败");
                xEventGroupSetBits(g_wifi_event_group, WIFI_FAIL_BIT);
            }
            else if (g_wifi_mode == WIFI_MODE_AP)
            {
                ESP_LOGI(TAG, "AP 模式，重新连接到 Wi-Fi");
            }
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

        reset_event_handler();

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }

    void wifi_provisioning::auto_connect(connect_callback_t connect_cb)
    {
        ESP_LOGI(TAG, "开始自动连接 Wi-Fi ...");

        m_retry_count = 0;
        m_connect_cb = connect_cb;

        // 使用 scoped_exit 确保返回时回调失败。
        scoped_exit failed_exit([&]
                                { call_connect_cb(wifi_status::FAILED, m_ssid); });

        std::string ap_password;

        // 首先从 NVS 中读取 Wi-Fi 的 SSID 和密码，如果读取成功，则直接连接。
        nvs_handle_t nvs_handle;
        auto err = nvs_open(wifi_settings, NVS_READONLY, &nvs_handle);
        if (err == ESP_OK)
        {
            // 使用 scoped_exit 来确保 nvs_close 能够被调用。
            scoped_exit e([&]
                          { nvs_close(nvs_handle); });

            size_t len = 0;
            char buf[64] = {0};
            err = nvs_get_str(nvs_handle, "ssid", nullptr, &len);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "NVS 获取 SSID 长度失败, 开始配网");
                return;
            }

            err = nvs_get_str(nvs_handle, "ssid", buf, &len);
            if (err == ESP_OK)
            {
                m_ssid = buf;
            }
            else
            {
                ESP_LOGE(TAG, "NVS 获取 SSID 长度失败, 开始配网");
                return;
            }

            len = 0;

            err = nvs_get_str(nvs_handle, "password", nullptr, &len);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "NVS 获取密码长度失败, 开始配网");
                return;
            }

            memset(&buf[0], 0, sizeof(buf));
            err = nvs_get_str(nvs_handle, "password", buf, &len);
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
        m_retry_count = 0;

        // 扫描 Wi-Fi
        if (!g_wifi_start)
        {
            ESP_ERROR_CHECK(esp_wifi_start());
            g_wifi_start = true;
        }

        ESP_ERROR_CHECK(esp_wifi_scan_start(nullptr, false));

        scoped_exit stop_wifi_scan([&]
                             { esp_wifi_scan_stop(); });

        xEventGroupClearBits(g_wifi_event_group, WIFI_DONE_BIT | WIFI_FAIL_BIT);
        auto bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_DONE_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & WIFI_FAIL_BIT)
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
        scoped_exit free_ap_records([&]
                                   { free(ap_records); });

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
        free_ap_records();

        // 停止扫描
        stop_wifi_scan();

        // 回调扫描结果
        call_scan_cb(m_wifi_list);
    }

    bool wifi_provisioning::connect_wifi(const std::string &ssid, const std::string &password)
    {
        m_ssid = ssid;

        // 保存 Wi-Fi 模式
        g_wifi_mode = WIFI_MODE_STA;

        // 初始化 Wi-Fi
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_deinit());
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

        g_wifi_start = false;

        // 设置 Wi-Fi 配置并连接到 Wi-Fi
        wifi_config_t wifi_config = {};
        memset(&wifi_config, 0, sizeof(wifi_config_t));

        strcpy((char *)wifi_config.sta.ssid, ssid.c_str());
        strcpy((char *)wifi_config.sta.password, password.c_str());

        return connect_wifi_impl(&wifi_config);
    }

    bool wifi_provisioning::create_ap(const std::string &ap_ssid, const std::string &ap_password)
    {
        if (ap_ssid.empty())
        {
            ESP_LOGW(TAG, "SSID is empty");
            return false;
        }

        m_ssid = ap_ssid;

        // 保存 Wi-Fi 模式
        g_wifi_mode = WIFI_MODE_AP;

        // 初始化 Wi-Fi
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_deinit());
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        g_wifi_start = false;

        // 注册事件处理程序
        reset_event_handler();

        wifi_config_t wifi_config = {};

        strcpy((char *)wifi_config.ap.ssid, m_ssid.c_str());
        if (!ap_password.empty())
        {
            strcpy((char *)wifi_config.ap.password, ap_password.c_str());
            wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        }
        else
        {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        wifi_config.ap.ssid_len = m_ssid.size();
        wifi_config.ap.channel = 1;
        wifi_config.ap.max_connection = 4;
        wifi_config.ap.beacon_interval = 100;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
        if (!g_wifi_start)
        {
            ESP_ERROR_CHECK(esp_wifi_start());
            g_wifi_start = true;
        }

        ESP_LOGI(TAG, "WiFi AP 已经启动, SSID: %s", m_ssid.c_str());

        return true;
    }

    bool wifi_provisioning::start_config_server(std::string ap_ssid /* = "ESP32"*/, std::string ap_password /* = ""*/, int port /* = 80*/)
    {
        // 创建 Wi-Fi 热点
        if (!create_ap(ap_ssid, ap_password))
            return false;

        // 启动 DNS 服务器
        start_dns();

        // 创建 HTTP 服务器
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = port;
        config.max_uri_handlers = 24;
        config.max_resp_headers = 24;
        config.uri_match_fn = httpd_uri_match_wildcard;

        if (httpd_start(&g_httpd_server, &config) == ESP_OK)
        {
            // 注册 URI 处理程序
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

            httpd_uri_t http_wifi_webconfig = {
                .uri = "/webconfig",
                .method = HTTP_GET,
                .handler = [](httpd_req_t *req) -> esp_err_t
                {
                    auto self = (wifi_provisioning *)req->user_ctx;
                    return self->http_wifi_web_config_handler((void *)req);
                },
                .user_ctx = (void *)this // 用户上下文（可选）
            };
            httpd_register_uri_handler(g_httpd_server, &http_wifi_webconfig);

            const char* captive_portal_urls[] = {
                "/hotspot-detect.html",         // Apple
                "/generate_204",                // Android
                "/mobile/status.php",           // Android
                "/check_network_status.txt",    // Windows
                "/ncsi.txt",                    // Windows
                "/connecttest.txt",             // Windows
                "/redirect",                    // Windows
                "/fwlink/",                     // Microsoft
                "/connectivity-check.html",     // Firefox
                "/success.txt",                 // Various
                "/portal.html",                 // Various
                "/library/test/success.html"    // Apple
            };

            for (const auto& url : captive_portal_urls)
            {
                httpd_uri_t captive_redirect_uri = {
                    .uri = url,
                    .method = HTTP_GET,
                    .handler = [](httpd_req_t *req) -> esp_err_t
                    {
                        auto self = (wifi_provisioning *)req->user_ctx;
                        return self->captive_redirect_uri_handler((void* )req);
                    },
                    .user_ctx = (void *)this // 用户上下文（可选）
                };

                httpd_register_uri_handler(g_httpd_server, &captive_redirect_uri);
            }

            ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to start HTTP server");
        }

        return true;
    }

    bool wifi_provisioning::connect_wifi_impl(void* wc)
    {
        // 获取 Wi-Fi 配置指针
        wifi_config_t* wifi_config = (wifi_config_t*)wc;
        if (!wifi_config || !wifi_config->sta.ssid[0])
        {
            ESP_LOGE(TAG, "Wi-Fi 配置无效");
            return false;
        }

        // 停止 Wi-Fi 扫描
        esp_wifi_scan_stop();

        // 保存 Wi-Fi 模式
        g_wifi_mode = WIFI_MODE_STA;

        // 初始化 Wi-Fi
        wifi_config->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config->sta.failure_retry_cnt = 1;

        // 开始连接 Wi-Fi
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, wifi_config));
        if (!g_wifi_start)
        {
            ESP_ERROR_CHECK(esp_wifi_start());
            g_wifi_start = true;
        }

        auto ret = esp_wifi_connect();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Wi-Fi 连接失败: %s", esp_err_to_name(ret));
            return false;
        }

        ESP_LOGI(TAG, "开始连接 WiFi %s, 密码: %s",
            wifi_config->sta.ssid, wifi_config->sta.password);

        xEventGroupClearBits(g_wifi_event_group, WIFI_DONE_BIT | WIFI_FAIL_BIT);
        auto bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_DONE_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        if ((bits & WIFI_FAIL_BIT))
        {
            ESP_LOGE(TAG, "Wi-Fi 连接失败");
            return false;
        }

        ESP_LOGI(TAG, "Wi-Fi 连接成功");

        return true;
    }

    void wifi_provisioning::start_dns()
    {
        ESP_LOGI(TAG, "Start DNS server...");

        m_dns_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_dns_fd < 0)
        {
            ESP_LOGE(TAG, "Failed to create DNS socket: %s", strerror(errno));
            return;
        }

        struct sockaddr_in dns_addr;
        memset(&dns_addr, 0, sizeof(dns_addr));
        dns_addr.sin_family = AF_INET;
        dns_addr.sin_port = htons(53);
        dns_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(m_dns_fd, (struct sockaddr *)&dns_addr, sizeof(dns_addr)) < 0)
        {
            ESP_LOGE(TAG, "Failed to bind DNS socket: %s", strerror(errno));
            close(m_dns_fd);
            m_dns_fd = -1;
            return;
        }

        ESP_LOGI(TAG, "DNS server started on port 53");

        xTaskCreate([](void* arg) {
            wifi_provisioning* self = static_cast<wifi_provisioning*>(arg);
            self->dns_handler();
        }, "dns_server", 4096, this, 5, NULL);
    }

    void wifi_provisioning::stop_dns()
    {
        ESP_LOGI(TAG, "stop DNS server...");
        if (m_dns_fd >= 0)
        {
            close(m_dns_fd);
            m_dns_fd = -1;
            ESP_LOGI(TAG, "DNS server stopped");
        }
    }

    void wifi_provisioning::dns_handler()
    {
        if (m_dns_fd < 0)
            return;

        while (!m_abort)
        {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);

            char buffer[512];
            ssize_t len = recvfrom(m_dns_fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &addr_len);
            if (len < 0)
            {
                ESP_LOGE(TAG, "Failed to receive DNS request: %s", strerror(errno));
                continue;
            }

            ESP_LOGI(TAG, "Received DNS request from %s:%d", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

            // 这里可以固定返回一个 IP 地址 192.168.4.1
            buffer[2] |= 0x80;  // Set response flag
            buffer[3] |= 0x80;  // Set Recursion Available
            buffer[7] = 1;      // Set answer count to 1

            // Add answer section
            memcpy(&buffer[len], "\xc0\x0c", 2);  // Name pointer
            len += 2;
            memcpy(&buffer[len], "\x00\x01\x00\x01\x00\x00\x00\x1c\x00\x04", 10);  // Type, class, TTL, data length
            len += 10;
            esp_ip4_addr_t ip_info;
            IP4_ADDR(&ip_info, 192, 168, 4, 1);  // IP address
            memcpy(&buffer[len], &ip_info.addr, 4);  // 192.168.4.1
            len += 4;
            ESP_LOGI(TAG, "Sending DNS response to %s", inet_ntoa(ip_info.addr));

            sendto(m_dns_fd, buffer, len, 0, (struct sockaddr *)&client_addr, addr_len);
        }
    }

    void wifi_provisioning::stop()
    {
        m_abort = true;

        if (g_httpd_server)
        {
            httpd_stop(g_httpd_server);
            g_httpd_server = nullptr;
        }

        stop_dns();

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

        ESP_LOGI(TAG, "处理 http_test_handler 请求");

        // 处理请求
        httpd_resp_send(req, "Hello, World!", -1);

        return ESP_OK;
    }

    int wifi_provisioning::http_wifi_list_handler(void *arg)
    {
        auto req = (httpd_req_t *)arg;

        ESP_LOGI(TAG, "处理 http_wifi_list_handler 请求!!!");

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

        ESP_LOGI(TAG, "处理 http_wifi_config_handler 请求");

        char buf[100]; // 用于存储POST数据的缓冲区
        int ret;
        std::string error_msg = "error";

        scoped_exit failed_exit([&] () mutable
        {
            httpd_resp_set_type(req, "application/json");
            sprintf(buf, "{ \"result\": \"%s\" }", error_msg.c_str());
            httpd_resp_send(req, buf, -1);
        });

        // 获取POST数据
        ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret <= 0)
        {
            return ESP_OK;
        }

        // 确保缓冲区以null结尾
        buf[ret] = '\0';
        ESP_LOGI(TAG, "POST数据: %s", buf);

        // 解析 JSON 数据，获取 SSID 和密码
        cJSON *root = cJSON_Parse(buf);
        if (!root)
        {
            error_msg = "json parse error";
            return ESP_OK;
        }

        scoped_exit root_deleter([&]
            { cJSON_Delete(root); });

        cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
        cJSON *password = cJSON_GetObjectItem(root, "password");

        if (!ssid || !password)
        {
            error_msg = "ssid or password is null";
            return ESP_OK;
        }

        ESP_LOGI(TAG, "SSID: %s, Password: %s", ssid->valuestring, password->valuestring);

        // 存储 Wi-Fi 配置到 NVS
        nvs_handle_t nvs_handle;
        auto err = nvs_open(wifi_settings, NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "保存 Wi-Fi 配置到 NVS");
            err = nvs_set_str(nvs_handle, "ssid", ssid->valuestring);
            if (err != ESP_OK)
            {
                error_msg = "ssid save error";

                ESP_LOGI(TAG, "保存 SSID 失败, ERROR: %d", err);
                nvs_close(nvs_handle);

                return ESP_OK;
            }

            err = nvs_set_str(nvs_handle, "password", password->valuestring);
            if (err != ESP_OK)
            {
                error_msg = "password save error";

                ESP_LOGI(TAG, "保存密码失败, ERROR: %d", err);
                nvs_close(nvs_handle);

                return ESP_OK;
            }

            ESP_ERROR_CHECK(nvs_commit(nvs_handle));
            nvs_close(nvs_handle);
        }

        // 设置 Wi-Fi 配置并连接到 Wi-Fi
        wifi_config_t wifi_config = {};
        memset(&wifi_config, 0, sizeof(wifi_config_t));

        strcpy((char *)wifi_config.sta.ssid, ssid->valuestring);
        strcpy((char *)wifi_config.sta.password, password->valuestring);

        // 连接到 Wi-Fi
        if (connect_wifi_impl(&wifi_config))
        {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{ \"result\": \"ok\" }", -1);
        }
        else
        {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{ \"result\": \"failed\" }", -1);
        }

        failed_exit.cancel();

        return ESP_OK;
    }

    int wifi_provisioning::http_wifi_web_config_handler(void *arg)
    {
        auto req = (httpd_req_t *)arg;

        ESP_LOGI(TAG, "处理 http_wifi_web_config_handler 请求");

        // 处理请求
        httpd_resp_send(req,
R"xxxxxxxx(<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>WiFi 配置</title>
    <style>
        body {
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            min-height: 100vh;
            margin: 0;
            font-family: Arial, sans-serif;
            background-color: #1a1a1a;
            color: #ffffff;
            padding: 20px;
            box-sizing: border-box;
        }
        .input-group {
            margin: 10px 0;
            text-align: center;
            width: 100%;
            max-width: 300px;
        }
        input {
            padding: 8px;
            width: 100%;
            max-width: 300px;
            background-color: #2d2d2d;
            border: 1px solid #404040;
            color: #ffffff;
            border-radius: 4px;
            box-sizing: border-box;
            font-size: 16px;
        }
        input::placeholder {
            color: #888888;
        }
        .button-group {
            display: flex;
            gap: 10px;
            width: 100%;
            max-width: 300px;
            justify-content: center;
            margin: 15px 0;
        }
        button {
            padding: 10px 25px;
            background-color: #404040;
            color: #ffffff;
            border: none;
            border-radius: 4px;
            cursor: pointer;
            transition: background-color 0.3s;
            font-size: 16px;
            flex: 1;
        }
        button:hover {
            background-color: #505050;
        }
        #wifiList {
            list-style: none;
            padding: 0;
            max-height: 250px;
            overflow-y: auto;
            width: 100%;
            max-width: 300px;
            background-color: #2d2d2d;
            border: 1px solid #404040;
            border-radius: 4px;
        }
        #wifiList li {
            padding: 10px;
            cursor: pointer;
            text-align: center;
            border-bottom: 1px solid #404040;
            font-size: 16px;
        }
        #wifiList li:last-child {
            border-bottom: none;
        }
        #wifiList li:hover {
            background-color: #383838;
        }

        @media (max-width: 600px) {
            body {
                padding: 10px;
            }
            .input-group, input, .button-group, #wifiList {
                max-width: 100%;
            }
            input, button, #wifiList li {
                font-size: 14px;
            }
            button {
                padding: 8px 15px;
            }
            #wifiList li {
                padding: 8px;
            }
            .button-group {
                flex-direction: column;
                gap: 8px;
            }
        }
    </style>
</head>
<body>
    <div class="input-group">
        <input type="text" id="ssid" placeholder="请输入SSID">
    </div>
    <div class="input-group">
        <input type="password" id="password" placeholder="请输入密码">
    </div>
    <div class="button-group">
        <button onclick="configureWifi()">配置</button>
        <button onclick="loadWifiList()">刷新</button>
    </div>
    <ul id="wifiList"></ul>

    <script>
        function configureWifi() {
            const ssid = document.getElementById('ssid').value;
            const password = document.getElementById('password').value;
            const data = {
                ssid: ssid,
                password: password
            };

            fetch('http://192.168.4.1/wc', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(data)
            })
            .then(response => response.json())
            .then(data => {
                const wifiList = document.getElementById('wifiList');
                wifiList.innerHTML = '';
                const li = document.createElement('li');
                if (data.result === 'ok') {
                    li.textContent = '连接 WiFi 成功';
                } else {
                    li.textContent = `连接失败: ${data.result}`;
                }
                wifiList.appendChild(li);
                console.log('Success:', data);
            })
            .catch(error => console.error('Error:', error));
        }

        function loadWifiList() {
            fetch('http://192.168.4.1/wl')
                .then(response => response.json())
                .then(data => {
                    data.sort((a, b) => b.rssi - a.rssi);
                    const wifiList = document.getElementById('wifiList');
                    wifiList.innerHTML = '';

                    data.forEach(wifi => {
                        const li = document.createElement('li');
                        li.textContent = wifi.ssid;
                        li.onclick = () => {
                            document.getElementById('ssid').value = wifi.ssid;
                        };
                        wifiList.appendChild(li);
                    });
                })
                .catch(error => console.error('Error:', error));
        }
        window.onload = loadWifiList;
    </script>
</body>
</html>)xxxxxxxx", -1);

        return ESP_OK;
    }

    int wifi_provisioning::captive_redirect_uri_handler(void *arg)
    {
        ESP_LOGI(TAG, "处理 captive_redirect_uri_handler 请求");

        auto req = (httpd_req_t *)arg;

        std::string url = "http://192.168.4.1/webconfig";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", url.c_str());
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, nullptr, 0);

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

    void wifi_provisioning::clear_wifi_config()
    {
        nvs_handle_t nvs_handle;
        auto err = nvs_open(wifi_settings, NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK)
        {
            nvs_erase_all(nvs_handle);
            nvs_close(nvs_handle);

            ESP_LOGI(TAG, "Wi-Fi 配置已清除");
            return;
        }

        ESP_LOGW(TAG, "Wi-Fi 清除配置时 nvs 打开失败, ERROR: %d", err);
    }
}
