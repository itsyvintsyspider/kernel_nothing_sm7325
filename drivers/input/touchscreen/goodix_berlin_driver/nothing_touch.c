// SPDX-License-Identifier: GPL-2.0-only

#include "nothing_touch.h"

static struct nothing_touch_pdata *touch_pdata;
static struct nothing_touch *nothing_touch_device;
static struct nothing_touch nothing_touch_dev;

#define RAW_SIZE (PAGE_SIZE * 12)

static int nothing_touch_dev_open(struct inode *inode, struct file *file)
{
	struct nothing_touch *dev = NULL;
	int i = MINOR(inode->i_rdev);
	struct nothing_touch_pdata *touch_pdata;

	pr_info("%s\n", __func__);
	dev = nothing_touch_dev_get(i);
	if (!dev) {
		pr_err("%s cant get dev\n", __func__);
		return -ENOMEM;
	}
	touch_pdata = dev_get_drvdata(dev->dev);

	file->private_data = touch_pdata;
	return 0;
}

static ssize_t nothing_touch_dev_read(struct file *file, char __user *buf,
				      size_t count, loff_t *pos)
{
	return 0;
}

static ssize_t nothing_touch_dev_write(struct file *file, const char __user *buf,
				       size_t count, loff_t *pos)
{
	return 0;
}

static unsigned int nothing_touch_dev_poll(struct file *file, poll_table *wait)
{
	return 0;
}

static long nothing_touch_dev_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	int ret = -EINVAL;
	int buf[NOTHING_TOUCH_MAX_BUF] = { 0, };
	struct nothing_touch_pdata *pdata = file->private_data;
	void __user *argp = (void __user *)arg;
	struct nothing_touch_interface *touch_data = NULL;
	struct nothing_touch *dev = pdata->device;
	int user_cmd = _IOC_NR(cmd);

	mutex_lock(&dev->mutex);
	ret = copy_from_user(&buf, (int __user *)argp, sizeof(buf));
	if (buf[0] < 0 || buf[0] > 1) {
		pr_err("%s invalid param\n", __func__);
		mutex_unlock(&dev->mutex);
		return -EINVAL;
	}

	touch_data = pdata->touch_data[buf[0]];
	if (!pdata || !touch_data || !dev) {
		pr_err("%s invalid memory\n", __func__);
		mutex_unlock(&dev->mutex);
		return -ENOMEM;
	}

	pr_info("%s cmd:%d, touchId:%d, mode:%d, value:%d\n", __func__,
		user_cmd, buf[0], buf[1], buf[2]);

	switch (user_cmd) {
	case SET_CUR_VALUE:
		if (touch_data->setModeValue)
			buf[0] = touch_data->setModeValue(buf[1], buf[2]);
		break;
	case GET_CUR_VALUE:
	case GET_DEF_VALUE:
	case GET_MIN_VALUE:
	case GET_MAX_VALUE:
		if (touch_data->getModeValue)
			buf[0] = touch_data->getModeValue(buf[1], user_cmd);
		break;
	case RESET_MODE:
		if (touch_data->resetMode)
			buf[0] = touch_data->resetMode(buf[1]);
		break;
	case GET_MODE_VALUE:
		if (touch_data->getModeValue)
			ret = touch_data->getModeAll(buf[1], buf);
		break;
	case SET_LONG_VALUE:
		if (touch_data->setModeLongValue && buf[2] <= NOTHING_TOUCH_MAX_BUF)
			ret = touch_data->setModeLongValue(buf[1], buf[2],
							   &buf[3]);
		break;
	default:
		pr_err("%s don't support mode\n", __func__);
		ret = -EINVAL;
		break;
	}

	if (user_cmd == SET_CUR_VALUE) {
		touch_data->thp_cmd_buf[0] = user_cmd;
		touch_data->thp_cmd_buf[1] = buf[0];
		touch_data->thp_cmd_buf[2] = buf[1];
		touch_data->thp_cmd_buf[3] = buf[2];
		touch_data->thp_cmd_size = 4;
		sysfs_notify(&nothing_touch_device->dev->kobj, NULL,
			     "touch_thp_cmd");
	} else if (user_cmd == SET_LONG_VALUE) {
		touch_data->thp_cmd_buf[0] = user_cmd;
		touch_data->thp_cmd_buf[1] = buf[0];
		touch_data->thp_cmd_buf[2] = buf[1];
		touch_data->thp_cmd_buf[3] = buf[2];
		memcpy(&(touch_data->thp_cmd_buf[4]), &buf[3],
		       sizeof(int) * buf[2]);
		touch_data->thp_cmd_size = 4 + buf[2];
		sysfs_notify(&nothing_touch_device->dev->kobj, NULL,
			     "touch_thp_cmd");
	} else if (user_cmd == RESET_MODE) {
		touch_data->thp_cmd_buf[0] = user_cmd;
		touch_data->thp_cmd_buf[1] = buf[0];
		touch_data->thp_cmd_buf[2] = buf[1];
		touch_data->thp_cmd_size = 3;
		sysfs_notify(&nothing_touch_device->dev->kobj, NULL,
			     "touch_thp_cmd");
	}

	if (ret >= 0)
		ret = copy_to_user((int __user *)argp, &buf, sizeof(buf));
	else
		pr_err("%s can't get data from touch driver\n", __func__);

	mutex_unlock(&dev->mutex);

	return ret;
}

