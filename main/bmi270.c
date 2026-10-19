#include "bmi270.h"

#include <string.h>

#include "bmi220fw.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ssd1306.h"

#define BMI220_ADDR_LOW         0x68
#define BMI220_ADDR_HIGH        0x69
#define BMI220_CHIP_ID_REG      0x00
#define BMI220_CHIP_ID          0x26
#define BMI220_DATA_REG         0x0C
#define BMI220_TEMP_REG         0x22
#define BMI220_INTERNAL_STATUS  0x21
#define BMI220_FIFO_COUNT       0x24
#define BMI220_FIFO_CONFIG0     0x48
#define BMI220_FIFO_CONFIG1     0x49
#define BMI220_ACC_CONF         0x40
#define BMI220_ACC_RANGE        0x41
#define BMI220_GYR_CONF         0x42
#define BMI220_GYR_RANGE        0x43
#define BMI220_INIT_CTRL        0x59
#define BMI220_INIT_ADDR        0x5B
#define BMI220_INIT_DATA        0x5E
#define BMI220_PWR_CONF         0x7C
#define BMI220_PWR_CTRL         0x7D
#define BMI220_CMD              0x7E
#define BMI220_RA_GYR_CAS       0x3C
#define BMI220_I2C_SPEED_HZ     100000
#define BMI220_TIMEOUT_MS       100
#define BMI220_FW_CHUNK_SIZE    32

#define BMI220_CMD_SW_RESET     0xB6
#define BMI220_CMD_FIFO_FLUSH   0xB0
#define BMI220_INTERNAL_INIT_OK 0x01
#define BMI220_PWR_CONF_OFF     0x00
#define BMI220_PWR_CONF_FIFO_WAKEUP 0x02
#define BMI220_PWR_CTRL_ON      0x0E
#define BMI220_ACC_CONF_VALUE   0xA8
#define BMI220_ACC_RANGE_16G    0x03
#define BMI220_GYR_CONF_VALUE   0xEA
#define BMI220_GYR_RANGE_1000DPS 0x01
#define BMI220_FIFO_CONFIG0_VALUE 0x01
#define BMI220_FIFO_CONFIG1_VALUE 0xD0

static const char *TAG = "bmi220";

static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static uint8_t s_chip_id;
static int8_t s_zx_factor;
static bool s_present;
static SemaphoreHandle_t s_mutex;

static void scan_i2c_bus(i2c_master_bus_handle_t bus)
{
    bool any = false;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, BMI220_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02x", addr);
            any = true;
        }
    }

    if (!any) {
        ESP_LOGW(TAG, "I2C scan found no devices");
    }
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    if (!s_present || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, data, len, BMI220_TIMEOUT_MS);
    if (ret == ESP_OK) {
        return ESP_OK;
    }

    ret = i2c_master_transmit(s_dev, &reg, 1, BMI220_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    return i2c_master_receive(s_dev, data, len, BMI220_TIMEOUT_MS);
}

