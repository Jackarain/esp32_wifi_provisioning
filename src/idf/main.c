//
// Copyright (C) 2019 Jack.
//
// Author: jack
// Email:  jack.wgm at gmail dot com
//

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <nvs_flash.h>
#include <nvs.h>

#include <esp_log.h>
#include <esp_system.h>

static const char *TAG = "GUEST";
void setup();
void loop();

void loop_task()
{
    setup();

    for(;;) {
        usleep(1000000);
        loop();
    }
}

void app_main(void)
{
    const char* version = esp_get_idf_version();
    ESP_LOGI(TAG, "*** 当前 idf 版本: %s ***", version);
    ESP_LOGI(TAG, "*** 可用内存大小(bytes): %d ***", (int)esp_get_free_heap_size());

    esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGI(TAG, "NVS Flash 初始化失败，正在擦除...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    loop_task();
}
