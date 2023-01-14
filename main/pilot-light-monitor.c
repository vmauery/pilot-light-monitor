/* Pilot Light Monitor
 *
 * Copyright 2023 Vernon Mauery <vernon@mauery.org>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <apps/esp_sntp.h>
#include <driver/ledc.h>
#include <driver/rtc_io.h>
#include <driver/uart.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#include <soc/rtc.h>
#include <soc/soc_caps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "nanoprintf.h"
#include "private-info.h"

const char* TAG = "pilot-light-monitor";

/*set the ssid and password via "idf.py menuconfig"*/
#define DEFAULT_SSID CONFIG_PLM_WIFI_SSID
#define DEFAULT_PWD CONFIG_PLM_WIFI_PASSWORD

#define DEFAULT_LISTEN_INTERVAL CONFIG_PLM_WIFI_LISTEN_INTERVAL

#if CONFIG_PLM_POWER_SAVE_MIN_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MIN_MODEM
#elif CONFIG_PLM_POWER_SAVE_MAX_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MAX_MODEM
#else /* CONFIG_PLM_POWER_SAVE_NONE or undefined */
#define DEFAULT_PS_MODE WIFI_PS_NONE
#endif

void https_get(const char* host, const char* path, const char* query);
void https_post(const char* uri, const char* data, const char* type,
                const char* auth);
char* urlencode(const char* msg);

void ulog(const char* msg)
{
    char* smsg = urlencode(msg);
    https_get(UPTIME_HOST, "/uptime/log/", smsg);
    free(smsg);
}

void light_usleep(uint64_t us)
{
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(us));
    esp_light_sleep_start();
}

void deep_usleep(uint64_t us)
{
    // All the work is done; go to sleep
    ESP_LOGI(TAG, "Enabling timer wakeup, %dus\n", (int)(us / 1000000));
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(us));

    ESP_LOGI(TAG, "Entering deep sleep\n");
    uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);

    esp_deep_sleep_start();
}

const int FLAME_OFF_MARK = 6;
const int FLAME_ON_MARK = 8;

#define RED_LED GPIO_NUM_6
#define GREEN_LED GPIO_NUM_7

#define FIXED_POINT 1024
struct windowed_ave
{
    int value;
    int count;
    int window;
};

static RTC_DATA_ATTR int sleep_count;

static RTC_DATA_ATTR int low_bat_count;
static RTC_DATA_ATTR struct windowed_ave batt_v_ave;

static RTC_DATA_ATTR int pilot_light_out;
static RTC_DATA_ATTR struct windowed_ave flame_v_ave;

static RTC_DATA_ATTR int wake_count;

void RTC_IRAM_ATTR esp_wake_deep_sleep(void)
{
    esp_default_wake_deep_sleep();
    static RTC_RODATA_ATTR const char fmt_str[] = "Wake count %d\n";
    esp_rom_printf(fmt_str, wake_count++);
}

static TaskHandle_t xMainTask = NULL;
const UBaseType_t xEthReadyIndex = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        // signal main that we have networking up
        vTaskNotifyGiveIndexedFromISR(xMainTask, xEthReadyIndex, NULL);
    }
}

static void wifi_shutdown(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_loop_delete_default();
    nvs_flash_deinit();
}

