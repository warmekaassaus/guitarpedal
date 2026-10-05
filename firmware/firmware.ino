/*
   GIG COUNTDOWN TIMER
   ====================

   Wemos D1 mini / ESP8266

   OLED:
     SDA = D2 / GPIO4
     SCL = D1 / GPIO5
     SSD1305 128x32 I2C

   Rotary encoder:
     A  = D6 / GPIO12
     B  = D5 / GPIO14
     SW = D7 / GPIO13

   Footswitch:
     D0 / GPIO16

     Roland DP-2 = normally CLOSED

     Wiring:
       3.3V --- 10k --- D0 --- DP-2 --- GND

     Released = LOW
     Pressed  = HIGH

   NeoPixels:
     DATA = D4 / GPIO2
     NUM_LEDS = 8

   Timer:
     Encoder changes duration in 5-minute steps
     Encoder button = toggle duration lock
     Pedal:
       stopped -> start
       running -> pause
       paused -> resume
     Pedal held 2 sec = reset
     After zero = overtime

*/

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>


// ============================================================
// PINS
// ============================================================

#define ENC_A       D6
#define ENC_B       D5
#define ENC_SW      D7

#define PEDAL       D0

#define LED_PIN     D4
#define NUM_LEDS    8


// ============================================================
// SETTINGS
// ============================================================

#define DEFAULT_DURATION_MIN 60

#define MIN_DURATION_MIN     5
#define MAX_DURATION_MIN     240
#define DURATION_STEP_MIN    1

#define PEDAL_HOLD_MS        2000

#define LED_BRIGHTNESS       40

#define EEPROM_SIZE          16
#define EEPROM_MAGIC         0x47


// ============================================================
// OLED
// ============================================================

U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C oled(
  U8G2_R0,
  U8X8_PIN_NONE
);


// ============================================================
// NEOPIXELS
// ============================================================

Adafruit_NeoPixel strip(
  NUM_LEDS,
  LED_PIN,
  NEO_GRB + NEO_KHZ800
);


// ============================================================
// TIMER STATE
// ============================================================

enum TimerState {
  TIMER_STOPPED,
  TIMER_RUNNING,
  TIMER_PAUSED
};

TimerState timerState = TIMER_STOPPED;


// Programmed duration
uint16_t programmedMinutes = DEFAULT_DURATION_MIN;
uint16_t lastSavedMinutes = DEFAULT_DURATION_MIN;

// Current countdown
uint32_t remainingMs = 0;

// Time at which the timer crossed zero
unsigned long overtimeStartedAt = 0;

// Used for elapsed-time calculation
unsigned long lastTimerUpdate = 0;


// ============================================================
// PEDAL STATE
// ============================================================

bool pedalPressed = false;
bool lastPedalReading = false;

unsigned long pedalPressStarted = 0;
bool pedalHoldHandled = false;

// Reset confirmation flash (three blue flashes)
bool resetFlashActive = false;
unsigned long resetFlashStarted = 0;
const unsigned long RESET_FLASH_INTERVAL_MS = 150;
const unsigned long RESET_FLASH_DURATION_MS = 900;


// ============================================================
// ENCODER
// ============================================================

volatile int encoderDelta = 0;
volatile uint8_t lastEncoderState = 0;

int encoderPosition = 0;

// Accumulate four quadrature transitions into one detent
int encoderTransitionAccumulator = 0;
bool encoderUnlocked = false;
bool showUnlockPrompt = false;
unsigned long unlockPromptStarted = 0;
const unsigned long UNLOCK_PROMPT_MS = 2000;


// Valid quadrature transitions
const int8_t transitionTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};


// ============================================================
// ENCODER INTERRUPT
// ============================================================

void ICACHE_RAM_ATTR encoderISR() {

  uint8_t a = digitalRead(ENC_A);
  uint8_t b = digitalRead(ENC_B);

  uint8_t currentState = (a << 1) | b;

  uint8_t index =
    (lastEncoderState << 2) |
    currentState;

  encoderDelta += transitionTable[index];

  lastEncoderState = currentState;
}


