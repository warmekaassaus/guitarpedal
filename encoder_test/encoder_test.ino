/*
   Wemos D1 mini / ESP8266
   Encoder + pedal test

   Encoder:
     A  = D6 / GPIO12
     B  = D5 / GPIO14
     SW = D7 / GPIO13

   Pedal (normally closed):
     Pedal contact between D0 / GPIO16 and GND

     Released = circuit CLOSED = LOW
     Pressed  = circuit OPEN   = HIGH
*/

#define ENC_A   D6
#define ENC_B   D5
#define ENC_SW  D7
#define PEDAL   D0

volatile int encoderDelta = 0;
volatile uint8_t lastEncoderState = 0;

int encoderPosition = 0;

bool lastButtonState = HIGH;
bool lastPedalState = LOW;

// Quadrature transition table
const int8_t transitionTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

void ICACHE_RAM_ATTR encoderISR() {
  uint8_t a = digitalRead(ENC_A);
  uint8_t b = digitalRead(ENC_B);

  uint8_t currentState = (a << 1) | b;
  uint8_t index = (lastEncoderState << 2) | currentState;

  encoderDelta += transitionTable[index];

  lastEncoderState = currentState;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("==========================");
  Serial.println(" ENCODER + PEDAL TEST");
  Serial.println("==========================");

  // Encoder
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);

  // Pedal is normally CLOSED - this pin doesn't have internap pullup so added 10k external pullup to 3v3
  pinMode(PEDAL, INPUT);

  // Initialise encoder state
  lastEncoderState =
    (digitalRead(ENC_A) << 1) |
     digitalRead(ENC_B);

  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encoderISR, CHANGE);

  Serial.println("Ready.");
  Serial.println("Rotate encoder, press encoder button, or press pedal.");
  Serial.println();
}

void loop() {

  // -------------------------
  // Encoder
  // -------------------------

  noInterrupts();
  int delta = encoderDelta;
  encoderDelta = 0;
  interrupts();

  if (delta != 0) {
    encoderPosition += delta;

    Serial.print("Encoder position: ");
    Serial.println(encoderPosition);
  }


  // -------------------------
  // Encoder button
  // -------------------------

  bool buttonState = digitalRead(ENC_SW);

  if (buttonState != lastButtonState) {
    delay(10);  // simple debounce

    buttonState = digitalRead(ENC_SW);

    if (buttonState != lastButtonState) {
      lastButtonState = buttonState;

      if (buttonState == LOW) {
        Serial.println("Encoder button: PRESSED");
      } else {
        Serial.println("Encoder button: RELEASED");
      }
    }
  }


  // -------------------------
  // Pedal
  // -------------------------

  bool pedalState = digitalRead(PEDAL);

  if (pedalState != lastPedalState) {
    delay(10);  // simple debounce

    pedalState = digitalRead(PEDAL);

    if (pedalState != lastPedalState) {
      lastPedalState = pedalState;

      if (pedalState == HIGH) {
        Serial.println("PEDAL: PRESSED");
      } else {
        Serial.println("PEDAL: RELEASED");
      }
    }
  }
}