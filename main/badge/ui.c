#include "ui.h"
#include "driver/usb_serial_jtag.h"
#include "led.h"

enum screen_order {SCREEN_LOGO, SCREEN_RADAR, SCREEN_RSSI, SCREEN_WIFI, SCREEN_ADMIN, SCREEN_SNAKE, SCREEN_RAINBOW, NUM_SCREENS};
static lv_obj_t* screens[NUM_SCREENS];
static int8_t current_screen = SCREEN_LOGO;

static uint32_t last_trigger = -1;

static lv_obj_t *radar_node[MAX_NEARBY_NODE] = {0};
static lv_obj_t *radar_node_number[MAX_NEARBY_NODE] = {0};
static lv_obj_t *table_rssi, *table_wifi;
static lv_obj_t *admin_switch, *admin_switch_sta;
static lv_obj_t *admin_switch_text, *admin_switch_sta_text;
static lv_obj_t *admin_ssid, *admin_pass;
static lv_obj_t *admin_client_ip, *admin_gateway_ip;
static lv_obj_t *rainbow_label;

static bool ap_started = false;
static bool sta_connected = false;

static uint8_t admin_state = ADMIN_STATE_OFF;

// Screen objects and task handles declared extern in ui.h
lv_obj_t *screen_logo;
lv_obj_t *screen_radar;
lv_obj_t *screen_rssi;
lv_obj_t *screen_wifi;
lv_obj_t *screen_snake;
lv_obj_t *screen_admin;
lv_obj_t *screen_rainbow;
lv_timer_t* snake_task_handle;
lv_timer_t* radar_task_handle;
lv_timer_t* rssi_task_handle;
lv_timer_t* wifi_scan_task_handle;
lv_timer_t* backlight_task_handle;

static uint8_t up_button_press_counter = 0;
static uint8_t down_button_press_counter = 0;
static int8_t counter_screen = -1; // Initialize to invalid screen index

// Forward declarations
void ui_update_ip_info(void);
void ui_list_all_netifs(void);
static void ui_rainbow_next(void);
static void ui_wifi_scan(bool on);

void restore_current_task(){
    if(current_screen == SCREEN_RSSI){
        lv_timer_resume(rssi_task_handle);
    }
    else if(current_screen == SCREEN_RADAR){
        lv_timer_resume(radar_task_handle);
    }
    else if(current_screen == SCREEN_WIFI){
        lv_timer_resume(wifi_scan_task_handle);
        ui_wifi_scan(admin_state == ADMIN_STATE_OFF);
    }
}

void pause_current_task(){
    if(current_screen == SCREEN_RSSI){
        lv_timer_pause(rssi_task_handle);
    }
    else if(current_screen == SCREEN_RADAR){
        lv_timer_pause(radar_task_handle);
    }
    else if(current_screen == SCREEN_WIFI){
        lv_timer_pause(wifi_scan_task_handle);
        ui_wifi_scan(false);
    }
}

/*
* Update the screen backlight status
* returns status of backlight (true if backlight off)
*/

static bool ui_update_backlight(bool trigger)
{
    uint32_t span = lv_tick_get() - last_trigger;

    // On USB power: dim after USB_DIM_TIMEOUT_MS, never switch off.
    // shortcut: "power" = USB host enumerated (usb_serial_jtag_is_connected); a plain
    // charger/power bank is not detected, add a VBUS sense pin if that is needed.
    static int8_t usb_dimmed = -1;   // -1: not on USB; the first USB pass always applies
    if (trigger || !usb_serial_jtag_is_connected())
        usb_dimmed = -1;
    else
    {
        bool dim = span > USB_DIM_TIMEOUT_MS;
        if (dim != usb_dimmed) {
            usb_dimmed = dim;
            set_screen_led_backlight(dim ? badge_obj.brightness_mid : badge_obj.brightness_max);
            restore_current_task();   // may have been paused if the screen was off before plugging in
        }
        return false;
    }

    if (trigger)
    {
        set_screen_led_backlight(badge_obj.brightness_max);
        last_trigger = lv_tick_get();

        restore_current_task();
    }
    else
    {
        if (span > BRIGHT_OFF_TIMEOUT_MS){
            set_screen_led_backlight(badge_obj.brightness_off);
            pause_current_task();
        }
        else if (span > BRIGHT_MID_TIMEOUT_MS){
            set_screen_led_backlight(badge_obj.brightness_mid);
        }
    }

    /* Avoid doing action when backlight off */
    if (span > BRIGHT_OFF_TIMEOUT_MS){
        return true;
    }
    return false;
}

void ui_send_wifi_event(int event)
{
    xQueueSend(wifi_queue, &event, 0); // called under the LVGL lock: drop the event rather than freeze the UI
}

// Ask wifi_task to start/stop scanning; only sends on a state change.
static void ui_wifi_scan(bool on)
{
    static bool requested = false;
    if (on == requested) return;
    requested = on;
    ui_send_wifi_event(on ? EVENT_SCAN_START : EVENT_SCAN_STOP);
}

void scroll_up(lv_obj_t *screen){
    lv_obj_t *page = lv_obj_get_child(screen, 0);
    lv_obj_scroll_by_bounded(page, 0, 80, LV_ANIM_ON);
}

void scroll_down(lv_obj_t *screen){
    lv_obj_t *page = lv_obj_get_child(screen, 0);
    lv_obj_scroll_by_bounded(page, 0, -80, LV_ANIM_ON);
}

