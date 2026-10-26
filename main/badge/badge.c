#include <stdio.h>
#include <string.h>

#include "badge.h"

ble_node_t ble_nodes[MAX_NEARBY_NODE];
badge_obj_t badge_obj;

static SemaphoreHandle_t ble_nodes_mutex;

void ble_nodes_lock(void) {
    if (ble_nodes_mutex) xSemaphoreTake(ble_nodes_mutex, portMAX_DELAY);
}

void ble_nodes_unlock(void) {
    if (ble_nodes_mutex) xSemaphoreGive(ble_nodes_mutex);
}

void ble_nodes_snapshot(ble_node_t *out) {
    ble_nodes_lock();
    memcpy(out, ble_nodes, sizeof(ble_nodes));
    ble_nodes_unlock();
}

uint8_t count_ble_nodes(){
    ble_node_t nodes[MAX_NEARBY_NODE];
    ble_nodes_snapshot(nodes);

    uint8_t cnt = 0;
    for(int i=0; i<MAX_NEARBY_NODE; i++)
    {
        if(nodes[i].active) cnt++;
    }
    return cnt;
}

bool check_ble_set()
{   
    ble_node_t nodes[MAX_NEARBY_NODE];
    ble_nodes_snapshot(nodes);

    uint8_t set_bits = 0;
    set_bits |= 1 << (badge_obj.device_id-1);

    for(int i=0; i<MAX_NEARBY_NODE; i++)
    {
        if(nodes[i].active){
            set_bits |= 1 << (nodes[i].id-1);
        }
    }
    return set_bits == 0x7F;
}

char* load_file_content(char* filename){
    struct stat file_stat;
    if (stat(filename, &file_stat) == -1) {
        ESP_LOGI(__FILE__, "File not found");
        return NULL;
    }

    char *content_buf = (char*) calloc(1, file_stat.st_size + 1);
    if(!content_buf) {
        ESP_LOGE(__FILE__, "Cannot allocate memory");
        return NULL;
    }
    FILE *fp = fopen(filename, "r");
    if(!fp) {
        ESP_LOGE(__FILE__, "Cannot open file");
        free(content_buf);
        return NULL;
    }
    fread(content_buf, 1, file_stat.st_size, fp);
    fclose(fp);

    return content_buf;
}

// NULL when the file is missing, unreadable or not valid JSON
static cJSON* load_json(char* filename) {
    char* content = load_file_content(filename);
    cJSON* json = cJSON_Parse(content);
    free(content);
    return json;
}

cJSON* load_default() {
    return load_json(DEFAULT_FILE);
}

cJSON* load_settings() {
    return load_json(SETTINGS_FILE);
}

const char* json_get_str_value(cJSON* obj, const char *key)
{
    cJSON* item = cJSON_GetObjectItem(obj, key);
    const char* value = cJSON_IsString(item) ? item->valuestring : "";
    ESP_LOGI(__FILE__, "Get %s value: %s", key, value);
    return value;
}

int json_get_int_value(cJSON* obj, const char *key)
{
    cJSON* item = cJSON_GetObjectItem(obj, key);
    int value = cJSON_IsNumber(item) ? item->valueint : 0;
    ESP_LOGI(__FILE__, "Get %s value: %d", key, value);
    return value;
}

void json_set_str_value(cJSON* obj, const char *key, const char *value)
{
    if(cJSON_GetObjectItem(obj, key)){
        cJSON_DeleteItemFromObject(obj, key);
    }
    
    cJSON_AddStringToObject(obj, key, value);
    ESP_LOGI(__FILE__, "Set %s value: %s", key, value);
}

void json_set_int_value(cJSON* obj, const char *key, const double value)
{
    if(cJSON_GetObjectItem(obj, key)){
        cJSON_DeleteItemFromObject(obj, key);
    }

    cJSON_AddNumberToObject(obj, key, value);
    ESP_LOGI(__FILE__, "Set %s value: %f", key, value);
}

void save_settings(cJSON* json_settings) {
    // Write a temp file first: a power cut while writing must not leave a truncated settings.json
    char *json = cJSON_Print(json_settings);
    FILE *fp = fopen(SETTINGS_TMP_FILE, "w");
    if (!fp || !json) {
        ESP_LOGE(__FILE__, "Cannot write settings");
        if (fp) fclose(fp);
        cJSON_free(json);
        return;
    }
    int written = fprintf(fp, "%s", json);
    cJSON_free(json);
    if (fclose(fp) != 0 || written < 0) {
        ESP_LOGE(__FILE__, "Settings write failed");
        unlink(SETTINGS_TMP_FILE);
        return;
    }

    unlink(SETTINGS_FILE); // SPIFFS rename does not replace an existing file
    if (rename(SETTINGS_TMP_FILE, SETTINGS_FILE) != 0) {
        ESP_LOGE(__FILE__, "Cannot rename settings file");
        return;
    }

    ESP_LOGI(__FILE__, "Setting file saved");
}

static bool has_str(cJSON* obj, const char *key) {
    return cJSON_IsString(cJSON_GetObjectItem(obj, key));
}

