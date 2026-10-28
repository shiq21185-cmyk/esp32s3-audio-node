/*
 * D1 上电自检
 *
 * 目的只有一个：确认这块板子是活的，并且看清楚它到底是多少 Flash / 多少 PSRAM。
 * 不带任何业务逻辑，出问题好定位。
 *
 * 期望输出（N16R8）：
 *   chip   : esp32s3, 2 core(s), rev v0.2
 *   flash  : 16 MB
 *   psram  : 8192 KB (OCT)
 */
#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on (插电/按复位)";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "panic (崩溃重启)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_BROWNOUT: return "brownout (电压跌落!)";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
    }
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    printf("\n");
    printf("==================================================\n");
    printf("  小鲸鱼身体 · D1 上电自检\n");
    printf("==================================================\n");
    printf("  chip      : %s, %d core(s), rev v%d.%d\n",
           CONFIG_IDF_TARGET, chip.cores,
           chip.revision / 100, chip.revision % 100);
    printf("  features  : wifi=%d  ble=%d\n",
           (chip.features & CHIP_FEATURE_WIFI_BGN) ? 1 : 0,
           (chip.features & CHIP_FEATURE_BLE) ? 1 : 0);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("  flash     : %" PRIu32 " MB\n", flash_size / (1024 * 1024));
    } else {
        printf("  flash     : <读取失败>\n");
    }

#if CONFIG_SPIRAM
    printf("  psram     : %u KB  (%s, %s)\n",
           (unsigned)(esp_psram_get_size() / 1024),
           CONFIG_SPIRAM_MODE_OCT ? "OCT 八线" : "QUAD 四线",
#if CONFIG_SPIRAM_SPEED_120M
           "120 MHz"
#elif CONFIG_SPIRAM_SPEED_80M
           "80 MHz"
#elif CONFIG_SPIRAM_SPEED_40M
           "40 MHz"
#else
           "20 MHz"
#endif
    );
#else
    printf("  psram     : 未启用 (CONFIG_SPIRAM=n)\n");
#endif

    printf("  heap int  : %u KB free\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
#if CONFIG_SPIRAM
    printf("  heap psram: %u KB free\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#endif
    printf("  idf       : %s\n", esp_get_idf_version());
    printf("  reset     : %s\n", reset_reason_str(esp_reset_reason()));
    printf("  uptime    : %" PRIu64 " ms\n", esp_timer_get_time() / 1000);
    printf("==================================================\n");

    int i = 0;
    while (1) {
        printf("[alive %4d]  heap=%u KB  uptime=%" PRIu64 " s\n",
               i++,
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
               esp_timer_get_time() / 1000000);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
