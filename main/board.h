#pragma once
#include <cstdint>
#include <cstddef>

// CoreS3 / StackChan board helpers. All internal-I2C (SDA G12 / SCL G11) traffic
// goes through M5Unified's M5.In_I2C guarded by one mutex (board::i2c_lock()).
namespace board {

void init_m5();                       // M5.begin (display, touch, AXP2101, AW9523 power-up)
void i2c_lock();
void i2c_unlock();
struct I2cGuard { I2cGuard() { i2c_lock(); } ~I2cGuard() { i2c_unlock(); } };

bool i2c_write(uint8_t addr7, uint8_t reg, const uint8_t* data, size_t len, uint32_t freq = 400000);
bool i2c_read(uint8_t addr7, uint8_t reg, uint8_t* data, size_t len, uint32_t freq = 400000);
bool i2c_probe(uint8_t addr7);

void aw88298_reset_release();         // AW9523B P0.2 low->high (AW88298 RST), as factory fw
bool py32_servo_power(bool on);       // PY32L020 (0x6F) IO0 = VM_EN (factory hal_io_expander.cpp)

const char* reset_reason_str();
bool display_ok();
const char* panel_name();

}  // namespace board
