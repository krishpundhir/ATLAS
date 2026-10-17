/*

  ATLAS - ESP32-S3 firmware v1.0.0

  Two-wheel tennis ball launcher: Cytron MDD10A (PWM/DIR) + servo ball feeder.



  Libraries (Arduino Library Manager):

    - WebSockets by Markus Sattler (Links2004)   >= 2.4.0

    - ArduinoJson by Benoit Blanchon             >= 7.0.0

    - ESP32Servo by Kevin Harrington / John K. Bennett   >= 3.0.5

  Board: "ESP32 Dev Module" for classic ESP32-WROOM-32 (pin map below); S3 pins noted in comments. Core 3.x or 2.0.17,

         Serial monitor 115200 baud. (ESP32-S3 N16R8 only: Flash 16MB, PSRAM "OPI PSRAM".)



  Key rules implemented here:

   * Nothing starts on boot/reset/reconnect. Motors PWM=0, servo not driven until calibrated.

   * Phone never schedules launches; the ESP32 owns all drill timing (non-blocking).

   * RPM is only reported as measured if RPM sensors are compiled in (ENABLE_RPM_SENSORS).

   * Requested angles are never reported as physical angles.

   * Software e-stop is NOT a substitute for the physical DC e-stop.

*/

#include <Arduino.h>

#include <WiFi.h>

#include <WebServer.h>

#include <WebSocketsServer.h>

#include <ArduinoJson.h>

#include <ESP32Servo.h>

#include <Preferences.h>

#include <stdarg.h>

#include <stddef.h>

#include <math.h>
#include "ATLAS_forward.h"



#define FW_VERSION "1.0.0"

#define PROTOCOL_VERSION 1



// ============================================================================

// HARDWARE CONFIGURATION  (edit here)

// ============================================================================

// PIN MAP FOR CLASSIC ESP32-WROOM-32 (GPIO6-11 are flash; 0/2/5/12/15 are strapping: NOT used).
// For an ESP32-S3 use PWM1=4 DIR1=5 PWM2=6 DIR2=7.
static const int PIN_PWM1 = 25, PIN_DIR1 = 26;    // MDD10A PWM1/DIR1 -> wheel 1 (upper)

static const int PIN_PWM2 = 27, PIN_DIR2 = 33;    // MDD10A PWM2/DIR2 -> wheel 2 (lower)

#define MOTOR1_DIR_FWD HIGH                       // flip to LOW if wheel 1 spins the wrong way

#define MOTOR2_DIR_FWD HIGH

static const int PIN_SERVO = 18;                  // PROPOSED default; verify it is exposed on your DevKit



// --- Optional hardware (NOT assumed installed). Set to 1 only when physically fitted. ---

#define ENABLE_RPM_SENSORS 0                      // 1 = pulse sensors on PIN_RPM1/2 -> closed loop PID

static const int PIN_RPM1 = 22, PIN_RPM2 = 23;

#define ENABLE_ESTOP_SENSE 0                      // 1 = monitor e-stop circuit status input

static const int PIN_ESTOP_SENSE = 21;            // INPUT_PULLUP; LOW = circuit healthy (NC contact to GND)

#define ENABLE_INTERLOCK 0                        // 1 = guard/interlock switch input

static const int PIN_INTERLOCK = 16;              // INPUT_PULLUP; LOW = interlock closed (safe)

#define ENABLE_POSITIONING 0                      // 1 = you implemented the posXxx() hooks below

// Ball sensor: interface stub only (ballSensorAvailable()); feed events stay "commanded".



// --- Network ---

static const char*    AP_SSID = "ATLAS-Launcher";

static const char*    AP_PASS = "09876543"; // >= 8 chars. CHANGE for your lab.

static const uint16_t WS_PORT = 81;               // ws://192.168.4.1:81

static const uint16_t HTTP_PORT = 80;



// --- Limits / sizes ---

static const float    HARD_MAX_RPM = 9000.0f;     // absolute ceiling, cannot be configured higher

static const size_t   MAX_MSG_BYTES = 12288;

static const size_t   LOG_JSON_MAX = 1024;

static const uint8_t  MAX_STEPS = 40;

static const uint16_t MAX_REPS = 100;

static const uint16_t MAX_CYCLES = 50;

static const uint32_t MAX_TOTAL_SHOTS = 500;

static const uint32_t CONTROL_MS = 20;

static const uint32_t TELEMETRY_MS = 200;

static const uint32_t PWM_FREQ = 20000;           // MDD10A accepts up to 20 kHz

static const uint8_t  PWM_BITS = 10;

static const uint32_t PWM_MAX = (1u << PWM_BITS) - 1;

#define SERIAL_LOG_ENABLED 1



// ============================================================================

// LOGGER

// ============================================================================

static void logf(const char* tag, const char* fmt, ...) {

#if SERIAL_LOG_ENABLED

  char buf[256];

  va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);

  Serial.printf("[%lu ms] %s: %s\n", (unsigned long)millis(), tag, buf);

#endif

}

static float r1(float x) { return roundf(x * 10.0f) / 10.0f; }



// ============================================================================

// CONFIG (RAM, optionally persisted to NVS via save_config)

// ============================================================================

#define CFG_MAGIC 0xA7A50002u

struct Config {

  uint32_t magic = CFG_MAGIC;

  // wheels

  float maxRpm = 6000;            // soft limit (<= HARD_MAX_RPM). Raise only after mechanical verification.

  float maxPwmPct = 70;           // PWM duty ceiling in percent

  float rpmAtFullPwm = 10000;     // open-loop model: pwm = rpm / this. UNVERIFIED default - calibrate!

  float rampUp = 2000;            // RPM/s

  float rampDown = 3000;          // RPM/s

  float levelMinRpm = 1500;       // launch level mapping: level 0 = off; else min + (max-min)*level/100

  float levelMaxRpm = 6000;

  float wheelDiaMm = 0;           // 0 = unknown (surface speed not reported)

  float kmhPerRpm = 0;            // 0 = uncalibrated (no ball-speed estimate). Fill from experiments only.

  float stableTolPct = 3;

  uint32_t stableMs = 500;        // closed-loop: time within tolerance to count as stable

  uint32_t settleMs = 3000;       // open-loop: assumed settle time after ramp completes (NOT measured)

  uint16_t pulsesPerRev = 1;

  float kp = 0.00004f, ki = 0.00008f, kd = 0.0f;   // PID in PWM-fraction per RPM (untuned)

  // angle soft limits

  float elevMin = 0, elevMax = 60, yawMin = -30, yawMax = 30;

  // safety timing

  uint32_t readyTimeoutMs = 15000;

  uint32_t commTimeoutMs = 5000;

  // feeder servo (defaults are LOCKED: rest==feed==min==max, so feeder is "not calibrated")

  float servoRest = 90, servoFeed = 90, servoMin = 90, servoMax = 90, servoOffset = 0;

  uint16_t servoPulseMin = 500, servoPulseMax = 2400;

  uint32_t feedMoveMs = 400, feedHoldMs = 300, feedReturnMs = 400, feedTimeoutMs = 3000;

  uint32_t feedIntervalMs = 3000, maxFeedIntervalMs = 60000;

};

static Config cfg;

enum FT : uint8_t { FT_F, FT_U32, FT_U16 };

struct FieldDef { const char* key; FT t; size_t off; float lo, hi; };

#define CFF(k, m, lo, hi)   { k, FT_F,   offsetof(Config, m), lo, hi }

#define CFU32(k, m, lo, hi) { k, FT_U32, offsetof(Config, m), lo, hi }

#define CFU16(k, m, lo, hi) { k, FT_U16, offsetof(Config, m), lo, hi }

static const FieldDef FIELDS[] = {

  CFF("max_rpm", maxRpm, 100, HARD_MAX_RPM), CFF("max_pwm_pct", maxPwmPct, 5, 100),

  CFF("rpm_at_full_pwm", rpmAtFullPwm, 1000, 30000),

  CFF("ramp_up_rpm_s", rampUp, 50, 20000), CFF("ramp_down_rpm_s", rampDown, 50, 20000),

  CFF("level_min_rpm", levelMinRpm, 0, HARD_MAX_RPM), CFF("level_max_rpm", levelMaxRpm, 0, HARD_MAX_RPM),

  CFF("wheel_dia_mm", wheelDiaMm, 0, 500), CFF("kmh_per_rpm", kmhPerRpm, 0, 1),

  CFF("stable_tol_pct", stableTolPct, 0.5, 20), CFU32("stable_ms", stableMs, 50, 10000),

  CFU32("settle_ms", settleMs, 0, 30000), CFU16("pulses_per_rev", pulsesPerRev, 1, 1000),

  CFF("kp", kp, 0, 1), CFF("ki", ki, 0, 1), CFF("kd", kd, 0, 1),

  CFF("elev_min", elevMin, -90, 90), CFF("elev_max", elevMax, -90, 90),

  CFF("yaw_min", yawMin, -180, 180), CFF("yaw_max", yawMax, -180, 180),

  CFU32("ready_timeout_ms", readyTimeoutMs, 1000, 120000), CFU32("comm_timeout_ms", commTimeoutMs, 1000, 60000),

  CFF("servo_rest", servoRest, 0, 180), CFF("servo_feed", servoFeed, 0, 180),

  CFF("servo_min", servoMin, 0, 180), CFF("servo_max", servoMax, 0, 180), CFF("servo_offset", servoOffset, -30, 30),

  CFU16("servo_pulse_min_us", servoPulseMin, 400, 1500), CFU16("servo_pulse_max_us", servoPulseMax, 1500, 2600),

  CFU32("feed_move_ms", feedMoveMs, 50, 5000), CFU32("feed_hold_ms", feedHoldMs, 0, 5000),

  CFU32("feed_return_ms", feedReturnMs, 50, 5000), CFU32("feed_timeout_ms", feedTimeoutMs, 200, 20000),

  CFU32("feed_interval_ms", feedIntervalMs, 100, 600000), CFU32("max_feed_interval_ms", maxFeedIntervalMs, 1000, 600000),

};

