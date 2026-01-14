// main/my_ui.h
#ifndef MY_UI_H
#define MY_UI_H

#include "lvgl.h"

// 全局 UI 指针，方便在 MQTT 回调中更新
extern lv_obj_t *ui_time_label;
extern lv_obj_t *ui_count_label;
extern lv_obj_t *ui_task_container;
extern lv_obj_t *ui_tasks[3]; // 最多显示3个任务
extern lv_obj_t *ui_dates[3]; // 对应的截止时间

void init_manual_ui(void);

#endif