#include "tca_io.h"
#include "hw.h"

#include <string.h>

#include "esp_log.h"
#include "i2cdev.h"
#include "tca95x5.h"

static const char *TAG = "tca_io";

static i2c_dev_t s_tca_dev;
static bool      s_initialized = false;

esp_err_t tca_io_init(void)
{
    memset(&s_tca_dev, 0, sizeof(s_tca_dev));

    esp_err_t ret = i2cdev_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2cdev_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = tca95x5_init_desc(&s_tca_dev, HW_TCA_I2C_ADDR, HW_TCA_I2C_PORT,
                            HW_TCA_SDA_GPIO, HW_TCA_SCL_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tca95x5_init_desc failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* All pins as inputs (0=out, 1=in) */
    ret = tca95x5_port_set_mode(&s_tca_dev, 0xFFFF);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "port_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "TCA9555 initialised on I2C%d SDA=%d SCL=%d addr=0x%02x",
             HW_TCA_I2C_PORT, HW_TCA_SDA_GPIO, HW_TCA_SCL_GPIO, HW_TCA_I2C_ADDR);
    return ESP_OK;
}

esp_err_t tca_io_read_sd_detect(bool *present)
{
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    uint32_t level = 0;
    esp_err_t ret = tca95x5_get_level(&s_tca_dev, HW_TCA_SD_DETECT_PIN, &level);
    if (ret != ESP_OK)
        return ret;
    /* Active-low: pin LOW (level=0) means card is present */
    *present = (level == 0);
    return ESP_OK;
}