static const size_t NFIELDS = sizeof(FIELDS) / sizeof(FIELDS[0]);



static float fieldGet(const Config& c, const FieldDef& f) {

  const uint8_t* b = (const uint8_t*)&c + f.off;

  if (f.t == FT_F) return *(const float*)b;

  if (f.t == FT_U32) return (float)*(const uint32_t*)b;

  return (float)*(const uint16_t*)b;

}

static void fieldSet(Config& c, const FieldDef& f, float v) {

  uint8_t* b = (uint8_t*)&c + f.off;

  if (f.t == FT_F) *(float*)b = v;

  else if (f.t == FT_U32) *(uint32_t*)b = (uint32_t)lroundf(v);

  else *(uint16_t*)b = (uint16_t)lroundf(v);

}

static uint32_t feedMinIntervalFor(const Config& c) {

  uint32_t cyc = c.feedMoveMs + c.feedHoldMs + c.feedReturnMs + 200;

  return cyc < 500 ? 500 : cyc;

}

static uint32_t feedMinInterval() { return feedMinIntervalFor(cfg); }

static bool feederCalibratedFor(const Config& c) {

  return c.servoMax > c.servoMin && fabsf(c.servoFeed - c.servoRest) >= 1.0f;

}

static bool feederCalibrated() { return feederCalibratedFor(cfg); }



// returns nullptr if OK else static message

static const char* validateConfig(const Config& c) {

  if (c.levelMinRpm > c.levelMaxRpm) return "level_min_rpm > level_max_rpm";

  if (c.levelMaxRpm > c.maxRpm) return "level_max_rpm > max_rpm";

  if (c.elevMin >= c.elevMax) return "elev_min must be < elev_max";

  if (c.yawMin >= c.yawMax) return "yaw_min must be < yaw_max";

  if (c.servoMin > c.servoMax) return "servo_min > servo_max";

  if (c.servoRest < c.servoMin || c.servoRest > c.servoMax) return "servo_rest outside servo_min..servo_max";

  if (c.servoFeed < c.servoMin || c.servoFeed > c.servoMax) return "servo_feed outside servo_min..servo_max";

  if (c.servoPulseMin >= c.servoPulseMax) return "servo pulse min >= max";

  uint32_t cyc = c.feedMoveMs + c.feedHoldMs + c.feedReturnMs;

  if (c.feedTimeoutMs <= cyc) return "feed_timeout_ms must exceed move+hold+return";

  if (c.feedIntervalMs < feedMinIntervalFor(c)) return "feed_interval_ms below feeder minimum cycle";

  if (c.feedIntervalMs > c.maxFeedIntervalMs) return "feed_interval_ms > max_feed_interval_ms";

  return nullptr;

}

static Preferences prefs;

static void saveConfig() { prefs.begin("atlas", false); prefs.putBytes("cfg", &cfg, sizeof cfg); prefs.end(); }

static void loadConfig() {

  prefs.begin("atlas", true);

  Config tmp;

  size_t n = prefs.getBytesLength("cfg");

  if (n == sizeof tmp) {

    prefs.getBytes("cfg", &tmp, sizeof tmp);

    const char* e = validateConfig(tmp);

    if (tmp.magic == CFG_MAGIC && !e) { cfg = tmp; logf("CONFIG", "loaded from flash"); }

    else logf("WARN", "stored config invalid (%s), using defaults", e ? e : "magic");

  } else logf("CONFIG", "no stored config, using defaults");

  prefs.end();

}



// ============================================================================

// MACHINE STATE

// ============================================================================

enum MState : uint8_t { S_INIT, S_IDLE, S_READY, S_SPINUP, S_POSITIONING, S_READY_LAUNCH, S_FEEDING,

                        S_DRILL, S_PAUSED, S_STOPPING, S_FAULT, S_ESTOP };

static const char* const STATE_NAMES[] = {"INITIALIZING", "IDLE", "READY", "SPINNING_UP", "POSITIONING",

  "READY_TO_LAUNCH", "FEEDING", "RUNNING_DRILL", "PAUSED", "STOPPING", "FAULT", "EMERGENCY_STOP"};

#define BIT(s) (1u << (s))

static const uint16_t ALLOWED[12] = {

  /*INIT*/ BIT(S_IDLE),

  /*IDLE*/ BIT(S_READY) | BIT(S_SPINUP) | BIT(S_DRILL),

  /*READY*/ BIT(S_IDLE) | BIT(S_SPINUP) | BIT(S_DRILL),

  /*SPINUP*/ BIT(S_POSITIONING) | BIT(S_READY_LAUNCH) | BIT(S_STOPPING),

  /*POSITIONING*/ BIT(S_SPINUP) | BIT(S_READY_LAUNCH) | BIT(S_STOPPING),

  /*READY_LAUNCH*/ BIT(S_SPINUP) | BIT(S_POSITIONING) | BIT(S_FEEDING) | BIT(S_STOPPING),

  /*FEEDING*/ BIT(S_READY_LAUNCH) | BIT(S_SPINUP) | BIT(S_STOPPING),

  /*DRILL*/ BIT(S_PAUSED) | BIT(S_STOPPING),

  /*PAUSED*/ BIT(S_DRILL) | BIT(S_STOPPING),

  /*STOPPING*/ BIT(S_IDLE),

  /*FAULT*/ BIT(S_IDLE),

  /*ESTOP*/ BIT(S_IDLE),

};  // FAULT/ESTOP are reachable from any state via raiseFault() (forced)

static MState state = S_INIT;

static bool setState(MState n, const char* why) {

  if (n == state) return true;

  if (!(ALLOWED[state] & BIT(n))) {

    logf("WARN", "illegal state transition %s -> %s blocked (%s)", STATE_NAMES[state], STATE_NAMES[n], why);

    return false;

  }

  logf("STATE", "%s -> %s (%s)", STATE_NAMES[state], STATE_NAMES[n], why);

  state = n;

  return true;

}



enum Fault : uint8_t { FLT_NONE, FLT_ESTOP_SW, FLT_ESTOP_HW, FLT_INTERLOCK, FLT_FEED_TIMEOUT, FLT_READY_TIMEOUT,

                       FLT_RPM_SENSOR, FLT_OVERSPEED, FLT_CONFIG, FLT_INTERNAL };

static const char* const FAULT_NAMES[] = {"NONE", "ESTOP_SOFTWARE", "ESTOP_INPUT", "INTERLOCK_OPEN", "FEED_TIMEOUT",

  "READY_TIMEOUT", "RPM_SENSOR_FAULT", "OVERSPEED", "CONFIG_INVALID", "INTERNAL"};

static Fault fault = FLT_NONE;

static char faultMsg[96] = "";



// ============================================================================

// ERROR HELPERS

// ============================================================================

static char errMsg[128];

static const char* fail(const char* code, const char* fmt, ...) {

  va_list a; va_start(a, fmt); vsnprintf(errMsg, sizeof errMsg, fmt, a); va_end(a);

  return code;

}

#define TRY(expr) do { const char* e__ = (expr); if (e__) return e__; } while (0)



// ============================================================================

// MOTOR CONTROL

// ============================================================================

#if ESP_ARDUINO_VERSION_MAJOR >= 3

static void pwmBegin(int pin, int ch) { ledcAttach(pin, PWM_FREQ, PWM_BITS); }

static void pwmWrite(int pin, int ch, uint32_t d) { ledcWrite(pin, d); }

#else

static void pwmBegin(int pin, int ch) { ledcSetup(ch, PWM_FREQ, PWM_BITS); ledcAttachPin(pin, ch); }

static void pwmWrite(int pin, int ch, uint32_t d) { ledcWrite(ch, d); }

#endif

// channels 6/7 chosen so ESP32Servo (allocates from 0 upward) does not collide on core 2.x

static const int PWM_CH[2] = {6, 7};

static const int PWM_PIN[2] = {PIN_PWM1, PIN_PWM2};

static const int DIR_PIN[2] = {PIN_DIR1, PIN_DIR2};



static void setMotorPwm(int i, float frac) {

  if (frac < 0) frac = 0; if (frac > 1) frac = 1;

  pwmWrite(PWM_PIN[i], PWM_CH[i], (uint32_t)lroundf(frac * PWM_MAX));

}

static void motorsInitSafe() {

  pinMode(PIN_DIR1, OUTPUT); pinMode(PIN_DIR2, OUTPUT);

  digitalWrite(PIN_DIR1, MOTOR1_DIR_FWD); digitalWrite(PIN_DIR2, MOTOR2_DIR_FWD);

  for (int i = 0; i < 2; i++) { pwmBegin(PWM_PIN[i], PWM_CH[i]); pwmWrite(PWM_PIN[i], PWM_CH[i], 0); }

}



struct Wheel {

  float ramped = 0, pwm = 0, integ = 0, prevErr = 0;

  bool atTarget = false; uint32_t atTargetSince = 0, stableSince = 0;

};

static Wheel W[2];

static float req[2] = {0, 0};            // requested wheel RPM targets

static bool wheelsEnabled = false;       // wheels only spin when true

static bool syncMode = true;

static float baseRpm = 0, diffRpm = 0, launchLevel = -1;

static float rpmMeas[2] = {0, 0};

static bool rpmValid[2] = {false, false};

static const bool HAVE_RPM = (ENABLE_RPM_SENSORS != 0);



#if ENABLE_RPM_SENSORS

static volatile uint32_t pulseCnt[2] = {0, 0};

static volatile uint32_t lastPulseUs[2] = {0, 0};

void IRAM_ATTR isrRpm0() { uint32_t n = micros(); if (n - lastPulseUs[0] > 100) { pulseCnt[0]++; lastPulseUs[0] = n; } }

void IRAM_ATTR isrRpm1() { uint32_t n = micros(); if (n - lastPulseUs[1] > 100) { pulseCnt[1]++; lastPulseUs[1] = n; } }

static uint32_t lastRpmUs = 0, lastCnt[2] = {0, 0};

