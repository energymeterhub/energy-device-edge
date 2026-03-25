// wifi_mgr.c
// - If cfg has SSID: try STA connect with compatibility-oriented defaults
// - Retry transient failures before falling back to SoftAP
// - Keep provisioning available through SoftAP when STA is unavailable
//
// IMPORTANT: do NOT block inside event handler.

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "wifi_mgr.h"   // must provide device_config_t

static const char *TAG = "wifi_mgr";

// Event bits
#define WIFI_BIT_GOT_IP   BIT0
#define WIFI_BIT_STA_FAIL BIT1

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WIFI_RECOVERY_TIMEOUT_MS 15000
#define WIFI_CONNECT_MAX_RETRIES 4
#define WIFI_RETRY_DELAY_MS 1200

static EventGroupHandle_t s_wifi_ev;
static bool s_inited;
static bool s_ap_active;
static wifi_err_reason_t s_last_sta_reason = WIFI_REASON_UNSPECIFIED;
static int8_t s_last_sta_rssi;

static void build_fallback_ap_ssid(char *ssid, size_t ssid_size)
{
    if (!ssid || ssid_size == 0) return;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid, ssid_size, "energy_device_edge_%02X%02X", mac[4], mac[5]);
}

static const char *wifi_reason_to_string(wifi_err_reason_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "auth_expire";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4way_timeout";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon_timeout";
    case WIFI_REASON_NO_AP_FOUND:
        return "no_ap_found";
    case WIFI_REASON_AUTH_FAIL:
        return "auth_fail";
    case WIFI_REASON_ASSOC_FAIL:
        return "assoc_fail";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake_timeout";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection_fail";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "security_incompatible";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "authmode_threshold";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "rssi_threshold";
    default:
        return "unknown";
    }
}

static bool wifi_reason_is_retryable(wifi_err_reason_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_AKMP_INVALID:
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
    case WIFI_REASON_INVALID_RSN_IE_CAP:
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return false;
    default:
        return true;
    }
}

static esp_err_t wifi_wait_for_sta_connection(TickType_t total_timeout, int max_retries)
{
    TickType_t start = xTaskGetTickCount();
    int retries = 0;

    while (true) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= total_timeout) {
            break;
        }

        TickType_t remaining = total_timeout - elapsed;
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_ev,
            WIFI_BIT_GOT_IP | WIFI_BIT_STA_FAIL,
            pdTRUE,
            pdFALSE,
            remaining);

        if (bits & WIFI_BIT_GOT_IP) {
            return ESP_OK;
        }

        if ((bits & WIFI_BIT_STA_FAIL) == 0) {
            break;
        }

        bool retryable = wifi_reason_is_retryable(s_last_sta_reason);
        if (!retryable || retries >= max_retries) {
            ESP_LOGW(TAG, "STA connect failed: %s (%u), retries=%d/%d",
                     wifi_reason_to_string(s_last_sta_reason),
                     (unsigned)s_last_sta_reason,
                     retries,
                     max_retries);
            return ESP_FAIL;
        }

        retries++;
        ESP_LOGW(TAG, "STA retry %d/%d after %s (%u)",
                 retries,
                 max_retries,
                 wifi_reason_to_string(s_last_sta_reason),
                 (unsigned)s_last_sta_reason);

        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
        (void)esp_wifi_disconnect();

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect retry failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    return ESP_ERR_TIMEOUT;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "GOT_IP: " IPSTR, IP2STR(&e->ip_info.ip));
        if (s_wifi_ev) xEventGroupSetBits(s_wifi_ev, WIFI_BIT_GOT_IP);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)event_data;
        s_last_sta_reason = (wifi_err_reason_t)e->reason;
        s_last_sta_rssi = e->rssi;
        ESP_LOGW(TAG, "STA_DISCONNECTED reason=%s(%u) rssi=%d",
                 wifi_reason_to_string(s_last_sta_reason),
                 (unsigned)s_last_sta_reason,
                 (int)s_last_sta_rssi);
        if (s_wifi_ev) xEventGroupSetBits(s_wifi_ev, WIFI_BIT_STA_FAIL);
        return;
    }
}

static esp_err_t wifi_mgr_init_once(void)
{
    if (s_inited) return ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    // Default netifs, safe to create once.
    (void)esp_netif_create_default_wifi_sta();
    (void)esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Disable power-save during bring-up to reduce handshake issues.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_wifi_ev = xEventGroupCreate();
    if (!s_wifi_ev) return ESP_ERR_NO_MEM;

    s_inited = true;
    return ESP_OK;
}