void ui_button_up()
{
    // Check if this is the first button press or if we've changed screens
    if (counter_screen != current_screen) {
        // Reset counters when screen changes
        up_button_press_counter = 0;
        down_button_press_counter = 0;
        // Update counter_screen to current screen
        counter_screen = current_screen;
        //printf("DEBUG: Started counting UP presses on screen index: %d\n", current_screen);
    }
    
    // Increment counter for UP button presses
    up_button_press_counter++;
    ESP_LOGI("UI", "UP button press count: %d on screen index: %d", up_button_press_counter, current_screen);
    
    // Check if we've reached 7 presses
    if (up_button_press_counter == 7) {
        // printf("DEBUG: UP button pressed 7 times on screen %s (index: %d)\n", current_screen);
        
        // Call set_completed() function when on SCREEN_LOGO (index 0)
        if (current_screen == SCREEN_LOGO) {
            ESP_LOGI("UI", "Summoning sequence activated!");
            set_completed();
        }        
        
        // Reset the counter after reaching 7
        up_button_press_counter = 0;
    }

    // Check if the backlight update is needed
    if(ui_update_backlight(true)){
        return;
    }

    switch(current_screen){
        case SCREEN_SNAKE:
            lv_timer_resume(snake_task_handle);
            snake_set_dir(1);
            break;
        case SCREEN_RAINBOW:
            ui_rainbow_next();          // any short press -> next animation
            break;
        case SCREEN_ADMIN:
            switch(admin_state){
                case ADMIN_STATE_OFF: // AP and STA disabled: enable AP
                    ui_send_wifi_event(EVENT_HOTSPOT_START);
                    lv_obj_set_hidden(admin_switch_sta, true);
                    admin_state = ADMIN_STATE_AP;
                    break;
                case ADMIN_STATE_AP: // AP enabled: disable AP
                    ui_send_wifi_event(EVENT_HOTSPOT_STOP);
                    lv_obj_set_hidden(admin_switch_sta, !UI_STA_ENABLED);
                    lv_obj_set_hidden(admin_ssid, true);
                    admin_state = ADMIN_STATE_OFF;
                    break;
                case ADMIN_STATE_STA: // STA connected: manual IP refresh
                    ESP_LOGI("UI", "Manual IP refresh triggered via UP button");
                    ui_update_ip_info();
                    break;
            }   
            break;
        case SCREEN_RSSI:
        case SCREEN_WIFI:
            scroll_up(screens[current_screen]);
            break;
        default:
            ESP_LOGI(__FILE__, "Button up, no actions");
    }
}

void ui_button_down()
{
    // Check if this is the first button press or if we've changed screens
    if (counter_screen != current_screen) {
        // Reset counters when screen changes
        up_button_press_counter = 0;
        down_button_press_counter = 0;
        // Update counter_screen to current screen
        counter_screen = current_screen;
    }
    
    // Increment counter for DOWN button presses
    down_button_press_counter++;
    ESP_LOGI("UI", "DOWN button press count: %d on screen index: %d", down_button_press_counter, current_screen);
    
    // Check if we've reached 7 presses
    if (down_button_press_counter == 7) {
        // Call rainbow() function when on SCREEN_LOGO (index 0)
        if (current_screen == SCREEN_LOGO) {
            ESP_LOGI("UI", "Rainbow sequence activated!");
            rainbow();
        }        
        
        // Reset the counter after reaching 7
        down_button_press_counter = 0;
    }

    if(ui_update_backlight(true)){
        return;
    }

    switch(current_screen){
        case SCREEN_SNAKE:
            lv_timer_resume(snake_task_handle);
            snake_set_dir(-1);
            break;
        case SCREEN_RAINBOW:
            ui_rainbow_next();          // any short press -> next animation
            break;
        case SCREEN_ADMIN:
            switch(admin_state){
                case ADMIN_STATE_OFF: // AP and STA disabled: enable STA
#if UI_STA_ENABLED
                    ui_send_wifi_event(EVENT_STA_START);
                    lv_label_set_text(admin_switch_sta_text, "Started...");
                    admin_state = ADMIN_STATE_STA;
#endif
                    break;
                case ADMIN_STATE_AP: // AP enabled: refresh IP labels
                    ui_update_ip_info();
                    break;
                case ADMIN_STATE_STA: // STA enabled: disable STA
                    ui_send_wifi_event(EVENT_STA_STOP);
                    break;
            }
            break;
        case SCREEN_RSSI:
        case SCREEN_WIFI:
            scroll_down(screens[current_screen]);
            break;
        default:
            ESP_LOGI(__FILE__, "Button down, no actions");
    }
}

