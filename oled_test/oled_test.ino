#include <Wire.h>
#include <U8g2lib.h>

U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C oled(
  U8G2_R0,
  U8X8_PIN_NONE
);

void setup() {
  Wire.begin(D2, D1);

  oled.begin();
  oled.clearBuffer();

  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 10, "OLED TEST");

  oled.setFont(u8g2_font_7x14B_tf);
  oled.drawStr(0, 28, "Hello!");

  oled.sendBuffer();
}

void loop() {}