#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length; int device_address; int scl_speed_hz; } i2c_device_config_t;
#define I2C_ADDR_BIT_LEN_7 7
int bsp_i2c_init(void);
void *bsp_i2c_bus(void);
int i2c_master_bus_add_device(void *bus, const i2c_device_config_t *config, void **device);
int i2c_master_bus_rm_device(void *dev);
int i2c_master_transmit_receive(void *dev, const uint8_t *reg, size_t a, uint8_t *buf, size_t n, int timeout);
int i2c_master_transmit(void *dev, const uint8_t *bytes, size_t n, int timeout);
