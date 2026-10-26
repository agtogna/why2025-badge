#include <stdio.h>
#include <string.h>

#include "badge/badge.h"

void app_main() 
{
    ESP_LOGI(__FILE__, "MAIN START: free_heap_size = %lu\n", esp_get_free_heap_size());

    badge_init();
    led_init();
    
    // start bluetooth
    if (bt_init() == ESP_OK) {
        xTaskCreatePinnedToCore(bt_task, "bt_task", 4096, NULL, 6, NULL, 0);
    } else {
        ESP_LOGE(__FILE__, "BLE init failed, running without radar");
    }

    // start wifi management
    wifi_init();
    xTaskCreatePinnedToCore(wifi_task, "wifi_task", 4096, NULL, 5, NULL, 0);

    // ESP32-C3 is single core: everything runs on core 0 (IDF 5+ asserts on core 1).
    xTaskCreatePinnedToCore(ui_task, "ui_task", 8192, NULL, 0, NULL, 0);

    xTaskCreatePinnedToCore(led_task, "led_task", 2048, NULL, 10, NULL, 0);

    xTaskCreatePinnedToCore(button_task, "button_task", 4096, NULL, 1, NULL, 0);

    // Handle HTTP webserver start/stop
    static httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &disconnect_handler, &server));

    ESP_LOGI(__FILE__, "MAIN END: free_heap_size = %lu\n", esp_get_free_heap_size());
}

