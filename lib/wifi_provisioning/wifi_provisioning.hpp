//
// Copyright (C) 2019 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#ifndef WIFI_PROVISIONING_HPP
#define WIFI_PROVISIONING_HPP

#include <string>
#include <vector>
#include <functional>
#include <atomic>

namespace esp32_wifi_util
{
    enum class wifi_status
    {
        NOT_CONFIGURED,     // 未配置
        CONNECTING,         // 连接中
        CONNECTED,          // 已连接
        FAILED,             // 连接失败
        AP_MODE             // AP模式
    };

    struct wifi_network
    {
        std::string ssid;
        int8_t rssi;        // 信号强度
        uint8_t auth_mode;  // 认证模式
    };

    using connect_callback_t = std::function<void(wifi_status, std::string)>;
    using scan_callback_t = std::function<void(std::vector<wifi_network>)>;

    class wifi_provisioning
    {
        // noncopyable for wifi_provisioning
        wifi_provisioning(const wifi_provisioning&) = delete;
        wifi_provisioning& operator=(const wifi_provisioning&) = delete;

        friend void Wifi_Event_Handler(
            void* arg, const char* event_base, int32_t event_id, void* event_data);

    public:
        wifi_provisioning();
        ~wifi_provisioning() = default;

    public:
        // 开始自动配网
        void auto_connect(connect_callback_t connect_cb);

        // 开始配置服务器，通常在 auto_connect
        bool start_config_server(std::string ap_ssid = "ESP32", std::string ap_password = "", int port = 80);

        // 扫描 Wi-Fi 网络
        void scan_networks(scan_callback_t scan_callback);

        // 连接到指定的 Wi-Fi 网络
        bool connect_wifi(const std::string& ssid, const std::string& password);

        // 创建一个 Wi-Fi 热点
        bool create_ap(const std::string& ssid, const std::string& password);

        // 停止，当调用 stop 时，会停止所有的 Wi-Fi 操作，如果已经连接到 Wi-Fi，不会断开连接。
        // 通常用于程序退出时调用，或者成功连接到 Wi-Fi 后调用以释放资源。
        void stop();

        // 连接到指定的 Wi-Fi 网络的 SSID
        std::string get_connected_ssid() const;

        // 获取连接的 IP 地址
        std::string get_connected_ip() const;

    private:
        void call_connect_cb(wifi_status status, const std::string& ssid);
        void call_scan_cb(const std::vector<wifi_network>& wifi_list);

        int http_test_handler(void* arg);
        int http_wifi_list_handler(void* arg);
        int http_wifi_config_handler(void* arg);

        void wifi_event_handler(const char* event_base, int32_t event_id, void* event_data);

    private:
        connect_callback_t m_connect_cb;
        scan_callback_t m_scan_cb;

        std::string m_ssid;
        std::vector<wifi_network> m_wifi_list;

        std::atomic_bool m_abort{ false };
    };
}

#endif // WIFI_PROVISIONING_HPP