static int nothing_touch_dev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct nothing_touch_pdata *pdata = file->private_data;
	unsigned long start = vma->vm_start;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long page;
	unsigned long pos;

	if (!pdata) {
		pr_err("%s invalid memory\n", __func__);
		return -ENOMEM;
	}

	pos = (unsigned long)pdata->phy_base + offset;
	page = pos >> PAGE_SHIFT;

	if (remap_pfn_range(vma, start, page, size, PAGE_SHARED))
		return -EAGAIN;
	else
		pr_info("%s remap_pfn_range %u, size:%ld, success\n", __func__,
			(unsigned int)page, size);

	return 0;
}

static int nothing_touch_dev_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations nothingtouch_dev_fops = {
	.owner = THIS_MODULE,
	.open = nothing_touch_dev_open,
	.read = nothing_touch_dev_read,
	.write = nothing_touch_dev_write,
	.poll = nothing_touch_dev_poll,
	.mmap = nothing_touch_dev_mmap,
	.unlocked_ioctl = nothing_touch_dev_ioctl,
	.compat_ioctl = nothing_touch_dev_ioctl,
	.release = nothing_touch_dev_release,
	.llseek = no_llseek,
};

static struct nothing_touch nothing_touch_dev = {
	.misc_dev = {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "nothing-touch",
		.fops = &nothingtouch_dev_fops,
		.parent = NULL,
	},
	.mutex = __MUTEX_INITIALIZER(nothing_touch_dev.mutex),
	.palm_mutex = __MUTEX_INITIALIZER(nothing_touch_dev.palm_mutex),
	.prox_mutex = __MUTEX_INITIALIZER(nothing_touch_dev.prox_mutex),
	.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(nothing_touch_dev.wait_queue),
};

struct nothing_touch *nothing_touch_dev_get(int minor)
{
	if (nothing_touch_dev.misc_dev.minor == minor)
		return &nothing_touch_dev;
	else
		return NULL;
}
EXPORT_SYMBOL_GPL(nothing_touch_dev_get);

struct class *get_nothing_touch_class(void)
{
	return nothing_touch_dev.class;
}
EXPORT_SYMBOL_GPL(get_nothing_touch_class);

int nothingtouch_register_modedata(int touch_id,
				   struct nothing_touch_interface *data)
{
	struct nothing_touch_interface *touch_data = NULL;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[touch_id];
	pr_info("%s\n", __func__);

	mutex_lock(&nothing_touch_dev.mutex);

