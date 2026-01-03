
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "driver/gpio.h"
#include "driver/adc.h"        
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_err.h"

#include "cJSON.h"
#include "esp32-dht11.h"
#include "ssd1306.h"


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "font8x8_basic.h"
#pragma GCC diagnostic pop

#define TAG "LEAF_NODE"
#define Vref 3.3f


#define DHT_PIN             GPIO_NUM_4
#define PIR_PIN             GPIO_NUM_27
#define LDR_ADC_CHANNEL     ADC1_CHANNEL_6   // GPIO34 


static const uint8_t MESH_ID[6] = { 0x7A, 0x10, 0x20, 0x30, 0x40, 0x50 };
#define ROUTER_SSID         "Tinh Hoa"
#define ROUTER_PASS         "TinhHoa978"
#define ROUTER_CHANNEL      0                 // auto

//SoftAP MAC (BSSID) của Relay A/B 
static const uint8_t RELAY_A_BSSID[6] = { 0x88,0x57,0x21,0xB3,0x56,0xF5 };
static const uint8_t RELAY_B_BSSID[6] = { 0x00,0x70,0x07,0x7E,0x6F,0xBD }; 


#define ONLY_USE_RELAY_A      0   // 1 = CHỈ dùng Relay A (không xét B)
#define ENABLE_AUTO_FALLBACK  0   // 1 = nếu không thấy A/B sau N lần -> bật self-organized
#define FALLBACK_WAIT_SCANS  20   // số vòng quét trước khi fallback


static uint8_t           tx_buf[256];
static mesh_data_t       data;
static SSD1306_t         oled;

static volatile bool     g_mesh_connected = false;
static mesh_addr_t       g_root_addr;
static volatile bool     g_root_addr_ok = false;

static mesh_addr_t       g_parent_bssid = {0};
static mesh_addr_t       g_mesh_id_addr = { .addr = {0} };
static bool              g_mesh_started  = false;

static volatile bool     g_reselect_task_running = false;


typedef struct {
    uint8_t bssid[6];
    char    ssid[33];
    uint8_t channel;
    int8_t  rssi;
} mesh_parent_t;

static inline bool bssid_is_nonzero(const uint8_t b[6]) {
    return b[0]|b[1]|b[2]|b[3]|b[4]|b[5];
}

// mở kênh 1-13 và băng thông 20MHz
static void wifi_set_country_1_13(void)
{
    wifi_country_t c = { .cc = "CN", .schan = 1, .nchan = 13, .policy = WIFI_COUNTRY_POLICY_MANUAL };
    ESP_ERROR_CHECK(esp_wifi_set_country(&c));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
}

static void try_set_bandwidth(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return;
    if (mode & WIFI_MODE_STA) esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (mode & WIFI_MODE_AP)  esp_wifi_set_bandwidth(WIFI_IF_AP,  WIFI_BW_HT20);
}

static void pir_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
}

static void log_path(void)
{
    uint8_t layer = esp_mesh_get_layer();
    ESP_LOGI(TAG, "layer=%u, hops=%u, parent=" MACSTR ", root=" MACSTR,
             layer, (layer>0? layer-1:0), MAC2STR(g_parent_bssid.addr), MAC2STR(g_root_addr.addr));
}


static bool find_best_parent(mesh_parent_t *out)
{
    //Scan blocking. task đang gọi sẽ bị “chặn” (block) đến khi firmware quét xong tất cả kênh.
    wifi_scan_config_t sc = { .ssid=0, .bssid=0, .channel=0, .show_hidden=true };
    ESP_ERROR_CHECK(esp_wifi_scan_start(&sc, true));

    //Lấy số lượng AP rồi cấp phát đúng size
    uint16_t ap_num = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_num));
    if (ap_num == 0) {
        ESP_LOGI(TAG, "Scan xong: KHÔNG thấy AP nào");
        return false;
    }

    wifi_ap_record_t *recs = (wifi_ap_record_t*)calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!recs) {
        ESP_LOGE(TAG, "calloc ap records fail");
        return false;
    }

    uint16_t n = ap_num;
    esp_err_t e = esp_wifi_scan_get_ap_records(&n, recs);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records err: %s", esp_err_to_name(e));
        free(recs);
        return false;
    }

    bool needA = bssid_is_nonzero(RELAY_A_BSSID);
    bool needB = (!ONLY_USE_RELAY_A) && bssid_is_nonzero(RELAY_B_BSSID);

    bool seenA = false, seenB = false;
    wifi_ap_record_t recA = {0}, recB = {0};

    for (int i = 0; i < n; ++i) {
        if (needA && !memcmp(recs[i].bssid, RELAY_A_BSSID, 6)) { recA = recs[i]; seenA = true; }
        if (needB && !memcmp(recs[i].bssid, RELAY_B_BSSID, 6)) { recB = recs[i]; seenB = true; }
    }

    if (!seenA && !seenB) {
        ESP_LOGI(TAG, "Scan xong: KHÔNG thấy Relay A/B");
        free(recs);
        return false;
    }

    const wifi_ap_record_t *best = NULL;
    if (seenA && seenB) best = (recA.rssi >= recB.rssi) ? &recA : &recB;
    else if (seenA)     best = &recA;
    else                best = &recB;

    memset(out, 0, sizeof(*out));
    memcpy(out->bssid, best->bssid, 6);
    strlcpy(out->ssid, (const char*)best->ssid, sizeof(out->ssid));
    out->channel = best->primary;
    out->rssi    = best->rssi;

    ESP_LOGI(TAG, "Chọn parent: SSID=\"%s\", BSSID=" MACSTR ", ch=%u, RSSI=%d",
             out->ssid, MAC2STR(out->bssid), out->channel, out->rssi);

    free(recs);
    return true;
}