static int16_t le16(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static esp_err_t read_regs_from_dev(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, data, len, BMI220_TIMEOUT_MS);
    if (ret == ESP_OK) {
        return ESP_OK;
    }

    ret = i2c_master_transmit(dev, &reg, 1, BMI220_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    return i2c_master_receive(dev, data, len, BMI220_TIMEOUT_MS);
}

static esp_err_t write_reg_to_dev(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {reg, value};
    return i2c_master_transmit(dev, tx, sizeof(tx), BMI220_TIMEOUT_MS);
}

static esp_err_t write_reg16_to_dev(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value)
{
    uint8_t tx[3] = {reg, (uint8_t)(value & 0xFF), (uint8_t)(value >> 8)};
    return i2c_master_transmit(dev, tx, sizeof(tx), BMI220_TIMEOUT_MS);
}

static esp_err_t write_bytes_to_dev(i2c_master_dev_handle_t dev, uint8_t reg,
                                    const uint8_t *data, size_t len)
{
    uint8_t tx[1 + BMI220_FW_CHUNK_SIZE];

    if (data == NULL || len > BMI220_FW_CHUNK_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    tx[0] = reg;
    memcpy(&tx[1], data, len);
    return i2c_master_transmit(dev, tx, len + 1, BMI220_TIMEOUT_MS);
}

// Round up and allow for a call immediately before the next scheduler tick.
// At 100 Hz, pdMS_TO_TICKS(1) alone is zero and a 12 ms wait can be too short.
static void bmi220_wait_ms(uint32_t ms)
{
    TickType_t ticks = (TickType_t)((ms * configTICK_RATE_HZ + 999U) / 1000U);
    vTaskDelay(ticks + 1);
}

static esp_err_t bmi220_upload_firmware(i2c_master_dev_handle_t dev)
{
    uint8_t status = 0;

    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_CMD, BMI220_CMD_SW_RESET),
                        TAG, "soft reset");
    bmi220_wait_ms(12);

    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_PWR_CONF, BMI220_PWR_CONF_OFF),
                        TAG, "disable power save");
    bmi220_wait_ms(1);

    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_INIT_CTRL, 0x00),
                        TAG, "start firmware init");

    for (uint16_t pos = 0; pos < sizeof(bmi220_firmware);) {
        uint16_t remaining = (uint16_t)(sizeof(bmi220_firmware) - pos);
        uint16_t chunk = remaining > BMI220_FW_CHUNK_SIZE ? BMI220_FW_CHUNK_SIZE : remaining;
        uint16_t pos_words = pos >> 1;
        uint16_t init_pos = (pos_words & 0x000F) | ((pos_words << 4) & 0xFF00);

        ESP_RETURN_ON_ERROR(write_reg16_to_dev(dev, BMI220_INIT_ADDR, init_pos),
                            TAG, "set firmware address");
        ESP_RETURN_ON_ERROR(write_bytes_to_dev(dev, BMI220_INIT_DATA,
                                               &bmi220_firmware[pos], chunk),
                            TAG, "write firmware");
        pos = (uint16_t)(pos + chunk);
    }

    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_INIT_CTRL, 0x01),
                        TAG, "end firmware init");
    bmi220_wait_ms(140);

    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_PWR_CONF, BMI220_PWR_CONF_FIFO_WAKEUP),
                        TAG, "set fifo self wakeup");
    ESP_RETURN_ON_ERROR(read_regs_from_dev(dev, BMI220_INTERNAL_STATUS, &status, 1),
                        TAG, "read internal status");
    ESP_LOGI(TAG, "internal_status=0x%02x", status);
    if ((status & BMI220_INTERNAL_INIT_OK) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static esp_err_t bmi220_apply_config(i2c_master_dev_handle_t dev)
{
    uint8_t zx_factor_reg = 0;

    if (read_regs_from_dev(dev, BMI220_RA_GYR_CAS, &zx_factor_reg, 1) == ESP_OK) {
        uint8_t sign_byte = (uint8_t)((zx_factor_reg << 1) & 0x80);
        s_zx_factor = (int8_t)(zx_factor_reg | sign_byte);
        ESP_LOGI(TAG, "zx_factor=%d", s_zx_factor);
    }

    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_GYR_CONF, BMI220_GYR_CONF_VALUE),
                        TAG, "set gyro conf");
    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_GYR_RANGE, BMI220_GYR_RANGE_1000DPS),
                        TAG, "set gyro range");
    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_ACC_CONF, BMI220_ACC_CONF_VALUE),
                        TAG, "set acc conf");
    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_ACC_RANGE, BMI220_ACC_RANGE_16G),
                        TAG, "set acc range");
    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_PWR_CTRL, BMI220_PWR_CTRL_ON),
                        TAG, "power acc gyro temp");
    bmi220_wait_ms(100);

    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_FIFO_CONFIG0, BMI220_FIFO_CONFIG0_VALUE),
                        TAG, "set fifo config0");
    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_FIFO_CONFIG1, BMI220_FIFO_CONFIG1_VALUE),
                        TAG, "set fifo config1");
    bmi220_wait_ms(4);
    ESP_RETURN_ON_ERROR(write_reg_to_dev(dev, BMI220_CMD, BMI220_CMD_FIFO_FLUSH),
                        TAG, "flush fifo");
    bmi220_wait_ms(2);

    return ESP_OK;
}

static void dump_probe_registers(i2c_master_dev_handle_t dev)
{
    uint8_t regs[24] = {0};
    esp_err_t ret = read_regs_from_dev(dev, 0x00, regs, sizeof(regs));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "debug dump 0x00..0x17 failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "dump 00-07: %02x %02x %02x %02x %02x %02x %02x %02x",
             regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7]);
    ESP_LOGI(TAG, "dump 08-0f: %02x %02x %02x %02x %02x %02x %02x %02x",
             regs[8], regs[9], regs[10], regs[11], regs[12], regs[13], regs[14], regs[15]);
    ESP_LOGI(TAG, "dump 10-17: %02x %02x %02x %02x %02x %02x %02x %02x",
             regs[16], regs[17], regs[18], regs[19], regs[20], regs[21], regs[22], regs[23]);
}