static void ui_wifi_task(lv_timer_t *arg)
{
    if (lv_screen_active() != screen_wifi)
    {
        lv_timer_pause(wifi_scan_task_handle);
        return;
    }

    static wifi_ap_record_t aps[WIFI_SCAN_MAX];
    const uint8_t count = (admin_state == ADMIN_STATE_OFF) ? wifi_get_scan(aps, WIFI_SCAN_MAX) : 0;

    lv_table_set_row_count(table_wifi, (count ? count : 1) + 1);
    if (count == 0) {
        lv_table_set_cell_value(table_wifi, 1, 0, admin_state == ADMIN_STATE_OFF ? "Scanning..." : "Turn off AP/STA");
        lv_table_set_cell_value(table_wifi, 1, 1, "");
        lv_table_set_cell_value(table_wifi, 1, 2, "");
        return;
    }

    char buf[40];
    for (int i = 0; i < count; i++) {
        const int row = i + 1;
        const char* ssid = (const char*)aps[i].ssid;
        snprintf(buf, sizeof(buf), "%.14s", ssid[0] ? ssid : "<hidden>");
        lv_table_set_cell_value(table_wifi, row, 0, buf);

        snprintf(buf, sizeof(buf), "%d", aps[i].primary);
        lv_table_set_cell_value(table_wifi, row, 1, buf);

        const int rssi = aps[i].rssi;
        const int bars = rssi > -55 ? 5 : rssi > -65 ? 4 : rssi > -75 ? 3 : rssi > -85 ? 2 : 1;
        snprintf(buf, sizeof(buf), "%d %.*s%.*s", rssi, bars, "#####", 5 - bars, ".....");
        lv_table_set_cell_value(table_wifi, row, 2, buf);
    }
}

static void ui_rssi_task(lv_timer_t *arg)
{
    if (lv_screen_active() != screen_rssi)
    {
        lv_timer_pause(rssi_task_handle);
        return;
    }
    
    ble_node_t nodes[MAX_NEARBY_NODE];
    ble_nodes_snapshot(nodes);
    uint8_t count = 0;
    for (int i = 0; i < MAX_NEARBY_NODE; i++) count += nodes[i].active;
    lv_table_set_row_count(table_rssi, count+1);

    char buf[BADGE_BUF_SIZE] = {0};

    uint8_t pos = 1;

    for (int i = 0; i < MAX_NEARBY_NODE; i++) {
        if(!nodes[i].active) continue;
        
        lv_table_set_cell_value(table_rssi, pos, 0, nodes[i].name);

        snprintf(buf, sizeof(buf), "%d dBm", nodes[i].rssi);
        lv_table_set_cell_value(table_rssi, pos, 1, buf);

        snprintf(buf, sizeof(buf), "0x0%d", nodes[i].id);
        lv_table_set_cell_value(table_rssi, pos, 2, buf);

        pos++;
    }
}

static void ui_backlight_task(lv_timer_t *arg){
    ui_update_backlight(false);
}

static void ui_radar_task(lv_timer_t *arg)
{

    if (lv_screen_active() != screen_radar)
    {
        lv_timer_pause(radar_task_handle);
        return;
    }

    if (lv_screen_active() == screen_radar)
    {
        static int mode;
        ble_node_t nodes[MAX_NEARBY_NODE];
        ble_nodes_snapshot(nodes);
        if (mode)
        {
            for (int i = 0; i < MAX_NEARBY_NODE; i++)
            {
                if(nodes[i].active) {
                    int32_t x = rand() % 80, y = rand() % 60;

                    if (nodes[i].rssi > -70)
                    {
                        // near range.
                        if (x < 50)
                            x = (LV_HOR_RES - x) / 2 - 20;
                        else
                            x = (LV_HOR_RES + x) / 2 - 20;
                        if (y < 40)
                            y = (LV_VER_RES - y) / 2 - 20;
                        else
                            y = (LV_VER_RES + y) / 2 - 20;
                    }
                    else if (nodes[i].rssi > -90)
                    {
                        // middle range.
                        if (x < 50)
                            x = (LV_HOR_RES - 100 - x) / 2;
                        else
                            x = (LV_HOR_RES + 100 + x) / 2 - 20;
                        if (y < 40)
                            y = (LV_VER_RES - 80 - y) / 2;
                        else
                            y = (LV_VER_RES + 80 - y) / 2 - 20;
                    }
                    else
                    {
                        // long range
                        if (x < 50)
                            x = (LV_HOR_RES - 200 - x) / 2;
                        else
                            x = (LV_HOR_RES + 200 + x) / 2 - 20;
                        if (y < 40)
                            y = (LV_VER_RES - 160 - y) / 2;
                        else
                            y = (LV_VER_RES + 160 + y) / 2 - 20;
                    }

                    lv_obj_set_pos(radar_node[i], x, y);
                    lv_label_set_text_fmt(radar_node_number[i], "%d", nodes[i].id);
                    lv_obj_set_hidden(radar_node[i], false);
                    lv_obj_fade_in(radar_node[i], 1000, 0);
                } else {
                    lv_obj_set_hidden(radar_node[i], true);
                }
            }
        }
        else
        {
            for (int i = 0; i < MAX_NEARBY_NODE; i++)
            {
                if (lv_obj_is_visible(radar_node[i]))
                    lv_obj_fade_out(radar_node[i], 1000, 0);
            }
        }

        mode = !mode;
    }
}

void ui_screen_splash_init(){
    LV_IMAGE_DECLARE(img_logo);

    screen_logo = lv_obj_create(NULL);
    lv_obj_t *logo = lv_image_create(screen_logo);
    lv_image_set_src(logo, &img_logo);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, 0);
  /*Change the logo's background color*/
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_bg_opa(&style, LV_OPA_COVER);
    lv_style_set_bg_color(&style, lv_color_white());
    lv_obj_add_style(logo, &style, 0);

    screens[SCREEN_LOGO] = screen_logo;
}