static void fill_sta_config(wifi_config_t *sta, const device_config_t *cfg)
{
    memset(sta, 0, sizeof(*sta));
    strlcpy((char *)sta->sta.ssid, cfg->wifi.ssid, sizeof(sta->sta.ssid));
    strlcpy((char *)sta->sta.password, cfg->wifi.password, sizeof(sta->sta.password));

    sta->sta.bssid_set = 0;
    sta->sta.channel = 0;
    sta->sta.listen_interval = 3;
    sta->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta->sta.threshold.rssi = -127;
    sta->sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta->sta.pmf_cfg.capable = true;
    sta->sta.pmf_cfg.required = false;
}

esp_err_t wifi_mgr_start_sta_if_configured(const device_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    ESP_ERROR_CHECK(wifi_mgr_init_once());

    if (cfg->wifi.ssid[0] == '\0') {
        ESP_LOGI(TAG, "ssid empty -> skip STA");
        return ESP_FAIL;
    }

    xEventGroupClearBits(s_wifi_ev, WIFI_BIT_GOT_IP | WIFI_BIT_STA_FAIL);

    wifi_config_t sta = {0};
    fill_sta_config(&sta, cfg);

    // Start from a clean Wi-Fi state.
    (void)esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ap_active = false;

    // Disable power-save because DHCP can fail on some routers otherwise.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Use the hardware MAC to avoid router rejection of custom MACs.
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "Using hardware MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    ESP_LOGI(TAG, "STA connecting... ssid=\"%s\"", cfg->wifi.ssid);
    ESP_ERROR_CHECK(esp_wifi_connect());

    esp_err_t err = wifi_wait_for_sta_connection(pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS),
                                                 WIFI_CONNECT_MAX_RETRIES);
    if (err == ESP_OK) return ESP_OK;

    ESP_LOGW(TAG, "STA failed/timeout: %s", esp_err_to_name(err));
    return err;
}

esp_err_t wifi_mgr_start_ap_force(void)
{
    ESP_ERROR_CHECK(wifi_mgr_init_once());

    wifi_config_t ap = {0};
    char ap_ssid[sizeof(ap.ap.ssid)] = {0};
    build_fallback_ap_ssid(ap_ssid, sizeof(ap_ssid));
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = 0;

    // Open AP for recovery mode.
    ap.ap.password[0] = '\0';
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100;
    ap.ap.ssid_hidden = 0;
    ap.ap.pmf_cfg.required = false;

    (void)esp_wifi_stop();
    // Use APSTA mode so scans still work while SoftAP is active.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ap_active = true;

    ESP_LOGI(TAG, "SoftAP started: SSID=%s (open)", (char *)ap.ap.ssid);
    return ESP_OK;
}

esp_err_t wifi_mgr_start_ap_if_needed(const device_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    // If SSID is configured, let the caller decide when AP fallback is needed.
    if (cfg->wifi.ssid[0] != '\0') return ESP_OK;

    return wifi_mgr_start_ap_force();
}

esp_err_t wifi_mgr_scan(wifi_ap_record_t **out_list, uint16_t *count)
{
    if (!out_list || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    *out_list = NULL;

    ESP_ERROR_CHECK(wifi_mgr_init_once());

    // Scans require STA or APSTA mode.
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if ((mode & WIFI_MODE_STA) == 0) {
        // Temporarily enable APSTA if STA is missing.
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 0,
        .scan_time.active.max = 0 // default
    };

    // Blocking scan.
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t num = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&num));
    if (num == 0) {
        return ESP_OK;
    }

    // Limit the result size to avoid excessive memory use.
    if (num > 20) num = 20;

    wifi_ap_record_t *list = (wifi_ap_record_t *)malloc(num * sizeof(wifi_ap_record_t));
    if (!list) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&num, list));

    *out_list = list;
    *count = num;

    ESP_LOGI(TAG, "scan done: %d APs found", num);
    return ESP_OK;
}

bool wifi_mgr_is_ap_mode(void)
{
    return s_ap_active;
}

esp_err_t wifi_mgr_try_sta_recovery(const device_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (cfg->wifi.ssid[0] == '\0') return ESP_ERR_INVALID_STATE;

    ESP_ERROR_CHECK(wifi_mgr_init_once());

    xEventGroupClearBits(s_wifi_ev, WIFI_BIT_GOT_IP | WIFI_BIT_STA_FAIL);

    wifi_config_t sta = {0};
    fill_sta_config(&sta, cfg);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_connect());

    esp_err_t err = wifi_wait_for_sta_connection(pdMS_TO_TICKS(WIFI_RECOVERY_TIMEOUT_MS),
                                                 WIFI_CONNECT_MAX_RETRIES);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Recovery STA probe succeeded");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Recovery STA probe failed: %s", esp_err_to_name(err));
    return err;
}
