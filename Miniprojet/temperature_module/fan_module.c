#include <linux/device.h>  /* needed for sysfs handling */
#include <linux/gpio.h>    /* needed for GPIO handling */
#include <linux/init.h>    /* needed for macros */
#include <linux/kernel.h>  /* needed for debugging */
#include <linux/module.h>  /* needed by all modules */
#include <linux/thermal.h> /* needed for thermal handling */
#include <linux/timer.h>   /* needed for timer handling */


enum fan_mode { AUTO = 0, MANUAL = 1 };

enum fan_freq {
    LOW = 0,     // 2Hz
    MEDIUM = 1,  // 5Hz
    HIGH = 2,    // 10Hz
    MAX = 3      // 20Hz
};

struct fan_data {
    enum fan_mode mode;  // 0: auto, 1: manuel
    enum fan_freq freq;  // 0: low, 1: medium, 2: high, 3: max
    int cpu_temp;
    struct thermal_zone_device* cpu_thermal_zone;
    struct timer_list fan_timer;
    int led_state;
};

struct fan_data fan;

void check_temperature_and_update_freq(void) {
    thermal_zone_get_temp(fan.cpu_thermal_zone, &fan.cpu_temp);

    if (fan.cpu_temp < 35000) {
        fan.freq = LOW;
    } else if (fan.cpu_temp < 40000) {
        fan.freq = MEDIUM;
    } else if (fan.cpu_temp < 45000) {
        fan.freq = HIGH;
    } else {
        fan.freq = MAX;
    }
}

ssize_t sysfs_show_temp(struct device* dev, struct device_attribute* attr,
                        char* buf) {
    thermal_zone_get_temp(fan.cpu_thermal_zone, &fan.cpu_temp);
    sprintf(buf, "%d\n", fan.cpu_temp / 1000);

    return strlen(buf);
}
DEVICE_ATTR(temp, 0444, sysfs_show_temp, NULL);

ssize_t sysfs_show_freq(struct device* dev, struct device_attribute* attr,
                        char* buf) {
    sprintf(buf, "%d\n", fan.freq);
    return strlen(buf);
}

ssize_t sysfs_store_freq(struct device* dev, struct device_attribute* attr,
                         const char* buf, size_t count) {
    int val;
    int res = kstrtoint(buf, 10, &val);
    if (res == -EINVAL) {
        pr_info("Failed to convert frequency value\n");
        return -EINVAL;
    }
    if (res == -ERANGE) {
        pr_info("Frequency value out of range\n");
        return -ERANGE;
    }
    if (val < LOW || val > MAX) {
        pr_info("Invalid frequency value: %d\n", val);
        return -EINVAL;
    }

    if (fan.mode == AUTO) {
        pr_info("Cannot set frequency in AUTO mode\n");
        return -EINVAL;
    }

    fan.freq = val;

    return count;
}
DEVICE_ATTR(freq, 0664, sysfs_show_freq, sysfs_store_freq);

ssize_t sysfs_show_mode(struct device* dev, struct device_attribute* attr,
                        char* buf) {
    switch (fan.mode) {
        case AUTO:
            sprintf(buf, "AUTO\n");
            break;
        case MANUAL:
            sprintf(buf, "MANUAL\n");
            break;
    }

    return strlen(buf);
}

ssize_t sysfs_store_mode(struct device* dev, struct device_attribute* attr,
                         const char* buf, size_t count) {
    int val;
    int res = kstrtoint(buf, 10, &val);
    if (res == -EINVAL) {
        pr_info("Failed to convert mode value\n");
        return -EINVAL;
    }
    if (res == -ERANGE) {
        pr_info("Mode value out of range\n");
        return -ERANGE;
    }
    if (val < AUTO || val > MANUAL) {
        pr_info("Invalid mode value: %d\n", val);
        return -EINVAL;
    }
    fan.mode = val;
    return count;
}
DEVICE_ATTR(mode, 0664, sysfs_show_mode, sysfs_store_mode);

