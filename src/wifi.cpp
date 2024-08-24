#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include <string.h>

#include "globals.h"
#include "settings.h"
#include "wifi.h"

#include <esp_mac.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/ip4_addr.h>
#include <mdns.h>

#include <esp_log.h>
static const char * TAG8 = "sc:wifi....";

static bool          wifi_connected        = false;
static bool          s_sta_connected       = false;
static bool          s_ap_active           = false;
static bool          s_ap_client_connected = false;
static esp_netif_t * sta_netif;
static esp_netif_t * ap_netif;

// Function to handle WiFi events
static void wifi_event_handler (void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data) {
    ESP_LOGV (TAG8, "trace: %s(event_base = '%s', event_id = %ld)", __func__, event_base, event_id);

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI (TAG8, "received event WIFI_EVENT_STA_START, connecting");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI (TAG8, "received event WIFI_EVENT_STA_CONNECTED, recording connected");
            s_sta_connected = true;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI (TAG8, "received event WIFI_EVENT_STA_DISCONNECTED");
            s_sta_connected = false;
            if (!s_ap_client_connected) {
                esp_wifi_connect();  // Attempt to reconnect immediately
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t * event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI (TAG8, "station " MACSTR " connected, aid=%d", MAC2STR (event->mac), event->aid);
            s_ap_client_connected = true;

            // Reapply AP settings to ensure correct gateway configuration
            esp_netif_ip_info_t ip_info;
            IP4_ADDR (&ip_info.ip, 192, 168, 4, 1);
            IP4_ADDR (&ip_info.gw, 0, 0, 0, 0);
            IP4_ADDR (&ip_info.netmask, 255, 255, 255, 0);
            ESP_ERROR_CHECK (esp_netif_dhcps_stop (ap_netif));
            ESP_ERROR_CHECK (esp_netif_set_ip_info (ap_netif, &ip_info));
            ESP_ERROR_CHECK (esp_netif_dhcps_start (ap_netif));

            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t * event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI (TAG8, "station " MACSTR " disconnected, aid=%d", MAC2STR (event->mac), event->aid);
            s_ap_client_connected = false;
            break;
        }
        }
    }
    else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t * event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI (TAG8, "got ip:" IPSTR, IP2STR (&event->ip_info.ip));
            wifi_connected = true;
        }
    }
}

