#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "mqtt_client.h"
#include "esp_sntp.h"
#include "cJSON.h"

// 硬件驱动引用 (厂商提供的驱动)
#include "user_app.h"
#include "user_config.h"
#include "lvgl.h"
#include "driver/gpio.h" // 记得引入头文件

static const char *TAG = "EPAPER_MAIN";
static SemaphoreHandle_t lvgl_mux = NULL;

// ================== 配置区域 ==================
// [请修改] 替换为你的 EMQX 服务器地址
#define EMQX_BROKER_URL "mqtts://your-emqx-server-address:8883"
#define EMQX_USERNAME   "your_mqtt_username" // [请修改] 你的MQTT用户名
#define EMQX_PASSWORD   "your_mqtt_password" // [请修改] 你的MQTT密码
#define EMQX_TOPIC      "feishu/messages/tasks"

// 嵌入证书声明
extern const uint8_t mqtt_ca_pem_start[] asm("_binary_mqtt_ca_crt_start");
extern const uint8_t mqtt_ca_pem_end[]   asm("_binary_mqtt_ca_crt_end");

// 声明外部中文字体 (必须在 extern "C" 中)
extern "C" {
    LV_FONT_DECLARE(ui_font_FontCN16);
}

// ================== 全局 UI 对象 ==================
lv_obj_t *ui_time_label = NULL;
lv_obj_t *ui_count_label = NULL;
lv_obj_t *ui_tasks[3] = {NULL};
lv_obj_t *ui_dates[3] = {NULL};

// ================== 1. 手写 UI 初始化函数 ==================
void init_manual_ui(void) {
    // 1. 设置背景纯白
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    
    // 2. 顶部状态栏 - 时间
    ui_time_label = lv_label_create(scr);
    // 使用中文字体以支持可能的中文日期格式，且字体大小合适
    lv_obj_set_style_text_font(ui_time_label, &ui_font_FontCN16, 0); 
    lv_label_set_text(ui_time_label, "连接中...");
    lv_obj_set_style_text_color(ui_time_label, lv_color_black(), 0);
    // 调整位置，稍微留出边距
    lv_obj_align(ui_time_label, LV_ALIGN_TOP_LEFT, 2, 5); 

    // 2. 顶部状态栏 - 计数
    ui_count_label = lv_label_create(scr);
    lv_obj_set_style_text_font(ui_count_label, &ui_font_FontCN16, 0); 
    lv_label_set_text(ui_count_label, "0");
    lv_obj_set_style_text_color(ui_count_label, lv_color_black(), 0);
    lv_obj_align(ui_count_label, LV_ALIGN_TOP_RIGHT, -5, 5);

    // 分割线
    lv_obj_t *line = lv_line_create(scr);
    static lv_point_t line_points[] = { {0, 25}, {200, 25} };
    lv_line_set_points(line, line_points, 2);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, lv_color_black(), 0);

    // 3. 创建3个任务槽位
    for(int i=0; i<3; i++) {
        // 任务标题
        ui_tasks[i] = lv_label_create(scr);
        lv_label_set_long_mode(ui_tasks[i], LV_LABEL_LONG_DOT); // 超长显示省略号
        lv_obj_set_width(ui_tasks[i], 190);
        lv_obj_set_style_text_color(ui_tasks[i], lv_color_black(), 0);
        
        // 【关键】设置中文字体，否则显示方框
        lv_obj_set_style_text_font(ui_tasks[i], &ui_font_FontCN16, 0); 
        
        lv_obj_align(ui_tasks[i], LV_ALIGN_TOP_LEFT, 5, 35 + (i * 55)); // 垂直间隔
        lv_label_set_text(ui_tasks[i], "等待数据...");

        // 截止时间 (放在标题下方)
        ui_dates[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(ui_dates[i], lv_color_black(), 0);
        
        // 【关键】设置中文字体
        lv_obj_set_style_text_font(ui_dates[i], &ui_font_FontCN16, 0);
        
        lv_obj_align(ui_dates[i], LV_ALIGN_TOP_LEFT, 5, 35 + (i * 55) + 20);
        lv_label_set_text(ui_dates[i], "--/-- --:--");
    }
}