void ui_screen_radar_init(){
    // Page for radar
    LV_IMAGE_DECLARE(img_radar);
    
    screen_radar = lv_obj_create(NULL);
    lv_obj_t *img = lv_image_create(screen_radar);
    lv_image_set_src(img, &img_radar);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT, 0, 0);

    for (int i = 0; i < sizeof(radar_node) / sizeof(lv_obj_t *); i++)
    {
        radar_node[i] = lv_button_create(img);
        lv_obj_set_size(radar_node[i], 20, 20);
        lv_obj_add_state(radar_node[i], LV_STATE_CHECKED); // set to solid color.
        lv_obj_set_hidden(radar_node[i], true);
        radar_node_number[i] = lv_label_create(radar_node[i]);
        lv_label_set_text(radar_node_number[i], "X");
    }

    screens[SCREEN_RADAR] = screen_radar;
}

// v9 table has no per-cell align: center the CH column (col 1) at draw time
static void table_wifi_center_ch_cb(lv_event_t *e)
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_label_dsc_t *dsc = lv_draw_task_get_label_dsc(t);
    if (dsc && dsc->base.part == LV_PART_ITEMS && dsc->base.id2 == 1) dsc->align = LV_TEXT_ALIGN_CENTER;
}

void ui_screen_wifi_init(){
    static lv_style_t style;
    lv_style_init(&style);

    screen_wifi = lv_obj_create(NULL);
    lv_obj_t *page = lv_obj_create(screen_wifi);
    lv_obj_remove_style_all(page); // bare scroll container: no bg, border or padding
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES);
    int32_t content_w = LV_HOR_RES / 3;

    lv_style_set_text_font(&style, &lv_font_montserrat_14);

    table_wifi = lv_table_create(page);
    lv_obj_remove_style(table_wifi, NULL, LV_PART_MAIN);
    lv_table_set_column_count(table_wifi, 3);
    lv_table_set_column_width(table_wifi, 0, 14 * content_w / 10);
    lv_table_set_column_width(table_wifi, 1, 5 * content_w / 10);
    lv_table_set_column_width(table_wifi, 2, 11 * content_w / 10);
    lv_obj_add_style(table_wifi, &style, 0);
    lv_obj_set_send_draw_task_events(table_wifi, true);
    lv_obj_add_event_cb(table_wifi, table_wifi_center_ch_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);


    lv_table_set_cell_value(table_wifi, 0, 0, "SSID");
    lv_table_set_cell_value(table_wifi, 0, 1, "CH");
    lv_table_set_cell_value(table_wifi, 0, 2, "dBm");

    screens[SCREEN_WIFI] = screen_wifi;
}

void ui_screen_rssi_init(){
    // page for rssi
    static lv_style_t style;
    lv_style_init(&style);

    screen_rssi = lv_obj_create(NULL);
    lv_obj_t *screen_rssi_page = lv_obj_create(screen_rssi);
    lv_obj_remove_style_all(screen_rssi_page); // bare scroll container: no bg, border or padding
    lv_obj_set_scrollbar_mode(screen_rssi_page, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_size(screen_rssi_page, LV_HOR_RES, LV_VER_RES);
    int32_t content_w = LV_HOR_RES / 3;

    lv_style_set_text_font(&style, &lv_font_montserrat_14);

    table_rssi = lv_table_create(screen_rssi_page);
    lv_obj_remove_style(table_rssi, NULL, LV_PART_MAIN);
    lv_table_set_column_count(table_rssi, 3);
    lv_table_set_column_width(table_rssi, 0, content_w);
    lv_table_set_column_width(table_rssi, 1, content_w);
    lv_table_set_column_width(table_rssi, 2, content_w);
    lv_obj_add_style(table_rssi, &style, 0);
    lv_obj_set_style_text_align(table_rssi, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS);


    lv_table_set_cell_value(table_rssi, 0, 0, "NAME");
    lv_table_set_cell_value(table_rssi, 0, 1, "RSSI");
    lv_table_set_cell_value(table_rssi, 0, 2, "ID");


    screens[SCREEN_RSSI] = screen_rssi;
}

void ui_screen_admin_init(){
    // page for admin
    screen_admin = lv_obj_create(NULL);
    admin_switch = lv_button_create(screen_admin);
    lv_obj_set_size(admin_switch, 200, 50);
    lv_obj_set_pos(admin_switch, 60, 35);
    admin_switch_text = lv_label_create(admin_switch);
    lv_label_set_text(admin_switch_text, "TURN ON AP");

    admin_ssid = lv_label_create(screen_admin);
    lv_obj_align_to(admin_ssid, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 10);
    lv_obj_set_hidden(admin_ssid, true);
    admin_pass = lv_label_create(screen_admin);
    lv_obj_align_to(admin_pass, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 30);
    lv_obj_set_hidden(admin_pass, true);

    admin_client_ip = lv_label_create(screen_admin);
    lv_obj_align_to(admin_client_ip, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 50);
    lv_label_set_text(admin_client_ip, "Client IP: [Not Connected]");
    lv_obj_set_hidden(admin_client_ip, true);
    admin_gateway_ip = lv_label_create(screen_admin);
    lv_obj_align_to(admin_gateway_ip, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 70);
    lv_label_set_text(admin_gateway_ip, "Gateway: [Not Available]");
    lv_obj_set_hidden(admin_gateway_ip, true);

    admin_switch_sta = lv_button_create(screen_admin);
    lv_obj_set_size(admin_switch_sta, 200, 50);
    lv_obj_set_pos(admin_switch_sta, 60, 180);
    admin_switch_sta_text = lv_label_create(admin_switch_sta);
    lv_obj_set_hidden(admin_switch_sta, !UI_STA_ENABLED);
    lv_label_set_text(admin_switch_sta_text, "WIFI STA");

    screens[SCREEN_ADMIN] = screen_admin;
}

void ui_screen_snake_init(){
    // page for snake
    screen_snake = lv_obj_create(NULL);
    snake_reset(screen_snake);

    screens[SCREEN_SNAKE] = screen_snake;
}

// Refresh the rainbow screen's label to the currently active animation's
// name, re-centering it (the text length changes between animation names).
static void ui_rainbow_update_label(void) {
    lv_label_set_text_fmt(rainbow_label, "%s\n\n(short press:\nnext animation)",
                          rainbow_current_animation_name());
    lv_obj_align(rainbow_label, LV_ALIGN_CENTER, 0, 0);
}

// Advance to the next LED animation (called on any short press while on the
// rainbow screen) and update the label to match.
static void ui_rainbow_next(void) {
    rainbow_next_animation();
    ui_rainbow_update_label();
}

void ui_screen_rainbow_init(){
    // page for the rainbow LED loop easter egg (black background)
    screen_rainbow = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_rainbow, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_rainbow, LV_OPA_COVER, 0);

    rainbow_label = lv_label_create(screen_rainbow);
    lv_obj_set_style_text_color(rainbow_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(rainbow_label, LV_TEXT_ALIGN_CENTER, 0);
    ui_rainbow_update_label();

    screens[SCREEN_RAINBOW] = screen_rainbow;
}

void ui_ap_start_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    lvgl_port_lock(0);
    ap_started = true;

    ESP_LOGI("UI", "AP started handler called");
    lv_label_set_text(admin_switch_text, "TURN OFF AP");

    char buf[BADGE_BUF_SIZE + 19] = {0};
    snprintf(buf, sizeof(buf), "SSID: %s", badge_obj.ap_ssid);
    lv_label_set_text(admin_ssid, buf);
    snprintf(buf, sizeof(buf), "PASS: %s", badge_obj.ap_password);
    lv_label_set_text(admin_pass, buf);

    lv_obj_set_hidden(admin_ssid, false);
    lv_obj_set_hidden(admin_pass, false);

    // Update IP information immediately
    ui_update_ip_info();
    
    // TODO: Also create a delayed task to retry getting IP info
    // xTaskCreate(ui_delayed_ip_update_task, "delayed_ip_update", 2048, NULL, 5, NULL);

    lv_obj_add_state(admin_switch, LV_STATE_CHECKED | LV_STATE_PRESSED);
    admin_state = ADMIN_STATE_AP;
    lvgl_port_unlock();
}