void fan_timer_callback(struct timer_list* timer) {
    if (fan.mode == AUTO) {
        check_temperature_and_update_freq();
    }

    // toggle led
    fan.led_state = !fan.led_state;
    gpio_set_value(10, fan.led_state);

    // Update timer for next toggle based on frequency
    unsigned long period;
    switch (fan.freq) {
        case LOW:
            period = 500;
            break;
        case MEDIUM:
            period = 200;
            break;
        case HIGH:
            period = 100;
            break;
        case MAX:
            period = 50;
            break;
        default:
            period = 500;
            break;
    }
    mod_timer(&fan.fan_timer, jiffies + msecs_to_jiffies(period));
}

static struct class* sysfs_class;
static struct device* sysfs_device;

static int __init fan_module_init(void) {
    int status = 0;

    // Create sysfs class and device
    sysfs_class = class_create(THIS_MODULE, "fan_module_class");
    if (IS_ERR(sysfs_class)) {
        pr_err("Failed to create sysfs class\n");
        return PTR_ERR(sysfs_class);
    }

    sysfs_device = device_create(sysfs_class, NULL, MKDEV(0, 0), NULL,
                                 "fan_module_device");
    if (IS_ERR(sysfs_device)) {
        pr_err("Failed to create sysfs device\n");
        status = PTR_ERR(sysfs_device);
        goto err_device;
    }

    status = device_create_file(sysfs_device, &dev_attr_temp);
    if (status) {
        pr_err("Failed to create temp attribute\n");
        goto err_attr;
    }

    status = device_create_file(sysfs_device, &dev_attr_mode);
    if (status) {
        pr_err("Failed to create mode attribute\n");
        goto err_attr_mode;
    }

    status = device_create_file(sysfs_device, &dev_attr_freq);
    if (status) {
        pr_err("Failed to create freq attribute\n");
        goto err_attr_freq;
    }

    // Get thermal zone for CPU temperature
    fan.cpu_thermal_zone = thermal_zone_get_zone_by_name("cpu-thermal");
    if (IS_ERR(fan.cpu_thermal_zone)) {
        pr_err("Failed to get thermal zone\n");
        status = PTR_ERR(fan.cpu_thermal_zone);
        goto err_thermal;
    }

    // Initialize fan data
    fan.mode = AUTO;
    fan.freq = LOW;
    thermal_zone_get_temp(fan.cpu_thermal_zone, &fan.cpu_temp);

    fan.led_state = 0;
    status = gpio_request(10, "status_led");
    if (status) {
        pr_err("Failed to request GPIO 10\n");
        goto err_thermal;
    }

    // Set GPIO direction to output
    status = gpio_direction_output(10, 0);
    if (status) {
        pr_err("Failed to set GPIO direction\n");
        goto err_gpio;
    }

    // Initialize and start the timer
    timer_setup(&fan.fan_timer, fan_timer_callback, 0);
    mod_timer(&fan.fan_timer, jiffies + msecs_to_jiffies(500));

    pr_info("Miniprojet fan_module loaded\n");
    return 0;

err_gpio:
    gpio_free(10);
err_thermal:
    device_remove_file(sysfs_device, &dev_attr_freq);
err_attr_freq:
    device_remove_file(sysfs_device, &dev_attr_mode);
err_attr_mode:
    device_remove_file(sysfs_device, &dev_attr_temp);
err_attr:
    device_destroy(sysfs_class, MKDEV(0, 0));
err_device:
    class_destroy(sysfs_class);
    return status;
}

static void __exit fan_module_exit(void) {
    del_timer_sync(&fan.fan_timer);
    device_remove_file(sysfs_device, &dev_attr_temp);
    device_remove_file(sysfs_device, &dev_attr_mode);
    device_remove_file(sysfs_device, &dev_attr_freq);
    device_destroy(sysfs_class, 0);
    class_destroy(sysfs_class);
    gpio_free(10);

    pr_info("Miniprojet fan_module unloaded\n");
}

module_init(fan_module_init);
module_exit(fan_module_exit);

MODULE_AUTHOR("Rafael Dousse <rafael.dousse@master.hes-so.ch>");
MODULE_DESCRIPTION("Module fan_module");
MODULE_LICENSE("GPL");