static void rpmTick() {

  uint32_t now = micros();

  if (now - lastRpmUs < 200000) return;

  uint32_t dt = now - lastRpmUs; lastRpmUs = now;

  for (int i = 0; i < 2; i++) {

    noInterrupts(); uint32_t c = pulseCnt[i]; interrupts();

    uint32_t d = c - lastCnt[i]; lastCnt[i] = c;

    float raw = (float)d * 60e6f / ((float)dt * cfg.pulsesPerRev);

    rpmMeas[i] = rpmValid[i] ? rpmMeas[i] + 0.35f * (raw - rpmMeas[i]) : raw;   // EMA filter

    rpmValid[i] = true;

  }

}

#else

static void rpmTick() {}

#endif



static void emergencyOutputsOff() {

  wheelsEnabled = false;

  for (int i = 0; i < 2; i++) { W[i].ramped = 0; W[i].pwm = 0; W[i].integ = 0; W[i].atTarget = false; W[i].stableSince = 0; setMotorPwm(i, 0); }

}



static void controlTick(float dt) {

  for (int i = 0; i < 2; i++) {

    Wheel& w = W[i];

    float sp = wheelsEnabled ? req[i] : 0.0f;

    float step = ((sp > w.ramped) ? cfg.rampUp : cfg.rampDown) * dt;

    if (sp > w.ramped) w.ramped = fminf(sp, w.ramped + step); else w.ramped = fmaxf(sp, w.ramped - step);

    bool atT = fabsf(w.ramped - sp) < 1.0f;

    if (atT && sp > 0) { if (!w.atTarget) { w.atTarget = true; w.atTargetSince = millis(); } } else w.atTarget = false;

    float out = 0;

    if (w.ramped > 0.5f) {

      out = w.ramped / cfg.rpmAtFullPwm;                       // open-loop feed-forward

#if ENABLE_RPM_SENSORS

      if (rpmValid[i]) {

        float err = w.ramped - rpmMeas[i];

        w.integ += err * dt * cfg.ki; w.integ = constrain(w.integ, -0.25f, 0.25f);

        out += cfg.kp * err + w.integ + cfg.kd * (err - w.prevErr) / dt; w.prevErr = err;

        float tol = cfg.stableTolPct / 100.0f * fmaxf(sp, 1.0f);

        if (atT && fabsf(rpmMeas[i] - sp) <= tol) { if (!w.stableSince) { w.stableSince = millis(); if (!w.stableSince) w.stableSince = 1; } }

        else w.stableSince = 0;

      }

#endif

    } else { w.integ = 0; w.prevErr = 0; w.stableSince = 0; }

    out = constrain(out, 0.0f, cfg.maxPwmPct / 100.0f);

    w.pwm = out; setMotorPwm(i, out);

  }

}

static bool wheelAtSpeed(int i) {

  float sp = wheelsEnabled ? req[i] : 0.0f;

  if (sp <= 0) return W[i].ramped < 1.0f;

  if (!W[i].atTarget) return false;

#if ENABLE_RPM_SENSORS

  return W[i].stableSince && (millis() - W[i].stableSince >= cfg.stableMs);

#else

  return millis() - W[i].atTargetSince >= cfg.settleMs;   // time-based assumption, NOT a measurement

#endif

}

static bool wheelsReady() { return wheelsEnabled && (req[0] > 0 || req[1] > 0) && wheelAtSpeed(0) && wheelAtSpeed(1); }

static bool wheelsRamped() {

  for (int i = 0; i < 2; i++) { float sp = wheelsEnabled ? req[i] : 0.0f; if (sp <= 0 ? W[i].ramped >= 1.0f : !W[i].atTarget) return false; }

  return true;

}

static float levelToRpm(float l) { return l <= 0 ? 0 : cfg.levelMinRpm + (cfg.levelMaxRpm - cfg.levelMinRpm) * (l / 100.0f); }



// ============================================================================

// POSITIONING (elevation / yaw)  - integration hooks. Hardware NOT installed by default.

// ============================================================================

static float reqElev = 0, reqYaw = 0;

static bool posPending = false;

static bool posAvailable() { return ENABLE_POSITIONING != 0; }

static void posRequest(float elev, float yaw) { logf("POSITION", "hook: move to elev=%.1f yaw=%.1f (implement actuators here)", elev, yaw); }

static bool posReached() { return false; }                 // TODO: |actual-target| < tolerance

static bool posActual(float& elev, float& yaw) { return false; }  // TODO: read position sensors

static void posStop() {}                                   // called on faults/e-stop

// Future: homing, limit switches, position error and fault reporting plug in here.



// ============================================================================

// SERVO FEEDER (non-blocking)

// ============================================================================

static Servo servo;

static bool servoAttached = false;

enum FeedState : uint8_t { FD_IDLE, FD_MOVE, FD_HOLD, FD_RETURN };

static const char* const FD_NAMES[] = {"IDLE", "MOVING_TO_RELEASE", "HOLDING_RELEASE", "RETURNING_TO_REST"};

static FeedState fd = FD_IDLE;

static uint32_t fdT = 0, fdCycleT = 0, feedCount = 0, nextFeedAt = 0, feedNotReadySince = 0, feedPauseRemain = 0;

static bool feedAuto = false, feedPaused = false;

static uint32_t lastRxMs = 0;

static int activeClient = -1;

static WebSocketsServer webSocket(WS_PORT);

static WebServer http(HTTP_PORT);



static bool ballSensorAvailable() { return false; }       // hook for future ball sensor



static void sendDoc(JsonDocument& doc) {

  if (activeClient < 0) return;

  String s; serializeJson(doc, s);

  webSocket.sendTXT((uint8_t)activeClient, s);

}

static void sendEventSimple(const char* ev, const char* detail) {

  JsonDocument d; d["type"] = "event"; d["event"] = ev; d["t"] = millis();

  if (detail) d["detail"] = detail;

  sendDoc(d);

}

static void servoWriteDeg(float deg) {

  if (!servoAttached) return;

  float a = constrain(deg + cfg.servoOffset, cfg.servoMin, cfg.servoMax);

  servo.write((int)lroundf(a));

  logf("SERVO", "write %.1f deg (requested %.1f, offset %.1f)", a, deg, cfg.servoOffset);

}

static void feederApplyConfig() {

  if (!feederCalibrated()) { logf("FEEDER", "NOT calibrated (servo_min/max/rest/feed): servo output idle, feeding refused"); return; }

  if (servoAttached) servo.detach();

  servo.setPeriodHertz(50);

  servo.attach(PIN_SERVO, cfg.servoPulseMin, cfg.servoPulseMax);

  servoAttached = true;

  servoWriteDeg(cfg.servoRest);                           // moves to REST only, never to feed position

}

static void feederAbort() {

  if (fd != FD_IDLE) logf("FEEDER", "cycle aborted in state %s, returning to rest", FD_NAMES[fd]);

  fd = FD_IDLE;

  if (servoAttached && feederCalibrated()) servoWriteDeg(cfg.servoRest);

}

static bool interlockOk() {

#if ENABLE_INTERLOCK

  return digitalRead(PIN_INTERLOCK) == LOW;

#else

  return true;

#endif

}

static bool feederPermitted(bool inDrill, const char** why) {

  if (fault != FLT_NONE) { *why = "fault_active"; return false; }

  if (!feederCalibrated()) { *why = "feeder_not_calibrated"; return false; }

  if (!interlockOk()) { *why = "interlock_open"; return false; }

  if (inDrill) { if (state != S_DRILL) { *why = "not_in_drill"; return false; } }

  else if (state != S_READY_LAUNCH) { *why = "not_ready_to_launch"; return false; }

  if (fd != FD_IDLE) { *why = "feeder_busy"; return false; }

  return true;

}

static void feederStart() {

  fd = FD_MOVE; fdT = fdCycleT = millis();

  if (state == S_READY_LAUNCH) setState(S_FEEDING, "feed cycle");

  logf("FEEDER", "feed START (#%lu) -> release position", (unsigned long)(feedCount + 1));

  servoWriteDeg(cfg.servoFeed);

}

static void feederTick() {

  uint32_t now = millis();

  switch (fd) {

    case FD_MOVE: if (now - fdT >= cfg.feedMoveMs) { fd = FD_HOLD; fdT = now; } break;

    case FD_HOLD: if (now - fdT >= cfg.feedHoldMs) { servoWriteDeg(cfg.servoRest); fd = FD_RETURN; fdT = now; } break;

    case FD_RETURN:

      if (now - fdT >= cfg.feedReturnMs) {

        fd = FD_IDLE; feedCount++;

        logf("FEEDER", "feed COMPLETE (#%lu) - commanded activation, NOT a confirmed launch", (unsigned long)feedCount);

        JsonDocument d; d["type"] = "event"; d["event"] = "feed_event"; d["n"] = feedCount; d["commanded"] = true;

        d["confirmed"] = nullptr; d["t"] = now; sendDoc(d);

        if (state == S_FEEDING) setState(S_READY_LAUNCH, "feed done");

      }

      break;

    default: break;

  }

  // timed (manual continuous) feeding

  if (feedAuto && !feedPaused && fd == FD_IDLE && (int32_t)(now - nextFeedAt) >= 0) {

    const char* why = "";

    if (feederPermitted(false, &why) && wheelsReady()) {

      feedNotReadySince = 0; nextFeedAt = now + cfg.feedIntervalMs; feederStart();

    } else {

      if (!feedNotReadySince) feedNotReadySince = now;

      if (now - feedNotReadySince > cfg.readyTimeoutMs) {

        feedAuto = false;

        logf("WARN", "timed feeding stopped: not ready for %lu ms (%s)", (unsigned long)cfg.readyTimeoutMs, why);

        sendEventSimple("feeding_stopped", why);

      }

    }

  }

}



// ============================================================================

// DRILLS

// ============================================================================

static void raiseFault(Fault f, const char* fmt, ...);

struct Step { float elev, yaw, m1, m2, spin; uint32_t intervalMs, pauseAfterMs; uint16_t reps; bool feed, waitStable, requirePos, holdAfter; };

