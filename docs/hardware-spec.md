# AI Passport 硬件规格

## 主控
- **芯片**: ESP32-C3 (QFN32) rev v1.1
- **内核**: RISC-V 32-bit, 单核, 160MHz
- **WiFi**: 2.4 GHz 802.11 b/g/n
- **BLE**: Bluetooth 5.0 LE (NimBLE)
- **USB**: 原生 USB Serial/JTAG (GPIO18/19)

## 存储
- **Flash**: 8MB (XMC 品牌)
- **PSRAM**: 无
- **分区**:
  - bootloader: 0x000000, 20KB
  - nvs: 0x009000, 24KB
  - phy_init: 0x00F000, 4KB
  - factory (app): 0x010000, 3MB
  - appfs (FAT): 0x310000, ~5MB

## 显示
- **面板**: ST7789P3, 240×320, RGB565
- **接口**: 4-line SPI (SPI2_HOST)
- **引脚**: MOSI=9, SCLK=8, CS=1, DC=20, RST=硬接3.3V(无MCU复位), BL=21
- **背光**: LEDC PWM, 10-bit, 5kHz
- **频率**: 40MHz SPI clock
- **反色**: 出厂需反色 (INVON)

## 按键
- **方案**: 3键共享ADC1_CH0 (GPIO0), 分压电阻区分
- **上**: 0Ω → 0mV
- **下**: 1kΩ → ~300mV  
- **确定**: 2.2kΩ → ~595mV
- **松开**: 无通路 → 3300mV
- **窗口**: {0,150}, {150,447}, {447,1900} mV

## 音频
- **Codec**: ES8311, 全双工 I2S
- **I2C 地址**: 0x18 (7-bit)
- **I2S**: MCLK=6, BCLK=5, WS=3, DOUT=2, DIN=4
- **功放**: 无 MCU 控制 (常通)

## 电池
- **电量计**: CW2017, I2C 地址 0x63
- **型号**: 4.2V 520mAh (锂离子)
- **I2C 总线**: 与 ES8311 共用 I2C0

## I2C 总线
- **端口**: I2C_NUM_0
- **SDA**: GPIO10
- **SCL**: GPIO7
- **设备**: ES8311 (0x18) + CW2017 (0x63)

## 供电
- **USB**: 5V USB-C
- **电池**: 3.7V 锂离子, 520mAh
- **充电管理**: 由硬件管理, 无 MCU 控制

## 物理
- **尺寸**: 约卡片大小
- **屏幕**: 240×320 彩色
- **按键**: 3个 (上/下/确定)
- **USB**: USB-C 口

## 注意事项
- 无 PSRAM, LVGL 缓冲区受限
- GPIO0 同时用于 ADC 按键和下载模式
- USB 串口默认 UART0 的 TX 在 GPIO21, 与背光冲突
- Flash 有坏块 (0xB6000, 0xC4000 等), XMC 品质问题
- 无触摸屏, 无 IMU, 无额外 GPIO 引出