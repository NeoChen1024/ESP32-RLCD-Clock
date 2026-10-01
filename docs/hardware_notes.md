# RLCD Time Scale Monitor — Hardware Notes

This is the board and media reference for the Waveshare ESP32-S3 RLCD 4.2.
The firmware design and current progress remain in
[the roadmap](roadmap.md).
See also the [local schematic](ESP32-S3-RLCD-4.2-schematic.pdf). Firmware
data flow is covered in [architecture](architecture.md) and storage partitioning in [storage](storage.md).

## Board and connections

- ESP32-S3; the development board has revision v0.2. Dual-core Xtensa LX7 at up to
  240 MHz, 512 KiB SRAM, 16 MiB flash, 8 MiB embedded PSRAM, 2.4 GHz
  Wi-Fi and Bluetooth LE.
- 4.2-inch ST7305 reflective panel, native 300×400 and used as a 400×300
  landscape display. Its SPI bus runs in mode 0 at 24 MHz in this firmware.
- PCF85063A RTC, SHTC3 temperature/humidity sensor, 18650 battery holder,
  microSD slot, KEY/BOOT buttons and a 2×8 expansion header.
- USB-Serial/JTAG is the host-facing serial interface (`303a:1001`); UART0
  on GPIO43/44 is unused. The device node may change; discover it via
  `/dev/serial/by-id`.

| Connection | GPIO / address | Source or note |
| --- | --- | --- |
| RLCD MOSI / SCK | 12 / 11 | Vendor `11_U8G2_Test` example |
| RLCD DC / CS / RESET / TE | 5 / 40 / 41 / 6 | Same vendor example |
| Shared I²C SDA / SCL | 13 / 14 | SHTC3 and PCF85063A |
| SHTC3 / PCF85063A | 0x70 / 0x51 | 7-bit I²C addresses |
| Battery voltage | GPIO4, ADC1 channel 3 | 3× divider; ADC 12 dB attenuation |
| SDMMC CLK / CMD / D0 | 38 / 21 / 39 | Vendor `06_SD_Card` example, 1-bit bus at 20 MHz |
| I²S MCLK / BCLK / WS / DOUT / DIN | 16 / 9 / 45 / 8 / 10 | Vendor `07_Audio_Test` `board_cfg.txt` (`S3_RLCD_4_2`) |
| ES8311 DAC / ES7210 ADC | 0x18 / 0x40 | 7-bit, on the shared I²C bus; ES7210 (microphones) is unused |
| Speaker PA enable | GPIO46, active high | Vendor configuration uses 6 dB PA gain |

## Panel orientation and polarity

The ST7305 u8g2 backend uses `U8G2_R1` rotation. Drawing coordinates are
400×300 landscape, while the panel's physical buffer is 304×400 after tile
padding. The host's buffer is 400×304. `snapshot.c` maps the target buffer
back to the host layout before PBM/BMP encoding; the SPI flush uses the native
buffer without that remap.

The panel's observed ink polarity is **bit 0 = black, bit 1 = white**, the
opposite of the u8g2/host buffer convention. The vendor-derived ST7305
backend therefore XORs tile bytes with `0xFF` only when sending them to the
panel. The in-memory buffer and exported snapshots remain in host convention
so screenshots can be compared byte for byte.

## RTC power and verification boundary

No RTC backup battery is installed or currently available. The 18650 or USB
power can keep the board running, but a full loss of RTC power can stop its
oscillator. The PCF85063A oscillator-stop (OS) flag then invalidates a boot
time read; the firmware does not treat an unverified date as trusted time.
RTC-seeded boot works while RTC power is maintained (TRUSTED or RTC_HOLD,
see [time](time.md#time-state)). Independent
backup operation across a true power loss is **unverified** and deferred
until a backup battery is available.

The RTC uses two-digit years (this driver supports 2000–2099), BCD time
registers at `0x04`–`0x0A`, an OS flag in the seconds register, and one free
RAM byte at `0x03`. The application writes its own marker into that byte
after verifying a calibration. These register details are documented in the
[NXP PCF85063A data sheet](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf).