static void wifi_init_softap () {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    ESP_LOGI (TAG8, "Setting up soft AP");
    wifi_config_t wifi_config = {
        .ap = {
               .ssid            = {0},
               .password        = {0},
               .ssid_len        = 0,
               .channel         = 1,
               .authmode        = WIFI_AUTH_WPA_WPA2_PSK,
               .ssid_hidden     = 0,
               .max_connection  = 4,
               .beacon_interval = 100,
               .pairwise_cipher = WIFI_CIPHER_TYPE_TKIP_CCMP,
               .ftm_responder   = false,
               .pmf_cfg         = {
                        .capable  = true,
                        .required = false},
               .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
               },
    };

    strlcpy ((char *)wifi_config.ap.ssid, g_ap_ssid, sizeof (wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen (g_ap_ssid);
    strlcpy ((char *)wifi_config.ap.password, g_ap_pass, sizeof (wifi_config.ap.password));

    if (strlen (g_ap_pass) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK (esp_wifi_set_config (WIFI_IF_AP, &wifi_config));

    esp_netif_ip_info_t ip_info;
    IP4_ADDR (&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR (&ip_info.gw, 0, 0, 0, 0);  // Set gateway to 0.0.0.0 to indicate no internet route
    IP4_ADDR (&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK (esp_netif_dhcps_stop (ap_netif));
    ESP_ERROR_CHECK (esp_netif_set_ip_info (ap_netif, &ip_info));
    ESP_ERROR_CHECK (esp_netif_dhcps_start (ap_netif));

    ESP_LOGI (TAG8, "soft ap setup complete. ssid: %s", g_ap_ssid);
    s_ap_active = true;
}

static void wifi_init_sta (const char * ssid, const char * password) {
    ESP_LOGI (TAG8, "attempting ssid: %s", ssid);
    wifi_config_t wifi_config = {
        .sta = {
                .ssid            = {0},
                .password        = {0},
                .scan_method     = WIFI_FAST_SCAN,
                .bssid_set       = false,
                .bssid           = {0},
                .channel         = 0,
                .listen_interval = 0,
                .sort_method     = WIFI_CONNECT_AP_BY_SIGNAL,
                .threshold       = {
                      .rssi     = 0,
                      .authmode = WIFI_AUTH_WPA2_PSK},
                .pmf_cfg                                        = {.capable = true, .required = false},
                .rm_enabled                                     = 0,
                .btm_enabled                                    = 0,
                .mbo_enabled                                    = 0,
                .ft_enabled                                     = 0,
                .owe_enabled                                    = 0,
                .transition_disable                             = 0,
                .reserved                                       = 0,
                .sae_pwe_h2e                                    = WPA3_SAE_PWE_HUNT_AND_PECK,
                .sae_pk_mode                                    = WPA3_SAE_PK_MODE_AUTOMATIC,
                .failure_retry_cnt                              = 0,
                .he_dcm_set                                     = 0,
                .he_dcm_max_constellation_tx                    = 0,
                .he_dcm_max_constellation_rx                    = 0,
                .he_mcs9_enabled                                = 0,
                .he_su_beamformee_disabled                      = 0,
                .he_trig_su_bmforming_feedback_disabled         = 0,
                .he_trig_mu_bmforming_partial_feedback_disabled = 0,
                .he_trig_cqi_feedback_disabled                  = 0,
                .he_reserved                                    = 0,
                .sae_h2e_identifier                             = {0}},
    };

    strlcpy ((char *)wifi_config.sta.ssid, ssid, sizeof (wifi_config.sta.ssid));
    strlcpy ((char *)wifi_config.sta.password, password, sizeof (wifi_config.sta.password));
    ESP_ERROR_CHECK (esp_wifi_set_config (WIFI_IF_STA, &wifi_config));

    ESP_LOGI (TAG8, "connecting to ap ssid: %s", ssid);
    esp_wifi_connect();
}

// Function to reduce WiFi transmit power
static void wifi_attenuate_power () {
    ESP_LOGV (TAG8, "trace: %s()", __func__);
    /*
     * Wifi TX power levels are quantized.
     * See https://demo-dijiudu.readthedocs.io/en/latest/api-reference/wifi/esp_wifi.html
     * | range     | level             | net pwr  |
     * |-----------+-------------------+----------|
     * | [78, 127] | level0            | 19.5 dBm |
     * | [76, 77]  | level1            | 19   dBm |
     * | [74, 75]  | level2            | 18.5 dBm |
     * | [68, 73]  | level3            | 17   dBm |
     * | [60, 67]  | level4            | 15   dBm |
     * | [52, 59]  | level5            | 13   dBm |
     * | [44, 51]  | level5 -  2.0 dBm | 11   dBm |  <-- we'll use this
     * | [34, 43]  | level5 -  4.5 dBm |  8.5 dBm |
     * | [28, 33]  | level5 -  6.0 dBm |  7   dBm |
     * | [20, 27]  | level5 -  8.0 dBm |  5   dBm |
     * | [8,  19]  | level5 - 11.0 dBm |  2   dBm |
     * | [-128, 7] | level5 - 14.0 dBm | -1   dBM |
     */
    // Not required, but we read the starting power just for informative purposes
    int8_t curr_wifi_power = 0;
    ESP_ERROR_CHECK (esp_wifi_get_max_tx_power (&curr_wifi_power));
    ESP_LOGI (TAG8, "default max tx power: %d", curr_wifi_power);

    const int8_t MAX_TX_PWR = 44;  // level 5 - 2dBm = 11dBm
    ESP_LOGI (TAG8, "setting wifi max power to %d", MAX_TX_PWR);
    ESP_ERROR_CHECK (esp_wifi_set_max_tx_power (MAX_TX_PWR));

    ESP_ERROR_CHECK (esp_wifi_get_max_tx_power (&curr_wifi_power));
    ESP_LOGI (TAG8, "confirmed new max tx power: %d", curr_wifi_power);
}

// Function to initialize WiFi
void wifi_init () {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    s_sta_connected       = false;
    s_ap_client_connected = false;

    ESP_ERROR_CHECK (esp_netif_init());
    ESP_ERROR_CHECK (esp_event_loop_create_default());

    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK (esp_wifi_init (&cfg));

    // Set storage to RAM before any other WiFi calls
    ESP_ERROR_CHECK (esp_wifi_set_storage (WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK (esp_event_handler_register (WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK (esp_event_handler_register (IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK (esp_wifi_set_mode (WIFI_MODE_APSTA));

    // Clear any existing WiFi configuration
    wifi_config_t wifi_config   = {};
    wifi_config.sta.ssid[0]     = '\0';
    wifi_config.sta.password[0] = '\0';
    wifi_config.sta.bssid_set   = false;
    ESP_ERROR_CHECK (esp_wifi_set_config (WIFI_IF_STA, &wifi_config));

    // Clear AP configuration as well
    wifi_config_t ap_config      = {};
    ap_config.ap.ssid[0]         = '\0';
    ap_config.ap.password[0]     = '\0';
    ap_config.ap.ssid_len        = 0;
    ap_config.ap.channel         = 1;
    ap_config.ap.authmode        = WIFI_AUTH_OPEN;
    ap_config.ap.ssid_hidden     = 0;
    ap_config.ap.max_connection  = 4;
    ap_config.ap.beacon_interval = 100;
    ESP_ERROR_CHECK (esp_wifi_set_config (WIFI_IF_AP, &ap_config));

    wifi_init_softap();

    // Disconnect if we're connected to any AP
    esp_wifi_disconnect();

    ESP_ERROR_CHECK (esp_wifi_start());

    wifi_attenuate_power();

    ESP_LOGI (TAG8, "wifi initialization complete");
}

void start_mdns_service () {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    ESP_LOGI (TAG8, "starting mdns service");
    esp_err_t err = mdns_init();
    if (err) {
        ESP_LOGE (TAG8, "mdns init failed: %d", err);
        return;
    }

    // Set the hostname
    ESP_ERROR_CHECK (mdns_hostname_set ("sotacat"));

    // Set the default instance
    ESP_ERROR_CHECK (mdns_instance_name_set ("SOTAcat SOTAmat Service"));

    // You can also add services to announce
    mdns_service_add (NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI (TAG8, "mdns service started");
}

void wifi_task (void * pvParameters) {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    TaskNotifyConfig * config = (TaskNotifyConfig *)pvParameters;
    wifi_init();

    const int  STA_CONNECT_TIMEOUT = (6 * 1000);  // 6 seconds timeout
    int        current_ssid        = 1;
    TickType_t last_attempt_time   = xTaskGetTickCount();

    while (!s_sta_connected && !s_ap_client_connected) {
        if ((xTaskGetTickCount() - last_attempt_time) * portTICK_PERIOD_MS >= STA_CONNECT_TIMEOUT) {
            if (current_ssid == 1) {
                wifi_init_sta (g_sta1_ssid, g_sta1_pass);
                current_ssid = 2;
            }
            else {
                wifi_init_sta (g_sta2_ssid, g_sta2_pass);
                current_ssid = 1;
            }
            last_attempt_time = xTaskGetTickCount();
        }
        vTaskDelay (pdMS_TO_TICKS (100));
    }

    start_mdns_service();

    xTaskNotify (config->setup_task_handle, config->notification_bit, eSetBits);

    // Monitor connection status
    while (true) {
        if (!s_sta_connected && !s_ap_client_connected) {
            // If connection is lost, start cycling again
            current_ssid      = 1;
            last_attempt_time = xTaskGetTickCount() - STA_CONNECT_TIMEOUT;  // Force immediate attempt
        }
        vTaskDelay (pdMS_TO_TICKS (1000));
    }
}

void start_wifi_task (TaskNotifyConfig * config) {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    xTaskCreate (&wifi_task, "wifi_task", 4096, (void *)config, SC_TASK_PRIORITY_NORMAL, NULL);
}

bool is_wifi_connected () {
    return wifi_connected;
}
