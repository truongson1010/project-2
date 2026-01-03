
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "mqtt_client.h"

#define TAG "ROOT_NODE"

// MESH ID
static const uint8_t MESH_ID[6] = { 0x7A,0x10,0x20,0x30,0x40,0x50 };
#define ROUTER_SSID     "Tinh Hoa"
#define ROUTER_PASS     "TinhHoa978"
#define ROUTER_CHANNEL  0          // 0 = auto, theo router


#define MQTT_URI        "mqtt://192.168.1.9:1883"  
#define MQTT_USERNAME   NULL                 
#define MQTT_PASSWORD   NULL
#define MQTT_BASE_TOPIC "mesh"                  

static esp_netif_t *g_mesh_netif_sta = NULL;
static esp_netif_t *g_mesh_netif_ap  = NULL;
static esp_mqtt_client_handle_t g_mqtt = NULL;
static bool g_mqtt_connected = false;

// cho phép kết nối từ kênh 1 đến 13
static void wifi_country_1_13(void) {
    wifi_country_t c = { .cc = "CN", .schan = 1, .nchan = 13, .policy = WIFI_COUNTRY_POLICY_MANUAL };
    esp_wifi_set_country(&c);
    esp_wifi_set_ps(WIFI_PS_NONE);
}
// ép chạy ở băng thông 20MHz chạy xa hơn và ít nhiễu hơn 40MHz
static void try_set_bw20(void) {
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) != ESP_OK) return;
    if (m & WIFI_MODE_STA) esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (m & WIFI_MODE_AP)  esp_wifi_set_bandwidth(WIFI_IF_AP,  WIFI_BW_HT20);
}

static void mqtt_evt_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            g_mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT: CONNECTED");
            break;
        case MQTT_EVENT_DISCONNECTED:
            g_mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT: DISCONNECTED");
            break;
        default:
            break;
    }
}

static void mqtt_start_if_needed(void) {
    if (g_mqtt) return; 
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
    };
    g_mqtt = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(g_mqtt, ESP_EVENT_ANY_ID, mqtt_evt_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(g_mqtt));
    ESP_LOGI(TAG, "MQTT: connecting to %s", MQTT_URI);
}


static void ip_evt_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "GOT IP: " IPSTR ", GW: " IPSTR ", MASK: " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw), IP2STR(&ev->ip_info.netmask));
        mqtt_start_if_needed(); 
    }
}


static void mesh_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    switch (id) {
        case MESH_EVENT_STARTED: {
            ESP_LOGI(TAG, "Mesh started (ROOT)");
            break;
        }
        case MESH_EVENT_PARENT_CONNECTED: {
            mesh_event_connected_t *e = (mesh_event_connected_t*)event_data;
            ESP_LOGI(TAG, "ROOT parent connected? layer=%d is_root=%s", e->self_layer, esp_mesh_is_root()?"YES":"NO");
            if (esp_mesh_is_root()) {
                // Đảm bảo DHCP Client chạy trên mesh STA để lấy IP
                esp_netif_dhcpc_stop(g_mesh_netif_sta);    // dừng trước, tránh EALREADY
                esp_netif_dhcpc_start(g_mesh_netif_sta);   // khởi động lại
           
            }
            try_set_bw20();
            break;
        }
        case MESH_EVENT_CHILD_CONNECTED: {
            mesh_event_child_connected_t *e = (mesh_event_child_connected_t*)event_data;
            ESP_LOGI(TAG, "Child + " MACSTR ", aid=%d", MAC2STR(e->mac), e->aid);
            break;
        }
        case MESH_EVENT_CHILD_DISCONNECTED: {
            mesh_event_child_disconnected_t *e = (mesh_event_child_disconnected_t*)event_data;
            ESP_LOGW(TAG, "Child - " MACSTR ", aid=%d", MAC2STR(e->mac), e->aid);
            break;
        }
        default:
            break;
    }
}


static void mesh_recv_task(void *arg) {
    mesh_addr_t from;
    uint8_t rx_buf[512];
    mesh_data_t rx = {
        .data  = rx_buf,
        .size  = sizeof(rx_buf),
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_DEF
    };
    int flag = 0;
    char topic[128];

    for(;;){
        rx.size = sizeof(rx_buf);
        esp_err_t err = esp_mesh_recv(&from, &rx, portMAX_DELAY, &flag, NULL, 0);
        if (err == ESP_OK) {
            size_t n = (rx.size < sizeof(rx_buf)) ? rx.size : sizeof(rx_buf)-1;
            rx_buf[n] = '\0';
            snprintf(topic, sizeof(topic), "%s/%02x:%02x:%02x:%02x:%02x:%02x",
                     MQTT_BASE_TOPIC,
                     from.addr[0], from.addr[1], from.addr[2],
                     from.addr[3], from.addr[4], from.addr[5]);

            ESP_LOGI(TAG, "RX %uB from " MACSTR " -> MQTT [%s]", (unsigned)rx.size, MAC2STR(from.addr), topic);

            if (g_mqtt_connected && g_mqtt) {
                int msg_id = esp_mqtt_client_publish(g_mqtt, topic, (const char*)rx.data, (int)rx.size, 0, 0);
                if (msg_id < 0) {
                    ESP_LOGW(TAG, "MQTT publish failed");
                }
            } else {
                ESP_LOGW(TAG, "MQTT not connected — skip publish");
            }
            flag = 0;
        }
    }
}

static void mesh_apply_config_open(void) {
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy(cfg.mesh_id.addr, MESH_ID, 6);
    cfg.channel = ROUTER_CHANNEL; // 0 = auto

    cfg.router.ssid_len = strlen(ROUTER_SSID);
    strlcpy((char*)cfg.router.ssid,     ROUTER_SSID, sizeof(cfg.router.ssid));
    strlcpy((char*)cfg.router.password, ROUTER_PASS, sizeof(cfg.router.password));

    cfg.mesh_ap.max_connection         = 2;   // 2 children
    cfg.mesh_ap.nonmesh_max_connection = 0;   // chặn STA ngoài mesh
    memset(cfg.mesh_ap.password, 0, sizeof(cfg.mesh_ap.password)); // OPEN

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_OPEN)); // chỉ cần mesh id để vô chứ không cần mật khẩu wifi nội bộ
    ESP_LOGW(TAG, "Mesh AP auth=OPEN (no password)");
}

void app_main(void) {
    ESP_LOGI(TAG, "ROOT node start");

  
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

   
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_evt_handler, NULL));

    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&g_mesh_netif_sta, &g_mesh_netif_ap));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    wifi_country_1_13();
    ESP_ERROR_CHECK(esp_wifi_start());

 
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));

    // Cố định ROOT: tắt self-organized trước khi set type
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(false, false));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(6));
    ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));

    mesh_apply_config_open();


    ESP_ERROR_CHECK(esp_mesh_start());
    try_set_bw20();

  
    uint8_t sta_mac[6], ap_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
    esp_wifi_get_mac(WIFI_IF_AP,  ap_mac);
    ESP_LOGI(TAG, "ROOT STA MAC : " MACSTR, MAC2STR(sta_mac));
    ESP_LOGI(TAG, "ROOT BSSID   : " MACSTR " (Mesh SoftAP)", MAC2STR(ap_mac));


    xTaskCreate(mesh_recv_task, "mesh_recv", 6144, NULL, 4, NULL);
}
