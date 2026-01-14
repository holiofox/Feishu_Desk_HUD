#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_sntp.h"
#include "freertos/semphr.h" // 引入互斥锁

// --- UI 和 BSP 头文件 ---
#include "ui.h"
#include "cJSON.h"
#include "esp_sparkbot_bsp.h"
#include "bsp_board_extra.h"
// -----------------------

// ================== 用户配置区域 (请修改此处) ==================
// [请修改] 替换为您的 EMQX 服务器地址 (例如 mqtts://xxx.emqxsl.cn:8883)
#define EMQX_BROKER_URL    "mqtts://your-broker-address:8883"

// [请修改] 替换为您的 MQTT 用户名
#define EMQX_USERNAME      "your_mqtt_username"

// [请修改] 替换为您的 MQTT 密码
#define EMQX_PASSWORD      "your_mqtt_password"

// [可选] 消息主题
#define EMQX_TOPIC         "feishu/messages/tasks" 
// ============================================================

#define EMQX_CA_PATH       "./emqxsl-ca.crt"
#define MAX_TASKS          10 // 最大缓存任务数

static const char *TAG = "feishu_screen_app";

// --- 全局数据结构 ---
typedef struct {
    char summary[64];
    char due_str[32];
    bool is_valid;
} task_item_t;

static task_item_t g_tasks[MAX_TASKS]; 
static int g_total_tasks = 0;          
static int g_scroll_offset = 0;        
static SemaphoreHandle_t xTaskDataMutex = NULL; 

// 嵌入CA证书内容
// 注意：这是 DigiCert Global Root G2 (EMQX Serverless 常用证书)
// 如果您使用的是其他自建服务器，请替换为相应的 CA 证书
static const char *mqtt_ca_cert =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
    "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
    "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
    "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
    "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
    "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
    "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
    "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
    "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
    "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
    "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
    "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
    "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
    "MrY=\n"
    "-----END CERTIFICATE-----\n";

void safe_strncpy_ellipsis(char *dest, const char *src, size_t size) {
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, size - 1);
    dest[size - 1] = '\0';
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp.aliyun.com");
    sntp_init();
}

void format_timestamp(time_t raw_time, char* buffer, size_t size) {
    struct tm t_info;
    if (raw_time == 0) {
        strcpy(buffer, "00月00日00:00");
        return;
    }
    localtime_r(&raw_time, &t_info);
    strftime(buffer, size, "%m月%d日%H:%M", &t_info);
}

static void update_time_task(void *arg)
{
    char time_buf[32];
    vTaskDelay(pdMS_TO_TICKS(2000)); 
    while (1) {
        time_t now;
        time(&now);
        format_timestamp(now, time_buf, sizeof(time_buf));

        bsp_display_lock(0);
        if (ui_time) {
            lv_label_set_text(ui_time, time_buf);
        }
        bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(10000)); 
    }
}

