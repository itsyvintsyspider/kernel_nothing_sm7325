// SPDX-License-Identifier: GPL-2.0-only

#ifndef __NOTHING_TOUCH_H
#define __NOTHING_TOUCH_H

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/mempolicy.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/rtc.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

/* CUR, DEFAULT, MIN, MAX */
#define NOTHING_TOUCH_VALUE_TYPE_SIZE 6
#define NOTHING_TOUCH_GRIP_SIZE 9
#define NOTHING_TOUCH_MAX_BUF 256
#define NOTHING_TOUCH_BTN_INFO 0x152
#define NOTHING_TOUCH_MAX_TOUCH_ID 10
#define NOTHING_TOUCH_RAW_BUF_NUM 4

enum nothing_touch_value_type {
	SET_CUR_VALUE = 0,
	GET_CUR_VALUE,
	GET_DEF_VALUE,
	GET_MIN_VALUE,
	GET_MAX_VALUE,
	GET_MODE_VALUE,
	RESET_MODE,
	SET_LONG_VALUE,
};

enum nothing_touch_mode_type {
	Touch_Game_Mode = 0,
	Touch_Active_MODE = 1,
	Touch_UP_THRESHOLD = 2,
	Touch_Tolerance = 3,
	Touch_Aim_Sensitivity = 4,
	Touch_Tap_Stability = 5,
	Touch_Expert_Mode = 6,
	Touch_Edge_Filter = 7,
	Touch_Panel_Orientation = 8,
	Touch_Report_Rate = 9,
	Touch_Fod_Enable = 10,
	Touch_Aod_Enable = 11,
	Touch_Resist_RF = 12,
	Touch_Idle_Time = 13,
	Touch_Doubletap_Mode = 14,
	Touch_Grip_Mode = 15,
	Touch_Power_Status = 19,
	Touch_Mode_NUM = 20,
};

#define VALUE_TYPE_SIZE     NOTHING_TOUCH_VALUE_TYPE_SIZE
#define VALUE_GRIP_SIZE     NOTHING_TOUCH_GRIP_SIZE
#define MAX_BUF_SIZE        NOTHING_TOUCH_MAX_BUF
#define BTN_INFO            NOTHING_TOUCH_BTN_INFO
#define MAX_TOUCH_ID        NOTHING_TOUCH_MAX_TOUCH_ID
#define RAW_BUF_NUM         NOTHING_TOUCH_RAW_BUF_NUM
#define NOTHING_TOUCH_MODE_COUNT Touch_Mode_NUM

struct nothing_touch_interface {
	int thp_cmd_buf[NOTHING_TOUCH_MAX_BUF];
	int thp_cmd_size;
	int touch_mode[Touch_Mode_NUM][NOTHING_TOUCH_VALUE_TYPE_SIZE];
	int (*setModeValue)(int mode, int value);
	int (*setModeLongValue)(int mode, int value_len, int *value);
	int (*getModeValue)(int mode, int value_type);
	int (*getModeAll)(int mode, int *modevalue);
	int (*resetMode)(int mode);
	int (*prox_sensor_read)(void);
	int (*prox_sensor_write)(int on);
	int (*palm_sensor_read)(void);
	int (*palm_sensor_write)(int on);
	int (*get_touch_rx_num)(void);
	int (*get_touch_tx_num)(void);
	int (*get_touch_x_resolution)(void);
	int (*get_touch_y_resolution)(void);
	int (*enable_touch_raw)(int en);
	int (*enable_clicktouch_raw)(int count);
	int (*enable_touch_delta)(bool en);
	u8 (*panel_vendor_read)(void);
	u8 (*panel_color_read)(void);
	u8 (*panel_display_read)(void);
	char (*touch_vendor_read)(void);

	bool is_enable_touchraw;
	int thp_downthreshold;
	int thp_upthreshold;
	int thp_movethreshold;
	int thp_noisefilter;
	int thp_islandthreshold;
	int thp_smooth;
	int thp_dump_raw;
	bool is_enable_touchdelta;
};

struct nothing_touch {
	struct miscdevice misc_dev;
	struct device *dev;
	struct class *class;
	struct attribute_group attrs;
	struct mutex mutex;
	struct mutex palm_mutex;
	struct mutex prox_mutex;
	wait_queue_head_t wait_queue;
};

#define NOTHING_LAST_TOUCH_EVENTS_MAX 512
#define LAST_TOUCH_EVENTS_MAX NOTHING_LAST_TOUCH_EVENTS_MAX

enum nothing_touch_state {
	EVENT_INIT,
	EVENT_DOWN,
	EVENT_UP,
};

struct nothing_touch_event {
	u32 slot;
	enum nothing_touch_state state;
	struct timespec64 touch_time;
};

struct nothing_last_touch_event {
	int head;
	struct nothing_touch_event touch_event_buf[NOTHING_LAST_TOUCH_EVENTS_MAX];
};

struct nothing_touch_pdata {
	struct nothing_touch *device;
	struct nothing_touch_interface *touch_data[2];
	int suspend_state;
	dma_addr_t phy_base;
	int raw_head;
	int raw_tail;
	int raw_len;
	unsigned int *raw_buf[NOTHING_TOUCH_RAW_BUF_NUM];
	unsigned int *raw_data;
	spinlock_t raw_lock;
	int palm_value;
	bool palm_changed;
	int prox_value;
	bool prox_changed;
	struct proc_dir_entry *last_touch_events_proc;
	struct nothing_last_touch_event *last_touch_events;
};

struct nothing_touch *nothing_touch_dev_get(int minor);

extern struct class *get_nothing_touch_class(void);

extern int update_nothing_palm_sensor_value(int value);

extern int update_nothing_prox_sensor_value(int value);

extern int nothingtouch_register_modedata(int touch_id,
					  struct nothing_touch_interface *data);

#endif /* __NOTHING_TOUCH_H */