struct DrillDef { char id[40]; char name[40]; uint16_t cycles; uint32_t cyclePauseMs; uint8_t n; Step s[MAX_STEPS]; };

static DrillDef loaded, staging;

enum DState : uint8_t { DS_NONE, DS_LOADED, DS_RUNNING, DS_PAUSED, DS_DONE, DS_FAILED, DS_CANCELLED };

static const char* const DS_NAMES[] = {"NONE", "LOADED", "RUNNING", "PAUSED", "DONE", "FAILED", "CANCELLED"};

enum DPhase : uint8_t { PH_PREP, PH_FEED };

struct DrillRun {

  DState st = DS_NONE; DPhase ph = PH_PREP; uint8_t step = 0; uint16_t rep = 0, cycle = 0;

  uint32_t shots = 0, total = 0, notBefore = 0, shotStart = 0, pausedAt = 0, waitSince = 0;

  bool pauseReq = false, aimNot = false; char err[96] = "";

};

static DrillRun dr;

static bool drillBusy() { return dr.st == DS_RUNNING || dr.st == DS_PAUSED; }



static const char* stepNum(JsonObjectConst s, int i, const char* k, float lo, float hi, float& v, bool required, float def) {

  JsonVariantConst x = s[k];

  if (x.isNull()) { if (required) return fail("invalid_drill", "step %d: missing '%s'", i + 1, k); v = def; return nullptr; }

  if (!x.is<float>()) return fail("invalid_drill", "step %d: '%s' must be a number", i + 1, k);

  v = x.as<float>();

  if (isnan(v) || v < lo || v > hi) return fail("invalid_drill", "step %d: '%s'=%.1f outside [%.1f, %.1f]", i + 1, k, v, lo, hi);

  return nullptr;

}

static const char* stepBool(JsonObjectConst s, int i, const char* k, bool& v, bool def) {

  JsonVariantConst x = s[k];

  if (x.isNull()) { v = def; return nullptr; }

  if (!x.is<bool>()) return fail("invalid_drill", "step %d: '%s' must be true/false", i + 1, k);

  v = x.as<bool>(); return nullptr;

}

static const char* parseDrill(JsonObjectConst d, DrillDef& out, JsonArray warns) {

  memset(&out, 0, sizeof out);

  const char* id = d["id"] | "";

  if (!strlen(id) || strlen(id) >= sizeof out.id) return fail("invalid_drill", "'id' must be 1..39 characters");

  strlcpy(out.id, id, sizeof out.id);

  strlcpy(out.name, d["name"] | "unnamed", sizeof out.name);

  float cycles = 1, cp = 0;

  TRY(stepNum(d, -1, "cycles", 1, MAX_CYCLES, cycles, false, 1));

  TRY(stepNum(d, -1, "cycle_pause_ms", 0, 600000, cp, false, 0));

  out.cycles = (uint16_t)cycles; out.cyclePauseMs = (uint32_t)cp;

  JsonArrayConst arr = d["steps"].as<JsonArrayConst>();

  if (arr.isNull()) return fail("invalid_drill", "missing 'steps' array");

  if (arr.size() < 1 || arr.size() > MAX_STEPS) return fail("invalid_drill", "steps must be 1..%d (got %u)", MAX_STEPS, (unsigned)arr.size());

  uint32_t sumReps = 0; int i = 0; bool anyFeed = false;

  for (JsonVariantConst v : arr) {

    JsonObjectConst s = v.as<JsonObjectConst>();

    if (s.isNull()) return fail("invalid_drill", "step %d is not an object", i + 1);

    Step& st = out.s[i];

    float elev, yaw, m1, m2, spin, iv, reps, pa, sp = -1;

    TRY(stepNum(s, i, "elev", cfg.elevMin, cfg.elevMax, elev, true, 0));

    TRY(stepNum(s, i, "yaw", cfg.yawMin, cfg.yawMax, yaw, true, 0));

    bool hasSpeed = !s["speed"].isNull();

    if (hasSpeed) TRY(stepNum(s, i, "speed", 0, cfg.maxRpm, sp, true, 0));

    if (!s["m1"].isNull()) TRY(stepNum(s, i, "m1", 0, cfg.maxRpm, m1, true, 0));

    else if (hasSpeed) m1 = sp; else return fail("invalid_drill", "step %d: need 'm1' or 'speed'", i + 1);

    if (!s["m2"].isNull()) TRY(stepNum(s, i, "m2", 0, cfg.maxRpm, m2, true, 0));

    else if (hasSpeed) m2 = sp; else return fail("invalid_drill", "step %d: need 'm2' or 'speed'", i + 1);

    TRY(stepNum(s, i, "spin_rpm", -cfg.maxRpm, cfg.maxRpm, spin, false, 0));

    float w1 = m1 + spin / 2, w2 = m2 - spin / 2;

    if (w1 < 0 || w2 < 0 || w1 > cfg.maxRpm || w2 > cfg.maxRpm)

      return fail("invalid_drill", "step %d: effective wheel speeds %.0f/%.0f outside [0, %.0f]", i + 1, w1, w2, cfg.maxRpm);

    bool feed, ws, rp;

    TRY(stepBool(s, i, "feed", feed, true));

    TRY(stepBool(s, i, "wait_stable", ws, true));

    TRY(stepBool(s, i, "require_position", rp, false));

    if (feed && w1 <= 0 && w2 <= 0) return fail("invalid_drill", "step %d: feeding with both wheels at 0 RPM", i + 1);

    uint32_t minIv = feed ? feedMinInterval() : 100;

    TRY(stepNum(s, i, "interval_ms", minIv, 600000, iv, true, 0));

    TRY(stepNum(s, i, "reps", 1, MAX_REPS, reps, false, 1));

    TRY(stepNum(s, i, "pause_after_ms", 0, 600000, pa, false, 0));

    const char* nx = s["next"] | "auto";

    if (strcmp(nx, "auto") && strcmp(nx, "pause")) return fail("invalid_drill", "step %d: 'next' must be 'auto' or 'pause'", i + 1);

    if (feed && !feederCalibrated()) return fail("feeder_not_calibrated", "step %d feeds but the feeder servo is not calibrated (set_config servo_*)", i + 1);

    if (rp && !posAvailable()) return fail("positioning_unavailable", "step %d requires positioning but no positioning hardware is installed", i + 1);

    st = Step{elev, yaw, m1, m2, spin, (uint32_t)iv, (uint32_t)pa, (uint16_t)reps, feed, ws, rp, !strcmp(nx, "pause")};

    sumReps += (uint32_t)reps; anyFeed |= feed; i++;

  }

  out.n = (uint8_t)i;

  if (sumReps * out.cycles > MAX_TOTAL_SHOTS) return fail("invalid_drill", "total shots %lu exceeds limit %lu", (unsigned long)(sumReps * out.cycles), (unsigned long)MAX_TOTAL_SHOTS);

  if (!posAvailable()) warns.add("aim_not_performed: no positioning hardware - elevation/yaw are stored but NOT physically applied");

  if (!HAVE_RPM) warns.add("open_loop: wheel RPM is not measured; readiness is time-based");

  warns.add("step interval_ms takes precedence over the global feed interval during drills");

  return nullptr;

}

static uint32_t drillTotalShots(const DrillDef& d) { uint32_t s = 0; for (int i = 0; i < d.n; i++) s += d.s[i].reps; return s * d.cycles; }



static void targetsChanged(const char* src) {

  logf("TARGET", "wheel1=%.0f RPM, wheel2=%.0f RPM (%s, %s)", req[0], req[1], src, HAVE_RPM ? "closed-loop" : "open-loop, RPM not measured");

  if (state == S_IDLE && (req[0] > 0 || req[1] > 0)) setState(S_READY, "targets set");

}

static void stopAll(const char* reason);

static void drillFinish(DState st, const char* why) {

  dr.st = st; if (st == DS_CANCELLED || st == DS_FAILED) strlcpy(dr.err, why, sizeof dr.err);

  if (fd != FD_IDLE) feederAbort();

  wheelsEnabled = false;

  if (state == S_DRILL || state == S_PAUSED) setState(S_STOPPING, why);

  logf("DRILL", "%s (%s) shots=%lu/%lu", DS_NAMES[st], why, (unsigned long)dr.shots, (unsigned long)dr.total);

  JsonDocument d; d["type"] = "event"; d["event"] = st == DS_DONE ? "drill_complete" : (st == DS_FAILED ? "drill_failed" : "drill_cancelled");

  d["id"] = loaded.id; d["shots"] = dr.shots; d["detail"] = why; sendDoc(d);

}

static void drillBeginStep(uint32_t notBefore) {

  const Step& s = loaded.s[dr.step];

  req[0] = s.m1 + s.spin / 2; req[1] = s.m2 - s.spin / 2; wheelsEnabled = true;

  reqElev = s.elev; reqYaw = s.yaw;

  if (posAvailable()) { posRequest(s.elev, s.yaw); posPending = true; }

  else { dr.aimNot = true; logf("WARN", "step %u: elev=%.1f yaw=%.1f NOT physically applied (no positioning hardware)", dr.step + 1, s.elev, s.yaw); }

  dr.ph = PH_PREP; dr.rep = 0; dr.notBefore = notBefore; dr.waitSince = 0;

  logf("DRILL", "step %u/%u cycle %u/%u: w1=%.0f w2=%.0f elev=%.1f yaw=%.1f reps=%u interval=%lu ms",

       dr.step + 1, loaded.n, dr.cycle + 1, loaded.cycles, req[0], req[1], s.elev, s.yaw, s.reps, (unsigned long)s.intervalMs);

  JsonDocument d; d["type"] = "event"; d["event"] = "drill_step"; d["step"] = dr.step + 1; d["cycle"] = dr.cycle + 1; sendDoc(d);

}