	if (data->setModeValue)
		touch_data->setModeValue = data->setModeValue;
	if (data->getModeValue)
		touch_data->getModeValue = data->getModeValue;
	if (data->resetMode)
		touch_data->resetMode = data->resetMode;
	if (data->getModeAll)
		touch_data->getModeAll = data->getModeAll;
	if (data->setModeLongValue)
		touch_data->setModeLongValue = data->setModeLongValue;
	if (data->palm_sensor_read)
		touch_data->palm_sensor_read = data->palm_sensor_read;
	if (data->enable_touch_raw)
		touch_data->enable_touch_raw = data->enable_touch_raw;
	if (data->enable_touch_delta)
		touch_data->enable_touch_delta = data->enable_touch_delta;
	if (data->get_touch_rx_num)
		touch_data->get_touch_rx_num = data->get_touch_rx_num;
	if (data->get_touch_tx_num)
		touch_data->get_touch_tx_num = data->get_touch_tx_num;
	if (data->get_touch_x_resolution)
		touch_data->get_touch_x_resolution =
			data->get_touch_x_resolution;
	if (data->get_touch_y_resolution)
		touch_data->get_touch_y_resolution =
			data->get_touch_y_resolution;
	if (data->palm_sensor_write)
		touch_data->palm_sensor_write = data->palm_sensor_write;
	if (data->prox_sensor_read)
		touch_data->prox_sensor_read = data->prox_sensor_read;
	if (data->prox_sensor_write)
		touch_data->prox_sensor_write = data->prox_sensor_write;
	if (data->panel_vendor_read)
		touch_data->panel_vendor_read = data->panel_vendor_read;
	if (data->panel_color_read)
		touch_data->panel_color_read = data->panel_color_read;
	if (data->panel_display_read)
		touch_data->panel_display_read = data->panel_display_read;
	if (data->touch_vendor_read)
		touch_data->touch_vendor_read = data->touch_vendor_read;

	mutex_unlock(&nothing_touch_dev.mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(nothingtouch_register_modedata);

int update_nothing_palm_sensor_value(int value)
{
	struct nothing_touch *dev = NULL;

	mutex_lock(&nothing_touch_dev.palm_mutex);

	if (!touch_pdata) {
		mutex_unlock(&nothing_touch_dev.palm_mutex);
		return -ENODEV;
	}

	dev = touch_pdata->device;

	if (value != touch_pdata->palm_value) {
		pr_info("%s value: %d\n", __func__, value);
		touch_pdata->palm_value = value;
		touch_pdata->palm_changed = true;
		sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "palm_sensor");
	}

	mutex_unlock(&nothing_touch_dev.palm_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(update_nothing_palm_sensor_value);

int update_nothing_prox_sensor_value(int value)
{
	mutex_lock(&nothing_touch_dev.prox_mutex);

	if (!touch_pdata) {
		mutex_unlock(&nothing_touch_dev.prox_mutex);
		return -ENODEV;
	}

	if (value != touch_pdata->prox_value) {
		pr_info("%s value: %d\n", __func__, value);
		touch_pdata->prox_value = value;
		touch_pdata->prox_changed = true;
		sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "prox_sensor");
	}

	mutex_unlock(&nothing_touch_dev.prox_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(update_nothing_prox_sensor_value);

static ssize_t enable_touchraw_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct nothing_touch_interface *touch_data = NULL;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->is_enable_touchraw);
}

static ssize_t enable_touchraw_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data = NULL;
	unsigned int input;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	pr_info("%s,%d\n", __func__, input);
	if (touch_data->enable_touch_raw)
		touch_data->enable_touch_raw(!!input);

	touch_data->is_enable_touchraw = !!input;
	touch_pdata->raw_tail = 0;
	touch_pdata->raw_head = 0;

	return count;
}

static ssize_t enable_touchdelta_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct nothing_touch_interface *touch_data = NULL;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	return snprintf(buf, PAGE_SIZE, "%d\n",
			touch_data->is_enable_touchdelta);
}

static ssize_t enable_touchdelta_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data = NULL;
	unsigned int input;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	pr_info("%s,%d\n", __func__, input);
	if (touch_data->enable_touch_delta)
		touch_data->enable_touch_delta(!!input);

	touch_data->is_enable_touchdelta = !!input;
	return count;
}

static ssize_t thp_cmd_status_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct nothing_touch_interface *touch_data = NULL;

	mutex_lock(&nothing_touch_dev.mutex);

	if (!touch_pdata) {
		mutex_unlock(&nothing_touch_dev.mutex);
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	memcpy(buf, touch_data->thp_cmd_buf,
	       touch_data->thp_cmd_size * sizeof(int));
	mutex_unlock(&nothing_touch_dev.mutex);
	return touch_data->thp_cmd_size * sizeof(int);
}