// ================== 2. 逻辑辅助函数 ==================
static bool example_lvgl_lock(int timeout_ms) {
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

static void example_lvgl_unlock(void) {
    xSemaphoreGive(lvgl_mux);
}

void format_timestamp(time_t raw_time, char* buffer, size_t size) {
    if (raw_time == 0) {
        snprintf(buffer, size, "无截止");
        return;
    }
    struct tm t_info;
    localtime_r(&raw_time, &t_info);
    // 格式化为：月-日 时:分
    strftime(buffer, size, "截止: %m-%d %H:%M", &t_info);
}

// ================== 3. UI 更新逻辑 ==================
void update_ui_from_json(cJSON *root) {
    if (example_lvgl_lock(-1)) {
        if (cJSON_IsArray(root)) {
            int total = cJSON_GetArraySize(root);
            
            // 更新总数
            if(ui_count_label) {
                char buf[32];
                sprintf(buf, "待办: %d", total);
                lv_label_set_text(ui_count_label, buf);
            }

            // 更新列表
            for(int i=0; i<3; i++) {
                if (i < total) {
                    cJSON *item = cJSON_GetArrayItem(root, i);
                    cJSON *summary = cJSON_GetObjectItem(item, "summary");
                    cJSON *due = cJSON_GetObjectItem(item, "dueTimestamp");
                    
                    char due_str[32];
                    long long ts = 0;
                    if (due) {
                        ts = cJSON_IsString(due) ? atoll(due->valuestring) : (long long)due->valuedouble;
                    }
                    format_timestamp((time_t)(ts/1000), due_str, 32);

                    if(ui_tasks[i]) lv_label_set_text(ui_tasks[i], summary ? summary->valuestring : "");
                    if(ui_dates[i]) lv_label_set_text(ui_dates[i], due_str);
                } else {
                    if(ui_tasks[i]) lv_label_set_text(ui_tasks[i], "");
                    if(ui_dates[i]) lv_label_set_text(ui_dates[i], "");
                }
            }
        }
        example_lvgl_unlock();
    }
}

// ================== 4. MQTT & Time & Drivers ==================

// 驱动刷新回调 (保留原厂逻辑，适配墨水屏)
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    uint16_t *buffer = (uint16_t *)color_map;
    driver->EPD_Clear(); // 墨水屏特性：先清空显存
    for(int y = area->y1; y <= area->y2; y++) {
        for(int x = area->x1; x <= area->x2; x++) {
            // 简单的二值化处理: 大于阈值设为黑，否则白
            uint8_t color = (*buffer < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
            driver->EPD_DrawColorPixel(x,y,color);
            buffer++;
        }
    }
    driver->EPD_DisplayPart(); // 局部刷新
    lv_disp_flush_ready(drv);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        esp_mqtt_client_subscribe(event->client, EMQX_TOPIC, 1);
        ESP_LOGI(TAG, "MQTT Connected");
    } else if (event_id == MQTT_EVENT_DATA) {
        ESP_LOGI(TAG, "Data Received");
        char *json_str = (char *)malloc(event->data_len + 1);
        if (json_str) {
            memcpy(json_str, event->data, event->data_len);
            json_str[event->data_len] = 0;
            cJSON *root = cJSON_Parse(json_str);
            if (root) {
                update_ui_from_json(root);
                cJSON_Delete(root);
            }
            free(json_str);
        }
    }
}

static void update_time_task(void *arg) {
    // 增加buffer大小以容纳日期 "MM-DD HH:MM"
    char time_buf[32]; 
    while (1) {
        time_t now;
        time(&now);
        struct tm t_info;
        localtime_r(&now, &t_info);
        
        // 格式化为：01-15 12:30
        strftime(time_buf, sizeof(time_buf), "%m-%d %H:%M", &t_info);
        
        if (example_lvgl_lock(-1)) {
            if(ui_time_label) lv_label_set_text(ui_time_label, time_buf);
            example_lvgl_unlock();
        }
        // 10秒刷新一次，确保分钟变化及时显示
        vTaskDelay(pdMS_TO_TICKS(10000)); 
    }
}

static void example_increase_lvgl_tick(void *arg) {
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static void example_lvgl_port_task(void *arg) {
    uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    for(;;) {
        if (example_lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            example_lvgl_unlock();
        }
        if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

extern "C" void app_main(void) {

    gpio_config_t power_conf = {};
    power_conf.pin_bit_mask = (1ULL << 17); // 配置 GPIO 17
    power_conf.mode = GPIO_MODE_OUTPUT;
    power_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    power_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&power_conf);
    
    gpio_set_level(GPIO_NUM_17, 1); // 输出高电平，锁定电源
	
    // 1. 基础系统初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. 硬件驱动初始化 (来自 user_app.h)
    user_app_init(); 

    // 3. LVGL 初始化
    lv_init();
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;
    
    // PSRAM 分配显存
    lv_color_t *buffer_1 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN , MALLOC_CAP_SPIRAM);
    lv_color_t *buffer_2 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN , MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&disp_buf, buffer_1, buffer_2, EPD_WIDTH * EPD_HEIGHT);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EPD_WIDTH;
    disp_drv.ver_res = EPD_HEIGHT;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.full_refresh = 1; // 墨水屏通常需要 full refresh 标记
    lv_disp_drv_register(&disp_drv);

    // 4. 定时器与任务
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .arg = NULL,           
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = false  
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    lvgl_mux = xSemaphoreCreateMutex();
    
    // 5. 构建 UI (手动 + 中文字体)
    if(example_lvgl_lock(-1)) {
        init_manual_ui();
        example_lvgl_unlock();
    }
    
    // 6. 启动 LVGL 线程
    xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", 8 * 1024, NULL, 4, NULL, 1);

    // 7. 网络连接
    ESP_ERROR_CHECK(example_connect()); // 连接 WiFi
    
    // 8. 校时 (使用新 API + 时区设置)
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    
    // 【关键】设置中国时区 (CST-8 = UTC+8)
    setenv("TZ", "CST-8", 1);
    tzset();
    
    // 启动时间刷新任务
    xTaskCreate(update_time_task, "time_task", 2048, NULL, 5, NULL);

    // 9. 启动 MQTT
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = EMQX_BROKER_URL;
    mqtt_cfg.credentials.username = EMQX_USERNAME;
    mqtt_cfg.credentials.authentication.password = EMQX_PASSWORD;
    
    // 【关键】修复 MQTT 证书配置字段
    mqtt_cfg.broker.verification.certificate = (const char *)mqtt_ca_pem_start;

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}