void ui_ap_stop_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    lvgl_port_lock(0);
    ap_started = false;

    lv_label_set_text(admin_switch_text, "TURN ON AP");
    lv_obj_set_hidden(admin_ssid, true);
    lv_obj_set_hidden(admin_pass, true);
    lv_obj_set_hidden(admin_client_ip, true);
    lv_obj_set_hidden(admin_gateway_ip, true);
    lv_obj_set_hidden(admin_switch_sta, !UI_STA_ENABLED);

    lv_obj_remove_state(admin_switch, LV_STATE_CHECKED | LV_STATE_PRESSED);//enabl(admin_switch);
    admin_state = ADMIN_STATE_OFF;
    lvgl_port_unlock();
}

void ui_sta_connected_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    lvgl_port_lock(0);
    sta_connected = true;

    ESP_LOGI("UI", "STA connected handler called");
    ESP_LOGI("UI", "Current admin_state: %d", admin_state);
    ESP_LOGI("UI", "Current screen: %d", current_screen);
    
    lv_obj_add_state(admin_switch_sta, LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_label_set_text(admin_switch_sta_text, "Connected (DOWN: stop)");
    
    // Update IP information when connected as station immediately
    ESP_LOGI("UI", "About to call ui_update_ip_info from STA connected handler");
    ui_update_ip_info();
    ESP_LOGI("UI", "ui_update_ip_info call completed from STA connected handler");
    
    // TODO: Also create a delayed task to retry getting IP info
    // xTaskCreate(ui_delayed_ip_update_task, "delayed_ip_update", 2048, NULL, 5, NULL);
    
    admin_state = ADMIN_STATE_STA;
    lvgl_port_unlock();
}

void ui_sta_disconnected_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    lvgl_port_lock(0);
    sta_connected = false;
    lv_obj_remove_state(admin_switch_sta, LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_hidden(admin_client_ip, true);
    lv_obj_set_hidden(admin_gateway_ip, true);
    admin_state = ADMIN_STATE_OFF;
    lvgl_port_unlock();
}

void ui_sta_stop_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    lvgl_port_lock(0);
    sta_connected = false;
    lv_label_set_text(admin_switch_sta_text, "WIFI STA");
    lv_obj_set_hidden(admin_client_ip, true);
    lv_obj_set_hidden(admin_gateway_ip, true);
    admin_state = ADMIN_STATE_OFF;
    lvgl_port_unlock();
}

