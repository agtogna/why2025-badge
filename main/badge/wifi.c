#include "wifi.h"
#include "esp_eap_client.h"

static const char *TAG = "WIFI";

// Configure WPA2/WPA3-Enterprise (802.1X) EAP credentials for the STA
// interface. Must be called after esp_wifi_set_config() and before
// esp_wifi_start(). Uses PEAP/EAP-TTLS with username+password; server
// certificate validation is intentionally not enforced (EMF marks it
// optional), so no CA certificate needs to be embedded.
static void sta_apply_enterprise(void)
{
	const char* identity = strlen(badge_obj.sta_identity) > 0
		? badge_obj.sta_identity : badge_obj.sta_username;

	ESP_ERROR_CHECK( esp_eap_client_set_identity(
		(const unsigned char*)identity, strlen(identity)) );
	ESP_ERROR_CHECK( esp_eap_client_set_username(
		(const unsigned char*)badge_obj.sta_username, strlen(badge_obj.sta_username)) );
	ESP_ERROR_CHECK( esp_eap_client_set_password(
		(const unsigned char*)badge_obj.sta_password, strlen(badge_obj.sta_password)) );
	ESP_ERROR_CHECK( esp_wifi_sta_enterprise_enable() );

	ESP_LOGI(TAG, "STA WPA-Enterprise enabled (identity:%s username:%s)",
		identity, badge_obj.sta_username);
}
static wifi_mode_t curr_mode = WIFI_MODE_NULL;
QueueHandle_t wifi_queue;

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
const int FAIL_BIT = BIT1;

static int retry_num = 0;
static int ap_clients_num = 0;
static esp_timer_handle_t inactivity_timer;

static void inactivity_timer_callback(void* arg)
{
	if(!ap_clients_num){
		stop_wifi();
	} else {
		ESP_LOGE(__FILE__, "Timer should not be running...");
	}
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        uint32_t ev = EVENT_SCAN_DONE;
        if (wifi_queue) xQueueSend(wifi_queue, &ev, 0);
        return;
    }
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
		ap_clients_num++;
		esp_timer_stop(inactivity_timer);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
		ap_clients_num--;
		if(ap_clients_num < 0) ap_clients_num = 0;
		if(!ap_clients_num) esp_timer_start_once(inactivity_timer, AP_INACTIVITY_TIMEOUT_S * 1000000);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_num < STA_MAXIMUM_RETRY) {
			ui_connection_progress(retry_num+1, STA_MAXIMUM_RETRY);
            esp_wifi_connect();
            retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP (%d/%d)", retry_num, STA_MAXIMUM_RETRY);
			xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        } else {
            xEventGroupSetBits(wifi_event_group, FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		retry_num = 0;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    } 
}

void wifi_init(void)
{
	esp_log_level_set("wifi", ESP_LOG_WARN);
	static bool initialized = false;
	if (initialized) {
		return;
	}
	ESP_ERROR_CHECK(esp_netif_init());
	wifi_event_group = xEventGroupCreate();
	
	esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
	assert(ap_netif);
	esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
	assert(sta_netif);
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
	ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event_handler, NULL) );
	ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );
	ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &event_handler, NULL) );
	ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &event_handler, NULL) );
	ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &event_handler, NULL) );


	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_NULL) );
	ESP_ERROR_CHECK( esp_wifi_start() );

    curr_mode = WIFI_MODE_NULL;

	initialized = true;
}

bool start_wifi_ap(void)
{
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());
	
	const char* AP_WIFI_SSID = badge_obj.ap_ssid;
	const char* AP_WIFI_PASSWORD = badge_obj.ap_password;
	
	wifi_config_t wifi_config = { 0 };
	snprintf((char*)wifi_config.ap.ssid, SIZEOF(wifi_config.ap.ssid), "%s", AP_WIFI_SSID);
	snprintf((char*)wifi_config.ap.password, SIZEOF(wifi_config.ap.password), "%s", AP_WIFI_PASSWORD);
	wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
	wifi_config.ap.ssid_len = strlen(AP_WIFI_SSID);
	wifi_config.ap.max_connection = AP_MAX_STA_CONN;

	if (strlen(AP_WIFI_PASSWORD) == 0) {
		wifi_config.ap.authmode = WIFI_AUTH_OPEN;
	}


	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_AP) );
	ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_AP, &wifi_config) );
	ESP_ERROR_CHECK( esp_wifi_start() );
	ESP_ERROR_CHECK( esp_wifi_set_inactive_time(WIFI_IF_AP, AP_INACTIVITY_TIMEOUT_S) );

	const esp_timer_create_args_t timer_args = {
		.callback = &inactivity_timer_callback,
		.name = "inactivity-timer"
	};

	ESP_ERROR_CHECK(esp_timer_create(&timer_args, &inactivity_timer));

	ESP_LOGI(TAG, "WIFI_MODE_AP started. SSID:%s password:%s",
			 AP_WIFI_SSID, AP_WIFI_PASSWORD);

    curr_mode = WIFI_MODE_AP;
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());

	return ESP_OK;
}

