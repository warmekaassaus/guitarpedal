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
     Encoder button = reset timer

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

#define DEFAULT_DURATION_MIN   60

#define MIN_DURATION_MIN       1
#define MAX_DURATION_MIN       240
#define DURATION_STEP_MIN      1

#define PEDAL_HOLD_MS          1000
#define KNOB_HOLD_MS           1000

#define DEFAULT_LED_BRIGHTNESS 40
#define MIN_LED_BRIGHTNESS     0
#define MAX_LED_BRIGHTNESS     255
#define LED_BRIGHTNESS_STEP    5

#define EEPROM_SIZE            16

#define EEPROM_MAGIC           0x47
#define BRIGHTNESS_MAGIC       0xB6
#define HIGH_SCORE_MAGIC       0xD1


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
bool settingsSavePending = false;
unsigned long encoderLastChangedAt = 0;
const unsigned long EEPROM_SAVE_DELAY_MS = 1000;

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

// Easter egg triggers, both with the knob button held while stopped:
// tap the pedal 3x = dino game, hold the pedal DVD_HOLD_MS = DVD.
#define DVD_HOLD_MS            2000

bool gameActive = false;
bool dvdActive = false;
uint8_t eggTaps = 0;
unsigned long eggFirstTapAt = 0;
uint16_t gameHighScore = 0;


// ============================================================
// ENCODER
// ============================================================

volatile int encoderDelta = 0;
volatile uint8_t lastEncoderState = 0;

int encoderPosition = 0;

// Accumulate four quadrature transitions into one detent
int encoderTransitionAccumulator = 0;

// Brightness adjustment screen
bool brightnessScreenActive = false;
bool encoderHoldHandled = false;
unsigned long encoderPressStarted = 0;

// LED brightness settings
uint8_t ledBrightness = DEFAULT_LED_BRIGHTNESS;
uint8_t lastSavedBrightness = DEFAULT_LED_BRIGHTNESS;
bool brightnessSavePending = false;
unsigned long brightnessLastChangedAt = 0;


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
  uint8_t brightness;
  uint8_t brightnessMagic;
  uint16_t highScore;
  uint8_t highScoreMagic;
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

  // Brightness was added after the original firmware. If no valid
  // brightness value exists, keep the original default.
  if (settings.brightnessMagic == BRIGHTNESS_MAGIC) {
    ledBrightness = constrain(
      settings.brightness,
      MIN_LED_BRIGHTNESS,
      MAX_LED_BRIGHTNESS
    );
  } else {
    ledBrightness = DEFAULT_LED_BRIGHTNESS;
  }

  gameHighScore =
    (settings.highScoreMagic == HIGH_SCORE_MAGIC) ? settings.highScore : 0;

  lastSavedMinutes = programmedMinutes;
  lastSavedBrightness = ledBrightness;
}


void saveSettings() {

  Settings settings;

  settings.magic = EEPROM_MAGIC;
  settings.durationMinutes = programmedMinutes;
  settings.brightness = ledBrightness;
  settings.brightnessMagic = BRIGHTNESS_MAGIC;
  settings.highScore = gameHighScore;
  settings.highScoreMagic = HIGH_SCORE_MAGIC;

  EEPROM.put(0, settings);
  EEPROM.commit();

  lastSavedMinutes = programmedMinutes;
  lastSavedBrightness = ledBrightness;
  settingsSavePending = false;
  brightnessSavePending = false;
}


void scheduleSettingsSave() {
  settingsSavePending = (programmedMinutes != lastSavedMinutes);
  encoderLastChangedAt = millis();
}

void scheduleBrightnessSave() {
  brightnessSavePending = (ledBrightness != lastSavedBrightness);
  brightnessLastChangedAt = millis();
}