static ssize_t thp_cmd_status_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data = NULL;
	unsigned int input[NOTHING_TOUCH_MAX_BUF];
	const char *p = buf;
	bool new_data = false;
	int para_cnt = 0;
	int i = 0;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	memset(input, 0x00, sizeof(int) * NOTHING_TOUCH_MAX_BUF);

	for (p = buf; *p != '\0'; p++) {
		if (*p >= '0' && *p <= '9') {
			input[i] = input[i] * 10 + (*p - '0');
			if (!new_data) {
				new_data = true;
				para_cnt++;
			}
		} else if (*p == ' ') {
			if (new_data) {
				i++;
				new_data = false;
			}
		} else {
			break;
		}
	}

	pr_info("%s size:%d, cmd:%d, %d, %d, %d\n", __func__, para_cnt,
		input[0], input[1], input[2], input[3]);
	memcpy(touch_data->thp_cmd_buf, input, sizeof(int) * para_cnt);
	touch_data->thp_cmd_size = para_cnt;
	sysfs_notify(&nothing_touch_device->dev->kobj, NULL, "touch_thp_cmd");

	return count;
}

static ssize_t nothing_touch_tx_num_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	if (pdata->touch_data[0]->get_touch_tx_num)
		return snprintf(buf, PAGE_SIZE, "%d\n",
				pdata->touch_data[0]->get_touch_tx_num());
	else
		return 0;
}

static ssize_t nothing_touch_rx_num_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	if (pdata->touch_data[0]->get_touch_rx_num)
		return snprintf(buf, PAGE_SIZE, "%d\n",
				pdata->touch_data[0]->get_touch_rx_num());
	else
		return 0;
}

static ssize_t nothing_touch_x_resolution_show(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	if (pdata->touch_data[0]->get_touch_x_resolution)
		return snprintf(buf, PAGE_SIZE, "%d\n",
				pdata->touch_data[0]->get_touch_x_resolution());
	else
		return 0;
}

static ssize_t nothing_touch_y_resolution_show(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	if (pdata->touch_data[0]->get_touch_y_resolution)
		return snprintf(buf, PAGE_SIZE, "%d\n",
				pdata->touch_data[0]->get_touch_y_resolution());
	else
		return 0;
}

static ssize_t thp_downthreshold_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct nothing_touch_interface *touch_data = NULL;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_downthreshold);
}

static ssize_t thp_downthreshold_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data = NULL;
	unsigned int input;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	pr_info("%s,%d\n", __func__, input);
	touch_data->thp_downthreshold = input;
	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "touch_thp_downthd");

	return count;
}

static ssize_t thp_upthreshold_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct nothing_touch_interface *touch_data = NULL;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_upthreshold);
}

static ssize_t thp_upthreshold_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data = NULL;
	unsigned int input;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	pr_info("%s,%d\n", __func__, input);
	touch_data->thp_upthreshold = input;
	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "touch_thp_upthd");

	return count;
}

static ssize_t thp_movethreshold_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct nothing_touch_interface *touch_data = NULL;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_movethreshold);
}

static ssize_t thp_movethreshold_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data = NULL;
	unsigned int input;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	pr_info("%s,%d\n", __func__, input);
	touch_data->thp_movethreshold = input;
	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "touch_thp_movethd");

	return count;
}

static ssize_t thp_islandthreshold_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct nothing_touch_interface *touch_data = NULL;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	return snprintf(buf, PAGE_SIZE, "%d\n",
			touch_data->thp_islandthreshold);
}

static ssize_t thp_islandthreshold_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data = NULL;
	unsigned int input;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	pr_info("%s,%d\n", __func__, input);
	touch_data->thp_islandthreshold = input;
	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "touch_thp_islandthd");

	return count;
}

static ssize_t thp_smooth_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct nothing_touch_interface *touch_data = NULL;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_smooth);
}

static ssize_t thp_smooth_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct nothing_touch_interface *touch_data = NULL;
	unsigned int input;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	pr_info("%s,%d\n", __func__, input);
	touch_data->thp_smooth = input;
	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "touch_thp_smooth");

	return count;
}

static ssize_t thp_dump_frame_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct nothing_touch_interface *touch_data = NULL;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_dump_raw);
}

static ssize_t thp_dump_frame_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data = NULL;
	unsigned int input;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	pr_info("%s,%d\n", __func__, input);
	touch_data->thp_dump_raw = input;
	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL, "touch_thp_dump");

	return count;
}

