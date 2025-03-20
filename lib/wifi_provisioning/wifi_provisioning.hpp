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

        // 启动配置服务器，通常在自动连接 Wi-Fi 失败时调用（auto_connect）。
        // 该函数会启动一个 HTTP 服务器，用于手动配置设备的 Wi-Fi 连接，配置成功后，该设备将自动尝
        // 试连接到用户指定的 Wi-Fi 网络，具体用法：
        // 在 HTTP 成功启动后，用户可以通过访问 http://192.168.4.1/wl (GET 请求) 获取设备扫描到
        // 的可用 Wi-Fi 网络列表，并通过 http://192.168.4.1/wc (POST 请求) 提交一个 JSON 数据来
        // 配置 Wi-Fi，JSON 格式示例：
        //   {"ssid":"your_ssid","password":"your_password"}
        //
        // 函数参数：
        //   - ap_ssid: 配置模式下设备的 Wi-Fi 热点名称，默认为 "ESP32"。
        //   - ap_password: 配置模式下设备的 Wi-Fi 热点密码，默认为空（无密码）。
        //   - port: HTTP 服务器监听的端口号，默认为 80。
        // 返回值：
        //   - bool: 启动服务器成功返回 true，失败返回 false。
        bool start_config_server(std::string ap_ssid = "ESP32", std::string ap_password = "", int port = 80);

        // 扫描 Wi-Fi 网络
        void scan_networks(scan_callback_t scan_callback);

        // 连接到指定的 Wi-Fi 网络
        bool connect_wifi(const std::string& ssid, const std::string& password);

        // 创建一个 Wi-Fi 热点
        bool create_ap(const std::string& ap_ssid, const std::string& ap_password);

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

        void reset_event_handler();
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