void ui_connection_progress(uint8_t cur, uint8_t max){
    lvgl_port_lock(0); // called from the wifi event handler
    if(cur != max){
        char buf[BADGE_BUF_SIZE + 20] = {0}; // Increase the size of buf to accommodate the entire formatted string
        snprintf(buf, sizeof(buf), "Connecting (%d/%d)", cur, max);
        lv_label_set_text(admin_switch_sta_text, buf);
        lv_obj_set_hidden(admin_switch_sta_text, false);
    } else {
        lv_label_set_text(admin_switch_sta_text, "Connection failed!");
        lv_obj_set_hidden(admin_switch_sta_text, false);
    }
    lvgl_port_unlock();
}

void ui_update_ip_info() {
    char buf[BADGE_BUF_SIZE + 30] = {0};
    
    ESP_LOGI("UI", "=== IP INFO DEBUG ===");
    ESP_LOGI("UI", "sta_connected: %s, ap_started: %s, admin_state: %d", 
             sta_connected ? "true" : "false", 
             ap_started ? "true" : "false", 
             admin_state);
    
    // First, list all network interfaces for debugging
    ui_list_all_netifs();
    
    // Get AP interface and show AP IP when AP is started
    if (ap_started) {
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        ESP_LOGI("UI", "AP netif handle (WIFI_AP_DEF): %p", ap_netif);
        
        if (ap_netif) {
            esp_netif_ip_info_t ip_info;
            esp_err_t ret = esp_netif_get_ip_info(ap_netif, &ip_info);
            ESP_LOGI("UI", "AP esp_netif_get_ip_info returned: %s", esp_err_to_name(ret));
            ESP_LOGI("UI", "AP IP: " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI("UI", "AP Gateway: " IPSTR, IP2STR(&ip_info.gw));
            ESP_LOGI("UI", "AP Netmask: " IPSTR, IP2STR(&ip_info.netmask));
            
            if (ret == ESP_OK && ip_info.ip.addr != 0) {
                // Show AP IP information
                snprintf(buf, sizeof(buf), "AP IP: " IPSTR, IP2STR(&ip_info.ip));
                lv_label_set_text(admin_client_ip, buf);
                lv_obj_set_hidden(admin_client_ip, false);
                ESP_LOGI("UI", "Set admin_client_ip text to: %s", buf);
                
                snprintf(buf, sizeof(buf), "\nConnect to\nhttp://" IPSTR, IP2STR(&ip_info.gw));
                lv_label_set_text(admin_gateway_ip, buf);
                lv_obj_set_hidden(admin_gateway_ip, false);
                ESP_LOGI("UI", "Set admin_gateway_ip text to: %s", buf);
                ESP_LOGI("UI", "Successfully displayed AP IP info");
                return;
            }
        }
    }
    
    // Get STA interface and show STA IP when connected as station
    if (sta_connected) {
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        ESP_LOGI("UI", "STA netif handle (WIFI_STA_DEF): %p", sta_netif);
        
        if (sta_netif) {
            esp_netif_ip_info_t ip_info;
            esp_err_t ret = esp_netif_get_ip_info(sta_netif, &ip_info);
            ESP_LOGI("UI", "STA esp_netif_get_ip_info returned: %s", esp_err_to_name(ret));
            ESP_LOGI("UI", "STA IP: " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI("UI", "STA Gateway: " IPSTR, IP2STR(&ip_info.gw));
            ESP_LOGI("UI", "STA Netmask: " IPSTR, IP2STR(&ip_info.netmask));
            
            if (ret == ESP_OK && ip_info.ip.addr != 0) {
                ESP_LOGI("UI", "STA IP is valid, updating UI labels...");
                snprintf(buf, sizeof(buf), "Client IP: " IPSTR, IP2STR(&ip_info.ip));
                lv_label_set_text(admin_client_ip, buf);
                lv_obj_set_hidden(admin_client_ip, false);
                ESP_LOGI("UI", "Set admin_client_ip text to: %s", buf);
                
                snprintf(buf, sizeof(buf), "Gateway: " IPSTR, IP2STR(&ip_info.gw));
                lv_label_set_text(admin_gateway_ip, buf);
                lv_obj_set_hidden(admin_gateway_ip, false);
                ESP_LOGI("UI", "Set admin_gateway_ip text to: %s", buf);
                ESP_LOGI("UI", "Successfully displayed STA IP info");
                return;
            } else {
                ESP_LOGW("UI", "STA IP is not valid or error occurred. ret=%s, ip.addr=0x%08x", 
                         esp_err_to_name(ret), ip_info.ip.addr);
            }
        } else {
            ESP_LOGW("UI", "Could not get STA netif handle");
        }
    }
    
    // If we reach here, we couldn't get IP info through normal methods
    // Try iterating through all interfaces as fallback
    ESP_LOGI("UI", "Primary methods failed, trying to iterate through all interfaces...");
    
    esp_netif_t *netif = NULL;
    esp_netif_t *temp_netif = esp_netif_next_unsafe(netif);
    bool ip_found = false;
    
    while (temp_netif != NULL && !ip_found) {
        esp_netif_ip_info_t ip_info;
        esp_err_t ret = esp_netif_get_ip_info(temp_netif, &ip_info);
        
        if (ret == ESP_OK && ip_info.ip.addr != 0) {
            const char* desc = esp_netif_get_desc(temp_netif);
            ESP_LOGI("UI", "Found valid IP on interface %s: " IPSTR, 
                     desc ? desc : "unknown", IP2STR(&ip_info.ip));
            
            // Use interface description to determine type instead of IP range heuristic
            if (desc && strstr(desc, "ap")) {
                // AP interface
                snprintf(buf, sizeof(buf), "AP IP: " IPSTR, IP2STR(&ip_info.ip));
                lv_label_set_text(admin_client_ip, buf);
                lv_obj_set_hidden(admin_client_ip, false);
                
                snprintf(buf, sizeof(buf), "\nConnect to\nhttp://" IPSTR, IP2STR(&ip_info.gw));
                lv_label_set_text(admin_gateway_ip, buf);
                lv_obj_set_hidden(admin_gateway_ip, false);
                ip_found = true;
            } else if (desc && strstr(desc, "sta")) {
                // STA interface
                snprintf(buf, sizeof(buf), "Client IP: " IPSTR, IP2STR(&ip_info.ip));
                lv_label_set_text(admin_client_ip, buf);
                lv_obj_set_hidden(admin_client_ip, false);
                
                snprintf(buf, sizeof(buf), "\nConnect to\nhttp://" IPSTR, IP2STR(&ip_info.gw));
                lv_label_set_text(admin_gateway_ip, buf);
                lv_obj_set_hidden(admin_gateway_ip, false);
                ip_found = true;
            }
            ESP_LOGI("UI", "Successfully displayed IP info from interface iteration");
        }
        
        temp_netif = esp_netif_next_unsafe(temp_netif);
    }
    
    if (!ip_found) {
        ESP_LOGW("UI", "No valid IP information found to display");
        lv_obj_set_hidden(admin_client_ip, true);
        lv_obj_set_hidden(admin_gateway_ip, true);
    }
    
    ESP_LOGI("UI", "=== END IP INFO DEBUG ===");
}


static void ui_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_display_set_theme(disp, lv_theme_default_init(disp, lv_color_hex(0xe80b60), lv_color_hex(0x0000ff), false, &lv_font_montserrat_14));

    ui_screen_splash_init();


    ui_screen_radar_init();
    
    ui_screen_rssi_init();
    ui_screen_wifi_init();

    ui_screen_admin_init();

    ui_screen_snake_init();

    ui_screen_rainbow_init();

    radar_task_handle = lv_timer_create(ui_radar_task, 2000, NULL);
    rssi_task_handle = lv_timer_create(ui_rssi_task, 2000, NULL);
    wifi_scan_task_handle = lv_timer_create(ui_wifi_task, 1000, NULL);
    snake_task_handle = lv_timer_create(snake_task, 50, NULL);
    lv_timer_pause(radar_task_handle);
    lv_timer_pause(rssi_task_handle);
    lv_timer_pause(wifi_scan_task_handle);
    lv_timer_pause(snake_task_handle);

    // show first page.
    lv_screen_load(screens[current_screen]);

    // Turn on backlight and run backlight management task
    ui_update_backlight(true);
    backlight_task_handle = lv_timer_create(ui_backlight_task, 1000, NULL);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, &ui_ap_start_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, &ui_ap_stop_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ui_sta_connected_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ui_sta_disconnected_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, &ui_sta_stop_handler, NULL));
}