static void drillShotDone() {

  const Step& s = loaded.s[dr.step];

  dr.shots++; dr.rep++;

  logf("DRILL", "shot %lu/%lu done (step %u rep %u/%u)", (unsigned long)dr.shots, (unsigned long)dr.total, dr.step + 1, dr.rep, s.reps);

  if (dr.rep < s.reps) { dr.ph = PH_PREP; dr.notBefore = dr.shotStart + s.intervalMs; return; }

  bool lastStep = dr.step + 1 >= loaded.n, lastCycle = dr.cycle + 1 >= loaded.cycles;

  if (lastStep && lastCycle) { drillFinish(DS_DONE, "all steps complete"); return; }

  uint32_t nb = dr.shotStart + s.intervalMs + s.pauseAfterMs;

  if (lastStep) { dr.cycle++; dr.step = 0; nb += loaded.cyclePauseMs; } else dr.step++;

  drillBeginStep(nb);

  if (s.holdAfter) { dr.pauseReq = true; logf("DRILL", "step requests pause before next step"); }

}

static void drillTick() {

  if (dr.st != DS_RUNNING) return;

  uint32_t now = millis();

  if (dr.pauseReq && dr.ph == PH_PREP) {

    dr.st = DS_PAUSED; dr.pausedAt = now; dr.pauseReq = false; setState(S_PAUSED, "drill paused");

    logf("DRILL", "PAUSED (wheels keep spinning, feeder idle)"); return;

  }

  if (dr.ph == PH_FEED) { if (fd == FD_IDLE) drillShotDone(); return; }

  const Step& s = loaded.s[dr.step];

  if ((int32_t)(now - dr.notBefore) < 0) return;

  const char* why = "";

  bool ok = s.waitStable ? wheelsReady() : wheelsRamped();

  if (ok && s.requirePos) ok = posAvailable() && posReached();

  if (ok && s.feed) ok = feederPermitted(true, &why);

  if (!ok) {

    if (!dr.waitSince) dr.waitSince = now;

    if (now - dr.waitSince > cfg.readyTimeoutMs) raiseFault(FLT_READY_TIMEOUT, "step %u not ready after %lu ms %s", dr.step + 1, (unsigned long)cfg.readyTimeoutMs, why);

    return;

  }

  dr.waitSince = 0; dr.shotStart = now;

  if (s.feed) { feederStart(); dr.ph = PH_FEED; } else drillShotDone();

}



// ============================================================================

// FAULTS / SAFETY

// ============================================================================

static void raiseFault(Fault f, const char* fmt, ...) {

  if (fault != FLT_NONE && f != FLT_ESTOP_SW && f != FLT_ESTOP_HW) return;

  char m[96]; va_list a; va_start(a, fmt); vsnprintf(m, sizeof m, fmt, a); va_end(a);

  emergencyOutputsOff(); posStop();

  fault = f; strlcpy(faultMsg, m, sizeof faultMsg);

  feedAuto = false; feedPaused = false;

  if (fd != FD_IDLE) feederAbort();

  if (dr.st == DS_RUNNING || dr.st == DS_PAUSED) { dr.st = DS_FAILED; strlcpy(dr.err, m, sizeof dr.err); }

  MState tgt = (f == FLT_ESTOP_SW || f == FLT_ESTOP_HW) ? S_ESTOP : S_FAULT;

  logf("FAULT", "%s: %s", FAULT_NAMES[f], m);

  logf("STATE", "%s -> %s (forced by fault)", STATE_NAMES[state], STATE_NAMES[tgt]);

  state = tgt;

  JsonDocument d; d["type"] = "event"; d["event"] = "fault"; d["fault"] = FAULT_NAMES[f]; d["detail"] = m; sendDoc(d);

}

static void stopAll(const char* reason) {

  logf("SAFETY", "stop all: %s", reason);

  feedAuto = false; feedPaused = false;

  if (drillBusy()) drillFinish(DS_CANCELLED, reason);

  if (fd != FD_IDLE) feederAbort();

  wheelsEnabled = false; posPending = false;

  if (state == S_SPINUP || state == S_POSITIONING || state == S_READY_LAUNCH || state == S_FEEDING) setState(S_STOPPING, reason);

}

static bool estopHealthy() {

#if ENABLE_ESTOP_SENSE

  return digitalRead(PIN_ESTOP_SENSE) == LOW;

#else

  return true;

#endif

}

static void safetyTick() {

  uint32_t now = millis();

#if ENABLE_ESTOP_SENSE

  if (!estopHealthy() && state != S_ESTOP) raiseFault(FLT_ESTOP_HW, "physical E-stop circuit open/pressed");

#endif

#if ENABLE_INTERLOCK

  if (!interlockOk() && (wheelsEnabled || fd != FD_IDLE || drillBusy())) raiseFault(FLT_INTERLOCK, "interlock open");

#endif

  if (fd != FD_IDLE && now - fdCycleT > cfg.feedTimeoutMs) raiseFault(FLT_FEED_TIMEOUT, "feeder cycle exceeded %lu ms", (unsigned long)cfg.feedTimeoutMs);

#if ENABLE_RPM_SENSORS

  static uint32_t stallSince[2] = {0, 0};

  for (int i = 0; i < 2; i++) {

    if (rpmValid[i] && rpmMeas[i] > HARD_MAX_RPM * 1.05f) raiseFault(FLT_OVERSPEED, "wheel %d measured %.0f RPM", i + 1, rpmMeas[i]);

    if (W[i].ramped > 500 && W[i].pwm > 0.15f && rpmMeas[i] < 50) { if (!stallSince[i]) stallSince[i] = now; else if (now - stallSince[i] > 2000) raiseFault(FLT_RPM_SENSOR, "wheel %d: no RPM pulses (sensor fault or stall)", i + 1); }

    else stallSince[i] = 0;

  }

#endif

  bool active = wheelsEnabled || feedAuto || drillBusy();

  if (active) {

    if (activeClient < 0) stopAll("no client connected");

    else if (now - lastRxMs > cfg.commTimeoutMs) stopAll("communication timeout");

  }

}

static void updateStates() {

  if (posPending && posAvailable() && posReached()) { posPending = false; logf("POSITION", "target position reached"); }

  if (state == S_SPINUP || state == S_POSITIONING || state == S_READY_LAUNCH) {

    if (wheelsEnabled) {

      MState want = !wheelsReady() ? S_SPINUP : ((posAvailable() && posPending) ? S_POSITIONING : S_READY_LAUNCH);

      if (want != state) setState(want, "wheel/position status");

    }

  }

  if (state == S_STOPPING) {

    bool zero = W[0].ramped < 1 && W[1].ramped < 1;

#if ENABLE_RPM_SENSORS

    zero = zero && rpmMeas[0] < 100 && rpmMeas[1] < 100;

#endif

    if (zero && fd == FD_IDLE) setState(S_IDLE, "wheels stopped");

  }

}



// ============================================================================

// COMMANDS

// ============================================================================

typedef const char* (*Handler)(JsonObjectConst in, JsonDocument& out);

static const char* needNum(JsonObjectConst in, const char* k, float lo, float hi, float& v) {

  JsonVariantConst x = in[k];

  if (x.isNull()) return fail("missing_param", "missing '%s'", k);

  if (!x.is<float>()) return fail("invalid_param", "'%s' must be a number", k);

  v = x.as<float>();

  if (isnan(v) || v < lo || v > hi) return fail("out_of_range", "'%s'=%.2f outside [%.2f, %.2f]", k, v, lo, hi);

  return nullptr;

}

static void fillTelemetry(JsonObject o) {

  o["t"] = millis(); o["state"] = STATE_NAMES[state]; o["fault"] = FAULT_NAMES[fault]; o["fault_msg"] = faultMsg;

  JsonObject w = o["wheels"].to<JsonObject>();

  w["mode"] = syncMode ? "synchronized" : "independent"; w["enabled"] = wheelsEnabled;

  w["req1"] = r1(req[0]); w["req2"] = r1(req[1]); w["set1"] = r1(W[0].ramped); w["set2"] = r1(W[1].ramped);

  w["pwm1"] = r1(W[0].pwm * 100); w["pwm2"] = r1(W[1].pwm * 100);

  w["base"] = r1(baseRpm); w["diff"] = r1(diffRpm);

  if (launchLevel >= 0) w["level"] = r1(launchLevel); else w["level"] = nullptr;

  w["measured"] = HAVE_RPM;

  if (HAVE_RPM) { w["rpm1"] = r1(rpmMeas[0]); w["rpm2"] = r1(rpmMeas[1]); } else { w["rpm1"] = nullptr; w["rpm2"] = nullptr; }

  w["ready"] = wheelsReady(); w["ready_basis"] = HAVE_RPM ? "measured_rpm" : "elapsed_time_open_loop";

  if (cfg.wheelDiaMm > 0) { w["surf1_mps"] = r1(PI * cfg.wheelDiaMm / 1000.0f * W[0].ramped / 60.0f); w["surf2_mps"] = r1(PI * cfg.wheelDiaMm / 1000.0f * W[1].ramped / 60.0f); }

  if (cfg.kmhPerRpm > 0) w["est_ball_kmh"] = r1((W[0].ramped + W[1].ramped) / 2 * cfg.kmhPerRpm);

  JsonObject a = o["angle"].to<JsonObject>();

  a["req_elev"] = r1(reqElev); a["req_yaw"] = r1(reqYaw); a["physical"] = posAvailable(); a["pending"] = posPending;

  float ae, ay;

  if (posAvailable() && posActual(ae, ay)) { a["act_elev"] = r1(ae); a["act_yaw"] = r1(ay); } else { a["act_elev"] = nullptr; a["act_yaw"] = nullptr; }

  a["reached"] = posAvailable() && !posPending;

  JsonObject f = o["feeder"].to<JsonObject>();

  f["state"] = FD_NAMES[fd]; f["calibrated"] = feederCalibrated(); f["auto"] = feedAuto; f["paused"] = feedPaused;

  f["interval_ms"] = cfg.feedIntervalMs; f["feeds"] = feedCount; f["min_interval_ms"] = feedMinInterval(); f["ball_sensor"] = ballSensorAvailable();

  JsonObject d = o["drill"].to<JsonObject>();

  d["state"] = DS_NAMES[dr.st]; d["id"] = loaded.id; d["name"] = loaded.name;

  bool live = dr.st != DS_NONE && dr.st != DS_LOADED;

  d["step"] = live ? dr.step + 1 : 0; d["steps"] = loaded.n; d["cycle"] = live ? dr.cycle + 1 : 0; d["cycles"] = loaded.cycles;

  d["rep"] = dr.rep; d["reps"] = (dr.st != DS_NONE && loaded.n) ? loaded.s[dr.step < loaded.n ? dr.step : 0].reps : 0;

  d["shots"] = dr.shots; d["total_shots"] = dr.total; d["progress"] = dr.total ? (float)dr.shots / dr.total : 0.0f;

  d["phase"] = dr.ph == PH_FEED ? "FEEDING" : "PREPARING/WAITING"; d["aim_not_performed"] = dr.aimNot; d["pause_pending"] = dr.pauseReq;

  if (dr.err[0]) d["error"] = dr.err; else d["error"] = nullptr;

  o["clients"] = activeClient >= 0 ? 1 : 0;

}

