#include <linux/device.h>  /* needed for sysfs handling */
#include <linux/gpio.h>    /* needed for GPIO handling */
#include <linux/init.h>    /* needed for macros */
#include <linux/kernel.h>  /* needed for debugging */
#include <linux/module.h>  /* needed by all modules */
#include <linux/thermal.h> /* needed for thermal handling */
#include <linux/timer.h>   /* needed for timer handling */

// GPIO number for the status LED (GPIOA10 = gpio10)
#define STATUS_LED_GPIO 10

// Thermal zone name for CPU temperature
#define CPU_THERMAL_ZONE "cpu-thermal"

// Sysfs class and device names
#define SYSFS_CLASS_NAME "fan_module_class"
#define SYSFS_DEVICE_NAME "fan_module_device"

// Temperature thresholds in millidegrees Celsius
#define TEMP_LOW_THRESHOLD 35000
#define TEMP_MEDIUM_THRESHOLD 40000
#define TEMP_HIGH_THRESHOLD 45000

// Timer half-periods in ms (LED toggles at each expiry)
#define PERIOD_LOW 500     // 2Hz
#define PERIOD_MEDIUM 200  // 5Hz
#define PERIOD_HIGH 100    // 10Hz
#define PERIOD_MAX 50      // 20Hz

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
static struct class* sysfs_class;
static struct device* sysfs_device;

// Helpers

static unsigned long freq_to_period_ms(enum fan_freq freq) {
    switch (freq) {
        case LOW:
            return PERIOD_LOW;
        case MEDIUM:
            return PERIOD_MEDIUM;
        case HIGH:
            return PERIOD_HIGH;
        case MAX:
            return PERIOD_MAX;
        default:
            return PERIOD_LOW;
    }
}

static void check_temperature_and_update_freq(void) {
    thermal_zone_get_temp(fan.cpu_thermal_zone, &fan.cpu_temp);

    if (fan.cpu_temp < TEMP_LOW_THRESHOLD)
        fan.freq = LOW;
    else if (fan.cpu_temp < TEMP_MEDIUM_THRESHOLD)
        fan.freq = MEDIUM;
    else if (fan.cpu_temp < TEMP_HIGH_THRESHOLD)
        fan.freq = HIGH;
    else
        fan.freq = MAX;
}

// Sysfs attribute handlers

// Sysfs: temp (read)
ssize_t sysfs_show_temp(struct device* dev, struct device_attribute* attr,
                        char* buf) {
    thermal_zone_get_temp(fan.cpu_thermal_zone, &fan.cpu_temp);
    sprintf(buf, "%d\n", fan.cpu_temp / 1000);

    return strlen(buf);
}
DEVICE_ATTR(temp, 0444, sysfs_show_temp, NULL);

// Sysfs: freq (read/write)
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

// Sysfs: mode (read/write)

ssize_t sysfs_show_mode(struct device* dev, struct device_attribute* attr,
                        char* buf) {
    return sprintf(buf, "%d\n", fan.mode);
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

// Timer callback
void fan_timer_callback(struct timer_list* timer) {
    if (fan.mode == AUTO) {
        check_temperature_and_update_freq();
    }

    // toggle led
    fan.led_state = !fan.led_state;
    gpio_set_value(STATUS_LED_GPIO, fan.led_state);

    mod_timer(&fan.fan_timer,
              jiffies + msecs_to_jiffies(freq_to_period_ms(fan.freq)));
}

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
    status = gpio_request(STATUS_LED_GPIO, "status_led");
    if (status) {
        pr_err("Failed to request GPIO %d\n", STATUS_LED_GPIO);
        goto err_thermal;
    }

    // Set GPIO direction to output
    status = gpio_direction_output(STATUS_LED_GPIO, 0);
    if (status) {
        pr_err("Failed to set GPIO direction\n");
        goto err_gpio;
    }

    // Initialize and start the timer
    timer_setup(&fan.fan_timer, fan_timer_callback, 0);
    mod_timer(&fan.fan_timer, jiffies + msecs_to_jiffies(PERIOD_LOW));

    pr_info("Miniprojet fan_module loaded\n");
    return 0;

err_gpio:
    gpio_free(STATUS_LED_GPIO);
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
    gpio_free(STATUS_LED_GPIO);

    pr_info("Miniprojet fan_module unloaded\n");
}

module_init(fan_module_init);
module_exit(fan_module_exit);

MODULE_AUTHOR("Rafael Dousse <rafael.dousse@master.hes-so.ch>");
MODULE_DESCRIPTION("Module fan_module");
MODULE_LICENSE("GPL");