bool start_wifi_sta()
{
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());
	
	const char* STA_WIFI_SSID = badge_obj.sta_ssid;
	const char* STA_WIFI_PASSWORD = badge_obj.sta_password;

	wifi_config_t wifi_config = { 0 };
	snprintf((char*)wifi_config.sta.ssid, SIZEOF(wifi_config.sta.ssid), "%s", STA_WIFI_SSID);

	if (badge_obj.sta_enterprise) {
		// WPA2/WPA3-Enterprise: be PMF-capable so we can associate with
		// WPA3-Enterprise APs; EAP credentials are set after set_config().
		wifi_config.sta.pmf_cfg.capable = true;
		wifi_config.sta.pmf_cfg.required = false;
	} else if (strlen(STA_WIFI_PASSWORD) > 0) {
		// WPA2-PSK. Leave empty for open networks.
		snprintf((char*)wifi_config.sta.password, SIZEOF(wifi_config.sta.password), "%s", STA_WIFI_PASSWORD);
	}

	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
	ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
	if (badge_obj.sta_enterprise) {
		sta_apply_enterprise();
	}
    ESP_ERROR_CHECK( esp_wifi_start() );
	ESP_ERROR_CHECK( esp_wifi_connect() );

	int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
								   pdFALSE, pdTRUE, STA_TIMEOUT_MS / portTICK_PERIOD_MS);
	ESP_LOGI(TAG, "bits=%x", bits);
	if (bits & CONNECTED_BIT) {
		ESP_LOGI(TAG, "WIFI_MODE_STA connected. SSID:%s password:%s",
			 STA_WIFI_SSID, STA_WIFI_PASSWORD);
	} else {
		ESP_LOGI(TAG, "WIFI_MODE_STA can't connected. SSID:%s password:%s",
			 STA_WIFI_SSID, STA_WIFI_PASSWORD);
		stop_wifi();
	}

    curr_mode = WIFI_MODE_STA;
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());

	return (bits & CONNECTED_BIT) != 0;
}

bool start_wifi_apsta()
{
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());
	
	const char* AP_WIFI_SSID = badge_obj.ap_ssid;
	const char* AP_WIFI_PASSWORD = badge_obj.ap_password;
	const char* STA_WIFI_SSID = badge_obj.sta_ssid;
	const char* STA_WIFI_PASSWORD = badge_obj.sta_password;

	wifi_config_t ap_config = { 0 };
	
	snprintf((char*)ap_config.ap.ssid, SIZEOF(ap_config.ap.ssid), "%s", AP_WIFI_SSID);
	snprintf((char*)ap_config.ap.password, SIZEOF(ap_config.ap.password), "%s", AP_WIFI_PASSWORD);
	ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
	ap_config.ap.ssid_len = strlen(AP_WIFI_SSID);
	ap_config.ap.max_connection = AP_MAX_STA_CONN;

	if (strlen(AP_WIFI_PASSWORD) == 0) {
		ap_config.ap.authmode = WIFI_AUTH_OPEN;
	}

	wifi_config_t sta_config = { 0 };
	snprintf((char*)sta_config.sta.ssid, SIZEOF(sta_config.sta.ssid), "%s", STA_WIFI_SSID);
	if (badge_obj.sta_enterprise) {
		sta_config.sta.pmf_cfg.capable = true;
		sta_config.sta.pmf_cfg.required = false;
	} else {
		snprintf((char*)sta_config.sta.password, SIZEOF(sta_config.sta.password), "%s", STA_WIFI_PASSWORD);
	}


	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_APSTA) );
	ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_AP, &ap_config) );
	ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
	if (badge_obj.sta_enterprise) {
		sta_apply_enterprise();
	}
	ESP_ERROR_CHECK( esp_wifi_start() );
	ESP_LOGI(TAG, "WIFI_MODE_AP started. SSID:%s password:%s",
			 AP_WIFI_SSID, AP_WIFI_PASSWORD);

	ESP_ERROR_CHECK( esp_wifi_connect() );
	int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
								   pdFALSE, pdTRUE, STA_TIMEOUT_MS / portTICK_PERIOD_MS);
	ESP_LOGI(TAG, "bits=%x", bits);
	if (bits & CONNECTED_BIT) {
		ESP_LOGI(TAG, "WIFI_MODE_STA connected. SSID:%s password:%s",
			 STA_WIFI_SSID, STA_WIFI_PASSWORD);
	} else {
		ESP_LOGI(TAG, "WIFI_MODE_STA can't connected. SSID:%s password:%s",
			 STA_WIFI_SSID, STA_WIFI_PASSWORD);
		stop_wifi();
	}

    curr_mode = WIFI_MODE_APSTA;
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());

	return (bits & CONNECTED_BIT) != 0;
}