static void set_parent_to_candidate(const mesh_parent_t *cand)
{
    wifi_config_t p = {0};
    strlcpy((char*)p.sta.ssid,     cand->ssid,          sizeof(p.sta.ssid));
    p.sta.bssid_set = true;
    memcpy(p.sta.bssid, cand->bssid, 6);
    p.sta.channel = cand->channel;

    //OPEN auth cho STA threshold + password trống
    p.sta.threshold.authmode = WIFI_AUTH_OPEN;           // cho phép kết nối AP OPEN
    memset(p.sta.password, 0, sizeof(p.sta.password));   // OPEN => không mật khẩu

    esp_err_t err = esp_mesh_set_parent(&p, &g_mesh_id_addr, MESH_LEAF, 3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mesh_set_parent fail: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "set_parent OK -> " MACSTR " (SSID=%s, ch=%u)",
             MAC2STR(cand->bssid), cand->ssid, cand->channel);
}


static void reselect_parent_task(void *arg) {
    g_reselect_task_running = true;

    mesh_parent_t cand;
    int tries = 0;

    while (!find_best_parent(&cand)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++tries >= FALLBACK_WAIT_SCANS) break;
    }

    if (tries < FALLBACK_WAIT_SCANS) {
        set_parent_to_candidate(&cand);
        if (g_mesh_started && !g_mesh_connected) {
            ESP_LOGW(TAG, "Restart mesh để áp dụng parent mới");
            esp_mesh_stop();
            vTaskDelay(pdMS_TO_TICKS(150));
            esp_mesh_start();
        }
    }
#if ENABLE_AUTO_FALLBACK
    else {
        ESP_LOGW(TAG, "Không thấy Relay A/B sau %d lần -> bật self-organized (fallback) & start mesh", tries);
        esp_mesh_set_self_organized(true, true);
        if (!g_mesh_started) {
            esp_mesh_start();
            g_mesh_started = true;
        }
    }
#endif

    g_reselect_task_running = false;
    vTaskDelete(NULL);
}

static void schedule_reselect_parent(void) {
    if (!g_reselect_task_running) {
        xTaskCreate(reselect_parent_task, "reselect_parent", 4096, NULL, 5, NULL);
    }
}


static void mesh_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    switch (id) {
    case MESH_EVENT_PARENT_CONNECTED: {
        g_mesh_connected = true;
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_parent_bssid(&g_parent_bssid);

        ESP_LOGI(TAG, "PARENT_CONNECTED: " MACSTR ", layer:%d",
                 MAC2STR(g_parent_bssid.addr), connected->self_layer);

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "Parent RSSI: %d dBm", ap_info.rssi);
        }
        try_set_bandwidth();
        log_path(); // dùng để tránh -Wunused-function
        break;
    }
    case MESH_EVENT_PARENT_DISCONNECTED:
        g_mesh_connected = false;
        ESP_LOGW(TAG, "PARENT_DISCONNECTED -> reselect");
        schedule_reselect_parent();
        break;

    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *e = (mesh_event_no_parent_found_t*)event_data;
        ESP_LOGW(TAG, "NO_PARENT_FOUND scan=%d -> reselect", e->scan_times);
        schedule_reselect_parent();
        break;
    }
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *e = (mesh_event_layer_change_t*)event_data;
        ESP_LOGI(TAG, "Layer -> %d", e->new_layer);
        break;
    }
    case MESH_EVENT_ROOT_ADDRESS: {
        const mesh_event_root_address_t *e = (const mesh_event_root_address_t *)event_data;
        memcpy(g_root_addr.addr, e->addr, 6);
        g_root_addr_ok = true;
        ESP_LOGI(TAG, "Root MAC: " MACSTR, MAC2STR(g_root_addr.addr));
        break;
    }
    default:
        break;
    }
}