// ============================================================
// EEPROM
// ============================================================

struct Settings {
  uint8_t magic;
  uint16_t durationMinutes;
};


void loadSettings() {

  Settings settings;

  EEPROM.get(0, settings);

  if (
    settings.magic == EEPROM_MAGIC &&
    settings.durationMinutes >= MIN_DURATION_MIN &&
    settings.durationMinutes <= MAX_DURATION_MIN
  ) {

    programmedMinutes = settings.durationMinutes;

  } else {

    programmedMinutes = DEFAULT_DURATION_MIN;
  }

  // Track the value currently represented in EEPROM.
  lastSavedMinutes = programmedMinutes;
}


void saveSettings() {

  Settings settings;

  settings.magic = EEPROM_MAGIC;
  settings.durationMinutes = programmedMinutes;

  EEPROM.put(0, settings);
  EEPROM.commit();

  lastSavedMinutes = programmedMinutes;
}


// ============================================================
// TIMER
// ============================================================

void resetTimer() {

  remainingMs =
    (uint32_t)programmedMinutes * 60UL * 1000UL;

  overtimeStartedAt = 0;

  timerState = TIMER_STOPPED;

  lastTimerUpdate = millis();
}


void startTimer() {

  if (remainingMs == 0) {

    // If we're already at zero, start overtime
    overtimeStartedAt = millis();
  }

  lastTimerUpdate = millis();
  timerState = TIMER_RUNNING;
}


void pauseTimer() {

  // Update timer before pausing
  updateTimer();

  timerState = TIMER_PAUSED;
}


void resumeTimer() {

  lastTimerUpdate = millis();
  timerState = TIMER_RUNNING;
}


void updateTimer() {

  if (timerState != TIMER_RUNNING)
    return;

  unsigned long now = millis();

  uint32_t elapsed =
    now - lastTimerUpdate;

  lastTimerUpdate = now;


  // Still counting down
  if (remainingMs > 0) {

    if (elapsed >= remainingMs) {

      // We crossed zero during this update
      uint32_t timeToZero = remainingMs;

      remainingMs = 0;

      // Preserve the exact moment we reached zero
      overtimeStartedAt =
        now - (elapsed - timeToZero);

    } else {

      remainingMs -= elapsed;
    }
  }
}


uint32_t getOvertimeMs() {

  if (timerState != TIMER_RUNNING)
    return 0;

  if (remainingMs > 0)
    return 0;

  return millis() - overtimeStartedAt;
}


// ============================================================
// ENCODER
// ============================================================

void handleEncoder() {

  noInterrupts();

  int delta = encoderDelta;

  encoderDelta = 0;

  interrupts();


  if (delta == 0)
    return;


  // Ignore encoder rotation while locked, and show how to unlock.
  if (!encoderUnlocked) {
    encoderTransitionAccumulator = 0;
    showUnlockPrompt = true;
    unlockPromptStarted = millis();
    return;
  }

  // Only allow duration adjustment while stopped
  if (timerState != TIMER_STOPPED) {
    encoderTransitionAccumulator = 0;
    return;
  }


  encoderTransitionAccumulator += delta;


  // Four transitions = one physical detent
  while (encoderTransitionAccumulator >= 4) {

    programmedMinutes += DURATION_STEP_MIN;

    encoderTransitionAccumulator -= 4;

    if (programmedMinutes > MAX_DURATION_MIN)
      programmedMinutes = MAX_DURATION_MIN;

    resetTimer();
  }


  while (encoderTransitionAccumulator <= -4) {

    if (programmedMinutes > MIN_DURATION_MIN)
      programmedMinutes -= DURATION_STEP_MIN;

    encoderTransitionAccumulator += 4;

    resetTimer();
  }
}


// ============================================================
// ENCODER BUTTON
// ============================================================

bool lastEncoderButton = HIGH;