static ssize_t thp_noisefilter_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct nothing_touch_interface *touch_data = NULL;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_noisefilter);
}

static ssize_t thp_noisefilter_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct nothing_touch_interface *touch_data = NULL;
	unsigned int input;

	if (!touch_pdata)
		return -ENOMEM;

	touch_data = touch_pdata->touch_data[0];

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	pr_info("%s,%d\n", __func__, input);
	touch_data->thp_noisefilter = input;
	sysfs_notify(&nothing_touch_dev.dev->kobj, NULL,
		     "touch_thp_noisefilter");

	return count;
}

static ssize_t palm_sensor_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	pdata->palm_changed = false;

	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->palm_value);
}

static ssize_t palm_sensor_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	unsigned int input;
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	if (pdata->touch_data[0]->palm_sensor_write)
		pdata->touch_data[0]->palm_sensor_write(!!input);
	else
		pr_err("%s has not implement\n", __func__);

	pr_info("%s value:%d\n", __func__, !!input);

	return count;
}

static ssize_t prox_sensor_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	pdata->prox_changed = false;

	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->prox_value);
}

static ssize_t prox_sensor_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	unsigned int input;
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;

	if (pdata->touch_data[0]->prox_sensor_write)
		pdata->touch_data[0]->prox_sensor_write(!!input);
	else
		pr_err("%s has not implement\n", __func__);

	pr_info("%s value:%d\n", __func__, !!input);

	return count;
}

static ssize_t panel_vendor_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	if (pdata->touch_data[0]->panel_vendor_read)
		return snprintf(buf, PAGE_SIZE, "%c",
				pdata->touch_data[0]->panel_vendor_read());
	else
		return 0;
}

static ssize_t panel_color_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	if (pdata->touch_data[0]->panel_color_read)
		return snprintf(buf, PAGE_SIZE, "%c",
				pdata->touch_data[0]->panel_color_read());
	else
		return 0;
}

static ssize_t panel_display_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	if (pdata->touch_data[0]->panel_display_read)
		return snprintf(buf, PAGE_SIZE, "%c",
				pdata->touch_data[0]->panel_display_read());
	else
		return 0;
}

static ssize_t touch_vendor_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct nothing_touch_pdata *pdata = dev_get_drvdata(dev);

	if (pdata->touch_data[0]->touch_vendor_read)
		return snprintf(buf, PAGE_SIZE, "%c",
				pdata->touch_data[0]->touch_vendor_read());
	else
		return 0;
}

static ssize_t update_rawdata_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", "1");
}

static DEVICE_ATTR(enable_touch_raw, (S_IRUGO | S_IWUSR | S_IWGRP), enable_touchraw_show, enable_touchraw_store);
static DEVICE_ATTR(enable_touch_delta, (S_IRUGO | S_IWUSR | S_IWGRP), enable_touchdelta_show, enable_touchdelta_store);
static DEVICE_ATTR(touch_thp_cmd, (S_IRUGO | S_IWUSR | S_IWGRP), thp_cmd_status_show, thp_cmd_status_store);
static DEVICE_ATTR(touch_thp_tx_num, (S_IRUGO), nothing_touch_tx_num_show, NULL);
static DEVICE_ATTR(touch_thp_rx_num, (S_IRUGO), nothing_touch_rx_num_show, NULL);
static DEVICE_ATTR(touch_thp_x_resolution, (S_IRUGO), nothing_touch_x_resolution_show, NULL);
static DEVICE_ATTR(touch_thp_y_resolution, (S_IRUGO), nothing_touch_y_resolution_show, NULL);
static DEVICE_ATTR(touch_thp_downthd, (S_IRUGO | S_IWUSR | S_IWGRP), thp_downthreshold_show, thp_downthreshold_store);
static DEVICE_ATTR(touch_thp_upthd, (S_IRUGO | S_IWUSR | S_IWGRP), thp_upthreshold_show, thp_upthreshold_store);
static DEVICE_ATTR(touch_thp_movethd, (S_IRUGO | S_IWUSR | S_IWGRP), thp_movethreshold_show, thp_movethreshold_store);
static DEVICE_ATTR(touch_thp_islandthd, (S_IRUGO | S_IWUSR | S_IWGRP), thp_islandthreshold_show, thp_islandthreshold_store);
static DEVICE_ATTR(touch_thp_smooth, (S_IRUGO | S_IWUSR | S_IWGRP), thp_smooth_show, thp_smooth_store);
static DEVICE_ATTR(touch_thp_dump, (S_IRUGO | S_IWUSR | S_IWGRP), thp_dump_frame_show, thp_dump_frame_store);
static DEVICE_ATTR(touch_thp_noisefilter, (S_IRUGO | S_IWUSR | S_IWGRP), thp_noisefilter_show, thp_noisefilter_store);
static DEVICE_ATTR(palm_sensor, (S_IRUGO | S_IWUSR | S_IWGRP), palm_sensor_show, palm_sensor_store);
static DEVICE_ATTR(prox_sensor, (S_IRUGO | S_IWUSR | S_IWGRP), prox_sensor_show, prox_sensor_store);
static DEVICE_ATTR(panel_vendor, (S_IRUGO), panel_vendor_show, NULL);
static DEVICE_ATTR(panel_color, (S_IRUGO), panel_color_show, NULL);
static DEVICE_ATTR(panel_display, (S_IRUGO), panel_display_show, NULL);
static DEVICE_ATTR(touch_vendor, (S_IRUGO), touch_vendor_show, NULL);
static DEVICE_ATTR(update_rawdata, (S_IRUGO | S_IWUSR | S_IWGRP), update_rawdata_show, NULL);

