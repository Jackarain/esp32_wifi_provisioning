# ESP32 WiFi 配置库

这是一个为 ESP32 设备设计的 C++ WiFi 配置和管理库，它提供了自动连接 WiFi 网络、扫描可用网络、创建接入点（AP）以及通过 Web 界面手动配置 WiFi 的功能。

---

## 功能

- **自动 WiFi 连接**：自动尝试连接到之前配置的 WiFi 网络。
- **WiFi 网络扫描**：扫描并获取可用 WiFi 网络列表，包括信号强度和认证信息。
- **配置服务器**：在自动连接失败时启动 HTTP 服务器，可通过 Web 界面手动配置 WiFi。
- **接入点模式**：创建 WiFi 热点（AP）以便进行配置。
- **手动 WiFi 连接**：使用指定的 SSID 和密码连接到 WiFi 网络。

---

## 安装

1. 克隆或下载此仓库。
2. 在您的 ESP32 项目中的 lib 中包含 `wifi_provisioning` 库相关文件。

---

## 使用方法

### 基本示例

```cpp
#include "wifi_provisioning.hpp"
#include <iostream>

using namespace esp32_wifi_util;

void connect_callback(wifi_status status, std::string ssid) {
    switch (status) {
        case wifi_status::CONNECTED:
            std::cout << "已连接到 " << ssid << std::endl;
            break;
        case wifi_status::FAILED:
            std::cout << "连接失败" << std::endl;
            break;
        default:
            std::cout << "状态: " << static_cast<int>(status) << std::endl;
    }
}

void scan_callback(std::vector<wifi_network> networks) {
    for (const auto& net : networks) {
        std::cout << "SSID: " << net.ssid << ", RSSI: " << (int)net.rssi << std::endl;
    }
}

int main() {
    wifi_provisioning wifi;

    // 尝试自动连接示例
    wifi.auto_connect(connect_callback);

    // 扫描可用网络示例
    wifi.scan_networks(scan_callback);

    // 如果自动连接失败，启动配置服务器，可以在 auto_connect 回调中
    // wifi_status::FAILED 状态下调用
    if (!wifi.start_config_server("MyESP32", "password123")) {
        std::cout << "启动配置服务器失败" << std::endl;
    }

    // 手动连接到 WiFi 网络示例
    wifi.connect_wifi("MyWiFi", "mypassword");

    // 停止 wifi 配置并释放资源，注意调用 stop 并不断开已经连接的 wifi 网络
    wifi.stop();

    return 0;
}

```

## API 参考说明

### 枚举

- **`wifi_status`**：
  - `NOT_CONFIGURED`：WiFi 未配置。
  - `CONNECTING`：正在连接网络。
  - `CONNECTED`：已成功连接到网络。
  - `FAILED`：连接尝试失败。
  - `AP_MODE`：设备处于接入点模式。

### 结构体

- **`wifi_network`**：
  - `std::string ssid`：WiFi 网络的 SSID。
  - `int8_t rssi`：信号强度（RSSI）。
  - `uint8_t auth_mode`：网络的认证模式。

### 类：`wifi_provisioning`

#### 构造函数

- **`wifi_provisioning()`**：初始化 WiFi 配置实例。
- **不可复制**：复制构造函数和赋值运算符已被禁用。

#### 公共方法

- **`void auto_connect(connect_callback_t connect_cb)`**
  开始自动 WiFi 连接流程，并调用提供的回调函数报告连接状态。

- **`bool start_config_server(std::string ap_ssid = "ESP32", std::string ap_password = "", int port = 80)`**
  在 AP 模式下启动 HTTP 服务器以进行手动 WiFi 配置。成功返回 `true`，失败返回 `false`。
  - 默认 AP SSID：`"ESP32"`
  - 默认 AP 密码：`""`（无密码）
  - 默认端口：`80`
  - Web 界面：访问 `http://192.168.4.1/webconfig`
  - 获取 WiFi 列表：`GET http://192.168.4.1/wl`
  - 配置 WiFi：`POST http://192.168.4.1/wc`，JSON 格式：`{"ssid":"your_ssid","password":"your_password"}`

- **`void scan_networks(scan_callback_t scan_callback)`**
  扫描可用 WiFi 网络，并通过回调函数返回网络列表。

- **`bool connect_wifi(const std::string& ssid, const std::string& password)`**
  连接到指定的 WiFi 网络，成功返回 `true`。

- **`bool create_ap(const std::string& ap_ssid, const std::string& ap_password)`**
  创建带有指定 SSID 和密码的 WiFi 接入点，成功返回 `true`。

- **`void stop()`**
  停止所有 WiFi 操作，但不会断开已建立的连接。通常用于清理资源。

- **`std::string get_connected_ssid() const`**
  返回当前连接的 WiFi 网络的 SSID。

- **`std::string get_connected_ip() const`**
  返回设备在已连接网络中的 IP 地址。

- **`void clear_wifi_config()`**
  清除存储的 WiFi 配置信息。

---

## 配置 Web 界面

调用 `start_config_server()` 后：

1. 连接到 ESP32 的 AP（默认 SSID：`ESP32`）。
2. 打开浏览器，访问 `http://192.168.4.1/webconfig`，用户可以通过下面 web 页面配置：
    ![image](https://github.com/user-attachments/assets/04ef9527-34e6-4006-8ac2-6dd6de7c10bd)
3. 通过 API `GET http://192.168.4.1/wl` 获取可用网络列表。
4. 通过 API `POST http://192.168.4.1/wc` 提交 WiFi 凭据，JSON 格式如下：

   ```json
   {"ssid":"your_ssid","password":"your_password"}
   ```

## 贡献

欢迎提交问题或拉取请求以改进此库，期待您的贡献！
