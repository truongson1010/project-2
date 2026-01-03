
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mesh.h"

//#define TAG "RELAY_NODE_A"
#define TAG "RELAY_NODE_B"

static const uint8_t MESH_ID[6] = { 0x7A,0x10,0x20,0x30,0x40,0x50 };
#define ROUTER_SSID     "Tinh Hoa"
#define ROUTER_PASS     "TinhHoa978"
#define ROUTER_CHANNEL  0          // 0 = auto

static esp_netif_t *g_mesh_netif_sta = NULL;
static esp_netif_t *g_mesh_netif_ap  = NULL;
static volatile bool g_mesh_connected = false;
static mesh_addr_t   g_parent_bssid = {0};
static mesh_addr_t   g_root_addr    = {0};
static volatile bool g_have_root    = false;


static void wifi_country_1_13(void) {
    wifi_country_t c = { .cc = "CN", .schan = 1, .nchan = 13, .policy = WIFI_COUNTRY_POLICY_MANUAL };
    esp_wifi_set_country(&c);
    esp_wifi_set_ps(WIFI_PS_NONE);
}
static void try_set_bw20(void) {
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) != ESP_OK) return;
    if (m & WIFI_MODE_STA) esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (m & WIFI_MODE_AP)  esp_wifi_set_bandwidth(WIFI_IF_AP,  WIFI_BW_HT20);
}


static void mesh_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    switch (id) {
        case MESH_EVENT_STARTED: {
            ESP_LOGI(TAG, "Mesh started");
            try_set_bw20();
            break;
        }
        case MESH_EVENT_PARENT_CONNECTED: {
            g_mesh_connected = true;
            mesh_event_connected_t *e = (mesh_event_connected_t*)event_data;
            esp_mesh_get_parent_bssid(&g_parent_bssid);
            ESP_LOGI(TAG, "Parent " MACSTR ", layer=%d, is_root=%s",
                     MAC2STR(g_parent_bssid.addr), e->self_layer,
                     esp_mesh_is_root() ? "YES" : "NO");
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                ESP_LOGI(TAG, "Parent RSSI: %d dBm", ap_info.rssi);
            }
            try_set_bw20();
            break;
        }
        case MESH_EVENT_PARENT_DISCONNECTED: {
            g_mesh_connected = false;
            ESP_LOGW(TAG, "Parent disconnected");
            break;
        }
        case MESH_EVENT_ROUTING_TABLE_ADD: {
            mesh_event_routing_table_change_t *e = (mesh_event_routing_table_change_t*)event_data;
            ESP_LOGI(TAG, "+RT: %d, total=%d", e->rt_size_change, e->rt_size_new);
            break;
        }
        case MESH_EVENT_ROUTING_TABLE_REMOVE: {
            mesh_event_routing_table_change_t *e = (mesh_event_routing_table_change_t*)event_data;
            ESP_LOGI(TAG, "-RT: %d, total=%d", e->rt_size_change, e->rt_size_new);
            break;
        }
        case MESH_EVENT_ROOT_ADDRESS: {
            const mesh_event_root_address_t *ra = (const mesh_event_root_address_t*)event_data;
            memcpy(g_root_addr.addr, ra->addr, 6);
            g_have_root = true;
            ESP_LOGI(TAG, "Root MAC: " MACSTR, MAC2STR(g_root_addr.addr));
            break;
        }
        default:
            break;
    }
}



static void mesh_sniff_task(void *arg) {
    mesh_addr_t from;
    uint8_t rx_buf[256];
    mesh_data_t rx = {
        .data  = rx_buf,
        .size  = sizeof(rx_buf),
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P
    };
    int flag = 0;

    for (;;) {
        rx.size = sizeof(rx_buf);
        esp_err_t err = esp_mesh_recv(&from, &rx, 1000 / portTICK_PERIOD_MS, &flag, NULL, 0);
        if (err == ESP_OK) {
            size_t n = (rx.size < sizeof(rx_buf)) ? rx.size : sizeof(rx_buf)-1;
            rx_buf[n] = '\0';
            ESP_LOGI(TAG, "RX (child=" MACSTR "): %s", MAC2STR(from.addr), (char*)rx.data);
            flag = 0;
        }
    }
}


static void mesh_apply_config(void) {
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy(cfg.mesh_id.addr, MESH_ID, 6);

    cfg.channel = ROUTER_CHANNEL; 
    cfg.router.ssid_len = strlen(ROUTER_SSID);
    strlcpy((char*)cfg.router.ssid,     ROUTER_SSID, sizeof(cfg.router.ssid));
    strlcpy((char*)cfg.router.password, ROUTER_PASS, sizeof(cfg.router.password));

   
    cfg.mesh_ap.max_connection         = 6;
    cfg.mesh_ap.nonmesh_max_connection = 0; // chặn STA ngoài mesh
    memset(cfg.mesh_ap.password, 0, sizeof(cfg.mesh_ap.password));
   

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_OPEN));
    ESP_LOGW(TAG, "Mesh AP auth=OPEN (no password), không khuyến nghị production");
}

void app_main(void) {
    ESP_LOGI(TAG, "RELAY node start");

 
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }


    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&g_mesh_netif_sta, &g_mesh_netif_ap));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    wifi_country_1_13();
    ESP_ERROR_CHECK(esp_wifi_start());


    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));


    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(6));
    mesh_apply_config();

    ESP_ERROR_CHECK(esp_mesh_start());
    try_set_bw20();

    uint8_t sta_mac[6], ap_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
    esp_wifi_get_mac(WIFI_IF_AP,  ap_mac);
    ESP_LOGI(TAG, "RELAY STA MAC : " MACSTR, MAC2STR(sta_mac));
    ESP_LOGI(TAG, "RELAY BSSID   : " MACSTR " (Mesh SoftAP)", MAC2STR(ap_mac));

    xTaskCreate(mesh_sniff_task, "mesh_sniff", 4096, NULL, 4, NULL);
}
