/*
 * =========================================================================================
 *   BUBU ROBOT — AUTONOMOUS ROAMING ENGINE
 *   (DUAL ULTRASONIC WALL AVOIDANCE + IR BLACK TAPE BOUNDARY)
 * =========================================================================================
 *   FEATURES:
 *   1. IMMEDIATE AUTO-START: Runs immediately on power-up (no clap/sound sensor needed).
 *   2. IR BLACK TAPE BOUNDARY: TCRT5000 sensors (HIGH = black tape) turn away from boundaries.
 *   3. DUAL ULTRASONIC WALL AVOIDANCE: Detects obstacles with Low/High HC-SR04 and turns
 *      until BOTH sensors confirm clear pathway.
 *   4. RANDOM ROAM: Periodic gentle turns for natural, lifelike exploration.
 *   5. BROWNOUT DETECTOR DISABLED: Prevents brownout reboots during motor power surges.
 *   6. HEARTBEAT LED: GPIO 2 blinks continuously at 350ms to indicate active power.
 * =========================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// =========================================================================================
//  USER CONFIGURATION
// =========================================================================================
#define INVERT_LEFT_MOTOR   true
#define SPEED_FWD           175
#define SPEED_TURN          185
#define WALL_DISTANCE_CM    15.0
#define ENABLE_WIFI         true
#define ENABLE_IR_BOUNDARY  true

// =========================================================================================
//  PIN ASSIGNMENTS (ESP32 DevKit V1)
// =========================================================================================
#define PIN_ENA        25
#define PIN_IN1        26
#define PIN_IN2        27
#define PIN_IN3        14
#define PIN_IN4        13
#define PIN_ENB        33
#define PIN_TRIG_LOW   18
#define PIN_ECHO_LOW   19
#define PIN_TRIG_HIGH  21
#define PIN_ECHO_HIGH  22
#define PIN_IR_LEFT    32
#define PIN_IR_RIGHT   23
#define PIN_LED        2

// =========================================================================================
//  FUNCTION PROTOTYPES
// =========================================================================================
void stopMotors();
void moveForward(int speed);
void turnLeft(int speed);
void turnRight(int speed);
float readUltrasonic(int trigPin, int echoPin);

// =========================================================================================
//  STATE & TIMING
// =========================================================================================
int irLeftFloor  = 1;
int irRightFloor = 1;

unsigned long lastSensorReadTime     = 0;
unsigned long lastRandomTurnTime     = 0;
unsigned long nextRandomTurnInterval = 5000;
unsigned long lastLedBlinkTime       = 0;
bool ledState = false;

#if ENABLE_WIFI
const char*      AP_SSID  = "BUBU-Robot";
WebServer        server(80);
WebSocketsServer webSocket(81);
#endif

// =========================================================================================
//  SETUP
// =========================================================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector

  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n=======================================================");
  Serial.println("  BUBU ROBOT — AUTONOMOUS ROAMING ENGINE");
  Serial.println("  (Ultrasonic Wall Avoidance + IR Tape Boundary)");
  Serial.println("=======================================================");

  pinMode(PIN_ENA, OUTPUT); pinMode(PIN_IN1, OUTPUT); pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT); pinMode(PIN_IN4, OUTPUT); pinMode(PIN_ENB, OUTPUT);
  stopMotors();

  pinMode(PIN_TRIG_LOW,  OUTPUT); digitalWrite(PIN_TRIG_LOW,  LOW);
  pinMode(PIN_ECHO_LOW,  INPUT);
  pinMode(PIN_TRIG_HIGH, OUTPUT); digitalWrite(PIN_TRIG_HIGH, LOW);
  pinMode(PIN_ECHO_HIGH, INPUT);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

#if ENABLE_IR_BOUNDARY
  pinMode(PIN_IR_LEFT, INPUT);
  pinMode(PIN_IR_RIGHT, INPUT);
  int sumL = 0, sumR = 0;
  for (int i = 0; i < 5; i++) {
    sumL += digitalRead(PIN_IR_LEFT);
    sumR += digitalRead(PIN_IR_RIGHT);
    delay(20);
  }
  irLeftFloor  = (sumL >= 3) ? 1 : 0;
  irRightFloor = (sumR >= 3) ? 1 : 0;
  Serial.printf("IR Baseline: Left=%d, Right=%d\n", irLeftFloor, irRightFloor);
#endif

#if ENABLE_WIFI
  WiFi.disconnect(true); delay(40);
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_17dBm);
  if (WiFi.softAP(AP_SSID)) {
    Serial.printf("Wi-Fi AP: '%s'\n", AP_SSID);
    webSocket.begin();
    webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
      if (type == WStype_CONNECTED) webSocket.sendTXT(num, "CONNECTED");
    });
  }
#endif

  // Power-on flash sequence
  for (int i = 0; i < 6; i++) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(200); }
  digitalWrite(PIN_LED, LOW);

  lastRandomTurnTime     = millis();
  nextRandomTurnInterval = random(4000, 7000);

  Serial.println("BUBU ROBOT ACTIVE — Roaming starts now!\n");
}

// =========================================================================================
//  MAIN LOOP
// =========================================================================================
void loop() {
#if ENABLE_WIFI
  webSocket.loop();
#endif

  unsigned long now = millis();

  // --- HEARTBEAT LED (350ms continuous pulse confirms power & health) ---
  if (now - lastLedBlinkTime > 350) {
    lastLedBlinkTime = now;
    ledState = !ledState;
    digitalWrite(PIN_LED, ledState ? HIGH : LOW);
  }

  // Rate-limit sensor processing to every 50ms
  if (now - lastSensorReadTime < 50) return;
  lastSensorReadTime = now;

  // --- 1. IR BLACK TAPE SENSOR ---
#if ENABLE_IR_BOUNDARY
  {
    int irL = digitalRead(PIN_IR_LEFT);
    int irR = digitalRead(PIN_IR_RIGHT);
    bool leftOnTape  = (irL == HIGH);
    bool rightOnTape = (irR == HIGH);

    if (leftOnTape || rightOnTape) {
      stopMotors();
      Serial.println("⚠️ Black tape boundary detected! Turning away...");

      if (leftOnTape) turnRight(SPEED_TURN);
      else            turnLeft(SPEED_TURN);

      unsigned long tStart = millis();
      while (millis() - tStart < 3000) {
        if (digitalRead(PIN_IR_LEFT) == LOW && digitalRead(PIN_IR_RIGHT) == LOW) {
          delay(180); // Firmly angle robot back into safe zone
          break;
        }
        delay(10);
      }
      stopMotors(); delay(80);
      lastRandomTurnTime = millis();
      Serial.println("Tape cleared.");
    }
  }
#endif

  // --- 2. ULTRASONIC WALL CHECK ---
  {
    float dLow  = readUltrasonic(PIN_TRIG_LOW,  PIN_ECHO_LOW);
    float dHigh = readUltrasonic(PIN_TRIG_HIGH, PIN_ECHO_HIGH);

    bool wallLow  = (dLow  > 1.5 && dLow  <= WALL_DISTANCE_CM);
    bool wallHigh = (dHigh > 1.5 && dHigh <= WALL_DISTANCE_CM);

    if (wallLow || wallHigh) {
      Serial.printf("🛑 WALL! (Low=%.1f cm, High=%.1f cm) -> Turning until clear...\n", dLow, dHigh);
      stopMotors(); delay(80);

      if (random(0, 2) == 0) {
        Serial.println("Turning LEFT...");
        turnLeft(SPEED_TURN);
      } else {
        Serial.println("Turning RIGHT...");
        turnRight(SPEED_TURN);
      }

      unsigned long tStart = millis();
      delay(200); // Minimum turn duration
      while (millis() - tStart < 3000) {
        float cL = readUltrasonic(PIN_TRIG_LOW,  PIN_ECHO_LOW);
        float cH = readUltrasonic(PIN_TRIG_HIGH, PIN_ECHO_HIGH);
        if (cL > WALL_DISTANCE_CM && cH > WALL_DISTANCE_CM) {
          delay(150); // Extra turn to ensure clear pathway
          break;
        }
        delay(20);
      }
      stopMotors(); delay(80);
      lastRandomTurnTime     = millis();
      nextRandomTurnInterval = random(4000, 7000);
      Serial.println("Wall cleared.");
      return;
    }
  }

  // --- 3. RANDOM EXPLORATION (Periodic gentle turn) ---
  if (now - lastRandomTurnTime > nextRandomTurnInterval) {
    lastRandomTurnTime     = now;
    nextRandomTurnInterval = random(4000, 7000);
    Serial.println("🎲 Random roam pivot...");
    stopMotors(); delay(60);
    if (random(0, 2) == 0) turnLeft(SPEED_TURN);
    else                   turnRight(SPEED_TURN);
    delay(random(250, 400));
    stopMotors(); delay(60);
    return;
  }

  // --- 4. DRIVE FORWARD ---
  moveForward(SPEED_FWD);
}

// =========================================================================================
//  MOTOR FUNCTIONS
// =========================================================================================
void stopMotors() {
  digitalWrite(PIN_IN1, LOW); digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN3, LOW); digitalWrite(PIN_IN4, LOW);
  analogWrite(PIN_ENA, 0);    analogWrite(PIN_ENB, 0);
}

void moveForward(int speed) {
#if INVERT_LEFT_MOTOR
  digitalWrite(PIN_IN1, LOW);  digitalWrite(PIN_IN2, HIGH);
#else
  digitalWrite(PIN_IN1, HIGH); digitalWrite(PIN_IN2, LOW);
#endif
  analogWrite(PIN_ENA, constrain(speed, 0, 255));
  digitalWrite(PIN_IN3, HIGH); digitalWrite(PIN_IN4, LOW);
  analogWrite(PIN_ENB, constrain(speed, 0, 255));
}

void turnLeft(int speed) {
  digitalWrite(PIN_IN1, LOW); digitalWrite(PIN_IN2, LOW); analogWrite(PIN_ENA, 0);
  digitalWrite(PIN_IN3, HIGH); digitalWrite(PIN_IN4, LOW);
  analogWrite(PIN_ENB, constrain(speed, 0, 255));
}

void turnRight(int speed) {
  digitalWrite(PIN_IN1, LOW); digitalWrite(PIN_IN2, LOW); analogWrite(PIN_ENA, 0);
  digitalWrite(PIN_IN3, LOW); digitalWrite(PIN_IN4, HIGH);
  analogWrite(PIN_ENB, constrain(speed, 0, 255));
}

// =========================================================================================
//  ULTRASONIC DRIVER
// =========================================================================================
float readUltrasonic(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 18000);
  if (duration == 0) return 999.0;
  return (duration * 0.0343) / 2.0;
}