static esp_err_t probe_addr(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BMI220_I2C_SPEED_HZ,
    };

    i2c_master_dev_handle_t dev = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_config, &dev),
                        TAG, "add bmi220 device");

    uint8_t chip_id = 0;
    esp_err_t ret = ESP_OK;
    for (int attempt = 0; attempt < 3; attempt++) {
        ret = read_regs_from_dev(dev, BMI220_CHIP_ID_REG, &chip_id, 1);
        ESP_LOGI(TAG, "probe addr=0x%02x attempt=%d chip_id_ret=%s chip_id=0x%02x",
                 addr, attempt + 1, esp_err_to_name(ret), chip_id);
        if (ret != ESP_OK || chip_id == BMI220_CHIP_ID) {
            break;
        }
        bmi220_wait_ms(2);
    }
    s_addr = addr;
    s_chip_id = chip_id;
    if (ret == ESP_OK && chip_id != BMI220_CHIP_ID) {
        dump_probe_registers(dev);
        i2c_master_bus_rm_device(dev);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ret != ESP_OK) {
        i2c_master_bus_rm_device(dev);
        return ret;
    }

    s_dev = dev;
    s_present = true;
    return ESP_OK;
}

esp_err_t bmi270_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    i2c_master_bus_handle_t bus = ssd1306_get_i2c_bus();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    scan_i2c_bus(bus);

    esp_err_t probe_ret = probe_addr(bus, BMI220_ADDR_LOW);
    if (probe_ret == ESP_ERR_INVALID_RESPONSE) {
        s_present = false;
        ESP_LOGW(TAG, "device at 0x%02x has unsupported chip_id=0x%02x, expected BMI220 chip_id=0x%02x",
                 s_addr, s_chip_id, BMI220_CHIP_ID);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (probe_ret != ESP_OK) {
        probe_ret = probe_addr(bus, BMI220_ADDR_HIGH);
    }
    if (probe_ret == ESP_ERR_INVALID_RESPONSE) {
        s_present = false;
        ESP_LOGW(TAG, "device at 0x%02x has unsupported chip_id=0x%02x, expected BMI220 chip_id=0x%02x",
                 s_addr, s_chip_id, BMI220_CHIP_ID);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (probe_ret != ESP_OK) {
        s_present = false;
        ESP_LOGW(TAG, "BMI220 not found at 0x68 or 0x69");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "BMI220 found: addr=0x%02x chip_id=0x%02x", s_addr, s_chip_id);
    esp_err_t ret = bmi220_upload_firmware(s_dev);
    if (ret != ESP_OK) {
        s_present = false;
        ESP_LOGE(TAG, "BMI220 firmware init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bmi220_apply_config(s_dev);
    if (ret != ESP_OK) {
        s_present = false;
        ESP_LOGE(TAG, "BMI220 config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BMI220 initialized");
    return ESP_OK;
}

static esp_err_t read_sample_internal(bmi270_sample_t *sample, bool apply_gyro_cas)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(sample, 0, sizeof(*sample));
    sample->present = s_present;
    sample->bmi220 = s_chip_id == BMI220_CHIP_ID;
    sample->addr = s_addr;
    sample->chip_id = s_chip_id;

    if (!s_present) {
        return ESP_ERR_NOT_FOUND;
    }

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(BMI220_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    uint8_t data[12];
    ret = read_regs(BMI220_DATA_REG, data, sizeof(data));
    if (ret != ESP_OK) {
        goto out;
    }
    sample->acc_x = le16(&data[0]);
    sample->acc_y = le16(&data[2]);
    sample->acc_z = le16(&data[4]);
    sample->gyr_x = le16(&data[6]);
    sample->gyr_y = le16(&data[8]);
    sample->gyr_z = le16(&data[10]);
    if (apply_gyro_cas) {
        sample->gyr_x = (int16_t)((int32_t)sample->gyr_x -
                                  ((int32_t)s_zx_factor * sample->gyr_z) / 512);
    }

    uint8_t temp[2];
    if (read_regs(BMI220_TEMP_REG, temp, sizeof(temp)) == ESP_OK) {
        int16_t raw_temp = le16(temp);
        sample->temp_c10 = (int16_t)(230 + ((int32_t)raw_temp * 1280) / 65535);
    }

out:
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
    return ret;
}

esp_err_t bmi270_read_sample(bmi270_sample_t *sample)
{
    return read_sample_internal(sample, true);
}

esp_err_t bmi270_read_raw_sample(bmi270_sample_t *sample)
{
    return read_sample_internal(sample, false);
}
