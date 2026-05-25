
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define GPIO_EXPORT "/sys/class/gpio/export"
#define GPIO_UNEXPORT "/sys/class/gpio/unexport"
#define GPIO_LED "/sys/class/gpio/gpio10"
#define LED "10"
#define NUM_GPIOS 4

static int open_led() {
    // unexport pin out of sysfs (reinitialization)
    int f = open(GPIO_UNEXPORT, O_WRONLY);

    write(f, LED, strlen(LED));
    close(f);

    // export pin to sysfs
    f = open(GPIO_EXPORT, O_WRONLY);
    write(f, LED, strlen(LED));
    close(f);

    // config pin
    f = open(GPIO_LED "/direction", O_WRONLY);
    write(f, "out", 3);
    close(f);

    // open gpio value attribute
    f = open(GPIO_LED "/value", O_RDWR);
    return f;
}

static int open_button(const char* gpio) {
    int f;

    // export
    f = open(GPIO_EXPORT, O_WRONLY);
    write(f, gpio, strlen(gpio));
    close(f);

    // direction = input
    char path[100];
    sprintf(path, "/sys/class/gpio/gpio%s/direction", gpio);
    f = open(path, O_WRONLY);
    write(f, "in", 2);
    close(f);

    // edge = both (ou rising/falling)
    sprintf(path, "/sys/class/gpio/gpio%s/edge", gpio);
    f = open(path, O_WRONLY);
    write(f, "falling", 7);
    close(f);

    // open value
    sprintf(path, "/sys/class/gpio/gpio%s/value", gpio);
    f = open(path, O_RDONLY);

    char dummy;
    lseek(f, 0, SEEK_SET);
    read(f, &dummy, 1);

    return f;
}

static void update_timer(int timer_fd, double frequency) {
    long half_period_ns = (long)(1000000000.0 / (frequency * 2.0));

    struct itimerspec timer_spec = {0};

    timer_spec.it_value.tv_sec = half_period_ns / 1000000000;
    timer_spec.it_value.tv_nsec = half_period_ns % 1000000000;

    timer_spec.it_interval.tv_sec = half_period_ns / 1000000000;
    timer_spec.it_interval.tv_nsec = half_period_ns % 1000000000;

    timerfd_settime(timer_fd, 0, &timer_spec, NULL);
}

int main(int argc, char* argv[]) {
    openlog("silly_led_control", LOG_PID | LOG_CONS, LOG_USER);
    long duty = 2;          // %
    long period_ms = 1000;  // ms
    if (argc >= 2) period_ms = atoi(argv[1]);
    long period_ns = period_ms * 1000000;  // in ns => 1sec

    // compute duty period
    long p1 = period_ms / 100 * duty;
    long p2 = period_ms - p1;

    int led = open_led();
    int buttons[] = {open_button("0"), open_button("2"), open_button("3")};

    int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_fd == -1) {
        perror("timerfd_create");
        return EXIT_FAILURE;
    }

    double frequency = 2.0;  // in Hz
    update_timer(timer_fd, frequency);

    // poll logic
    struct pollfd fds[NUM_GPIOS];
    // Poll timer for led control
    fds[0].fd = timer_fd;
    fds[0].events = POLLIN;
    // Poll buttons for user input
    for (int i = 0; i < (NUM_GPIOS - 1); i++) {
        fds[i + 1].fd = buttons[i];
        fds[i + 1].events = POLLPRI;
    }

    int led_state = 0;
    while (1) {
        // second argument is the number of file descriptors in the array,
        // third is timeout in ms (-1 means infinite), 0 means return
        // immediately, positive value means wait for that amount of ms
        int ret = poll(fds, (nfds_t)NUM_GPIOS, -1);

        if (ret == -1) {
            perror("poll");
            return EXIT_FAILURE;
        }

        if (fds[0].revents & POLLIN) {
            uint64_t expirations;
            read(timer_fd, &expirations,
                 sizeof(expirations));  // clear the timer event by reading the
                                        // value (number of expirations since
                                        // last read)

            // toggle led
            led_state = !led_state;
            if (led_state) {
                pwrite(led, "1", 1, 0);
            } else {
                pwrite(led, "0", 1, 0);
            }
        }

        if (fds[1].revents & POLLPRI) {
            // button 0 pressed
            char value;
            lseek(fds[1].fd, 0, SEEK_SET);
            read(fds[1].fd, &value, 1);
            frequency += 1.0;
            update_timer(timer_fd, frequency);
            syslog(LOG_INFO, "Frequency increased: %.1f Hz", frequency);
        }

        if (fds[2].revents & POLLPRI) {
            // button 1 pressed
            char value;
            lseek(fds[2].fd, 0, SEEK_SET);
            read(fds[2].fd, &value, 1);
            frequency = 2.0;
            update_timer(timer_fd, frequency);
            syslog(LOG_INFO, "Frequency reset: %.1f Hz", frequency);
        }
        if (fds[3].revents & POLLPRI) {
            // button 2 pressed
            char value;
            lseek(fds[3].fd, 0, SEEK_SET);
            read(fds[3].fd, &value, 1);
            if (frequency > 1.0) {
                frequency -= 1.0;
            }
            update_timer(timer_fd, frequency);
            syslog(LOG_INFO, "Frequency decreased: %.1f Hz", frequency);
        }
    }
    closelog();

    return 0;
}