// --- 更新列表UI ---
static void update_task_list_ui() {
    bsp_display_lock(0);

    // 更新总数
    if (ui_todonumber) {
        char count_buf[16];
        sprintf(count_buf, "%d", g_total_tasks);
        lv_label_set_text(ui_todonumber, count_buf);
    }
    
    if(ui_Spinner2 && lv_obj_has_flag(ui_Spinner2, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(ui_Spinner2, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *labels_info[3] = {ui_task1info, ui_task2info, ui_task3info};
    lv_obj_t *labels_ddl[3] = {ui_task1ddl, ui_task2ddl, ui_task3ddl};

    for (int i = 0; i < 3; i++) {
        int task_idx = -1;

        if (g_total_tasks > 3) {
            // 循环滚动
            task_idx = (g_scroll_offset + i) % g_total_tasks;
        } else {
            // 静态显示
            if (i < g_total_tasks) {
                task_idx = i;
            } else {
                task_idx = -1; 
            }
        }

        if (task_idx >= 0 && task_idx < g_total_tasks && g_tasks[task_idx].is_valid) {
            if (labels_info[i]) lv_label_set_text(labels_info[i], g_tasks[task_idx].summary);
            if (labels_ddl[i])  lv_label_set_text(labels_ddl[i],  g_tasks[task_idx].due_str);
        } else {
            if (i == 0 && g_total_tasks == 0) {
                if (labels_info[i]) lv_label_set_text(labels_info[i], "暂无任务");
                if (labels_ddl[i])  lv_label_set_text(labels_ddl[i],  "00月00日00:00");
            } else {
                if (labels_info[i]) lv_label_set_text(labels_info[i], "");
                if (labels_ddl[i])  lv_label_set_text(labels_ddl[i],  "");
            }
        }
    }

    bsp_display_unlock();
}

// --- 列表滚动控制 ---
static void scroll_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000)); 

        if (xSemaphoreTake(xTaskDataMutex, portMAX_DELAY) == pdTRUE) {
            if (g_total_tasks > 3) {
                g_scroll_offset++;
                if (g_scroll_offset >= g_total_tasks) {
                    g_scroll_offset = 0;
                }
                update_task_list_ui(); 
            } else {
                if (g_scroll_offset != 0) {
                    g_scroll_offset = 0;
                    update_task_list_ui();
                }
            }
            xSemaphoreGive(xTaskDataMutex);
        }
    }
}

static void button_handler(touch_button_handle_t out_handle, touch_button_message_t *out_message, void *arg)
{
    (void) out_handle; 
    if (out_message->event == TOUCH_BUTTON_EVT_ON_PRESS) {
        ESP_LOGI(TAG, "Touch Button Pressed - Switching Screen");
        bsp_display_lock(0);
        lv_obj_t * act_scr = lv_scr_act();
        if (act_scr == ui_Screen1) {
            _ui_screen_change(&ui_Screen2, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_Screen2_screen_init);
        }
        else if (act_scr == ui_Screen2) {
            _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_Screen1_screen_init);
        }
        bsp_display_unlock();
    }
}

// --- MQTT 回调 (解析全量数组 - 彻底解决顺序和删除问题) ---
static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        esp_mqtt_client_subscribe(client, EMQX_TOPIC, 1);
        break;

    case MQTT_EVENT_DATA:
        if (event->topic && strncmp(event->topic, EMQX_TOPIC, event->topic_len) == 0) {
            ESP_LOGI(TAG, "Received Tasks Array");
            
            char *json_str = (char *)malloc(event->data_len + 1);
            if (json_str) {
                memcpy(json_str, event->data, event->data_len);
                json_str[event->data_len] = '\0';
                
                cJSON *root = cJSON_Parse(json_str);
                
                if (xSemaphoreTake(xTaskDataMutex, portMAX_DELAY) == pdTRUE) {
                    // [核心修改 1] 判断是否为数组
                    if (cJSON_IsArray(root)) {
                        ESP_LOGI(TAG, "Processing JSON Array...");
                        
                        // [核心修改 2] 先清空当前所有任务 (解决删除问题)
                        g_total_tasks = 0;
                        g_scroll_offset = 0;
                        memset(g_tasks, 0, sizeof(g_tasks));

                        int array_size = cJSON_GetArraySize(root);
                        // 限制最大数量
                        int count = (array_size > MAX_TASKS) ? MAX_TASKS : array_size;
                        
                        // [核心修改 3] 按顺序填充 (解决排序问题)
                        // 因为服务器已经排好序了，我们只要按顺序读进来即可
                        for (int i = 0; i < count; i++) {
                            cJSON *item = cJSON_GetArrayItem(root, i);
                            if (!item) continue;

                            cJSON *summary = cJSON_GetObjectItem(item, "summary");
                            cJSON *due = cJSON_GetObjectItem(item, "dueTimestamp");

                            // 填充标题
                            safe_strncpy_ellipsis(g_tasks[i].summary, 
                                cJSON_IsString(summary) ? summary->valuestring : "无标题", 64);

                            // 填充时间
                            long long ts_ms = 0;
                            if (cJSON_IsNumber(due)) ts_ms = (long long)due->valuedouble;
                            else if (cJSON_IsString(due)) ts_ms = atoll(due->valuestring);
                            
                            format_timestamp((time_t)(ts_ms / 1000), g_tasks[i].due_str, 32);
                            
                            g_tasks[i].is_valid = true;
                        }
                        
                        // 更新总数
                        g_total_tasks = count;
                        ESP_LOGI(TAG, "Updated List: %d tasks", g_total_tasks);
                    } 
                    else {
                        ESP_LOGW(TAG, "Received payload is not a JSON Array!");
                    }

                    // 刷新UI
                    update_task_list_ui();
                    
                    xSemaphoreGive(xTaskDataMutex);
                }

                cJSON_Delete(root);
                free(json_str);
            }
        }
        break;
    default:
        break;
    }
}