void handleDeferredSettingsSave() {
  if (settingsSavePending) {
    if (programmedMinutes == lastSavedMinutes) {
      settingsSavePending = false;
    } else if (millis() - encoderLastChangedAt >= EEPROM_SAVE_DELAY_MS) {
      saveSettings();
    }
  }

  if (brightnessSavePending) {
    if (ledBrightness == lastSavedBrightness) {
      brightnessSavePending = false;
    } else if (millis() - brightnessLastChangedAt >= EEPROM_SAVE_DELAY_MS) {
      saveSettings();
    }
  }
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

  // In brightness mode, the encoder adjusts brightness regardless
  // of the timer state.
  if (brightnessScreenActive) {
    encoderTransitionAccumulator += delta;

    while (encoderTransitionAccumulator >= 4) {
      encoderTransitionAccumulator -= 4;

      int newBrightness =
        (int)ledBrightness + LED_BRIGHTNESS_STEP;

      ledBrightness = constrain(
        newBrightness,
        MIN_LED_BRIGHTNESS,
        MAX_LED_BRIGHTNESS
      );

      strip.setBrightness(ledBrightness);
      scheduleBrightnessSave();
    }

    while (encoderTransitionAccumulator <= -4) {
      encoderTransitionAccumulator += 4;

      int newBrightness =
        (int)ledBrightness - LED_BRIGHTNESS_STEP;

      ledBrightness = constrain(
        newBrightness,
        MIN_LED_BRIGHTNESS,
        MAX_LED_BRIGHTNESS
      );

      strip.setBrightness(ledBrightness);
      scheduleBrightnessSave();
    }

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
    uint16_t previousMinutes = programmedMinutes;
    programmedMinutes += DURATION_STEP_MIN;
    encoderTransitionAccumulator -= 4;

    if (programmedMinutes > MAX_DURATION_MIN)
      programmedMinutes = MAX_DURATION_MIN;

    if (programmedMinutes != previousMinutes)
      scheduleSettingsSave();

    resetTimer();
  }

  while (encoderTransitionAccumulator <= -4) {
    uint16_t previousMinutes = programmedMinutes;

    if (programmedMinutes > MIN_DURATION_MIN)
      programmedMinutes -= DURATION_STEP_MIN;

    encoderTransitionAccumulator += 4;

    if (programmedMinutes != previousMinutes)
      scheduleSettingsSave();

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
        encoderPressStarted = millis();
        encoderHoldHandled = false;
        eggTaps = 0;
      } else {
        // The release after a long press only completes the hold that
        // entered brightness mode; a later short click exits it.
        if (!encoderHoldHandled) {
          if (brightnessScreenActive) {
            brightnessScreenActive = false;
            encoderTransitionAccumulator = 0;
          } else {
            resetTimer();
            resetFlashStarted = millis();
            resetFlashActive = true;
          }
        }
      }
    }
  }

  // Long press enters brightness adjustment.
  if (
    lastEncoderButton == LOW &&
    !encoderHoldHandled &&
    millis() - encoderPressStarted >= KNOB_HOLD_MS
  ) {
    brightnessScreenActive = true;
    encoderHoldHandled = true;
    encoderTransitionAccumulator = 0;

    // Show the current brightness immediately.
    strip.setBrightness(ledBrightness);
  }
}

// ============================================================
// PEDAL
// ============================================================