void ui_task(void *arg)
{
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = DISP_PIN_MOSI,
        .miso_io_num = -1, // write-only panel
        .sclk_io_num = DISP_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISP_BUF_PX * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = DISP_PIN_CS,
        .dc_gpio_num = DISP_PIN_DC,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISP_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // esp_lcd only sends SLPOUT/MADCTL/COLMOD/RAMCTRL; this glass also needs the
    // power, VCOM, gamma and gate setup the old lvgl_esp32_drivers st7789.c sent.
    static const struct { uint8_t cmd, len, data[14]; } panel_init[] = {
        {0xCF, 3, {0x00, 0x83, 0x30}},
        {0xED, 4, {0x64, 0x03, 0x12, 0x81}},
        {0xE8, 3, {0x85, 0x01, 0x79}},
        {0xCB, 5, {0x39, 0x2C, 0x00, 0x34, 0x02}},
        {0xF7, 1, {0x20}},
        {0xEA, 2, {0x00, 0x00}},
        {0xC0, 1, {0x26}},
        {0xC1, 1, {0x11}},
        {0xC5, 2, {0x35, 0x3E}},
        {0xC7, 1, {0xBE}},
        {0xB1, 2, {0x00, 0x1B}},
        {0xF2, 1, {0x08}},
        {0x26, 1, {0x01}},
        {0xE0, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17}},
        {0xE1, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E}},
        {0xB7, 1, {0x07}},
        {0xB6, 4, {0x0A, 0x82, 0x27, 0x00}},
    };
    for (size_t i = 0; i < sizeof(panel_init) / sizeof(panel_init[0]); i++)
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, panel_init[i].cmd, panel_init[i].data, panel_init[i].len));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false)); // INVOFF, as the old driver
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = DISP_BUF_PX,
        .double_buffer = true,
        .hres = DISP_HRES,
        .vres = DISP_VRES,
        .rotation = { .mirror_x = true, .mirror_y = true }, // port sets MADCTL 0xC0, as the old driver
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lvgl_port_add_disp(&disp_cfg);

    lvgl_port_lock(0);
    ui_init();
    lvgl_port_unlock();

    vTaskDelete(NULL); // lvgl_port runs the LVGL task from here on
}