static void mqtt5_app_start(void)
{
    esp_mqtt5_connection_property_config_t connect_property = {
        .session_expiry_interval = 10,
        .maximum_packet_size = 1024,
        .receive_maximum = 65535,
        .topic_alias_maximum = 2,
        .request_resp_info = true,
        .request_problem_info = true,
        .will_delay_interval = 10,
        .payload_format_indicator = true,
        .message_expiry_interval = 10,
        .response_topic = "/test/response",
        .correlation_data = "123456",
        .correlation_data_len = 6,
    };

    esp_mqtt_client_config_t mqtt5_cfg = {
        // [修改] 使用宏定义的 Broker URL，而不是隐含的 CONFIG_BROKER_URL
        .broker.address.uri = EMQX_BROKER_URL, 
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .network.disable_auto_reconnect = true,
        .credentials.username = EMQX_USERNAME,
        .credentials.authentication.password = EMQX_PASSWORD,
        .session.last_will.topic = "/topic/will",
        .session.last_will.msg = "i will leave",
        .session.last_will.msg_len = 12,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .broker.verification.certificate = mqtt_ca_cert,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt5_cfg);
    esp_mqtt5_client_set_user_property(&connect_property.user_property, NULL, 0);
    esp_mqtt5_client_set_user_property(&connect_property.will_user_property, NULL, 0);
    esp_mqtt5_client_set_connect_property(client, &connect_property);
    esp_mqtt5_client_delete_user_property(connect_property.user_property);
    esp_mqtt5_client_delete_user_property(connect_property.will_user_property);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    
    xTaskDataMutex = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    bsp_i2c_init();
    bsp_display_cfg_t custom_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .trans_size = BSP_LCD_H_RES * 10,
        .double_buffer = 0,
        .flags = { .buff_dma = false, .buff_spiram = true }
    };
    custom_cfg.lvgl_port_cfg.task_stack = 1024 * 30;
    custom_cfg.lvgl_port_cfg.task_affinity = 1;

    bsp_display_start_with_config(&custom_cfg);
    bsp_display_backlight_on();

    bsp_display_lock(0);
    ui_init(); 
    
    if(ui_task1info) lv_label_set_text(ui_task1info, "暂无任务");
    if(ui_task1ddl)  lv_label_set_text(ui_task1ddl,  "00月00日00:00");
    if(ui_task2info) lv_label_set_text(ui_task2info, ""); 
    if(ui_task2ddl)  lv_label_set_text(ui_task2ddl,  "");
    if(ui_task3info) lv_label_set_text(ui_task3info, "");
    if(ui_task3ddl)  lv_label_set_text(ui_task3ddl,  "");
    if(ui_Spinner2 && lv_obj_has_flag(ui_Spinner2, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(ui_Spinner2, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();

    bsp_touch_button_create(button_handler);

    // 连接 WiFi (通常在 menuconfig 中配置 SSID/密码，或在此处硬编码)
    // 确保您已在 sdkconfig 中配置了 WiFi 或修改 protocol_examples_common.h
    ESP_ERROR_CHECK(example_connect());
    initialize_sntp();

    mqtt5_app_start();
    
    xTaskCreate(update_time_task, "time_task", 2048, NULL, 5, NULL);
    xTaskCreate(scroll_task, "scroll_task", 2048, NULL, 5, NULL);
}