static bool has_num(cJSON* obj, const char *key) {
    return cJSON_IsNumber(cJSON_GetObjectItem(obj, key));
}

// Every key badge_init() reads must be there, or it would dereference NULL on each boot
static bool settings_valid(cJSON* json) {
    cJSON *badge = cJSON_GetObjectItem(json, "badge");
    cJSON *web = cJSON_GetObjectItem(json, "web");
    cJSON *ap = cJSON_GetObjectItem(json, "ap");
    cJSON *sta = cJSON_GetObjectItem(json, "sta");
    cJSON *display = cJSON_GetObjectItem(json, "display");

    if (!cJSON_IsObject(badge) || !cJSON_IsObject(web) || !cJSON_IsObject(ap) || !cJSON_IsObject(sta)) return false;
    if (!has_str(badge, "name") || !has_num(badge, "id") || !has_str(web, "login")) return false;
    if (!has_str(ap, "ssid") || !has_str(ap, "password") || !has_str(sta, "ssid") || !has_str(sta, "password")) return false;
    // display is optional, but a half-filled one is not
    return !display || (has_num(display, "brightness_max") && has_num(display, "brightness_mid") && has_num(display, "brightness_off"));
}

bool update_attribute(int id, char* data) { 
    switch(id){
        case 0: // Web login password
            snprintf(badge_obj.web_login, SIZEOF(badge_obj.web_login), "%s", data);
            break;
        case 1: // WiFi SSID
            snprintf(badge_obj.ap_ssid, SIZEOF(badge_obj.ap_ssid), "%s", data);
            break;
        case 2: // WiFi Password
            snprintf(badge_obj.ap_password, SIZEOF(badge_obj.ap_password), "%s", data);
            break;
        case 3: // Device name
            snprintf(badge_obj.device_name, SIZEOF(badge_obj.device_name), "%s", data);
            break;
    }

    cJSON* json_settings = load_settings();
    cJSON *obj_badge = cJSON_GetObjectItem(json_settings, "badge");
    cJSON *obj_web = cJSON_GetObjectItem(json_settings, "web");
    cJSON *obj_ap = cJSON_GetObjectItem(json_settings, "ap");

    json_set_str_value(obj_badge, "name", badge_obj.device_name);
    json_set_str_value(obj_ap, "ssid", badge_obj.ap_ssid);
    json_set_str_value(obj_ap, "password", badge_obj.ap_password);
    json_set_str_value(obj_web, "login", badge_obj.web_login);

    save_settings(json_settings);
    cJSON_Delete(json_settings);
    return true;
}

uint8_t generate_id(uint16_t seed) {
    //srand(seed);
    return 1+(esp_random()%7);
}