static const char* c_ping(JsonObjectConst, JsonDocument& out) { out["t"] = millis(); return nullptr; }

static const char* c_get_status(JsonObjectConst, JsonDocument& out) { fillTelemetry(out["status"].to<JsonObject>()); return nullptr; }

static const char* c_get_capabilities(JsonObjectConst, JsonDocument& out) {

  JsonObject c = out["caps"].to<JsonObject>();

  c["fw"] = FW_VERSION; c["protocol"] = PROTOCOL_VERSION;

  c["rpm_sensors"] = HAVE_RPM; c["positioning"] = posAvailable(); c["ball_sensor"] = ballSensorAvailable();

  c["estop_sense"] = ENABLE_ESTOP_SENSE != 0; c["interlock"] = ENABLE_INTERLOCK != 0;

  JsonObject f = c["feeder"].to<JsonObject>();

  f["present"] = true; f["calibrated"] = feederCalibrated(); f["min_interval_ms"] = feedMinInterval(); f["max_interval_ms"] = cfg.maxFeedIntervalMs;

  JsonObject l = c["limits"].to<JsonObject>();

  l["max_rpm"] = cfg.maxRpm; l["hard_max_rpm"] = HARD_MAX_RPM; l["elev_min"] = cfg.elevMin; l["elev_max"] = cfg.elevMax;

  l["yaw_min"] = cfg.yawMin; l["yaw_max"] = cfg.yawMax; l["max_steps"] = MAX_STEPS; l["max_reps"] = MAX_REPS;

  l["max_cycles"] = MAX_CYCLES; l["max_total_shots"] = MAX_TOTAL_SHOTS; l["min_step_interval_ms"] = 100; l["max_interval_ms"] = 600000;

  return nullptr;

}

static const char* c_get_config(JsonObjectConst, JsonDocument& out) {

  JsonObject c = out["config"].to<JsonObject>();

  for (size_t i = 0; i < NFIELDS; i++) c[FIELDS[i].key] = fieldGet(cfg, FIELDS[i]);

  out["feeder_calibrated"] = feederCalibrated();

  return nullptr;

}

static const char* c_set_config(JsonObjectConst in, JsonDocument& out) {

  if (drillBusy() || fd != FD_IDLE || wheelsEnabled) return fail("invalid_state", "stop wheels/drill/feeder before changing config");

  JsonObjectConst vals = in["values"].as<JsonObjectConst>();

  if (vals.isNull()) return fail("missing_param", "missing 'values' object");

  Config tmp = cfg; int n = 0;

  for (JsonPairConst kv : vals) {

    const char* k = kv.key().c_str(); const FieldDef* fdp = nullptr;

    for (size_t i = 0; i < NFIELDS; i++) if (!strcmp(FIELDS[i].key, k)) fdp = &FIELDS[i];

    if (!fdp) return fail("invalid_param", "unknown config key '%s'", k);

    if (!kv.value().is<float>()) return fail("invalid_param", "'%s' must be a number", k);

    float v = kv.value().as<float>();

    if (isnan(v) || v < fdp->lo || v > fdp->hi) return fail("out_of_range", "'%s'=%.4f outside [%.4f, %.4f]", k, v, fdp->lo, fdp->hi);

    fieldSet(tmp, *fdp, v); n++;

  }

  const char* e = validateConfig(tmp);

  if (e) return fail("invalid_config", "%s", e);

  cfg = tmp;

  req[0] = fminf(req[0], cfg.maxRpm); req[1] = fminf(req[1], cfg.maxRpm);

  feederApplyConfig();

  logf("CONFIG", "%d value(s) applied (RAM). Use save_config to persist.", n);

  out["applied"] = n; out["feeder_calibrated"] = feederCalibrated();

  return nullptr;

}

static const char* c_save_config(JsonObjectConst, JsonDocument&) { saveConfig(); logf("CONFIG", "saved to flash"); return nullptr; }



static const char* c_set_launch_speed(JsonObjectConst in, JsonDocument& out) {

  if (drillBusy()) return fail("drill_active", "drill controls the wheels");

  if (!syncMode) return fail("wrong_wheel_mode", "independent mode active: use set_wheel_speeds or set_wheel_mode");

  bool hasRpm = !in["rpm"].isNull(), hasLvl = !in["level_pct"].isNull();

  if (hasRpm == hasLvl) return fail("invalid_param", "provide exactly one of 'rpm' or 'level_pct'");

  float base, lvl = -1;

  if (hasRpm) TRY(needNum(in, "rpm", 0, cfg.maxRpm, base));

  else { TRY(needNum(in, "level_pct", 0, 100, lvl)); base = levelToRpm(lvl); }

  float diff = diffRpm;

  if (!in["diff_rpm"].isNull()) TRY(needNum(in, "diff_rpm", -cfg.maxRpm, cfg.maxRpm, diff));

  float w1 = base + diff / 2, w2 = base - diff / 2;

  if (w1 < 0 || w2 < 0 || w1 > cfg.maxRpm || w2 > cfg.maxRpm) return fail("out_of_range", "wheel targets %.0f/%.0f outside [0, %.0f]", w1, w2, cfg.maxRpm);

  baseRpm = base; diffRpm = diff; launchLevel = lvl; req[0] = w1; req[1] = w2;

  targetsChanged("set_launch_speed");

  out["wheel1"] = r1(w1); out["wheel2"] = r1(w2); out["open_loop"] = !HAVE_RPM;

  out["note"] = "wheel RPM is not ball exit speed";

  return nullptr;

}

static const char* c_set_wheel_speeds(JsonObjectConst in, JsonDocument& out) {

  if (drillBusy()) return fail("drill_active", "drill controls the wheels");

  if (syncMode) return fail("wrong_wheel_mode", "synchronized mode active: use set_launch_speed or set_wheel_mode");

  float a, b; TRY(needNum(in, "m1", 0, cfg.maxRpm, a)); TRY(needNum(in, "m2", 0, cfg.maxRpm, b));

  req[0] = a; req[1] = b; launchLevel = -1; targetsChanged("set_wheel_speeds");

  out["wheel1"] = a; out["wheel2"] = b; out["open_loop"] = !HAVE_RPM; return nullptr;

}

static const char* c_set_wheel_mode(JsonObjectConst in, JsonDocument& out) {

  if (drillBusy()) return fail("drill_active", "drill controls the wheels");

  const char* m = in["mode"] | "";

  if (!strcmp(m, "synchronized")) { syncMode = true; baseRpm = (req[0] + req[1]) / 2; diffRpm = req[0] - req[1]; }

  else if (!strcmp(m, "independent")) syncMode = false;

  else return fail("invalid_param", "mode must be 'synchronized' or 'independent'");

  logf("MODE", "wheel mode = %s", m); out["mode"] = m; return nullptr;

}

static const char* c_set_launch_angle(JsonObjectConst in, JsonDocument& out) {

  float e; TRY(needNum(in, "elev", cfg.elevMin, cfg.elevMax, e));

  reqElev = e; logf("POSITION", "requested elevation=%.1f deg (%s)", e, posAvailable() ? "hardware present" : "NO positioning hardware - stored only");

  out["physical"] = posAvailable(); if (!posAvailable()) out["warning"] = "positioning_hardware_unavailable: value stored, no physical motion";

  return nullptr;

}

static const char* c_set_yaw_angle(JsonObjectConst in, JsonDocument& out) {

  float y; TRY(needNum(in, "yaw", cfg.yawMin, cfg.yawMax, y));

  reqYaw = y; logf("POSITION", "requested yaw=%.1f deg (%s)", y, posAvailable() ? "hardware present" : "NO positioning hardware - stored only");

  out["physical"] = posAvailable(); if (!posAvailable()) out["warning"] = "positioning_hardware_unavailable: value stored, no physical motion";

  return nullptr;

}

static const char* c_apply_position(JsonObjectConst in, JsonDocument& out) {

  float e, y; TRY(needNum(in, "elev", cfg.elevMin, cfg.elevMax, e)); TRY(needNum(in, "yaw", cfg.yawMin, cfg.yawMax, y));

  reqElev = e; reqYaw = y; out["stored"] = true;

  if (!posAvailable()) { logf("POSITION", "apply elev=%.1f yaw=%.1f: stored only, no positioning hardware", e, y); return fail("positioning_unavailable", "requested angles stored; no positioning hardware installed, nothing moved"); }

  posRequest(e, y); posPending = true; return nullptr;

}

static const char* c_start_launch(JsonObjectConst, JsonDocument& out) {

  if (drillBusy()) return fail("drill_active", "drill controls the wheels");

  if (state != S_IDLE && state != S_READY) return fail("invalid_state", "launch not startable from %s", STATE_NAMES[state]);

  if (req[0] <= 0 && req[1] <= 0) return fail("no_target", "set a launch speed first");

  if (!interlockOk()) return fail("interlock_open", "interlock open");

  wheelsEnabled = true; setState(S_SPINUP, "start_launch"); out["state"] = STATE_NAMES[state]; return nullptr;

}

