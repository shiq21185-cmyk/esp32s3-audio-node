#include "cam_reg.h"

#include "esp_log.h"

static const char *TAG = "camreg";

/* 为什么不自己发 I2C：
 * 自己 include driver/i2c_master.h 会把新旧两套 I2C 驱动同时链进来，IDF 在启动时
 * 有 __attribute__((constructor)) 的 check_i2c_driver_conflict()，直接 abort：
 *     E i2c: CONFLICT! driver_ng is not allowed to be used with this old driver
 * 症状是板子每秒重启、日志停在 app_init。
 * 所以这里不碰 I2C 驱动，直接用 esp32-camera 自己那条 SCCB 总线（sccb-ng.c），
 * 它已经 i2c_master_bus_add_device 挂好 0x21 了。
 * SCCB_* 的声明在 esp32-camera/driver/private_include/sccb.h，不在公共 include 里，
 * 所以手动声明（签名一字不差，改驱动版本时留意）。 */
extern uint8_t SCCB_Read(uint8_t slv_addr, uint8_t reg);
extern int SCCB_Write(uint8_t slv_addr, uint8_t reg, uint8_t data);

#define CAMREG_ADDR 0x21
#define CAMREG_SCCB_ERR 0xFF /* sccb-ng 读失败时返回 0xFF */

int cam_reg_init(void)
{
    /* 总线由 esp32-camera 建好，这里没有要初始化的东西。
     * 探一次 0x0A（PID 高字节）确认真的通。 */
    uint8_t pid = SCCB_Read(CAMREG_ADDR, 0x0A);
    if (pid == CAMREG_SCCB_ERR || pid == 0x00) {
        ESP_LOGW(TAG, "寄存器直通可能不通：读 0x0A 得到 0x%02X（期望 0x76）", pid);
        return -1;
    }
    ESP_LOGI(TAG, "寄存器直通就绪（走 esp32-camera 的 SCCB，0x%02X，PID=0x%02X）",
             CAMREG_ADDR, pid);
    return 0;
}

int cam_reg_write(uint8_t reg, uint8_t val)
{
    int rc = SCCB_Write(CAMREG_ADDR, reg, val);
    if (rc != 0) {
        ESP_LOGW(TAG, "写 %02X=%02X 失败（rc=%d）", reg, val, rc);
        return -1;
    }
    return 0;
}

int cam_reg_read(uint8_t reg, uint8_t *out)
{
    if (!out) {
        return -1;
    }
    uint8_t v = SCCB_Read(CAMREG_ADDR, reg);
    if (v == CAMREG_SCCB_ERR) {
        ESP_LOGW(TAG, "读 %02X 失败", reg);
        return -1;
    }
    *out = v;
    return 0;
}
