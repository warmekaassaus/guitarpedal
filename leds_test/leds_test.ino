/*
   Wemos D1 mini / ESP8266
   NeoPixel / WS2812 test

   DATA = D4 / GPIO2
*/

#include <Adafruit_NeoPixel.h>

#define LED_PIN   D4
#define NUM_LEDS  8

#define BRIGHTNESS 255

Adafruit_NeoPixel strip(
  NUM_LEDS,
  LED_PIN,
  NEO_GRB + NEO_KHZ800
);


// ------------------------------------------------------------
// Set all LEDs to one colour
// ------------------------------------------------------------

void setAll(uint32_t colour) {

  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, colour);
  }

  strip.show();
}


// ------------------------------------------------------------
// Moving pixel
// ------------------------------------------------------------

void movingPixel(uint32_t colour) {

  for (int i = 0; i < NUM_LEDS; i++) {

    strip.clear();

    strip.setPixelColor(i, colour);

    strip.show();

    delay(100);
  }
}


// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {

  Serial.begin(115200);

  Serial.println();
  Serial.println("======================");
  Serial.println(" NEOPIXEL TEST");
  Serial.println("======================");

  Serial.print("LED pin: D4");
  Serial.println();

  Serial.print("Number of LEDs: ");
  Serial.println(NUM_LEDS);

  strip.begin();

  strip.setBrightness(BRIGHTNESS);

  strip.clear();
  strip.show();

  delay(500);
}


// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop() {

  Serial.println("RED");

  setAll(strip.Color(255, 0, 0));

  delay(1000);


  Serial.println("GREEN");

  setAll(strip.Color(0, 255, 0));

  delay(1000);


  Serial.println("BLUE");

  setAll(strip.Color(0, 0, 255));

  delay(1000);


  Serial.println("WHITE");

  setAll(strip.Color(255, 255, 255));

  delay(1000);


  Serial.println("OFF");

  setAll(strip.Color(0, 0, 0));

  delay(1000);


  Serial.println("Moving RED pixel");

  movingPixel(strip.Color(255, 0, 0));

  delay(500);


  Serial.println("Moving GREEN pixel");

  movingPixel(strip.Color(0, 255, 0));

  delay(500);


  Serial.println("Moving BLUE pixel");

  movingPixel(strip.Color(0, 0, 255));

  delay(1000);
}