static const char* c_stop_launch(JsonObjectConst, JsonDocument& out) { stopAll("stop_launch command"); out["state"] = STATE_NAMES[state]; return nullptr; }

static const char* c_clear_fault(JsonObjectConst, JsonDocument& out) {

  if (state != S_FAULT && state != S_ESTOP) return fail("invalid_state", "no fault latched");

  if (!estopHealthy()) return fail("fault_condition_persists", "physical E-stop circuit still open");

  if (!interlockOk()) return fail("fault_condition_persists", "interlock still open");

  logf("FAULT", "cleared (%s). No automatic restart.", FAULT_NAMES[fault]);

  fault = FLT_NONE; faultMsg[0] = 0; state = S_IDLE; out["state"] = "IDLE"; return nullptr;

}

static const char* c_estop(JsonObjectConst, JsonDocument& out) {

  raiseFault(FLT_ESTOP_SW, "software emergency stop requested (NOT a substitute for the physical e-stop)");

  out["note"] = "outputs disabled; use the PHYSICAL e-stop to remove motor power"; return nullptr;

}

// ---- feeding

static const char* c_set_feed_interval(JsonObjectConst in, JsonDocument& out) {

  if (drillBusy()) return fail("drill_active", "drill interval_ms takes precedence; global feed interval locked during drills");

  float iv; TRY(needNum(in, "interval_ms", feedMinInterval(), cfg.maxFeedIntervalMs, iv));

  cfg.feedIntervalMs = (uint32_t)lroundf(iv); out["interval_ms"] = cfg.feedIntervalMs; out["balls_per_min"] = 60000.0f / cfg.feedIntervalMs;

  logf("FEEDER", "interval = %lu ms (%.2f balls/min)", (unsigned long)cfg.feedIntervalMs, 60000.0f / cfg.feedIntervalMs); return nullptr;

}

static const char* c_set_feed_rate(JsonObjectConst in, JsonDocument& out) {

  if (drillBusy()) return fail("drill_active", "drill interval_ms takes precedence; global feed interval locked during drills");

  float bpm; TRY(needNum(in, "balls_per_min", 60000.0f / cfg.maxFeedIntervalMs, 60000.0f / feedMinInterval(), bpm));

  cfg.feedIntervalMs = (uint32_t)lroundf(60000.0f / bpm);       // interval_ms = 60000 / balls_per_minute

  if (cfg.feedIntervalMs < feedMinInterval()) cfg.feedIntervalMs = feedMinInterval();

  out["interval_ms"] = cfg.feedIntervalMs; out["balls_per_min"] = 60000.0f / cfg.feedIntervalMs;

  logf("FEEDER", "rate %.2f balls/min -> interval %lu ms", bpm, (unsigned long)cfg.feedIntervalMs); return nullptr;

}

static const char* c_feed_one(JsonObjectConst, JsonDocument& out) {

  if (drillBusy()) return fail("drill_active", "drill controls the feeder");

  const char* why = ""; if (!feederPermitted(false, &why)) return fail(why, "feeding not permitted: %s (state %s)", why, STATE_NAMES[state]);

  if (!wheelsReady()) return fail("wheels_not_ready", "wheels not at stable speed");

  if (posAvailable() && posPending) return fail("positioning_incomplete", "positioning not finished");

  feederStart(); out["commanded"] = true; out["note"] = "commanded feed, not a confirmed launch"; return nullptr;

}

static const char* c_start_feeding(JsonObjectConst in, JsonDocument& out) {

  if (drillBusy()) return fail("drill_active", "drill controls the feeder");

  if (!in["interval_ms"].isNull()) { float iv; TRY(needNum(in, "interval_ms", feedMinInterval(), cfg.maxFeedIntervalMs, iv)); cfg.feedIntervalMs = (uint32_t)lroundf(iv); }

  else if (!in["balls_per_min"].isNull()) { float b; TRY(needNum(in, "balls_per_min", 60000.0f / cfg.maxFeedIntervalMs, 60000.0f / feedMinInterval(), b)); cfg.feedIntervalMs = (uint32_t)lroundf(60000.0f / b); }

  const char* why = ""; if (!feederPermitted(false, &why)) return fail(why, "feeding not permitted: %s (state %s)", why, STATE_NAMES[state]);

  if (!wheelsReady()) return fail("wheels_not_ready", "wheels not at stable speed");

  feedAuto = true; feedPaused = false; nextFeedAt = millis(); feedNotReadySince = 0;

  logf("FEEDER", "timed feeding STARTED, interval %lu ms", (unsigned long)cfg.feedIntervalMs); out["interval_ms"] = cfg.feedIntervalMs; return nullptr;

}

static const char* c_stop_feeding(JsonObjectConst, JsonDocument&) { feedAuto = false; feedPaused = false; logf("FEEDER", "timed feeding STOPPED (current cycle finishes)"); return nullptr; }

static const char* c_pause_feeding(JsonObjectConst, JsonDocument&) {

  if (!feedAuto) return fail("invalid_state", "not feeding");

  feedPaused = true; uint32_t n = millis(); feedPauseRemain = (int32_t)(nextFeedAt - n) > 0 ? nextFeedAt - n : 0; logf("FEEDER", "timed feeding PAUSED"); return nullptr;

}

static const char* c_resume_feeding(JsonObjectConst, JsonDocument&) {

  if (!feedAuto || !feedPaused) return fail("invalid_state", "feeding not paused");

  feedPaused = false; nextFeedAt = millis() + feedPauseRemain; logf("FEEDER", "timed feeding RESUMED"); return nullptr;

}

// ---- drills

static const char* c_validate_drill(JsonObjectConst in, JsonDocument& out) {

  JsonObjectConst d = in["drill"].as<JsonObjectConst>(); if (d.isNull()) return fail("missing_param", "missing 'drill'");

  JsonArray w = out["warnings"].to<JsonArray>(); TRY(parseDrill(d, staging, w));

  out["valid"] = true; out["steps"] = staging.n; out["total_shots"] = drillTotalShots(staging); return nullptr;

}

static const char* c_upload_drill(JsonObjectConst in, JsonDocument& out) {

  if (drillBusy()) return fail("drill_active", "stop the running drill first");

  JsonObjectConst d = in["drill"].as<JsonObjectConst>(); if (d.isNull()) return fail("missing_param", "missing 'drill'");

  JsonArray w = out["warnings"].to<JsonArray>(); TRY(parseDrill(d, staging, w));

  loaded = staging; dr = DrillRun(); dr.st = DS_LOADED; dr.total = drillTotalShots(loaded);

  logf("DRILL", "uploaded id=%s name=\"%s\" steps=%u cycles=%u total_shots=%lu (RAM only, lost on reboot)", loaded.id, loaded.name, loaded.n, loaded.cycles, (unsigned long)dr.total);

  out["drill_id"] = loaded.id; out["steps"] = loaded.n; out["total_shots"] = dr.total; return nullptr;

}

static const char* c_start_drill(JsonObjectConst in, JsonDocument& out) {

  if (dr.st == DS_NONE) return fail("no_drill", "upload a drill first");

  if (drillBusy()) return fail("invalid_state", "drill already running");

  const char* id = in["id"] | ""; if (strlen(id) && strcmp(id, loaded.id)) return fail("drill_id_mismatch", "loaded drill is '%s'", loaded.id);

  if (state != S_IDLE && state != S_READY) return fail("invalid_state", "drill cannot start from %s", STATE_NAMES[state]);

  if (!interlockOk()) return fail("interlock_open", "interlock open");

  for (int i = 0; i < loaded.n; i++) {

    if (loaded.s[i].feed && !feederCalibrated()) return fail("feeder_not_calibrated", "feeder not calibrated");

    if (loaded.s[i].requirePos && !posAvailable()) return fail("positioning_unavailable", "step %d requires positioning", i + 1);

  }

  uint32_t total = drillTotalShots(loaded); dr = DrillRun(); dr.st = DS_RUNNING; dr.total = total;

  setState(S_DRILL, "start_drill");

  logf("DRILL", "START id=%s", loaded.id); drillBeginStep(millis());

  out["drill_id"] = loaded.id; return nullptr;

}

static const char* c_pause_drill(JsonObjectConst, JsonDocument& out) {

  if (dr.st != DS_RUNNING) return fail("invalid_state", "drill not running");

  dr.pauseReq = true; out["pending"] = (dr.ph == PH_FEED); logf("DRILL", "pause requested"); return nullptr;

}

static const char* c_resume_drill(JsonObjectConst, JsonDocument&) {

  if (dr.st != DS_PAUSED) return fail("invalid_state", "drill not paused");

  if (fault != FLT_NONE) return fail("fault_active", "fault active");

  uint32_t shift = millis() - dr.pausedAt; dr.notBefore += shift; dr.waitSince = 0; dr.st = DS_RUNNING; setState(S_DRILL, "resume");

  logf("DRILL", "RESUMED"); return nullptr;

}

static const char* c_stop_drill(JsonObjectConst, JsonDocument&) {

  if (!drillBusy()) return fail("invalid_state", "no drill running");

  drillFinish(DS_CANCELLED, "stopped by user"); return nullptr;

}

static const char* c_get_drill_status(JsonObjectConst, JsonDocument& out) {

  JsonDocument t; fillTelemetry(t.to<JsonObject>()); out["drill"] = t["drill"]; return nullptr;

}



struct Cmd { const char* name; Handler fn; bool needsNoFault; };

