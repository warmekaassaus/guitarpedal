/*
   Wemos D1 mini / ESP8266
   EC11 rotary encoder interrupt test

   ENC_A = D6 / GPIO12
   ENC_B = D5 / GPIO14
   ENC_SW = D7 / GPIO13
*/

#define ENC_A   D6
#define ENC_B   D5
#define ENC_SW  D7

volatile int encoderDelta = 0;

volatile uint8_t lastEncoderState = 0;

long position = 0;


// ------------------------------------------------------------
// Encoder interrupt
// ------------------------------------------------------------

void IRAM_ATTR encoderISR() {

  uint8_t a = digitalRead(ENC_A);
  uint8_t b = digitalRead(ENC_B);

  uint8_t state = (a << 1) | b;

  /*
     Quadrature transition table.

     Valid clockwise/counter-clockwise transitions
     each produce one count.
  */

  uint8_t transition =
    (lastEncoderState << 2) | state;

  switch (transition) {

    case 0b0001:
    case 0b0111:
    case 0b1110:
    case 0b1000:
      encoderDelta++;
      break;

    case 0b0010:
    case 0b1011:
    case 0b1101:
    case 0b0100:
      encoderDelta--;
      break;
  }

  lastEncoderState = state;
}


// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {

  Serial.begin(115200);

  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);

  // Read initial encoder state
  uint8_t a = digitalRead(ENC_A);
  uint8_t b = digitalRead(ENC_B);

  lastEncoderState = (a << 1) | b;

  /*
     Interrupt on BOTH edges of A and B.
  */

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

  Serial.println();
  Serial.println("======================");
  Serial.println(" EC11 ENCODER TEST");
  Serial.println("======================");
  Serial.println();
  Serial.println("ENC_A = D6");
  Serial.println("ENC_B = D5");
  Serial.println("ENC_SW = D7");
  Serial.println();
}


// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop() {

  // ----------------------------------------------------------
  // Encoder
  // ----------------------------------------------------------

  int delta;

  noInterrupts();

  delta = encoderDelta;
  encoderDelta = 0;

  interrupts();


  if (delta != 0) {

    position += delta;

    Serial.print("Position: ");
    Serial.print(position);

    Serial.print("   Delta: ");
    Serial.println(delta);
  }


  // ----------------------------------------------------------
  // Encoder push button
  // ----------------------------------------------------------

  static bool lastButton = HIGH;

  bool button = digitalRead(ENC_SW);

  if (button != lastButton) {

    delay(5);

    button = digitalRead(ENC_SW);

    if (button != lastButton) {

      lastButton = button;

      if (button == LOW) {
        Serial.println("Encoder button: PRESSED");
      }
      else {
        Serial.println("Encoder button: RELEASED");
      }
    }
  }
}