#define ENABLE_PM 1
static void init_wifi_power_save(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    // Configure dynamic frequency scaling:
    // maximum and minimum frequencies are set in sdkconfig,
    // automatic light sleep is enabled if tickless idle support is enabled.

#if CONFIG_PM_ENABLE
    // Configure dynamic frequency scaling:
    // maximum and minimum frequencies are set in sdkconfig,
    // automatic light sleep is enabled if tickless idle support is enabled.
#if CONFIG_IDF_TARGET_ESP32
    esp_pm_config_esp32_t pm_config = {
#elif CONFIG_IDF_TARGET_ESP32S2
    esp_pm_config_esp32s2_t pm_config = {
#elif CONFIG_IDF_TARGET_ESP32C3
    esp_pm_config_esp32c3_t pm_config = {
#elif CONFIG_IDF_TARGET_ESP32S3
    esp_pm_config_esp32s3_t pm_config = {
#elif CONFIG_IDF_TARGET_ESP32C2
    esp_pm_config_esp32c2_t pm_config = {
#endif
        .max_freq_mhz = CONFIG_PLM_MAX_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_PLM_MIN_CPU_FREQ_MHZ,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true
#endif
    };
    printf("enabling power management\n");
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
#endif // CONFIG_PM_ENABLE

    // init wifi as sta and set power save mode
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid = DEFAULT_SSID,
                .password = DEFAULT_PWD,
                .listen_interval = DEFAULT_LISTEN_INTERVAL,
            },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "esp_wifi_set_ps().");
    esp_wifi_set_ps(DEFAULT_PS_MODE);
}

/*---------------------------------------------------------------
        ADC General Macros
---------------------------------------------------------------*/
// ADC1 Channels
#define PLM_ADC1_CHAN0 ADC_CHANNEL_2
#define PLM_ADC1_CHAN1 ADC_CHANNEL_3

#define PLM_ADC_ATTEN ADC_ATTEN_DB_11

/*---------------------------------------------------------------
        ADC Calibration
---------------------------------------------------------------*/
static bool plm_adc_calibration_init(adc_unit_t unit, adc_atten_t atten,
                                     adc_cali_handle_t* out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

    if (!calibrated)
    {
        // ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK)
        {
            calibrated = true;
        }
    }

    *out_handle = handle;
    if (ret == ESP_OK)
    {
        // ESP_LOGI(TAG, "Calibration Success");
    }
    else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated)
    {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    }
    else
    {
        ESP_LOGE(TAG, "Invalid arg or no memory");
    }

    return calibrated;
}

static void plm_adc_calibration_deinit(adc_cali_handle_t handle)
{
    // ESP_LOGI(TAG, "deregister %s calibration scheme", "Curve Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(handle));
}

struct adc_conf
{
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t cal;
};

void __init_adc(struct adc_conf* adc)
{
    //-------------ADC1 Init---------------//
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc->unit));

    //-------------ADC1 Config---------------//
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = PLM_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(adc->unit, PLM_ADC1_CHAN0, &config));
    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(adc->unit, PLM_ADC1_CHAN1, &config));

    //-------------ADC1 Calibration Init---------------//
    adc->cal = NULL;
    if (!plm_adc_calibration_init(ADC_UNIT_1, PLM_ADC_ATTEN, &adc->cal))
    {
        adc->cal = NULL;
    }
}

void __fini_adc(struct adc_conf* adc)
{
    // Tear Down
    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc->unit));
    if (adc->cal)
    {
        plm_adc_calibration_deinit(adc->cal);
    }
}