static void send_sensor_task(void *arg)
{
    while (!g_mesh_connected || !g_root_addr_ok) vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "TX ready: layer=%u, root=" MACSTR, esp_mesh_get_layer(), MAC2STR(g_root_addr.addr));

    for (;;) {
        // DHT11
        struct dht11_reading dht = DHT11_read();
        int temp = 0, hum = 0;
        if (dht.status == DHT11_OK) { temp = dht.temperature; hum = dht.humidity; }
        else                        { ESP_LOGW(TAG, "DHT11 read error"); }

        // PIR
        int motion = gpio_get_level(PIR_PIN);

        // LDR 
        int   raw  = adc1_get_raw(LDR_ADC_CHANNEL);         
        float vout = ((float)raw / 4095.0f) * Vref;        
        ESP_LOGI(TAG, "Light raw=%d, Vout=%.2f V", raw, vout);

        // OLED 
        char line[24];
        ssd1306_clear(&oled);
        ssd1306_display_text(&oled, 0, "Node: Leaf_01", false);
        snprintf(line, sizeof(line), "Temp:%dC", temp);   ssd1306_display_text(&oled, 2, line, false);
        snprintf(line, sizeof(line), "Humi:%d%%", hum);   ssd1306_display_text(&oled, 3, line, false);
        snprintf(line, sizeof(line), "Light:%.2fV", vout);ssd1306_display_text(&oled, 4, line, false);
        snprintf(line, sizeof(line), "Motion:%s", motion ? "YES" : "NO");
        ssd1306_display_text(&oled, 5, line, false);

        // JSON
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "node_id", "Leaf_01");
        cJSON_AddStringToObject(root, "role", "leaf");
        cJSON_AddNumberToObject(root, "temp", temp);
        cJSON_AddNumberToObject(root, "humi", hum);
        cJSON_AddNumberToObject(root, "light_v", vout);
        cJSON_AddNumberToObject(root, "light_raw", raw);
        cJSON_AddNumberToObject(root, "motion", motion);

        char *json_str = cJSON_PrintUnformatted(root);
        size_t len = json_str ? strnlen(json_str, sizeof(tx_buf) - 1) : 0;
        if (json_str && len > 0) {
            memcpy(tx_buf, json_str, len);
            tx_buf[len] = '\0';
        } else {
            const char *fallback = "{\"err\":\"json\"}";
            len = strlen(fallback);
            memcpy(tx_buf, fallback, len + 1);
        }

        data.data  = tx_buf;
        data.size  = len;
        data.proto = MESH_PROTO_BIN;   
        data.tos   = MESH_TOS_P2P;     

      
        mesh_addr_t dest = {0};
        memcpy(dest.addr, g_root_addr.addr, 6);
        esp_err_t err = esp_mesh_send(&dest, &data, MESH_DATA_P2P, NULL, 0);
        if (err == ESP_OK) ESP_LOGI(TAG, "Sent to ROOT " MACSTR ": %s", MAC2STR(dest.addr), (char*)data.data);
        else               ESP_LOGE(TAG, "Mesh send failed: %s (0x%x)", esp_err_to_name(err), err);

        if (json_str) free(json_str);
        cJSON_Delete(root);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}


static void parent_select_and_start_mesh_task(void *arg)
{
    mesh_parent_t cand;
    int tries = 0;

    while (!find_best_parent(&cand)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++tries >= FALLBACK_WAIT_SCANS) break;
    }

    if (tries < FALLBACK_WAIT_SCANS) {
        set_parent_to_candidate(&cand);
        if (!g_mesh_started) {
            ESP_ERROR_CHECK(esp_mesh_start());
            g_mesh_started = true;
        }
    }
#if ENABLE_AUTO_FALLBACK
    else {
        ESP_LOGW(TAG, "Không thấy Relay A/B sau %d lần -> bật self-organized (fallback) & start mesh", tries);
        ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));
        if (!g_mesh_started) {
            ESP_ERROR_CHECK(esp_mesh_start());
            g_mesh_started = true;
        }
    }
#endif

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Leaf node started");
    memcpy(g_mesh_id_addr.addr, MESH_ID, 6);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(NULL, NULL));

    
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // tắt power save để giảm trễ
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_set_country_1_13();
    try_set_bandwidth();

  
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));

    // đang tắt self organized nếu quét k thấy relay bật self organized
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(false, false));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(6));
    ESP_ERROR_CHECK(esp_mesh_set_type(MESH_LEAF));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    cfg.channel = ROUTER_CHANNEL; 
    cfg.router.ssid_len = strlen(ROUTER_SSID);
    strlcpy((char*)cfg.router.ssid,     ROUTER_SSID, sizeof(cfg.router.ssid));
    strlcpy((char*)cfg.router.password, ROUTER_PASS, sizeof(cfg.router.password));
    memcpy(cfg.mesh_id.addr, MESH_ID, 6);

    cfg.mesh_ap.max_connection         = 1;
    cfg.mesh_ap.nonmesh_max_connection = 0;
    memset(cfg.mesh_ap.password, 0, sizeof(cfg.mesh_ap.password)); // OPEN
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_OPEN));
    ESP_LOGW(TAG, "Mesh AP auth=OPEN (no password)");


    xTaskCreate(parent_select_and_start_mesh_task, "parent_select", 6144, NULL, 6, NULL);

  
    DHT11_init(DHT_PIN);
    pir_init();

  
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(LDR_ADC_CHANNEL, ADC_ATTEN_DB_12);

  
    ssd1306_init(&oled);
    ssd1306_clear(&oled);
    ssd1306_display_text(&oled, 0, "Leaf Node Init", false);

   
    data.data = tx_buf;
    xTaskCreate(send_sensor_task, "send_sensor_data", 4096, NULL, 5, NULL);
}