void badge_init(){
    ble_nodes_mutex = xSemaphoreCreateMutex();

    // Init storage
    nvs_init();
    spiffs_init();

    // Init event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Init settings
    cJSON* json_settings = load_settings();

    if (!settings_valid(json_settings)) {
        ESP_LOGW(__FILE__, "Settings missing or corrupt, restoring defaults");
        cJSON_Delete(json_settings);
        unlink(SETTINGS_FILE);
        json_settings = load_default();

        cJSON *obj_badge = cJSON_GetObjectItem(json_settings, "badge");
        cJSON *obj_web = cJSON_GetObjectItem(json_settings, "web");
        cJSON *obj_ap = cJSON_GetObjectItem(json_settings, "ap");
        cJSON *obj_sta = cJSON_GetObjectItem(json_settings, "sta");
        cJSON *obj_display = cJSON_GetObjectItem(json_settings, "display");

        const char* web_login = json_get_str_value(obj_web, "login");
        const char* sta_ssid = json_get_str_value(obj_sta, "ssid");
        const char* sta_password = json_get_str_value(obj_sta, "password");

        // Load brightness settings with defaults if not present
        int brightness_max = obj_display ? json_get_int_value(obj_display, "brightness_max") : 255;
        int brightness_mid = obj_display ? json_get_int_value(obj_display, "brightness_mid") : 200;
        int brightness_off = obj_display ? json_get_int_value(obj_display, "brightness_off") : 0;

        // Update name and AP settings
        esp_efuse_mac_get_default(badge_obj.mac);
        badge_obj.short_mac = (badge_obj.mac[4] << 8) + badge_obj.mac[5];
        badge_obj.device_id = generate_id(badge_obj.short_mac);//1 + (badge_obj.short_mac % 7);
        snprintf(badge_obj.device_name, SIZEOF(badge_obj.device_name), "Saiyan-%04x", badge_obj.short_mac);
        snprintf(badge_obj.ap_ssid, SIZEOF(badge_obj.ap_ssid), "Saiyan-%04x", badge_obj.short_mac);
        snprintf(badge_obj.ap_password, SIZEOF(badge_obj.ap_password), "%02x%02x%02x%02x", badge_obj.mac[2], badge_obj.mac[3], badge_obj.mac[4], badge_obj.mac[5]);
        snprintf(badge_obj.web_login, SIZEOF(badge_obj.web_login), "%s", web_login);
        snprintf(badge_obj.sta_ssid, SIZEOF(badge_obj.sta_ssid), "%s", sta_ssid);
        snprintf(badge_obj.sta_password, SIZEOF(badge_obj.sta_password), "%s", sta_password);

        // Set brightness values
        badge_obj.brightness_max = (uint8_t)brightness_max;
        badge_obj.brightness_mid = (uint8_t)brightness_mid;
        badge_obj.brightness_off = (uint8_t)brightness_off;

        // Update settings object
        cJSON_AddNumberToObject(obj_badge, "id", (double)badge_obj.device_id);
        json_set_str_value(obj_badge, "name", badge_obj.device_name);
        json_set_str_value(obj_ap, "ssid", badge_obj.ap_ssid);
        json_set_str_value(obj_ap, "password", badge_obj.ap_password);

        // Save to settings.json
        save_settings(json_settings);
        cJSON_Delete(json_settings);
        
        ESP_LOGI(__FILE__, "Setting file saved to default values");
    }
    
    json_settings = load_settings();

    cJSON *obj_badge = cJSON_GetObjectItem(json_settings, "badge");
    cJSON *obj_web = cJSON_GetObjectItem(json_settings, "web");
    cJSON *obj_ap = cJSON_GetObjectItem(json_settings, "ap");
    cJSON *obj_sta = cJSON_GetObjectItem(json_settings, "sta");
    cJSON *obj_display = cJSON_GetObjectItem(json_settings, "display");

    const char* badge_name = json_get_str_value(obj_badge, "name"); // should be empty
    const char* web_login = json_get_str_value(obj_web, "login");
    const char* ap_ssid = json_get_str_value(obj_ap, "ssid"); // should be empty
    const char* ap_password = json_get_str_value(obj_ap, "password"); // should be empty
    const char* sta_ssid = json_get_str_value(obj_sta, "ssid");
    const char* sta_password = json_get_str_value(obj_sta, "password");

    // Load brightness settings with defaults if not present
    int brightness_max = obj_display ? json_get_int_value(obj_display, "brightness_max") : 255;
    int brightness_mid = obj_display ? json_get_int_value(obj_display, "brightness_mid") : 200;
    int brightness_off = obj_display ? json_get_int_value(obj_display, "brightness_off") : 0;

    // Update badge object
    esp_efuse_mac_get_default(badge_obj.mac);
    
    badge_obj.short_mac = (badge_obj.mac[4] << 8) + badge_obj.mac[5];
    badge_obj.device_id = (int)cJSON_GetObjectItem(obj_badge, "id")->valuedouble;
    //generate_id(badge_obj.short_mac);//1 + (badge_obj.short_mac % 7);

    snprintf(badge_obj.device_name, SIZEOF(badge_obj.device_name), "%s", badge_name);
    snprintf(badge_obj.web_login, SIZEOF(badge_obj.web_login), "%s", web_login);
    snprintf(badge_obj.ap_ssid, SIZEOF(badge_obj.ap_ssid), "%s", ap_ssid);
    snprintf(badge_obj.ap_password, SIZEOF(badge_obj.ap_password), "%s", ap_password);
    // A saved password under 8 chars makes the AP abort on start: fall back to the default one
    if (strlen(badge_obj.ap_password) > 0 && strlen(badge_obj.ap_password) < AP_PASSWORD_MIN_LEN) {
        snprintf(badge_obj.ap_password, SIZEOF(badge_obj.ap_password), "%02x%02x%02x%02x", badge_obj.mac[2], badge_obj.mac[3], badge_obj.mac[4], badge_obj.mac[5]);
    }
    snprintf(badge_obj.sta_ssid, SIZEOF(badge_obj.sta_ssid), "%s", sta_ssid);
    snprintf(badge_obj.sta_password, SIZEOF(badge_obj.sta_password), "%s", sta_password);

    // WPA2/WPA3-Enterprise (802.1X) STA settings (optional keys; default to
    // a plain open/PSK network when absent)
    cJSON* sta_ent = cJSON_GetObjectItem(obj_sta, "enterprise");
    badge_obj.sta_enterprise = cJSON_IsTrue(sta_ent);
    cJSON* sta_identity = cJSON_GetObjectItem(obj_sta, "identity");
    cJSON* sta_username = cJSON_GetObjectItem(obj_sta, "username");
    snprintf(badge_obj.sta_identity, SIZEOF(badge_obj.sta_identity), "%s",
             cJSON_IsString(sta_identity) ? sta_identity->valuestring : "");
    snprintf(badge_obj.sta_username, SIZEOF(badge_obj.sta_username), "%s",
             cJSON_IsString(sta_username) ? sta_username->valuestring : "");
    
    // Set brightness values
    badge_obj.brightness_max = (uint8_t)brightness_max;
    badge_obj.brightness_mid = (uint8_t)brightness_mid;
    badge_obj.brightness_off = (uint8_t)brightness_off;
    
    ESP_LOGI(__FILE__, "Setting file loaded");
    ESP_LOGI(__FILE__, "The badge ID is: %d", badge_obj.device_id);
    cJSON_Delete(json_settings);
    badge_obj.update = update_attribute;
}