void handleEncoderButton() {

  bool state = digitalRead(ENC_SW);

  if (state != lastEncoderButton) {

    delay(10);

    state = digitalRead(ENC_SW);

    if (state != lastEncoderButton) {

      lastEncoderButton = state;

      if (state == LOW) {

        // Save a changed duration when locking the setting controls.
        if (encoderUnlocked && programmedMinutes != lastSavedMinutes)
          saveSettings();

        encoderUnlocked = !encoderUnlocked;
        encoderTransitionAccumulator = 0;
        showUnlockPrompt = false;
      }
    }
  }
}


// ============================================================
// PEDAL
// ============================================================

void handlePedal() {

  /*
     Roland DP-2 is normally CLOSED.

     External pull-up on D0.
     Other side of pedal goes to GND.

     Released = LOW
     Pressed  = HIGH
  */

  bool reading =
    (digitalRead(PEDAL) == HIGH);


  // Detect change
  if (reading != lastPedalReading) {

    delay(10);

    reading =
      (digitalRead(PEDAL) == HIGH);

    if (reading != lastPedalReading) {

      lastPedalReading = reading;

      // --------------------------
      // Pedal pressed
      // --------------------------

      if (reading) {

        pedalPressed = true;

        pedalPressStarted = millis();
        pedalHoldHandled = false;

      }

      // --------------------------
      // Pedal released
      // --------------------------

      else {

        pedalPressed = false;

        if (!pedalHoldHandled) {

          if (timerState == TIMER_STOPPED) {

            startTimer();

          } else if (timerState == TIMER_RUNNING) {

            pauseTimer();

          } else if (timerState == TIMER_PAUSED) {

            resumeTimer();
          }
        }
      }
    }
  }


  // --------------------------
  // Long press
  // --------------------------

  if (
    pedalPressed &&
    !pedalHoldHandled &&
    millis() - pedalPressStarted >= PEDAL_HOLD_MS
  ) {

    resetTimer();

    resetFlashStarted = millis();
    resetFlashActive = true;
    pedalHoldHandled = true;
  }
}

// ============================================================
// DISPLAY HELPERS
// ============================================================

void formatTime(
  uint32_t milliseconds,
  char *buffer,
  size_t bufferSize
) {

  uint32_t totalSeconds =
    milliseconds / 1000UL;

  uint32_t minutes =
    totalSeconds / 60UL;

  uint32_t seconds =
    totalSeconds % 60UL;

  snprintf(
    buffer,
    bufferSize,
    "%02lu:%02lu",
    (unsigned long)minutes,
    (unsigned long)seconds
  );
}


// ============================================================
// OLED
// ============================================================

void drawStatus(const char *defaultStatus) {
  if (showUnlockPrompt) {
    if (millis() - unlockPromptStarted < UNLOCK_PROMPT_MS) {
      oled.drawStr(0, 9, "PRESS KNOB TO UNLOCK");
      return;
    }

    showUnlockPrompt = false;
  }

  if (encoderUnlocked)
    oled.drawStr(0, 9, "UNLOCKED");
  else
    oled.drawStr(0, 9, defaultStatus);
}


void updateDisplay() {

  char timeString[16];

  oled.clearBuffer();


  // --------------------------
  // STOPPED
  // --------------------------

  if (timerState == TIMER_STOPPED) {

    oled.setFont(u8g2_font_6x10_tf);

    drawStatus("LOCKED");

    oled.setFont(u8g2_font_logisoso20_tf);

    formatTime(
      remainingMs,
      timeString,
      sizeof(timeString)
    );

    oled.drawStr(32, 25, timeString);
  }


  // --------------------------
  // RUNNING
  // --------------------------

  else if (
    timerState == TIMER_RUNNING &&
    remainingMs > 0
  ) {

    oled.setFont(u8g2_font_6x10_tf);

    drawStatus("TIME");

    oled.setFont(u8g2_font_logisoso20_tf);

    formatTime(
      remainingMs,
      timeString,
      sizeof(timeString)
    );

    oled.drawStr(32, 25, timeString);
  }


  // --------------------------
  // PAUSED
  // --------------------------

  else if (timerState == TIMER_PAUSED) {

    oled.setFont(u8g2_font_6x10_tf);

    drawStatus("PAUSED");

    oled.setFont(u8g2_font_logisoso20_tf);

    formatTime(
      remainingMs,
      timeString,
      sizeof(timeString)
    );

    oled.drawStr(32, 25, timeString);
  }


  // --------------------------
  // OVERTIME
  // --------------------------

  else {

    uint32_t overtime =
      getOvertimeMs();

    oled.setFont(u8g2_font_6x10_tf);

    drawStatus("OVERTIME");

    oled.setFont(u8g2_font_logisoso20_tf);

    formatTime(
      overtime,
      timeString,
      sizeof(timeString)
    );

    oled.drawStr(32, 25, timeString);
  }


  oled.sendBuffer();
}