static const Cmd CMDS[] = {

  {"ping", c_ping, false}, {"get_status", c_get_status, false}, {"get_capabilities", c_get_capabilities, false},

  {"get_config", c_get_config, false}, {"set_config", c_set_config, true}, {"save_config", c_save_config, true},

  {"set_launch_speed", c_set_launch_speed, true}, {"set_wheel_speeds", c_set_wheel_speeds, true}, {"set_wheel_mode", c_set_wheel_mode, true},

  {"set_launch_angle", c_set_launch_angle, true}, {"set_yaw_angle", c_set_yaw_angle, true}, {"apply_position", c_apply_position, true},

  {"upload_drill", c_upload_drill, true}, {"validate_drill", c_validate_drill, false}, {"start_drill", c_start_drill, true},

  {"pause_drill", c_pause_drill, false}, {"resume_drill", c_resume_drill, true}, {"stop_drill", c_stop_drill, false},

  {"get_drill_status", c_get_drill_status, false},

  {"set_feed_interval", c_set_feed_interval, true}, {"set_feed_rate", c_set_feed_rate, true}, {"feed_one_ball", c_feed_one, true},

  {"start_feeding", c_start_feeding, true}, {"stop_feeding", c_stop_feeding, false}, {"pause_feeding", c_pause_feeding, false},

  {"resume_feeding", c_resume_feeding, true}, {"start_launch", c_start_launch, true}, {"stop_launch", c_stop_launch, false},

  {"clear_fault", c_clear_fault, false}, {"emergency_stop_request", c_estop, false},

};

static const size_t NCMDS = sizeof(CMDS) / sizeof(CMDS[0]);



static void sendAckDoc(uint8_t num, JsonDocument& out) { String s; serializeJson(out, s); webSocket.sendTXT(num, s); }

static void rejectRaw(uint8_t num, long id, const char* cmd, const char* code, const char* msg) {

  JsonDocument o; o["type"] = "ack"; o["id"] = id; o["cmd"] = cmd; o["ok"] = false;

  o["error"]["code"] = code; o["error"]["message"] = msg; sendAckDoc(num, o);

  logf("VALIDATION", "rejected: %s - %s", code, msg); logf("ACK", "id=%ld cmd=%s REJECTED (%s)", id, cmd, code);

}

static void handleMessage(uint8_t num, uint8_t* payload, size_t len) {

  lastRxMs = millis();

  // log every received message (truncated), including malformed/oversized ones

  if (len > LOG_JSON_MAX) { char t[LOG_JSON_MAX + 1]; memcpy(t, payload, LOG_JSON_MAX); t[LOG_JSON_MAX] = 0; logf("RX", "%s ...(truncated, %u bytes)", t, (unsigned)len); }

  else { char t[LOG_JSON_MAX + 1]; memcpy(t, payload, len); t[len] = 0; logf("RX", "%s", t); }

  if (len > MAX_MSG_BYTES) { rejectRaw(num, 0, "?", "message_too_large", "message exceeds limit"); return; }

  JsonDocument doc;

  DeserializationError e = deserializeJson(doc, (const char*)payload, len);

  if (e) { logf("ERROR", "malformed JSON: %s", e.c_str()); rejectRaw(num, 0, "?", "malformed_json", e.c_str()); return; }

  if (!doc.is<JsonObject>()) { rejectRaw(num, 0, "?", "malformed_json", "top-level value must be an object"); return; }

  JsonObjectConst in = doc.as<JsonObjectConst>();

  const char* type = in["type"] | ""; long id = in["id"] | 0L;

  if (!strlen(type)) { rejectRaw(num, id, "?", "missing_type", "missing 'type'"); return; }

  logf("COMMAND", "%s", type);

  const Cmd* c = nullptr;

  for (size_t i = 0; i < NCMDS; i++) if (!strcmp(CMDS[i].name, type)) c = &CMDS[i];

  if (!c) { rejectRaw(num, id, type, "unknown_command", "unknown command"); return; }

  if (c->needsNoFault && (state == S_FAULT || state == S_ESTOP)) { rejectRaw(num, id, type, "fault_active", "fault/e-stop latched: send clear_fault after resolving the cause"); return; }

  if (state == S_INIT) { rejectRaw(num, id, type, "invalid_state", "initializing"); return; }

  JsonDocument out; out["type"] = "ack"; out["id"] = id; out["cmd"] = type;

  errMsg[0] = 0;

  const char* err = c->fn(in, out);

  out["ok"] = (err == nullptr);

  if (err) { JsonObject eo = out["error"].to<JsonObject>(); eo["code"] = err; eo["message"] = errMsg; logf("VALIDATION", "rejected: %s - %s", err, errMsg); logf("ACK", "id=%ld cmd=%s REJECTED (%s)", id, type, err); }

  else { logf("VALIDATION", "accepted"); logf("ACK", "id=%ld cmd=%s accepted", id, type); }

  sendAckDoc(num, out);

}



// ============================================================================

// WEBSOCKET / HTTP

// ============================================================================

static void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {

  switch (type) {

    case WStype_CONNECTED: {

      logf("WS", "client %u connected from %s", num, webSocket.remoteIP(num).toString().c_str());

      lastRxMs = millis();

      if (activeClient >= 0 && activeClient != num) {

        uint8_t old = activeClient; activeClient = num;

        logf("WS", "replacing previous client %u", old);

        webSocket.disconnect(old);

        stopAll("client replaced (no automatic resume)");

      } else activeClient = num;

      break;

    }

    case WStype_DISCONNECTED:

      logf("WS", "client %u disconnected", num);

      if ((int)num == activeClient) { activeClient = -1; stopAll("client disconnected"); }

      break;

    case WStype_TEXT:

      if ((int)num != activeClient) { logf("WARN", "message from non-active client %u ignored", num); webSocket.disconnect(num); break; }

      handleMessage(num, payload, length);

      break;

    case WStype_BIN: logf("WARN", "binary frame (%u bytes) rejected", (unsigned)length); break;

    default: break;

  }

}

static void httpRoot() { http.send(200, "text/plain", "ATLAS launcher. WebSocket: ws://192.168.4.1:81  Status: /status"); }

static void httpStatus() {

  JsonDocument d; fillTelemetry(d.to<JsonObject>()); String s; serializeJson(d, s); http.send(200, "application/json", s);

}



// ============================================================================

// SETUP / LOOP

// ============================================================================

void setup() {

  motorsInitSafe();                                  // FIRST: outputs safe before anything else

#if ARDUINO_USB_CDC_ON_BOOT

  Serial.setTxTimeoutMs(0);                          // never block control loop if no host is reading

#endif

  Serial.begin(115200);

  uint32_t t0 = millis(); while (!Serial && millis() - t0 < 1500) delay(10);

#if ENABLE_ESTOP_SENSE

  pinMode(PIN_ESTOP_SENSE, INPUT_PULLUP);

#endif

#if ENABLE_INTERLOCK

  pinMode(PIN_INTERLOCK, INPUT_PULLUP);

#endif

#if ENABLE_RPM_SENSORS

  pinMode(PIN_RPM1, INPUT_PULLUP); pinMode(PIN_RPM2, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(PIN_RPM1), isrRpm0, RISING);

  attachInterrupt(digitalPinToInterrupt(PIN_RPM2), isrRpm1, RISING);

#endif

  logf("BOOT", "ATLAS firmware %s, protocol %d", FW_VERSION, PROTOCOL_VERSION);

  loadConfig();

  feederApplyConfig();                               // moves servo to REST only if calibrated; never feeds

  WiFi.mode(WIFI_AP);

  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 2);   // channel 1, max 2 clients
  logf("BOOT", "softAP start: %s", apOk ? "OK" : "FAILED");

  webSocket.begin(); webSocket.onEvent(onWsEvent); webSocket.enableHeartbeat(3000, 2000, 2);

  http.on("/", httpRoot); http.on("/status", httpStatus); http.begin();

  setState(S_IDLE, "startup complete");

  logf("BOOT", "---- startup summary ----");

  logf("BOOT", "Wi-Fi AP SSID=\"%s\" IP=%s  WS port %u", AP_SSID, WiFi.softAPIP().toString().c_str(), WS_PORT);

  logf("BOOT", "motors: PWM1=GPIO%d DIR1=GPIO%d PWM2=GPIO%d DIR2=GPIO%d, PWM=%lu Hz, outputs 0", PIN_PWM1, PIN_DIR1, PIN_PWM2, PIN_DIR2, (unsigned long)PWM_FREQ);

  logf("BOOT", "limits: max %.0f RPM (hard %.0f), max PWM %.0f%%, ramp +%.0f/-%.0f RPM/s", cfg.maxRpm, HARD_MAX_RPM, cfg.maxPwmPct, cfg.rampUp, cfg.rampDown);

  logf("BOOT", "feeder: GPIO%d, %s", PIN_SERVO, feederCalibrated() ? "calibrated (at REST)" : "NOT CALIBRATED - feeding disabled");

  logf("BOOT", "RPM sensors: %s | positioning: %s | ball sensor: no | e-stop sense: %s | interlock: %s",

       HAVE_RPM ? "yes" : "NO (open-loop)", posAvailable() ? "yes" : "NO", ENABLE_ESTOP_SENSE ? "yes" : "no", ENABLE_INTERLOCK ? "yes" : "no");

  logf("BOOT", "NOTE: software stop is not a substitute for the physical DC emergency stop");

}



void loop() {

  webSocket.loop(); http.handleClient();

  uint32_t now = millis();

  static uint32_t lastCtl = 0, lastTel = 0, lastRpmLog = 0;

  rpmTick();

  if (now - lastCtl >= CONTROL_MS) { float dt = (now - lastCtl) / 1000.0f; if (dt > 0.1f) dt = 0.1f; lastCtl = now; controlTick(dt); }

  safetyTick(); feederTick(); drillTick(); updateStates();

  if (now - lastTel >= TELEMETRY_MS) {

    lastTel = now;

    if (activeClient >= 0) { JsonDocument d; d["type"] = "telemetry"; fillTelemetry(d.as<JsonObject>()); sendDoc(d); }

  }

  if (wheelsEnabled && now - lastRpmLog >= 1000) {

    lastRpmLog = now;

    if (HAVE_RPM) logf("RPM", "measured %.0f/%.0f target %.0f/%.0f pwm %.0f/%.0f%%", rpmMeas[0], rpmMeas[1], req[0], req[1], W[0].pwm * 100, W[1].pwm * 100);

    else logf("RPM", "open-loop (NOT measured) setpoint %.0f/%.0f target %.0f/%.0f pwm %.0f/%.0f%%", W[0].ramped, W[1].ramped, req[0], req[1], W[0].pwm * 100, W[1].pwm * 100);

  }

}