void stop_wifi(){
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    curr_mode = WIFI_MODE_NULL;
    ESP_LOGI(TAG, "WIFI disabled");
	ESP_LOGI(__FILE__, "free_heap_size = %lu\n", esp_get_free_heap_size());
}

// Latest scan result, shared with the UI task.
static wifi_ap_record_t ap_snapshot[WIFI_SCAN_MAX];
static uint8_t ap_count = 0;
static SemaphoreHandle_t scan_mutex = NULL;

uint8_t wifi_get_scan(wifi_ap_record_t *out, uint8_t max)
{
	uint8_t n = 0;
	if (scan_mutex && xSemaphoreTake(scan_mutex, pdMS_TO_TICKS(100))) {
		n = ap_count < max ? ap_count : max;
		memcpy(out, ap_snapshot, n * sizeof(*out));
		xSemaphoreGive(scan_mutex);
	}
	return n;
}

// Non-blocking: WIFI_EVENT_SCAN_DONE posts EVENT_SCAN_DONE, so wifi_task keeps serving
// the queue (AP/STA start) while the radio scans.
static bool wifi_scan_begin(void)
{
	wifi_scan_config_t cfg = {
		.show_hidden = true,
		.scan_type = WIFI_SCAN_TYPE_ACTIVE,
		.scan_time.active.min = 50,
		.scan_time.active.max = 100,
	};
	if (esp_wifi_scan_start(&cfg, false) != ESP_OK) {
		ESP_LOGW(TAG, "Wi-Fi scan start failed");
		return false;
	}
	return true;
}

static void wifi_scan_collect(void)
{
	static wifi_ap_record_t recs[WIFI_SCAN_MAX];
	uint16_t n = WIFI_SCAN_MAX;
	if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {   // IDF returns them strongest first
		ESP_LOGW(TAG, "Wi-Fi scan failed");
		return;
	}
	xSemaphoreTake(scan_mutex, portMAX_DELAY);
	memcpy(ap_snapshot, recs, n * sizeof(*recs));
	ap_count = n;
	xSemaphoreGive(scan_mutex);
}

void wifi_task(void *arg)
{   
    wifi_queue = xQueueCreate(4, sizeof(uint32_t));
    scan_mutex = xSemaphoreCreateMutex();
    uint32_t wifi_event;
    bool scanning = false;       // scan screen is shown
    bool scan_running = false;   // a scan is in flight, waiting for EVENT_SCAN_DONE

    while (1) {
        if(!xQueueReceive(wifi_queue, &wifi_event, scanning ? pdMS_TO_TICKS(500) : portMAX_DELAY)) {
            if (scanning && !scan_running) scan_running = wifi_scan_begin();
            continue;
        }
        
        switch (wifi_event) {
            case EVENT_HOTSPOT_START:
                scanning = scan_running = false;
                stop_wifi();
                start_wifi_ap();
				esp_timer_start_once(inactivity_timer, AP_INACTIVITY_TIMEOUT_S * 1000000);
                break;
            case EVENT_STA_START:
                scanning = scan_running = false;
                stop_wifi();
                start_wifi_sta();
				retry_num = 0;
                break;
            case EVENT_SCAN_START:
                if (curr_mode == WIFI_MODE_NULL) {   // never disturb an active AP/STA
                    xSemaphoreTake(scan_mutex, portMAX_DELAY);
                    ap_count = 0;
                    xSemaphoreGive(scan_mutex);
                    if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK && esp_wifi_start() == ESP_OK) {
                        curr_mode = WIFI_MODE_STA;
                        scanning = true;
                        scan_running = wifi_scan_begin();
                    } else {
                        ESP_LOGW(TAG, "Wi-Fi scan start failed");
                        stop_wifi();
                    }
                }
                break;
            case EVENT_SCAN_STOP:
                if (scanning) {
                    scanning = scan_running = false;
                    stop_wifi();
                }
                break;
            case EVENT_SCAN_DONE:
                scan_running = false;
                if (scanning) wifi_scan_collect();
                else esp_wifi_clear_ap_list();   // free the list of a scan nobody wants
                break;
            case EVENT_HOTSPOT_STOP:
            case EVENT_STA_STOP:
                stop_wifi();
				esp_timer_stop(inactivity_timer);
                break;            
            default:
                ESP_LOGI(__FILE__, "not exists event 0x%04" PRIx32, wifi_event);
                break;
        }
        vTaskDelay( 1000 / portTICK_PERIOD_MS );
    }
}