// ============================================================
// LEDS
// ============================================================

void setAllLEDs(uint32_t colour) {

  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, colour);
  }

  strip.show();
}


void updateLEDs() {

  // Three blue flashes confirm that a long hold reset the timer.
  if (resetFlashActive) {

    unsigned long elapsed = millis() - resetFlashStarted;

    if (elapsed >= RESET_FLASH_DURATION_MS) {
      resetFlashActive = false;
    } else {
      bool flashOn = ((elapsed / RESET_FLASH_INTERVAL_MS) % 2) == 0;
      setAllLEDs(
        flashOn ? strip.Color(0, 0, 180) : strip.Color(0, 0, 0)
      );
      return;
    }
  }

  if (pedalPressed) {
    setAllLEDs(
      strip.Color(100, 100, 100)
      );
    return;
  }

  if (timerState == TIMER_STOPPED) {

    setAllLEDs(
      strip.Color(0, 0, 0)
    );
  }


  else if (timerState == TIMER_PAUSED) {

    // Amber / dim
    setAllLEDs(
      strip.Color(100, 40, 0)
    );
  }


  else if (remainingMs > 0) {

    // Green
    setAllLEDs(
      strip.Color(0, 150, 0)
    );
  }


  else {

    // Red = overtime
    setAllLEDs(
      strip.Color(180, 0, 0)
    );
  }
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(200);

  Serial.println();
  Serial.println("============================");
  Serial.println(" GIG COUNTDOWN TIMER");
  Serial.println("============================");


  // --------------------------
  // Encoder
  // --------------------------

  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);

  lastEncoderState =
    (digitalRead(ENC_A) << 1) |
     digitalRead(ENC_B);

  attachInterrupt(
    digitalPinToInterrupt(ENC_A),
    encoderISR,
    CHANGE
  );

  attachInterrupt(
    digitalPinToInterrupt(ENC_B),
    encoderISR,
    CHANGE
  );


  // --------------------------
  // Pedal
  // --------------------------

  // D0 has no useful internal pull-up.
  // External 10k pullup required.
  pinMode(PEDAL, INPUT);


  // --------------------------
  // OLED
  // --------------------------

  Wire.begin(D2, D1);

  oled.begin();

  oled.clearBuffer();

  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 10, "Gig Timer");
  oled.drawStr(0, 25, "Starting...");

  oled.sendBuffer();


  // --------------------------
  // LEDs
  // --------------------------

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();


  // --------------------------
  // EEPROM
  // --------------------------

  EEPROM.begin(EEPROM_SIZE);

  loadSettings();


  // --------------------------
  // Initial timer
  // --------------------------

  resetTimer();


  Serial.print("Duration: ");
  Serial.print(programmedMinutes);
  Serial.println(" min");

  Serial.println("Ready.");
}


// ============================================================
// MAIN LOOP
// ============================================================

unsigned long lastDisplayUpdate = 0;
unsigned long lastLEDUpdate = 0;

void loop() {

  // Timer calculation
  updateTimer();


  // Encoder
  handleEncoder();


  // Encoder pushbutton toggles duration-setting lock
  handleEncoderButton();


  // Foot pedal
  handlePedal();


  // OLED refresh ~25 Hz
  if (millis() - lastDisplayUpdate >= 40) {

    lastDisplayUpdate = millis();

    updateDisplay();
  }


  // LEDs ~50 Hz
  if (millis() - lastLEDUpdate >= 20) {

    lastLEDUpdate = millis();

    updateLEDs();
  }
}