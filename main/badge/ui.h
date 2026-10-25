#ifndef _UI_H
#define _UI_H

#include <stdio.h>
#include <stdlib.h>

#include "lvgl.h"
#include "lvgl_helpers.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_netif.h"
#include "esp_netif_ip_addr.h"

#include "driver/gpio.h"
#include "iot_button.h"
#include "button_gpio.h"

#define BUTTON_1 0x08 // DOWN button
#define BUTTON_2 0x09 // UP button

#include "badge.h"
#include "snake.h"

// Brightness constants - now configured via default.json
// Legacy values: SCREEN_BRIGHT_MAX=96, SCREEN_BRIGHT_MID=32, SCREEN_BRIGHT_OFF=0
// New default values: brightness_max=255, brightness_mid=200, brightness_off=0
// #define SCREEN_BRIGHT_MAX 96
// #define SCREEN_BRIGHT_MID 32  
// #define SCREEN_BRIGHT_OFF 0

#define BRIGHT_MID_TIMEOUT_MS 5000
#define BRIGHT_OFF_TIMEOUT_MS 15000
#define USB_DIM_TIMEOUT_MS 30000 // dim delay while a USB host is connected (no off)

#define ADMIN_STATE_OFF 0
#define ADMIN_STATE_AP 1
#define ADMIN_STATE_STA 2

// STA (Wi-Fi client) mode is kept in the code but not reachable from the UI.
// Set to 1 to show the admin STA button and allow starting it with DOWN.
#define UI_STA_ENABLED 0

extern lv_obj_t *screen_logo;  // page 0
extern lv_obj_t *screen_radar; // page 1
extern lv_obj_t *screen_rssi;  // page 2
extern lv_obj_t *screen_wifi;  // page 3
extern lv_obj_t *screen_snake; // page 5
extern lv_obj_t *screen_admin; // page 4
extern lv_obj_t *screen_rainbow;  // page 6

extern lv_task_t* snake_task_handle;
extern lv_task_t* radar_task_handle;
extern lv_task_t* rssi_task_handle;
extern lv_task_t* wifi_scan_task_handle;
extern lv_task_t* backlight_task_handle;

void ui_task(void *);
void button_task(void *arg);

void ui_update_ip_info();
void ui_manual_ip_update();
void ui_force_show_ip_labels();
void ui_list_all_netifs();
void ui_connection_progress(uint8_t cur, uint8_t max);

// void ui_button_up();
// void ui_button_down();
// void ui_switch_page_up();
// void ui_switch_page_down();
#endif