static struct attribute *touch_attr_group[] = {
	&dev_attr_enable_touch_raw.attr,
	&dev_attr_enable_touch_delta.attr,
	&dev_attr_touch_thp_cmd.attr,
	&dev_attr_touch_thp_tx_num.attr,
	&dev_attr_touch_thp_rx_num.attr,
	&dev_attr_touch_thp_x_resolution.attr,
	&dev_attr_touch_thp_y_resolution.attr,
	&dev_attr_touch_thp_downthd.attr,
	&dev_attr_touch_thp_upthd.attr,
	&dev_attr_touch_thp_movethd.attr,
	&dev_attr_touch_thp_islandthd.attr,
	&dev_attr_touch_thp_smooth.attr,
	&dev_attr_touch_thp_dump.attr,
	&dev_attr_touch_thp_noisefilter.attr,
	&dev_attr_palm_sensor.attr,
	&dev_attr_prox_sensor.attr,
	&dev_attr_panel_vendor.attr,
	&dev_attr_panel_color.attr,
	&dev_attr_panel_display.attr,
	&dev_attr_touch_vendor.attr,
	&dev_attr_update_rawdata.attr,
	NULL,
};

static void *event_start(struct seq_file *m, loff_t *p)
{
	int pos = 0;
	struct nothing_last_touch_event *event;

	if (!touch_pdata || !touch_pdata->last_touch_events)
		return NULL;

	event = touch_pdata->last_touch_events;
	if (*p >= NOTHING_LAST_TOUCH_EVENTS_MAX)
		return NULL;

	pos = (event->head + *p) & (NOTHING_LAST_TOUCH_EVENTS_MAX - 1);

	return event->touch_event_buf + pos;
}

static void *event_next(struct seq_file *m, void *v, loff_t *p)
{
	int pos = 0;
	struct nothing_last_touch_event *event;

	if (!touch_pdata || !touch_pdata->last_touch_events)
		return NULL;

	event = touch_pdata->last_touch_events;
	if (++*p >= NOTHING_LAST_TOUCH_EVENTS_MAX)
		return NULL;

	pos = (event->head + *p) & (NOTHING_LAST_TOUCH_EVENTS_MAX - 1);

	return event->touch_event_buf + pos;
}

static int32_t event_show(struct seq_file *m, void *v)
{
	struct nothing_touch_event *event_info;
	struct rtc_time tm;
	event_info = (struct nothing_touch_event *)v;

	if (event_info->state == EVENT_INIT)
		return 0;

	rtc_time64_to_tm(event_info->touch_time.tv_sec, &tm);
	seq_printf(m, "%d-%02d-%02d %02d:%02d:%02d.%09lu UTC Finger (%2d) %s\n",
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		   tm.tm_min, tm.tm_sec, event_info->touch_time.tv_nsec,
		   event_info->slot,
		   event_info->state == EVENT_DOWN ? "P" : "R");

	return 0;
}

