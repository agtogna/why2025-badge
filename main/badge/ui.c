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
lv_task_t* snake_task_handle;
lv_task_t* radar_task_handle;
lv_task_t* rssi_task_handle;
lv_task_t* wifi_scan_task_handle;
lv_task_t* backlight_task_handle;

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
        lv_task_set_prio(rssi_task_handle, LV_TASK_PRIO_LOW);
    }
    else if(current_screen == SCREEN_RADAR){
        lv_task_set_prio(radar_task_handle, LV_TASK_PRIO_LOW);
    }
    else if(current_screen == SCREEN_WIFI){
        lv_task_set_prio(wifi_scan_task_handle, LV_TASK_PRIO_LOW);
        ui_wifi_scan(admin_state == ADMIN_STATE_OFF);
    }
}

void pause_current_task(){
    if(current_screen == SCREEN_RSSI){
        lv_task_set_prio(rssi_task_handle, LV_TASK_PRIO_OFF);
    }
    else if(current_screen == SCREEN_RADAR){
        lv_task_set_prio(radar_task_handle, LV_TASK_PRIO_OFF);
    }
    else if(current_screen == SCREEN_WIFI){
        lv_task_set_prio(wifi_scan_task_handle, LV_TASK_PRIO_OFF);
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
    xQueueSend(wifi_queue, &event, portMAX_DELAY);
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
    lv_obj_t *page = lv_obj_get_child(screen, NULL);
    lv_page_scroll_ver(page, 80);
}

void scroll_down(lv_obj_t *screen){
    lv_obj_t *page = lv_obj_get_child(screen, NULL);
    lv_page_scroll_ver(page, -80);
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
            lv_task_set_prio(snake_task_handle, LV_TASK_PRIO_LOW);
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
                    ui_manual_ip_update();
                    // Also test forcing labels to be visible for debugging
                    ui_force_show_ip_labels();
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
            lv_task_set_prio(snake_task_handle, LV_TASK_PRIO_LOW);
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
                case ADMIN_STATE_AP: // AP enabled: test showing IP labels
                    ESP_LOGI("UI", "Force show IP labels test (DOWN button in AP mode)");
                    ui_force_show_ip_labels();
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

static void ui_wifi_task(lv_task_t *arg)
{
    if (lv_scr_act() != screen_wifi)
    {
        lv_task_set_prio(wifi_scan_task_handle, LV_TASK_PRIO_OFF);
        return;
    }

    static wifi_ap_record_t aps[WIFI_SCAN_MAX];
    const uint8_t count = (admin_state == ADMIN_STATE_OFF) ? wifi_get_scan(aps, WIFI_SCAN_MAX) : 0;

    lv_table_set_row_cnt(table_wifi, (count ? count : 1) + 1);
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
        lv_table_set_cell_align(table_wifi, row, 1, LV_LABEL_ALIGN_CENTER);

        const int rssi = aps[i].rssi;
        const int bars = rssi > -55 ? 5 : rssi > -65 ? 4 : rssi > -75 ? 3 : rssi > -85 ? 2 : 1;
        snprintf(buf, sizeof(buf), "%d %.*s%.*s", rssi, bars, "#####", 5 - bars, ".....");
        lv_table_set_cell_value(table_wifi, row, 2, buf);
    }
}

static void ui_rssi_task(lv_task_t *arg)
{
    if (lv_scr_act() != screen_rssi)
    {
        lv_task_set_prio(rssi_task_handle, LV_TASK_PRIO_OFF);
        return;
    }
    
    const uint8_t count = count_ble_nodes();
    lv_table_set_row_cnt(table_rssi, count+1);

    char buf[BADGE_BUF_SIZE] = {0};

    uint8_t pos = 1;

    for (int i = 0; i < MAX_NEARBY_NODE; i++) {
        if(!ble_nodes[i].active) continue;
        
        lv_table_set_cell_value(table_rssi, pos, 0, ble_nodes[i].name);
        lv_table_set_cell_align(table_rssi, pos, 0, LV_LABEL_ALIGN_CENTER);

        snprintf(buf, sizeof(buf), "%d dBm", ble_nodes[i].rssi);
        lv_table_set_cell_value(table_rssi, pos, 1, buf);
        lv_table_set_cell_align(table_rssi, pos, 1, LV_LABEL_ALIGN_CENTER);

        snprintf(buf, sizeof(buf), "0x0%d", ble_nodes[i].id);
        lv_table_set_cell_value(table_rssi, pos, 2, buf);
        lv_table_set_cell_align(table_rssi, pos, 2, LV_LABEL_ALIGN_CENTER);

        pos++;
    }
}

static void ui_backlight_task(lv_task_t *arg){
    ui_update_backlight(false);
}

static void ui_radar_task(lv_task_t *arg)
{

    if (lv_scr_act() != screen_radar)
    {
        lv_task_set_prio(radar_task_handle, LV_TASK_PRIO_OFF);
        return;
    }

    if (lv_scr_act() == screen_radar)
    {
        static int mode;
        if (mode)
        {
            for (int i = 0; i < MAX_NEARBY_NODE; i++)
            {
                if(ble_nodes[i].active) {
                    lv_coord_t x = rand() % 80, y = rand() % 60;

                    if (ble_nodes[i].rssi > -70)
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
                    else if (ble_nodes[i].rssi > -90)
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
                    lv_label_set_text_fmt(radar_node_number[i], "%d", ble_nodes[i].id);
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
    LV_IMG_DECLARE(img_logo);

    screen_logo = lv_obj_create(NULL, NULL);
    lv_obj_t *logo = lv_img_create(screen_logo, NULL);
    lv_img_set_src(logo, &img_logo);
    lv_obj_align(logo, NULL, LV_ALIGN_CENTER, 0, 0);
  /*Change the logo's background color*/
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_bg_opa(&style, LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_style_set_bg_color(&style, LV_STATE_DEFAULT, LV_COLOR_MAKE(0xff, 0xff, 0xff));
    lv_obj_add_style(logo, LV_OBJ_PART_MAIN, &style);

    screens[SCREEN_LOGO] = screen_logo;
}

void ui_screen_radar_init(){
    // Page for radar
    LV_IMG_DECLARE(img_radar);
    
    screen_radar = lv_obj_create(NULL, NULL);
    lv_obj_t *img = lv_img_create(screen_radar, NULL);
    lv_img_set_src(img, &img_radar);
    lv_obj_align(img, NULL, LV_ALIGN_IN_TOP_LEFT, 0, 0);

    for (int i = 0; i < sizeof(radar_node) / sizeof(lv_obj_t *); i++)
    {
        radar_node[i] = lv_btn_create(img, NULL);
        lv_obj_set_size(radar_node[i], 20, 20);
        lv_btn_toggle(radar_node[i]); // set to solid color.
        lv_obj_set_hidden(radar_node[i], true);
        radar_node_number[i] = lv_label_create(radar_node[i], NULL);
        lv_label_set_text(radar_node_number[i], "X");
    }

    screens[SCREEN_RADAR] = screen_radar;
}

void ui_screen_wifi_init(){
    static lv_style_t style;
    lv_style_init(&style);

    screen_wifi = lv_obj_create(NULL, NULL);
    lv_obj_t *page = lv_page_create(screen_wifi, NULL);

    lv_obj_t *scrollable = lv_page_get_scrollable(page);
    lv_cont_set_layout(scrollable, LV_LAYOUT_PRETTY_TOP);
    lv_obj_set_style_local_pad_all(scrollable, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
    lv_obj_set_style_local_pad_inner(scrollable, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
    lv_obj_set_style_local_pad_all(page, LV_PAGE_PART_BG, LV_STATE_DEFAULT, 0);
    lv_obj_set_style_local_pad_inner(page, LV_PAGE_PART_BG, LV_STATE_DEFAULT, 0);
    lv_page_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES);
    lv_coord_t content_w = lv_obj_get_width_grid(page, 3, 1);

    lv_style_set_text_font(&style, LV_OBJ_PART_MAIN, &lv_font_montserrat_14);

    table_wifi = lv_table_create(page, NULL);
    lv_obj_clean_style_list(table_wifi, LV_TABLE_PART_BG);
    lv_obj_set_drag_parent(table_wifi, true);
    lv_table_set_col_cnt(table_wifi, 3);
    lv_table_set_col_width(table_wifi, 0, 14 * content_w / 10);
    lv_table_set_col_width(table_wifi, 1, 5 * content_w / 10);
    lv_table_set_col_width(table_wifi, 2, 11 * content_w / 10);
    lv_obj_add_style(table_wifi, LV_OBJ_PART_MAIN, &style);

    lv_obj_align(table_wifi, page, LV_ALIGN_OUT_TOP_LEFT, 0, 0);

    lv_table_set_cell_value(table_wifi, 0, 0, "SSID");
    lv_table_set_cell_value(table_wifi, 0, 1, "CH");
    lv_table_set_cell_value(table_wifi, 0, 2, "dBm");
    lv_table_set_cell_align(table_wifi, 0, 1, LV_LABEL_ALIGN_CENTER);

    screens[SCREEN_WIFI] = screen_wifi;
}

void ui_screen_rssi_init(){
    // page for rssi
    static lv_style_t style;
    lv_style_init(&style);

    screen_rssi = lv_obj_create(NULL, NULL);
    lv_obj_t *screen_rssi_page = lv_page_create(screen_rssi, NULL);

    lv_obj_t *scrollable = lv_page_get_scrollable(screen_rssi_page);
    lv_cont_set_layout(scrollable, LV_LAYOUT_PRETTY_TOP);
    lv_obj_set_style_local_pad_all(scrollable, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
	lv_obj_set_style_local_pad_inner(scrollable, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
    lv_obj_set_style_local_pad_all(screen_rssi_page, LV_PAGE_PART_BG, LV_STATE_DEFAULT, 0);
    lv_obj_set_style_local_pad_inner(screen_rssi_page, LV_PAGE_PART_BG, LV_STATE_DEFAULT, 0);
    lv_page_set_scrollbar_mode(screen_rssi_page, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_size(screen_rssi_page, LV_HOR_RES, LV_VER_RES);
    lv_coord_t content_w = lv_obj_get_width_grid(screen_rssi_page, 3, 1);

    lv_style_set_text_font(&style, LV_OBJ_PART_MAIN, &lv_font_montserrat_14);

    table_rssi = lv_table_create(screen_rssi_page, NULL);
    lv_obj_clean_style_list(table_rssi, LV_TABLE_PART_BG);
    lv_obj_set_drag_parent(table_rssi, true);
    lv_table_set_col_cnt(table_rssi, 3);
    lv_table_set_col_width(table_rssi, 0, content_w);
    lv_table_set_col_width(table_rssi, 1, content_w);
    lv_table_set_col_width(table_rssi, 2, content_w);
    lv_obj_add_style(table_rssi, LV_OBJ_PART_MAIN, &style);

    lv_obj_align(table_rssi, screen_rssi_page, LV_ALIGN_OUT_TOP_LEFT, 0, 0);

    lv_table_set_cell_value(table_rssi, 0, 0, "NAME");
    lv_table_set_cell_value(table_rssi, 0, 1, "RSSI");
    lv_table_set_cell_value(table_rssi, 0, 2, "ID");

    lv_table_set_cell_align(table_rssi, 0, 0, LV_LABEL_ALIGN_CENTER);
    lv_table_set_cell_align(table_rssi, 0, 1, LV_LABEL_ALIGN_CENTER);
    lv_table_set_cell_align(table_rssi, 0, 2, LV_LABEL_ALIGN_CENTER);

    screens[SCREEN_RSSI] = screen_rssi;
}

void ui_screen_admin_init(){
    // page for admin
    screen_admin = lv_obj_create(NULL, NULL);
    admin_switch = lv_btn_create(screen_admin, NULL);
    lv_obj_set_size(admin_switch, 200, 50);
    lv_obj_set_pos(admin_switch, 60, 35);
    admin_switch_text = lv_label_create(admin_switch, NULL);
    lv_label_set_text(admin_switch_text, "TURN ON AP");

    admin_ssid = lv_label_create(screen_admin, NULL);
    lv_obj_align(admin_ssid, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 10);
    lv_obj_set_hidden(admin_ssid, true);
    admin_pass = lv_label_create(screen_admin, NULL);
    lv_obj_align(admin_pass, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 30);
    lv_obj_set_hidden(admin_pass, true);

    admin_client_ip = lv_label_create(screen_admin, NULL);
    lv_obj_align(admin_client_ip, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 50);
    lv_label_set_text(admin_client_ip, "Client IP: [Not Connected]");
    lv_obj_set_hidden(admin_client_ip, true);
    admin_gateway_ip = lv_label_create(screen_admin, NULL);
    lv_obj_align(admin_gateway_ip, admin_switch, LV_ALIGN_OUT_BOTTOM_MID, -50, 70);
    lv_label_set_text(admin_gateway_ip, "Gateway: [Not Available]");
    lv_obj_set_hidden(admin_gateway_ip, true);

    admin_switch_sta = lv_btn_create(screen_admin, NULL);
    lv_obj_set_size(admin_switch_sta, 200, 50);
    lv_obj_set_pos(admin_switch_sta, 60, 180);
    admin_switch_sta_text = lv_label_create(admin_switch_sta, NULL);
    lv_obj_set_hidden(admin_switch_sta, !UI_STA_ENABLED);
    lv_label_set_text(admin_switch_sta_text, "WIFI STA");

    screens[SCREEN_ADMIN] = screen_admin;
}

void ui_screen_snake_init(){
    // page for snake
    screen_snake = lv_obj_create(NULL, NULL);
    snake_reset(screen_snake);

    screens[SCREEN_SNAKE] = screen_snake;
}

// Refresh the rainbow screen's label to the currently active animation's
// name, re-centering it (the text length changes between animation names).
static void ui_rainbow_update_label(void) {
    lv_label_set_text_fmt(rainbow_label, "%s\n\n(short press:\nnext animation)",
                          rainbow_current_animation_name());
    lv_obj_align(rainbow_label, NULL, LV_ALIGN_CENTER, 0, 0);
}

// Advance to the next LED animation (called on any short press while on the
// rainbow screen) and update the label to match.
static void ui_rainbow_next(void) {
    rainbow_next_animation();
    ui_rainbow_update_label();
}

void ui_screen_rainbow_init(){
    // page for the rainbow LED loop easter egg (black background)
    screen_rainbow = lv_obj_create(NULL, NULL);
    lv_obj_set_style_local_bg_color(screen_rainbow, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
    lv_obj_set_style_local_bg_opa(screen_rainbow, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_COVER);

    rainbow_label = lv_label_create(screen_rainbow, NULL);
    lv_obj_set_style_local_text_color(rainbow_label, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_WHITE);
    lv_label_set_align(rainbow_label, LV_LABEL_ALIGN_CENTER);
    ui_rainbow_update_label();

    screens[SCREEN_RAINBOW] = screen_rainbow;
}

void ui_ap_start_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
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

    lv_btn_set_state(admin_switch, LV_BTN_STATE_CHECKED_PRESSED);
    admin_state = ADMIN_STATE_AP;
}

void ui_ap_stop_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ap_started = false;

    lv_label_set_text(admin_switch_text, "TURN ON AP");
    lv_obj_set_hidden(admin_ssid, true);
    lv_obj_set_hidden(admin_pass, true);
    lv_obj_set_hidden(admin_client_ip, true);
    lv_obj_set_hidden(admin_gateway_ip, true);
    lv_obj_set_hidden(admin_switch_sta, !UI_STA_ENABLED);

    lv_btn_set_state(admin_switch, LV_BTN_STATE_RELEASED);//enabl(admin_switch);
    admin_state = ADMIN_STATE_OFF;
}

void ui_sta_connected_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    sta_connected = true;

    ESP_LOGI("UI", "STA connected handler called");
    ESP_LOGI("UI", "Current admin_state: %d", admin_state);
    ESP_LOGI("UI", "Current screen: %d", current_screen);
    
    lv_btn_set_state(admin_switch_sta, LV_BTN_STATE_CHECKED_PRESSED);
    lv_label_set_text(admin_switch_sta_text, "Connected (DOWN: stop)");
    
    // Update IP information when connected as station immediately
    ESP_LOGI("UI", "About to call ui_update_ip_info from STA connected handler");
    ui_update_ip_info();
    ESP_LOGI("UI", "ui_update_ip_info call completed from STA connected handler");
    
    // TODO: Also create a delayed task to retry getting IP info
    // xTaskCreate(ui_delayed_ip_update_task, "delayed_ip_update", 2048, NULL, 5, NULL);
    
    admin_state = ADMIN_STATE_STA;
}

void ui_sta_disconnected_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    sta_connected = false;
    lv_btn_set_state(admin_switch_sta, LV_BTN_STATE_RELEASED);
    lv_obj_set_hidden(admin_client_ip, true);
    lv_obj_set_hidden(admin_gateway_ip, true);
    admin_state = ADMIN_STATE_OFF;
}

void ui_sta_stop_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    sta_connected = false;
    lv_label_set_text(admin_switch_sta_text, "WIFI STA");
    lv_obj_set_hidden(admin_client_ip, true);
    lv_obj_set_hidden(admin_gateway_ip, true);
    admin_state = ADMIN_STATE_OFF;
}

void ui_connection_progress(uint8_t cur, uint8_t max){
    if(cur != max){
        char buf[BADGE_BUF_SIZE + 20] = {0}; // Increase the size of buf to accommodate the entire formatted string
        snprintf(buf, sizeof(buf), "Connecting (%d/%d)", cur, max);
        lv_label_set_text(admin_switch_sta_text, buf);
        lv_obj_set_hidden(admin_switch_sta_text, false);
    } else {
        lv_label_set_text(admin_switch_sta_text, "Connection failed!");
        lv_obj_set_hidden(admin_switch_sta_text, false);
    }
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
    ui_screen_splash_init();


    ui_screen_radar_init();
    
    ui_screen_rssi_init();
    ui_screen_wifi_init();

    ui_screen_admin_init();

    ui_screen_snake_init();

    ui_screen_rainbow_init();

    radar_task_handle = lv_task_create(ui_radar_task, 2000, LV_TASK_PRIO_OFF, NULL);
    rssi_task_handle = lv_task_create(ui_rssi_task, 2000, LV_TASK_PRIO_OFF, NULL);
    wifi_scan_task_handle = lv_task_create(ui_wifi_task, 1000, LV_TASK_PRIO_OFF, NULL);
    snake_task_handle = lv_task_create(snake_task, 50, LV_TASK_PRIO_OFF, NULL);

    // show first page.
    lv_scr_load(screens[current_screen]);

    // Turn on backlight and run backlight management task
    ui_update_backlight(true);
    backlight_task_handle = lv_task_create(ui_backlight_task, 1000, LV_TASK_PRIO_LOW, NULL);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, &ui_ap_start_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, &ui_ap_stop_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ui_sta_connected_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ui_sta_disconnected_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, &ui_sta_stop_handler, NULL));
}

static void ui_tick_task(void *arg)
{
    lv_tick_inc(1);
}

void ui_task(void *arg)
{
    SemaphoreHandle_t xGuiSemaphore;
    xGuiSemaphore = xSemaphoreCreateMutex();

    lv_init();
    lvgl_driver_init();

    lv_color_t *buf1 = (lv_color_t*) heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = (lv_color_t*) heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);

    static lv_disp_buf_t disp_buf;
    uint32_t size_in_px = DISP_BUF_SIZE;

    lv_disp_buf_init(&disp_buf, buf1, buf2, size_in_px);
    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = disp_driver_flush;
    disp_drv.buffer = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &ui_tick_task,
        .name = "ui_tick_task",
    };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 1000));

    ui_init();

    while (1)
    {
        /* Delay 1 tick (assumes FreeRTOS tick is 10ms */
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Try to take the semaphore, call lvgl related function on success */
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY))
        {
            lv_task_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
    }

    free(buf1);
    free(buf2);
    vTaskDelete(NULL);
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
    lv_scr_load_anim(screens[current_screen], LV_SCR_LOAD_ANIM_OVER_TOP, 300, 0, false);

    restore_current_task();
}

void ui_switch_page_up()
{
    ui_update_backlight(true);

    current_screen--;
    current_screen = (NUM_SCREENS + (current_screen % NUM_SCREENS)) % NUM_SCREENS;
    ESP_LOGI("DISPLAY", "DISPLAY COUNTER: %d/%d", current_screen+1, NUM_SCREENS);

    ui_prepare_current_screen();
    lv_scr_load_anim(screens[current_screen], LV_SCR_LOAD_ANIM_OVER_BOTTOM, 300, 0, false);

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

void ui_manual_ip_update() {
    ESP_LOGI("UI", "Manual IP update triggered from admin screen");
    ui_update_ip_info();
}

void ui_force_show_ip_labels() {
    ESP_LOGI("UI", "Force showing IP labels for testing - calling ui_update_ip_info instead");
    ui_update_ip_info();
}