// Start or stop the rainbow LED loop, which runs on led_task (not LVGL), so entering or
// leaving that screen is just a flag.
static void ui_prepare_current_screen(void)
{
    ui_wifi_scan(current_screen == SCREEN_WIFI && admin_state == ADMIN_STATE_OFF);
    set_rainbow_loop_active(current_screen == SCREEN_RAINBOW);
    if (current_screen == SCREEN_RAINBOW) ui_rainbow_update_label(); // resets to animation 1
}

void ui_switch_page_down()
{
    ui_update_backlight(true);

    current_screen++;
    current_screen %= NUM_SCREENS;
    ESP_LOGI("DISPLAY", "DISPLAY COUNTER: %d/%d", current_screen+1, NUM_SCREENS);

    ui_prepare_current_screen();
    lv_screen_load_anim(screens[current_screen], LV_SCREEN_LOAD_ANIM_OVER_TOP, 300, 0, false);

    restore_current_task();
}

void ui_switch_page_up()
{
    ui_update_backlight(true);

    current_screen--;
    current_screen = (NUM_SCREENS + (current_screen % NUM_SCREENS)) % NUM_SCREENS;
    ESP_LOGI("DISPLAY", "DISPLAY COUNTER: %d/%d", current_screen+1, NUM_SCREENS);

    ui_prepare_current_screen();
    lv_screen_load_anim(screens[current_screen], LV_SCREEN_LOAD_ANIM_OVER_BOTTOM, 300, 0, false);

    restore_current_task();
}

typedef enum { BTN_ACT_CLICK, BTN_ACT_HELD, BTN_ACT_LONG_RELEASE } btn_action_t;
typedef struct {
    uint8_t pin;
    uint8_t action;
} btn_msg_t;

static QueueHandle_t button_events;
static bool button_long[2]; // only touched from the button driver's timer callbacks

static void button_send(uint8_t pin, uint8_t action)
{
    btn_msg_t msg = { .pin = pin, .action = action };
    xQueueSend(button_events, &msg, 0); // callbacks run in esp_timer context: never block
}

static void button_long_start_cb(void *handle, void *usr_data)
{
    uint8_t pin = (uintptr_t)usr_data;
    button_long[pin - BUTTON_1] = true;
    button_send(pin, BTN_ACT_HELD);
}

static void button_press_up_cb(void *handle, void *usr_data)
{
    uint8_t pin = (uintptr_t)usr_data;
    bool was_long = button_long[pin - BUTTON_1];
    button_long[pin - BUTTON_1] = false;
    button_send(pin, was_long ? BTN_ACT_LONG_RELEASE : BTN_ACT_CLICK);
}

static void button_create(uint8_t pin)
{
    const button_config_t btn_cfg = { .long_press_time = 1000 };
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = pin,
        .active_level = 0,
        .disable_pull = true, // buttons have external pull-ups
    };
    button_handle_t btn;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn));
    iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, button_long_start_cb, (void *)(uintptr_t)pin);
    iot_button_register_cb(btn, BUTTON_PRESS_UP, NULL, button_press_up_cb, (void *)(uintptr_t)pin);
}

void button_task(void *arg)
{
    btn_msg_t msg;
    button_events = xQueueCreate(4, sizeof(btn_msg_t));
    button_create(BUTTON_1);
    button_create(BUTTON_2);

    while (true)
    {
        if (xQueueReceive(button_events, &msg, portMAX_DELAY))
        {
            lvgl_port_lock(0);
            if (msg.action == BTN_ACT_HELD)
            {
                set_screen_led_backlight(badge_obj.brightness_mid);
            }
            else if (msg.pin == BUTTON_1) // DOWN button
            {
                if (msg.action == BTN_ACT_LONG_RELEASE) ui_switch_page_down();
                else ui_button_down();
            }
            else if (msg.pin == BUTTON_2) // UP button
            {
                if (msg.action == BTN_ACT_LONG_RELEASE) ui_switch_page_up();
                else ui_button_up();
            }
            lvgl_port_unlock();
        }
    }
}

void ui_list_all_netifs() {
    ESP_LOGI("UI", "=== LISTING ALL NETWORK INTERFACES ===");
    
    // Try to iterate through all available network interfaces
    esp_netif_t *netif = NULL;
    esp_netif_t *temp_netif = esp_netif_next_unsafe(netif);
    int count = 0;
    
    while (temp_netif != NULL) {
        count++;
        ESP_LOGI("UI", "Found netif %d: %p", count, temp_netif);
        
        // Get interface description
        const char* desc = esp_netif_get_desc(temp_netif);
        ESP_LOGI("UI", "Interface %d description: %s", count, desc ? desc : "unknown");
        
        // Get IP info for this interface
        esp_netif_ip_info_t ip_info;
        esp_err_t ret = esp_netif_get_ip_info(temp_netif, &ip_info);
        ESP_LOGI("UI", "Interface %d IP info (ret: %s):", count, esp_err_to_name(ret));
        ESP_LOGI("UI", "  IP: " IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGI("UI", "  Gateway: " IPSTR, IP2STR(&ip_info.gw));
        ESP_LOGI("UI", "  Netmask: " IPSTR, IP2STR(&ip_info.netmask));
        
        temp_netif = esp_netif_next_unsafe(temp_netif);
    }
    
    ESP_LOGI("UI", "Total network interfaces found: %d", count);
    ESP_LOGI("UI", "=== END NETIF LISTING ===");
}