static void event_stop(struct seq_file *m, void *v)
{
	return;
}

const struct seq_operations last_touch_events_seq_ops = {
	.start = event_start,
	.next = event_next,
	.stop = event_stop,
	.show = event_show,
};

static int32_t last_touch_events_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &last_touch_events_seq_ops);
}

struct proc_ops last_touch_events_ops = {
	.proc_open = last_touch_events_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release,
};

static int nothing_touch_probe(struct platform_device *pdev)
{
	int ret = 0;
	int i = 0;
	struct device *dev = &pdev->dev;
	struct nothing_touch_pdata *pdata;

	pr_info("%s enter\n", __func__);

	pdata = devm_kzalloc(dev, sizeof(struct nothing_touch_pdata),
			     GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->raw_data = (unsigned int *)kzalloc(RAW_SIZE, GFP_KERNEL);
	if (!pdata->raw_data) {
		ret = -ENOMEM;
		pr_err("%s alloc mem for raw data\n", __func__);
		goto parse_dt_err;
	}

	for (i = 0; i < NOTHING_TOUCH_RAW_BUF_NUM; i++) {
		pdata->raw_buf[i] =
			(unsigned int *)kzalloc(RAW_SIZE, GFP_KERNEL);
		if (!pdata->raw_buf[i]) {
			ret = -ENOMEM;
			pr_err("%s alloc mem for raw buf data\n", __func__);
			goto parse_dt_err;
		}
	}

	pdata->raw_head = 0;
	pdata->raw_tail = 0;
	pdata->phy_base = virt_to_phys(pdata->raw_data);

	pr_info("%s: kernel base:%lld, phy base:%lld\n", __func__,
		(unsigned long)pdata->raw_data, (unsigned long)pdata->phy_base);

	spin_lock_init(&pdata->raw_lock);

	ret = misc_register(&nothing_touch_dev.misc_dev);
	if (ret) {
		pr_err("%s create misc device err:%d\n", __func__, ret);
		goto parse_dt_err;
	}

	nothing_touch_device = &nothing_touch_dev;
	if (!nothing_touch_dev.class)
		nothing_touch_dev.class = class_create(THIS_MODULE, "touch");

	if (!nothing_touch_dev.class) {
		pr_err("%s create device class err\n", __func__);
		goto class_create_err;
	}

	nothing_touch_dev.dev = device_create(nothing_touch_dev.class, NULL, 'T',
					      NULL, "touch_dev");
	if (!nothing_touch_dev.dev) {
		pr_err("%s create device dev err\n", __func__);
		goto device_create_err;
	}

	pdata->touch_data[0] = (struct nothing_touch_interface *)kzalloc(
		sizeof(struct nothing_touch_interface), GFP_KERNEL);
	if (pdata->touch_data[0] == NULL) {
		ret = -ENOMEM;
		pr_err("%s alloc mem for touch_data\n", __func__);
		goto data_mem_err;
	}

	pdata->touch_data[1] = (struct nothing_touch_interface *)kzalloc(
		sizeof(struct nothing_touch_interface), GFP_KERNEL);
	if (pdata->touch_data[1] == NULL) {
		ret = -ENOMEM;
		pr_err("%s alloc mem for touch_data\n", __func__);
		goto sys_group_err;
	}

	pdata->last_touch_events = (struct nothing_last_touch_event *)kzalloc(
		sizeof(struct nothing_last_touch_event), GFP_KERNEL);
	if (pdata->last_touch_events == NULL) {
		ret = -ENOMEM;
		pr_err("%s: alloc mem for last touch evnets\n", __func__);
		goto sys_group_err;
	}

	pdata->device = &nothing_touch_dev;
	dev_set_drvdata(nothing_touch_dev.dev, pdata);
	touch_pdata = pdata;

	nothing_touch_dev.attrs.attrs = touch_attr_group;
	ret = sysfs_create_group(&nothing_touch_dev.dev->kobj,
				 &nothing_touch_dev.attrs);
	if (ret) {
		pr_err("%s ERROR: Cannot create sysfs structure!:%d\n",
		       __func__, ret);
		ret = -ENODEV;
		goto sys_group_err;
	}
	pdata->last_touch_events_proc = proc_create(
		"last_touch_events", 0644, NULL, &last_touch_events_ops);

	pr_info("%s probe successful\n", __func__);

	return ret;

sys_group_err:
	if (pdata->touch_data[0]) {
		kfree(pdata->touch_data[0]);
		pdata->touch_data[0] = NULL;
	}
	if (pdata->touch_data[1]) {
		kfree(pdata->touch_data[1]);
		pdata->touch_data[1] = NULL;
	}
	if (pdata->last_touch_events) {
		kfree(pdata->last_touch_events);
		pdata->last_touch_events = NULL;
	}
data_mem_err:
	device_destroy(nothing_touch_dev.class, 'T');
device_create_err:
	class_destroy(nothing_touch_dev.class);
	nothing_touch_dev.class = NULL;
class_create_err:
	misc_deregister(&nothing_touch_dev.misc_dev);
parse_dt_err:
	if (pdata->raw_data) {
		kfree(pdata->raw_data);
		pdata->raw_data = NULL;
	}

	for (i = 0; i < NOTHING_TOUCH_RAW_BUF_NUM; i++) {
		if (pdata->raw_buf[i]) {
			kfree(pdata->raw_buf[i]);
			pdata->raw_buf[i] = NULL;
		}
	}

	pr_err("%s fail!\n", __func__);

	return ret;
}

static int nothing_touch_remove(struct platform_device *pdev)
{
	int i;

	sysfs_remove_group(&nothing_touch_dev.dev->kobj,
			   &nothing_touch_dev.attrs);
	device_destroy(nothing_touch_dev.class, 'T');
	class_destroy(nothing_touch_dev.class);
	nothing_touch_dev.class = NULL;
	misc_deregister(&nothing_touch_dev.misc_dev);

	if (touch_pdata->raw_data) {
		kfree(touch_pdata->raw_data);
		touch_pdata->raw_data = NULL;
	}

	for (i = 0; i < NOTHING_TOUCH_RAW_BUF_NUM; i++) {
		if (touch_pdata->raw_buf[i]) {
			kfree(touch_pdata->raw_buf[i]);
			touch_pdata->raw_buf[i] = NULL;
		}
	}

	if (touch_pdata->last_touch_events) {
		kfree(touch_pdata->last_touch_events);
		touch_pdata->last_touch_events = NULL;
	}

	if (touch_pdata->last_touch_events_proc != NULL) {
		remove_proc_entry("last_touch_events", NULL);
		touch_pdata->last_touch_events_proc = NULL;
	}

	if (touch_pdata->touch_data[0]) {
		kfree(touch_pdata->touch_data[0]);
		touch_pdata->touch_data[0] = NULL;
	}

	if (touch_pdata->touch_data[1]) {
		kfree(touch_pdata->touch_data[1]);
		touch_pdata->touch_data[1] = NULL;
	}

	return 0;
}

static const struct of_device_id nothing_touch_of_match[] = {
	{ .compatible = "nothing-touch", },
	{},
};

static struct platform_driver nothing_touch_device_driver = {
	.probe  = nothing_touch_probe,
	.remove = nothing_touch_remove,
	.driver = {
		.name = "nothing-touch",
		.of_match_table = of_match_ptr(nothing_touch_of_match),
	}
};

static struct platform_device *nothing_touch_pdev;

static int __init nothing_touch_init(void)
{
	int ret;

	ret = platform_driver_register(&nothing_touch_device_driver);
	if (ret) {
		pr_err("%s: failed to register platform driver: %d\n",
		       __func__, ret);
		return ret;
	}

	nothing_touch_pdev = platform_device_register_simple(
		"nothing-touch", -1, NULL, 0);
	if (IS_ERR(nothing_touch_pdev)) {
		ret = PTR_ERR(nothing_touch_pdev);
		pr_err("%s: failed to register platform device: %d\n",
		       __func__, ret);
		platform_driver_unregister(&nothing_touch_device_driver);
		return ret;
	}

	return 0;
}

static void __exit nothing_touch_exit(void)
{
	platform_device_unregister(nothing_touch_pdev);
	platform_driver_unregister(&nothing_touch_device_driver);
}

module_init(nothing_touch_init);
module_exit(nothing_touch_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Nothing touch abstraction for Goodix Linux touchscreen");