void read_adc(int reps, int light_sleep, int* ch0, int* ch1)
{
    static struct adc_conf* adc = NULL;
    int i;
    int v0 = 0;
    int v1 = 0;

    if (!adc && (ch0 || ch1))
    {
        adc = malloc(sizeof(*adc));
        memset(adc, 0, sizeof(*adc));
        __init_adc(adc);
    }
    else if (adc && !(ch0 || ch1))
    {
        __fini_adc(adc);
        free(adc);
        adc = NULL;
    }

    for (i = 0; i < reps; i++)
    {
        int adc_raw;
        int voltage;

        if (ch0)
        {
            ESP_ERROR_CHECK(
                adc_oneshot_read(adc->unit, PLM_ADC1_CHAN0, &adc_raw));
            /*
            ESP_LOGI(TAG, "ADC%d channel[%d] raw data: %d", ADC_UNIT_1 + 1,
                     PLM_ADC1_CHAN0, adc_raw);
            */
            if (adc->cal)
            {
                ESP_ERROR_CHECK(
                    adc_cali_raw_to_voltage(adc->cal, adc_raw, &voltage));
                /*
                ESP_LOGI(TAG, "ADC%d channel[%d] real voltage: %d mV",
                         ADC_UNIT_1 + 1, PLM_ADC1_CHAN0, voltage);
               */
                v0 += voltage;
            }
            else
            {
                v0 += adc_raw;
            }
        }

        if (ch1)
        {
            ESP_ERROR_CHECK(
                adc_oneshot_read(adc->unit, PLM_ADC1_CHAN1, &adc_raw));
            /*
            ESP_LOGI(TAG, "ADC%d channel[%d] raw data: %d", ADC_UNIT_1 + 1,
                     PLM_ADC1_CHAN1, adc_raw);
            */
            if (adc->cal)
            {
                ESP_ERROR_CHECK(
                    adc_cali_raw_to_voltage(adc->cal, adc_raw, &voltage));
                /*
                ESP_LOGI(TAG, "ADC%d channel[%d] real voltage: %d mV",
                         ADC_UNIT_1 + 1, PLM_ADC1_CHAN1, voltage);
                */
                v1 += voltage;
            }
            else
            {
                v1 += adc_raw;
            }
        }
        // sleep for 10ms
        if (light_sleep)
        {
            light_usleep(10000);
        }
        else
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
    if (ch0)
    {
        v0 /= reps;
        *ch0 = v0;
    }
    if (ch1)
    {
        v1 /= reps;
        *ch1 = v1;
    }
}

void init_led(int led)
{
    gpio_reset_pin(led);
    // Set the GPIO as a push/pull output
    gpio_set_direction(led, GPIO_MODE_OUTPUT);
}

void set_led(int led, int val)
{
    gpio_set_level(led, val);
    return;
    switch (led)
    {
        case RED_LED:
        case GREEN_LED:
            gpio_set_level(led, val);
            break;
        default:
            break;
    }
}

void led_code(int led, uint32_t c)
{
    // each bit is 1/4 second - each code up to 8 seconds
    // 0 is off, 1 is on, start from LSB
    // if all remaining bits are 0, exit
    while (c)
    {
        int b = c & 1;
        c >>= 1;
        set_led(led, b);
        light_usleep(249 * 1000);
    }
    set_led(led, 0);
}

#define TIMER_WAKEUP_TIME_US (5 * 1000 * 1000)
void register_timer_wakeup(void)
{
    if (esp_sleep_enable_timer_wakeup(TIMER_WAKEUP_TIME_US))
    {
        ESP_LOGE(TAG, "Configure timer as wakeup source failed");
        return;
    }
    ESP_LOGI(TAG, "timer wakeup source is ready");
}

void windowed_ave_init(struct windowed_ave* a, int window)
{
    a->count = 0;
    a->value = 0;
    a->window = window;
}

void ave_new_value_limited(struct windowed_ave* a, int val)
{
    int prev_count = (a->count == a->window) ? (a->window - 1) : a->count;
    a->count = prev_count + 1;
    val *= FIXED_POINT;
    int delta = val - a->value;
    int sign = delta >= 0 ? 1 : -1;
    delta = abs(delta);
    int ok_delta = abs(a->value / (a->window / 2));
    if ((a->count == a->window) && (delta > ok_delta))
    {
        // allow the growth to happen slowly
        val = a->value + sign * ok_delta;
    }
    a->value = (prev_count * a->value + val) / a->count;
}
// return the closest integer to the average
int ave_val(struct windowed_ave* a)
{
    // integer rounding to nearest value
    return (a->value + FIXED_POINT / 2) / FIXED_POINT;
}
// return the whole part of the average
static inline int ave_whole(struct windowed_ave* a)
{
    // integer division with truncation
    return a->value / FIXED_POINT;
}
static inline int ave_millis(struct windowed_ave* a)
{
    // integer rounding to nearest value
    return (1000 * (a->value % FIXED_POINT)) / FIXED_POINT;
}

void send_sms(const char* to, const char* msg)
{
    if (!to || !msg)
    {
        return;
    }
    char* smsg = urlencode(msg);
    const char MSG_FMT[] = "To=%s&From=%s&Body=%s";
    size_t datalen = sizeof(MSG_FMT) + strnlen(smsg, 320) + sizeof(sms_from) +
                     strnlen(to, 16) + 1;
    char* data = malloc(datalen);
    if (!data)
    {
        return;
    }
    snprintf(data, datalen - 1, MSG_FMT, to, sms_from, smsg);
    free(smsg);
    https_post("https://api.twilio.com/2010-04-01/Accounts/" SID
               "/Messages.json",
               data, "application/x-www-form-urlencoded", basic_auth);
    free(data);
}

#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_DUTY (4095)      // Set duty to 50%. ((2 ** 13) - 1) * 50% = 4095
#define LEDC_FREQUENCY (5000) // Frequency in Hertz. Set frequency at 5 kHz

const int led_to_channel[GPIO_NUM_MAX] = {
    [RED_LED] = LEDC_CHANNEL_0,
};
void ledc_pwm_init(int led)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQUENCY, // Set output frequency at 5 kHz
        .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {.speed_mode = LEDC_MODE,
                                          .channel = led_to_channel[led],
                                          .timer_sel = LEDC_TIMER,
                                          .intr_type = LEDC_INTR_DISABLE,
                                          .gpio_num = led,
                                          .duty = 0, // Set duty to 0%
                                          .hpoint = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

void ledc_pwm_fini(int led, int idle)
{
    ledc_stop(LEDC_MODE, led_to_channel[led], idle);
}

// set duty in tenths of a percent (0-1000)
void set_led_duty(int led, int duty)
{
    duty = ((1 << 13) - 1) * duty / 1000;
    // printf("duty: %d\n", duty);
    ledc_set_duty(LEDC_MODE, led_to_channel[led], duty);
    ledc_update_duty(LEDC_MODE, led_to_channel[led]);
}

// timeout is approximately seconds, wait on is a notify identifier
int flame_to_led(int timeout, const UBaseType_t* wait_on)
{
    int flame_v = 0;
    ledc_pwm_init(RED_LED);
    int finder_timeout = timeout;
    while (--finder_timeout)
    {
        for (int j = 0; j < 20; j++)
        {
            read_adc(5, 0, &flame_v, NULL);
            int duty = MIN(flame_v / 28, 100) * 10;
            //    printf("flame_v = %d, duty = %d.%d\n", flame_v, duty / 10,
            //           duty % 10);
            set_led_duty(RED_LED, duty);
        }
        if (wait_on)
        {
            if (ulTaskNotifyTakeIndexed(*wait_on, pdTRUE, 0) == 1)
            {
                break;
            }
        }
    }
    ledc_pwm_fini(RED_LED, (flame_v > FLAME_OFF_MARK) ? 0 : 1);

    // return 1 for did not timeout, 0 for did timeout
    return finder_timeout ? 1 : 0;
}

void app_main(void)
{
    // device wakes up every wakeup_time_sec seconds, but only
    // brings up the network and reports every
    // wakeup_time_sec * report_tick_interval seconds
    // Ideally this would be every 30 minutes

    // for debugging, 20 is nice, but 60 is better for the battery
    const int wakeup_time_sec = 20;
    // for debugging, 5 is nice, but 30 is better for the battery
    const int report_tick_interval = 5;

    // get task ID for notifications
    xMainTask = xTaskGetCurrentTaskHandle();

    struct timeval now;
    gettimeofday(&now, NULL);

    int wkup_cause = esp_sleep_get_wakeup_cause();
    printf("wakeup cause: 0x%x\n", wkup_cause);
    switch (wkup_cause)
    {
        case ESP_SLEEP_WAKEUP_TIMER:
        {
            ESP_LOGI(TAG, "Wake up from timer.\n");
            break;
        }

        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
        {
            ESP_LOGI(TAG, "Not a deep sleep reset\n");
            sleep_count = 0;
            break;
        }
    }

    // start with a solid green for each round for proof of life
    init_led(GREEN_LED);
    set_led(GREEN_LED, 1);

    // Limit texts to once every 12 hours
    const int NOTIFY_LIMIT = 43200;
    int tick = sleep_count++;
    if (tick == 0)
    {
        pilot_light_out = 1;
        low_bat_count = -NOTIFY_LIMIT;
        windowed_ave_init(&flame_v_ave, 8);
        windowed_ave_init(&batt_v_ave, 32);
        // at first boot, do a flame_to_led for proof of life and
        // ease of programming (a good time with no deep or light sleeps)
        flame_to_led(10, NULL);
    }

    int flame_v = 0;
    int batt_v = 0;
    // monitor the flame for a full second, with light sleep enabled
    read_adc(100, 1, &flame_v, &batt_v);
    ave_new_value_limited(&flame_v_ave, flame_v);
    ave_new_value_limited(&batt_v_ave, batt_v);
    ESP_LOGI(TAG, "read_adc -> flame_v = %d, batt_v = %d (%d)\n", flame_v,
             batt_v, ave_val(&batt_v_ave));
    // full-burner =~ 14-18, pilot =~ 8-14, off =~ 0-6
    int last_pilot_light_out = pilot_light_out;
    int flame_ave = ave_val(&flame_v_ave);
    if (flame_ave < FLAME_OFF_MARK)
    {
        pilot_light_out = 1;
    }
    else if (flame_ave > FLAME_ON_MARK)
    {
        pilot_light_out = 0;
    }
    int pilot_light_out_notify = pilot_light_out && !last_pilot_light_out;

    // Brownout is ~1360, and the voltage drops quickly from 1775
    // Ideally notification of low battery should give us at least 24h
    const int LOW_BATT_MARK = 1775 * FIXED_POINT;
    // as battery is charging, the value will go up. Once the value hits
    // 2030 the battery should be considered full and we can send
    // a message to that effect.
    const int FULL_BATT_MARK = 2020 * FIXED_POINT;
    int low_battery = batt_v_ave.value < LOW_BATT_MARK;
    int low_battery_notify =
        low_battery &&
        (((tick - low_bat_count) * wakeup_time_sec) > NOTIFY_LIMIT);
    int full_battery = batt_v_ave.value > FULL_BATT_MARK;
    int full_battery_notify =
        full_battery &&
        (((tick - low_bat_count) * wakeup_time_sec) > NOTIFY_LIMIT / 2);

    // don't bother reporting flame_out or low_batt on first round
    if (tick == 0)
    {
        pilot_light_out_notify = 0;
        low_battery_notify = 0;
    }

    if (pilot_light_out)
    {
        led_code(RED_LED, 0x5555);
    }
    if (low_battery)
    {
        led_code(GREEN_LED, 0x5555);
    }
    if ((tick % report_tick_interval) == 0 || pilot_light_out_notify)
    {
        init_wifi_power_save();
        // wait for network
        uint32_t notify_value = flame_to_led(20, &xEthReadyIndex);
        if (notify_value == 1)
        {
            if (tick == 0)
            {
                ulog("alert=pilot_light_monitor_reboot");
                // printf("alert=pilot_light_monitor_reboot\n");
            }
            char q[256];
            // https://UPTIME_HOST/uptime/log?flame_v=702&batt_v=2032
            snprintf(q, sizeof(q) - 1,
                     "t=%d, f=%d, flame_v=%d, flame_v_ave=%d.%03d, "
                     "batt_v=%d, batt_v_ave=%d.%03d, heap=%d",
                     tick, !pilot_light_out, flame_v, ave_whole(&flame_v_ave),
                     ave_millis(&flame_v_ave), batt_v, ave_whole(&batt_v_ave),
                     ave_millis(&batt_v_ave), esp_get_free_heap_size());
            ulog(&q[0]);
            // printf("log: %s\n", q);
            if (pilot_light_out_notify)
            {
                send_sms(alert_num, "Pilot light is out");
                // printf("{\"msg\":\"Pilot light is out\"}\n");
            }
            if (low_battery_notify)
            {
                low_bat_count = tick;
                send_sms(alert_num, "Pilot light monitor low battery");
                // printf("{\"msg\":\"Pilot light monitor low
                // battery\"}\n");
            }
            if (full_battery_notify)
            {
                low_bat_count = tick;
                send_sms(alert_num, "Pilot light monitor battery charged");
            }
            if ((tick % report_tick_interval) == 0)
            {
                // send an uptime ping
                https_get(UPTIME_HOST, "/uptime/pilot_light_ping", NULL);
                // printf("%s%s\n", UPTIME_HOST, "/uptime/pilot_light_ping");
            }
        }
        else
        {
            led_code(RED_LED, 0xff00ff);
        }
        wifi_shutdown();
    }
#if 0
    else
    {
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(5 * 1000000));
        printf("Entering light sleep\n");
        /* To make sure the complete line is printed before entering sleep mode,
         * need to wait until UART TX FIFO is empty:
         */
        uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);
        esp_light_sleep_start();
    }
#endif

    deep_usleep(wakeup_time_sec * 1000000);
}