void handlePedal() {

  bool reading = (digitalRead(PEDAL) == HIGH);

  /*
     Roland DP-2 is normally CLOSED.

     External pull-up on D0.
     Other side of pedal goes to GND.

     Released = LOW
     Pressed  = HIGH
  */

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

        // While stopped, pedal taps with the knob held only count
        // towards the easter egg (and block the knob's click/hold).
        if (
          lastEncoderButton == LOW &&
          !brightnessScreenActive &&
          timerState == TIMER_STOPPED
        ) {
          pedalHoldHandled = true;
          encoderHoldHandled = true;
          registerEggTap();
        }

      }

      // --------------------------
      // Pedal released
      // --------------------------

      else {

        pedalPressed = false;

        if (!pedalHoldHandled) {
          if (getOvertimeMs() != 0) {
            resetTimer();

            resetFlashStarted = millis();
            resetFlashActive = true;
          } else if (timerState == TIMER_STOPPED) {

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

  // Easter egg: knob held, then the pedal held down (as the first
  // egg tap) long enough = DVD screensaver.
  if (
    pedalPressed &&
    eggTaps == 1 &&
    lastEncoderButton == LOW &&
    timerState == TIMER_STOPPED &&
    millis() - pedalPressStarted >= DVD_HOLD_MS
  ) {
    eggTaps = 0;
    startDvd();
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

void updateDisplay() {

  char timeString[16];

  oled.clearBuffer();

  if (brightnessScreenActive) {
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 9, "LED BRIGHTNESS");

    // 0..255 mapped to a 0..124 pixel bar.
    const int barX = 2;
    const int barY = 12;
    const int barW = 124;
    const int barH = 10;
    int fillW = ((int)ledBrightness * (barW - 2)) / 255;

    oled.drawFrame(barX, barY, barW, barH);

    if (fillW > 0)
      oled.drawBox(barX + 1, barY + 1, fillW, barH - 2);

    char brightnessString[8];
    snprintf(
      brightnessString,
      sizeof(brightnessString),
      "%u%%",
      (unsigned int)(((uint16_t)ledBrightness * 100U + 127U) / 255U)
    );

    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 32, brightnessString);

    oled.sendBuffer();
    return;
  }

  // --------------------------
  // STOPPED
  // --------------------------

  if (timerState == TIMER_STOPPED) {

    oled.setFont(u8g2_font_6x10_tf);

    oled.drawStr(0, 22, "SET");

    oled.setFont(u8g2_font_logisoso30_tn);

    formatTime(
      remainingMs,
      timeString,
      sizeof(timeString)
    );

    oled.drawStr(20, 32, timeString);
  }


  // --------------------------
  // RUNNING
  // --------------------------

  else if (
    timerState == TIMER_RUNNING &&
    remainingMs > 0
  ) {

    oled.setFont(u8g2_font_logisoso30_tn);

    formatTime(
      remainingMs,
      timeString,
      sizeof(timeString)
    );

    oled.drawStr(20, 32, timeString);
  }


  // --------------------------
  // PAUSED
  // --------------------------

  else if (timerState == TIMER_PAUSED) {

    // Pause symbol: two filled vertical bars.
    oled.drawBox(5, 13, 3, 10);
    oled.drawBox(11, 13, 3, 10);

    oled.setFont(u8g2_font_logisoso30_tn);

    formatTime(
      remainingMs,
      timeString,
      sizeof(timeString)
    );

    oled.drawStr(20, 32, timeString);
  }


  // --------------------------
  // OVERTIME
  // --------------------------

  else {

    uint32_t overtime =
      getOvertimeMs();
    
    // Plus symbol: two overlapping rectangles
    oled.drawBox(5, 16, 11, 3); // horizontal
    oled.drawBox(9, 12, 3, 11); // vertical

    oled.setFont(u8g2_font_logisoso30_tn);

    formatTime(
      overtime,
      timeString,
      sizeof(timeString)
    );


    oled.drawStr(20, 32, timeString);
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


void pulseLEDs(uint8_t red, uint8_t green, uint8_t blue) {
  const unsigned long PULSE_PERIOD_MS = 3000;

  float phase = (millis() % PULSE_PERIOD_MS) * 6.2831853f / PULSE_PERIOD_MS;
  float pulse = (sinf(phase - 1.5707963f) + 1.0f) * 0.5f;
  uint8_t level = (uint8_t)(pulse * 255.0f + 0.5f);

  uint8_t pulsedRed = ((uint16_t)red * level) / 255;
  uint8_t pulsedGreen = ((uint16_t)green * level) / 255;
  uint8_t pulsedBlue = ((uint16_t)blue * level) / 255;

  setAllLEDs(strip.Color(pulsedRed, pulsedGreen, pulsedBlue));
}


void updateLEDs() {
  // Brightness adjustment mode is a live LED preview.
  if (brightnessScreenActive) {
    strip.setBrightness(ledBrightness);
    setAllLEDs(strip.Color(255, 255, 255));
    return;
  }

  // Three blue flashes confirm that a long hold reset the timer.
  if (resetFlashActive) {

    unsigned long elapsed = millis() - resetFlashStarted;

    if (elapsed >= RESET_FLASH_DURATION_MS) {
      resetFlashActive = false;
    } else {
      bool flashOn = ((elapsed / RESET_FLASH_INTERVAL_MS) % 2) == 0;
      setAllLEDs(
        flashOn ? strip.Color(0, 0, 255) : strip.Color(0, 0, 0)
      );
      return;
    }
  }

  if (pedalPressed) {
    setAllLEDs(strip.Color(255, 255, 255));
    return;
  }

  if (timerState == TIMER_STOPPED) {

    setAllLEDs(
      strip.Color(0, 0, 0)
    );
  }


  else if (timerState == TIMER_PAUSED) {

    // Breathe the existing paused amber color.
    pulseLEDs(255, 100, 0);
  }


  else if (remainingMs > 0) {

    // Green
    setAllLEDs(
      strip.Color(0, 255, 0)
    );
  }


  else {

    // Pulsing red = overtime
    pulseLEDs(255, 0, 0);
  }
}


// ============================================================
// EASTER EGG: DINO GAME
// ============================================================

// Only reachable while stopped: hold the knob button and tap the
// pedal EGG_TAPS times within EGG_WINDOW_MS.
// Pedal = jump / restart, knob click = exit.

#define EGG_TAPS               3
#define EGG_WINDOW_MS          2000

#define GAME_STEP_MS           25
#define GAME_GROUND_Y          31
#define DINO_X                 8
#define DINO_W                 10
#define DINO_H                 11
#define NUM_CACTI              3

// Tuning knobs, in pixels per step
const float GAME_GRAVITY = 0.22f;
const float GAME_JUMP_VELOCITY = 2.7f;
const float GAME_START_SPEED = 1.5f;
const float GAME_MAX_SPEED = 4.0f;

// Frames differ only in the bottom (leg) row:
// 0 = both feet down (standing / jumping), 1-2 = running.
static const unsigned char dinoFrames[3][22] U8X8_PROGMEM = {
  {
    0xE0, 0x03, 0xA0, 0x03, 0xE0, 0x03, 0xE0, 0x00,
    0xF1, 0x00, 0xF9, 0x01, 0xFF, 0x00, 0x7E, 0x00,
    0x3C, 0x00, 0x24, 0x00, 0x6C, 0x00
  },
  {
    0xE0, 0x03, 0xA0, 0x03, 0xE0, 0x03, 0xE0, 0x00,
    0xF1, 0x00, 0xF9, 0x01, 0xFF, 0x00, 0x7E, 0x00,
    0x3C, 0x00, 0x24, 0x00, 0x0C, 0x00
  },
  {
    0xE0, 0x03, 0xA0, 0x03, 0xE0, 0x03, 0xE0, 0x00,
    0xF1, 0x00, 0xF9, 0x01, 0xFF, 0x00, 0x7E, 0x00,
    0x3C, 0x00, 0x24, 0x00, 0x60, 0x00
  }
};

struct Cactus {
  float x;
  uint8_t w;
  uint8_t h;
};

Cactus cacti[NUM_CACTI];

float dinoY = 0;         // height above the ground
float dinoVelocity = 0;
float gameSpeed = GAME_START_SPEED;
uint32_t gameSteps = 0;
uint16_t gameScore = 0;
bool gameOver = false;
bool gameNewHighScore = false;
unsigned long gameOverAt = 0;
unsigned long gameStartedAt = 0;
unsigned long milestoneAt = 0;
unsigned long gameLastStep = 0;
bool eggLastPedal = false;
bool eggLastKnob = LOW;


void registerEggTap() {
  unsigned long now = millis();

  if (eggTaps == 0 || now - eggFirstTapAt > EGG_WINDOW_MS) {
    eggTaps = 0;
    eggFirstTapAt = now;
  }

  if (++eggTaps >= EGG_TAPS) {
    eggTaps = 0;
    startGame();
  }
}


void spawnCactus(int i) {
  float farthest = 128;

  for (int j = 0; j < NUM_CACTI; j++) {
    if (j != i && cacti[j].x > farthest)
      farthest = cacti[j].x;
  }

  // Gap scales with speed so every gap stays jumpable.
  cacti[i].x = farthest + gameSpeed * random(30, 60);
  cacti[i].w = random(3, 7);
  cacti[i].h = random(6, 13);
}


void resetGame() {
  gameOver = false;
  dinoY = 0;
  dinoVelocity = 0;
  gameSpeed = GAME_START_SPEED;
  gameSteps = 0;
  gameScore = 0;
  gameNewHighScore = false;
  gameStartedAt = millis();
  milestoneAt = 0;

  for (int i = 0; i < NUM_CACTI; i++)
    cacti[i].x = -100;

  for (int i = 0; i < NUM_CACTI; i++)
    spawnCactus(i);
}


void startGame() {
  gameActive = true;
  eggLastPedal = (digitalRead(PEDAL) == HIGH);
  eggLastKnob = digitalRead(ENC_SW);
  randomSeed(micros());
  resetGame();
}


void exitEasterEgg() {
  gameActive = false;
  dvdActive = false;

  // Swallow the inputs that are still down so they don't leak into
  // the timer: the exit click must not reset, the pedal must not start.
  lastEncoderButton = LOW;
  encoderHoldHandled = true;

  lastPedalReading = (digitalRead(PEDAL) == HIGH);
  pedalPressed = lastPedalReading;
  pedalHoldHandled = true;

  noInterrupts();
  encoderDelta = 0;
  interrupts();
  encoderTransitionAccumulator = 0;
}


void stepGame(bool jumpPressed) {
  if (jumpPressed && dinoY == 0)
    dinoVelocity = GAME_JUMP_VELOCITY;

  dinoY += dinoVelocity;
  dinoVelocity -= GAME_GRAVITY;

  if (dinoY <= 0) {
    dinoY = 0;
    dinoVelocity = 0;
  }

  // Hitbox shrunk by 2px so near misses feel fair.
  int dinoLeft = DINO_X + 2;
  int dinoRight = DINO_X + DINO_W - 2;
  int dinoBottom = GAME_GROUND_Y - (int)dinoY;

  for (int i = 0; i < NUM_CACTI; i++) {
    cacti[i].x -= gameSpeed;

    if (cacti[i].x + cacti[i].w < 0)
      spawnCactus(i);

    int cx = (int)cacti[i].x;

    if (
      dinoLeft < cx + cacti[i].w &&
      cx < dinoRight &&
      dinoBottom > GAME_GROUND_Y - cacti[i].h
    ) {
      gameOver = true;
      gameOverAt = millis();

      if (gameScore > gameHighScore) {
        gameHighScore = gameScore;
        gameNewHighScore = true;
        saveSettings();
      }
      return;
    }
  }

  gameSteps++;
  uint16_t newScore = gameSteps / 4;

  if (newScore / 100 > gameScore / 100)
    milestoneAt = millis();

  gameScore = newScore;
  gameSpeed = min(GAME_MAX_SPEED, GAME_START_SPEED + gameScore / 200.0f);
}


void drawGame() {
  char text[16];

  oled.clearBuffer();
  oled.drawHLine(0, GAME_GROUND_Y, 128);

  // Legs swap every 4 steps (100ms) while running on the ground.
  int frame = (gameOver || dinoY > 0) ? 0 : 1 + (gameSteps / 4) % 2;

  oled.drawXBMP(
    DINO_X,
    GAME_GROUND_Y - DINO_H - (int)dinoY,
    DINO_W,
    DINO_H,
    dinoFrames[frame]
  );

  // Clip by hand: u8g2 coordinates are unsigned.
  for (int i = 0; i < NUM_CACTI; i++) {
    int left = max((int)cacti[i].x, 0);
    int right = min((int)cacti[i].x + cacti[i].w, 128);

    if (right > left)
      oled.drawBox(left, GAME_GROUND_Y - cacti[i].h, right - left, cacti[i].h);
  }

  oled.setFont(u8g2_font_6x10_tf);

  snprintf(text, sizeof(text), "%u", gameScore);
  oled.drawStr(128 - oled.getStrWidth(text), 9, text);

  if (gameOver) {
    oled.drawStr(37, 12, "GAME OVER");
    snprintf(text, sizeof(text), "HI %u", gameHighScore);
    oled.drawStr(37, 22, text);
  }

  oled.sendBuffer();
}


// The strip is two vertical columns on the pedal, chained as:
//   right column, LEDs 0-3, top to bottom
//   left column,  LEDs 4-7, bottom to top
// Row 0 is the bottom (the ground), the top row is the dino's max jump.
// ponytail: assumes the right column is first in the chain; if the
// game looks upside down, swap the two branches in ledAt().
#define LED_ROWS (NUM_LEDS / 2)

int ledAt(int column, int row) {
  return (column == 0) ? LED_ROWS - 1 - row : LED_ROWS + row;
}

// Effects are layered into this buffer, brightest channel wins.
uint8_t gameLeds[NUM_LEDS][3];

const unsigned long GAME_INTRO_MS = 400;
const unsigned long GAME_CRASH_MS = 700;
const unsigned long GAME_MILESTONE_MS = 800;
const float GAME_MAX_JUMP_HEIGHT =
  GAME_JUMP_VELOCITY * GAME_JUMP_VELOCITY / (2 * GAME_GRAVITY);

// One LED row = this many OLED pixels of height, so a cactus bar and
// the dino dot are drawn to the same scale as on screen.
const float GAME_PX_PER_ROW = GAME_MAX_JUMP_HEIGHT / LED_ROWS;


void blendGameLED(int i, uint8_t r, uint8_t g, uint8_t b, float amount) {
  if (i < 0 || i >= NUM_LEDS || amount <= 0)
    return;

  amount = min(amount, 1.0f);

  uint8_t colour[3] = { r, g, b };

  for (int c = 0; c < 3; c++)
    gameLeds[i][c] = max(gameLeds[i][c], (uint8_t)(colour[c] * amount));
}


// Same row on both columns.
void blendGameRow(int row, uint8_t r, uint8_t g, uint8_t b, float amount) {
  if (row < 0 || row >= LED_ROWS)
    return;

  blendGameLED(ledAt(0, row), r, g, b, amount);
  blendGameLED(ledAt(1, row), r, g, b, amount);
}


// Bar from the ground up to a fractional row; the top LED fades in.
void blendGameBar(float level, uint8_t r, uint8_t g, uint8_t b, float amount) {
  for (int row = 0; row < LED_ROWS; row++)
    blendGameRow(row, r, g, b, amount * constrain(level - row, 0.0f, 1.0f));
}


// Dot at a fractional row, split over two rows so it glides.
void blendGameDot(float level, uint8_t r, uint8_t g, uint8_t b) {
  int lo = floorf(level);
  float frac = level - lo;

  blendGameRow(lo, r, g, b, 1 - frac);
  blendGameRow(lo + 1, r, g, b, frac);
}


void showGameLEDs() {
  unsigned long now = millis();

  memset(gameLeds, 0, sizeof(gameLeds));

  if (gameOver) {
    // Crash: white flash at the ground, a burst shooting up both
    // columns, then a slow pulse. Gold for a new high score.
    unsigned long elapsed = now - gameOverAt;
    uint8_t green = gameNewHighScore ? 160 : 0;

    if (elapsed >= GAME_CRASH_MS) {
      pulseLEDs(255, green, 0);
      return;
    }

    float progress = elapsed / (float)GAME_CRASH_MS;
    float front = progress * 2 * LED_ROWS;

    for (int row = 0; row < LED_ROWS; row++) {
      if (elapsed < 80)
        blendGameRow(row, 255, 255, 255, 1 - row / (float)LED_ROWS);

      if (row <= front)
        blendGameRow(row, 255, green, 0, (1 - progress) * (1 - (front - row) / LED_ROWS));
    }

  } else if (milestoneAt != 0 && now - milestoneAt < GAME_MILESTONE_MS) {
    // Every 100 points: a rainbow climbing both columns.
    for (int row = 0; row < LED_ROWS; row++) {
      uint16_t hue = now * 150 - row * (65536 / LED_ROWS);
      uint32_t colour = strip.gamma32(strip.ColorHSV(hue));
      blendGameRow(row, colour >> 16, colour >> 8, colour, 1);
    }

  } else {
    // Ground: the bottom LEDs are the dino's feet, stepping left/right
    // in sync with the running animation on the OLED.
    bool onGround = (dinoY == 0);
    bool rightFoot = (gameSteps / 4) % 2;

    blendGameLED(ledAt(0, 0), 0, 255, 0, (onGround && rightFoot) ? 0.35f : 0.06f);
    blendGameLED(ledAt(1, 0), 0, 255, 0, (onGround && !rightFoot) ? 0.35f : 0.06f);

    // Intro: green rising up both columns.
    if (now - gameStartedAt < GAME_INTRO_MS)
      blendGameBar((now - gameStartedAt) * LED_ROWS / (float)GAME_INTRO_MS, 0, 255, 0, 1);

    // Nearest incoming cactus: a bar as tall as the cactus, fading in
    // from the horizon and turning from orange to red as it gets close.
    float nearestX = 1e9;
    int nearest = -1;

    for (int i = 0; i < NUM_CACTI; i++) {
      if (cacti[i].x + cacti[i].w > DINO_X && cacti[i].x < 128 && cacti[i].x < nearestX) {
        nearestX = cacti[i].x;
        nearest = i;
      }
    }

    if (nearest >= 0) {
      float closeness = constrain(1 - (nearestX - DINO_X) / (128.0f - DINO_X), 0.0f, 1.0f);

      blendGameBar(
        cacti[nearest].h / GAME_PX_PER_ROW,
        255, 120 * (1 - closeness), 0,
        closeness
      );
    }

    // Dino: a cyan dot at the height of its feet while jumping. Where
    // it overlaps the cactus bar the channels mix to white: near miss.
    if (!onGround)
      blendGameDot(dinoY / GAME_PX_PER_ROW, 0, 255, 255);
  }

  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, gameLeds[i][0], gameLeds[i][1], gameLeds[i][2]);

  strip.show();
}


void updateGame() {
  // Inputs are sampled once per step; 25ms spacing doubles as debounce.
  if (millis() - gameLastStep < GAME_STEP_MS)
    return;

  // ponytail: a slow frame drops time instead of catching up
  gameLastStep = millis();

  bool knob = digitalRead(ENC_SW);

  if (knob == LOW && eggLastKnob == HIGH) {
    exitEasterEgg();
    return;
  }

  eggLastKnob = knob;

  bool pedal = (digitalRead(PEDAL) == HIGH);
  bool pedalTapped = pedal && !eggLastPedal;
  eggLastPedal = pedal;

  if (gameOver) {
    // Short lockout so a panicked stomp doesn't skip the game over.
    if (pedalTapped && millis() - gameOverAt > 500)
      resetGame();
  } else {
    stepGame(pedalTapped);
  }

  drawGame();
  showGameLEDs();
}


// ============================================================
// EASTER EGG: DVD SCREENSAVER
// ============================================================

// The OLED is monochrome, so the logo's colour lives on the LEDs:
// it changes on every bounce, and the glow follows the logo around.
// Any knob or pedal press exits.

#define DVD_W                  32
#define DVD_H                  16
#define DVD_STEP_MS            25

// Tuning knobs, in pixels per step. Deliberately incommensurate so
// corner hits are rare (roughly one a minute or two), like the real thing.
const float DVD_SPEED_X = 0.6f;
const float DVD_SPEED_Y = 0.37f;

const unsigned long DVD_BOUNCE_FLASH_MS = 150;
const unsigned long DVD_CORNER_PARTY_MS = 2500;

// DVD Video logo: slanted "DVD" over the disc, "VIDEO" knocked out.
static const unsigned char dvdLogo[] U8X8_PROGMEM = {
  0xFC, 0x31, 0xD8, 0x1F, 0x0C, 0x33, 0xD8, 0x30,
  0x06, 0x33, 0x66, 0x30, 0x06, 0x33, 0x66, 0x30,
  0x06, 0x63, 0x63, 0x30, 0x83, 0xB1, 0x31, 0x18,
  0xC3, 0xE0, 0x30, 0x0C, 0x7F, 0x40, 0xF0, 0x07,
  0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00,
  0x78, 0x95, 0xB1, 0x1F, 0x7E, 0x55, 0x5D, 0x7F,
  0x7F, 0x55, 0x59, 0xFF, 0x7E, 0x55, 0x5D, 0x7F,
  0xF8, 0x96, 0xB1, 0x1F, 0x00, 0xFF, 0xFF, 0x00
};

float dvdX = 0;
float dvdY = 0;
float dvdVX = DVD_SPEED_X;
float dvdVY = DVD_SPEED_Y;
uint16_t dvdHue = 0;
int32_t dvdSteps = 0;
int32_t dvdLastHitX = -100;
int32_t dvdLastHitY = -200;
unsigned long dvdBounceAt = 0;
unsigned long dvdCornerAt = 0;
unsigned long dvdLastStep = 0;


void startDvd() {
  dvdActive = true;
  eggLastPedal = (digitalRead(PEDAL) == HIGH);
  eggLastKnob = digitalRead(ENC_SW);

  randomSeed(micros());
  dvdX = random(0, 128 - DVD_W);
  dvdY = random(0, 32 - DVD_H);
  dvdVX = random(2) ? DVD_SPEED_X : -DVD_SPEED_X;
  dvdVY = random(2) ? DVD_SPEED_Y : -DVD_SPEED_Y;
  dvdHue = random(65536);
  dvdSteps = 0;
  dvdLastHitX = -100;
  dvdLastHitY = -200;
  dvdBounceAt = 0;
  dvdCornerAt = 0;
}


void stepDvd() {
  const float maxX = 128 - DVD_W;
  const float maxY = 32 - DVD_H;

  dvdSteps++;
  dvdX += dvdVX;
  dvdY += dvdVY;

  bool hitX = (dvdX <= 0 || dvdX >= maxX);
  bool hitY = (dvdY <= 0 || dvdY >= maxY);

  if (hitX) {
    dvdX = constrain(dvdX, 0.0f, maxX);
    dvdVX = -dvdVX;
    dvdLastHitX = dvdSteps;
  }

  if (hitY) {
    dvdY = constrain(dvdY, 0.0f, maxY);
    dvdVY = -dvdVY;
    dvdLastHitY = dvdSteps;
  }

  if (!hitX && !hitY)
    return;

  // New colour on every bounce, always a clearly different hue.
  dvdHue += 65536 / 6 + random(65536 * 2 / 3);
  dvdBounceAt = millis();

  // Corner: both walls within one step of each other.
  if (
    (hitX && dvdSteps - dvdLastHitY <= 1) ||
    (hitY && dvdSteps - dvdLastHitX <= 1)
  )
    dvdCornerAt = millis();
}


void drawDvd() {
  oled.clearBuffer();
  oled.drawXBMP((int)dvdX, (int)dvdY, DVD_W, DVD_H, dvdLogo);

  // Corner hit: the whole screen flashes inverted.
  if (
    dvdCornerAt != 0 &&
    millis() - dvdCornerAt < DVD_CORNER_PARTY_MS &&
    ((millis() - dvdCornerAt) / 150) % 2 == 0
  ) {
    oled.setDrawColor(2);  // XOR
    oled.drawBox(0, 0, 128, 32);
    oled.setDrawColor(1);
  }

  oled.sendBuffer();
}


void showDvdLEDs() {
  unsigned long now = millis();

  memset(gameLeds, 0, sizeof(gameLeds));

  if (dvdCornerAt != 0 && now - dvdCornerAt < DVD_CORNER_PARTY_MS) {
    // Corner hit: rainbow racing up both columns.
    for (int row = 0; row < LED_ROWS; row++) {
      uint16_t hue = now * 300 - row * (65536 / LED_ROWS);
      uint32_t colour = strip.gamma32(strip.ColorHSV(hue));
      blendGameRow(row, colour >> 16, colour >> 8, colour, 1);
    }

  } else {
    // Glow in the logo's colour, brightest at the LEDs nearest the
    // logo: left/right column by X, row by Y (top of screen = top row).
    uint32_t colour = strip.gamma32(strip.ColorHSV(dvdHue));
    bool bouncing = (now - dvdBounceAt < DVD_BOUNCE_FLASH_MS);

    float across = dvdX / (128 - DVD_W);
    float level = (1 - dvdY / (32 - DVD_H)) * (LED_ROWS - 1);

    for (int column = 0; column < 2; column++) {
      float columnWeight = (column == 0) ? across : 1 - across;

      for (int row = 0; row < LED_ROWS; row++) {
        float rowWeight = max(0.0f, 1 - fabsf(row - level));
        float amount = bouncing
          ? 1
          : (0.15f + 0.85f * columnWeight) * (0.3f + 0.7f * rowWeight);

        blendGameLED(ledAt(column, row), colour >> 16, colour >> 8, colour, amount);
      }
    }
  }

  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, gameLeds[i][0], gameLeds[i][1], gameLeds[i][2]);

  strip.show();
}


void updateDvd() {
  if (millis() - dvdLastStep < DVD_STEP_MS)
    return;

  dvdLastStep = millis();

  // Same sampling-as-debounce as the game; any new press exits.
  bool knob = digitalRead(ENC_SW);
  bool pedal = (digitalRead(PEDAL) == HIGH);
  bool pressed =
    (knob == LOW && eggLastKnob == HIGH) ||
    (pedal && !eggLastPedal);

  eggLastKnob = knob;
  eggLastPedal = pedal;

  if (pressed) {
    exitEasterEgg();
    return;
  }

  stepDvd();
  drawDvd();
  showDvdLEDs();
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
  strip.setBrightness(DEFAULT_LED_BRIGHTNESS);
  strip.clear();
  strip.show();


  // --------------------------
  // EEPROM
  // --------------------------

  EEPROM.begin(EEPROM_SIZE);

  loadSettings();
  strip.setBrightness(ledBrightness);


  // --------------------------
  // Initial timer
  // --------------------------

  resetTimer();

  resetFlashStarted = millis();
  resetFlashActive = true;
  pedalHoldHandled = true;


  Serial.print("Duration: ");
  Serial.print(programmedMinutes);
  Serial.println(" min");

  Serial.print("LED brightness: ");
  Serial.print(ledBrightness);
  Serial.println(" / 255");

  Serial.println("Ready.");
}


// ============================================================
// MAIN LOOP
// ============================================================

unsigned long lastDisplayUpdate = 0;
unsigned long lastLEDUpdate = 0;

void loop() {

  if (gameActive) {
    updateGame();
    return;
  }

  if (dvdActive) {
    updateDvd();
    return;
  }

  // Timer calculation
  updateTimer();


  // Encoder
  handleEncoder();
  handleDeferredSettingsSave();


  // Encoder pushbutton
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