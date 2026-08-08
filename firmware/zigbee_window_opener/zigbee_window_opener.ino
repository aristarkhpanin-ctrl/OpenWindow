// ============================================================================
//  OpenWindow — Zigbee-привод открывания окна на проветривание
//  ---------------------------------------------------------------------------
//  Плата     : ESP32-C6 (ESP32-C6-DevKitC-1 или SuperMini)
//  Ядро      : arduino-esp32 3.1.0 и новее
//  Настройки : Tools → Zigbee mode      : "Zigbee ZCZR (coordinator/router)"
//              Tools → Partition Scheme : "Zigbee ZCZR 4MB with spiffs"
//              Tools → Erase All Flash  : "Enabled" (только при первой прошивке)
//  ---------------------------------------------------------------------------
//  Устройство представляется в сети как Window Covering (кластер 0x0102),
//  поэтому Home Assistant (ZHA), Zigbee2MQTT и deCONZ видят его как штору:
//  открыть / закрыть / стоп / задать позицию в процентах.
//
//  ВАЖНО про проценты. В стандарте ZCL:
//      0   = полностью ОТКРЫТО
//      100 = полностью ЗАКРЫТО
//  В Home Assistant у сущности cover наоборот (100 = открыто) — координатор
//  пересчитывает это сам, вам ничего делать не нужно.
// ============================================================================

#include "config.h"
#include <Zigbee.h>
#include <Preferences.h>
#include <Wire.h>

// ---------------------------------------------------------------------------
//  Глобальное состояние
// ---------------------------------------------------------------------------
ZigbeeWindowCovering zbCover(ZB_ENDPOINT);
Preferences prefs;

enum State  : uint8_t { ST_IDLE, ST_MOVING, ST_CALIBRATING, ST_FAULT };
enum Dir    : int8_t  { DIR_OPEN = -1, DIR_NONE = 0, DIR_CLOSE = +1 };

static State    g_state      = ST_IDLE;
static Dir      g_dir        = DIR_NONE;

static float    g_posPct     = 100.0f;              // текущая позиция, 0=открыто 100=закрыто
static uint8_t  g_targetPct  = 100;                 // куда едем
static uint32_t g_travelMs   = DEFAULT_TRAVEL_MS;   // время полного хода (калибруется)

static uint32_t g_moveStart  = 0;                   // millis() старта движения
static float    g_posAtStart = 100.0f;
static uint32_t g_lastTick   = 0;
static uint32_t g_lastReport = 0;

static uint32_t g_overCurrentSince = 0;             // для детекта защемления

// Печатать ток в Serial раз в 200 мс — включите на время подбора
// CURRENT_STALL_A (см. docs/04-electronics.md, §3.4), потом выключите.
#define DEBUG_CURRENT 0

// Запросы из Zigbee-колбэков (в самих колбэках ничего долгого делать нельзя)
static volatile bool    g_reqOpen   = false;
static volatile bool    g_reqClose  = false;
static volatile bool    g_reqStop   = false;
static volatile bool    g_reqGoto   = false;
static volatile uint8_t g_reqGotoPct = 0;

// ---------------------------------------------------------------------------
//  Светодиод состояния
// ---------------------------------------------------------------------------
static void led(uint8_t r, uint8_t g, uint8_t b) {
#ifdef RGB_BUILTIN
  rgbLedWrite(PIN_RGB_LED, r, g, b);   // если не компилируется — neopixelWrite(PIN_RGB_LED, r, g, b)
#else
  (void)r; (void)g; (void)b;
#endif
}

// ---------------------------------------------------------------------------
//  INA219 — минимальный драйвер, внешние библиотеки не нужны
// ---------------------------------------------------------------------------
#if USE_INA219
static bool g_inaOk = false;

static void inaInit() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(0x00);            // регистр конфигурации
  Wire.write(0x39);            // 32 В, ±320 мВ, 12 бит, непрерывное измерение
  Wire.write(0x9F);
  g_inaOk = (Wire.endTransmission() == 0);
  Serial.printf("[INA219] %s\n", g_inaOk ? "найден" : "НЕ найден — защита по току отключена");
}

// Возвращает ток мотора в амперах (по падению на шунте)
static float motorCurrent() {
  if (!g_inaOk) return 0.0f;
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(0x01);                                  // регистр Shunt Voltage
  if (Wire.endTransmission(false) != 0) return 0.0f;
  if (Wire.requestFrom(INA219_ADDR, 2) != 2) return 0.0f;
  int16_t raw = (int16_t)((Wire.read() << 8) | Wire.read());
  float vShunt = raw * 10e-6f;                       // LSB = 10 мкВ
  return fabsf(vShunt / INA219_SHUNT_OHM);
}
#else
static void  inaInit() {}
static float motorCurrent() { return 0.0f; }
#endif

// ---------------------------------------------------------------------------
//  Концевики
// ---------------------------------------------------------------------------
static bool limitClosedHit() {
#if USE_LIMIT_SWITCHES
  return digitalRead(PIN_LIMIT_CLOSED) == LOW;
#else
  return false;
#endif
}

static bool limitOpenHit() {
#if USE_LIMIT_SWITCHES
  return digitalRead(PIN_LIMIT_OPEN) == LOW;
#else
  return false;
#endif
}

// ---------------------------------------------------------------------------
//  Управление мотором (DRV8871: IN1/IN2, ШИМ по одному из входов)
// ---------------------------------------------------------------------------
static void motorStop() {
  ledcWrite(PIN_MOTOR_IN1, PWM_MAX);    // оба входа в HIGH = торможение обмоткой
  ledcWrite(PIN_MOTOR_IN2, PWM_MAX);
  delay(40);
  ledcWrite(PIN_MOTOR_IN1, 0);          // затем свободный выбег
  ledcWrite(PIN_MOTOR_IN2, 0);
}

static void motorDrive(Dir dir, uint16_t duty) {
  bool closing = (dir == DIR_CLOSE);
#if MOTOR_INVERT
  closing = !closing;
#endif
  if (closing) { ledcWrite(PIN_MOTOR_IN1, duty); ledcWrite(PIN_MOTOR_IN2, 0); }
  else         { ledcWrite(PIN_MOTOR_IN1, 0);    ledcWrite(PIN_MOTOR_IN2, duty); }
}

// Скважность с плавным разгоном и торможением у цели
static uint16_t rampedDuty(uint32_t elapsedMs, float remainingPct) {
  float k = 1.0f;
  if (elapsedMs < RAMP_MS) k = (float)elapsedMs / RAMP_MS;          // разгон
  float brakePct = 100.0f * RAMP_MS / (float)g_travelMs;            // зона торможения
  if (remainingPct < brakePct && brakePct > 0.5f) {
    k = fminf(k, remainingPct / brakePct);
  }
  uint16_t duty = (uint16_t)(DUTY_MIN + (DUTY_RUN - DUTY_MIN) * k);
  return constrain(duty, DUTY_MIN, DUTY_RUN);
}

// ---------------------------------------------------------------------------
//  Сообщаем позицию координатору
// ---------------------------------------------------------------------------
static void reportPosition() {
  uint8_t p = (uint8_t)lroundf(constrain(g_posPct, 0.0f, 100.0f));
  zbCover.setLiftPercentage(p);
  zbCover.setLiftPosition((uint16_t)(p * STROKE_MM / 100));
}

// ---------------------------------------------------------------------------
//  Движение
// ---------------------------------------------------------------------------
static void stopMotion(const char *why) {
  motorStop();
  g_dir   = DIR_NONE;
  g_state = ST_IDLE;
  g_overCurrentSince = 0;
  reportPosition();
  Serial.printf("[MOVE] стоп (%s), позиция %.1f%%\n", why, g_posPct);
  led(0, 8, 0);
}

static void startMotion(uint8_t targetPct) {
  targetPct = constrain(targetPct, 0, 100);

  // Уже на месте (в пределах 1 %) — ничего не делаем
  if (fabsf(g_posPct - targetPct) < 1.0f) {
    g_posPct = targetPct;
    reportPosition();
    return;
  }

  g_targetPct  = targetPct;
  g_dir        = (targetPct > g_posPct) ? DIR_CLOSE : DIR_OPEN;
  g_posAtStart = g_posPct;
  g_moveStart  = millis();
  g_lastTick   = g_moveStart;
  g_state      = ST_MOVING;
  g_overCurrentSince = 0;

  Serial.printf("[MOVE] %.1f%% → %u%% (%s)\n", g_posPct, targetPct,
                g_dir == DIR_CLOSE ? "закрываем" : "открываем");
  led(0, 0, 20);
}

// Обновление позиции по времени + обработка концевиков, тока, достижения цели
static void updateMotion() {
  uint32_t now = millis();
  uint32_t dt  = now - g_lastTick;
  g_lastTick   = now;

  // 1. Интегрируем позицию по времени
  float dPct = 100.0f * (float)dt / (float)g_travelMs;
  g_posPct  += (g_dir == DIR_CLOSE) ? dPct : -dPct;
  g_posPct   = constrain(g_posPct, 0.0f, 100.0f);

  // 2. Концевики — они всегда важнее расчёта по времени
  if (g_dir == DIR_CLOSE && limitClosedHit()) { g_posPct = 100.0f; stopMotion("концевик «закрыто»"); return; }
  if (g_dir == DIR_OPEN  && limitOpenHit())   { g_posPct = 0.0f;   stopMotion("концевик «открыто»");  return; }

  // 3. Защита от защемления
  uint32_t elapsed = now - g_moveStart;
  if (elapsed > INRUSH_IGNORE_MS) {
    float i = motorCurrent();
    if (i > CURRENT_STALL_A) {
      if (g_overCurrentSince == 0) g_overCurrentSince = now;
      if (now - g_overCurrentSince > STALL_CONFIRM_MS) {
        Serial.printf("[SAFE] перегрузка %.2f А — препятствие!\n", i);
        // отъезжаем назад, чтобы освободить то, что зажали
        Dir back = (g_dir == DIR_CLOSE) ? DIR_OPEN : DIR_CLOSE;
        motorStop();
        delay(150);
        motorDrive(back, DUTY_RUN);
        delay(BACKOFF_MS);
        motorStop();
        g_posPct += (back == DIR_CLOSE ? 1 : -1) * 100.0f * BACKOFF_MS / g_travelMs;
        g_posPct  = constrain(g_posPct, 0.0f, 100.0f);
        g_state   = ST_FAULT;
        g_dir     = DIR_NONE;
        reportPosition();
        led(30, 0, 0);
        return;
      }
    } else {
      g_overCurrentSince = 0;
    }
  }

  // 4. Аварийный таймаут
  if (elapsed > (uint32_t)(g_travelMs * MAX_TRAVEL_FACTOR)) {
    stopMotion("таймаут");
    return;
  }

  // 5. Достигли цели
  bool reached = (g_dir == DIR_CLOSE) ? (g_posPct >= g_targetPct) : (g_posPct <= g_targetPct);
  if (reached) {
    // В крайние положения доезжаем до упора/концевика, а не по таймеру
    if ((g_targetPct >= 100 && (limitClosedHit() || !USE_LIMIT_SWITCHES)) ||
        (g_targetPct <= 0   && (limitOpenHit()   || !USE_LIMIT_SWITCHES)) ||
        (g_targetPct > 0 && g_targetPct < 100)) {
      g_posPct = g_targetPct;
      stopMotion("цель достигнута");
      return;
    }
  }

  // 6. Крутим мотор с нужной скважностью
  float remaining = fabsf(g_targetPct - g_posPct);
  motorDrive(g_dir, rampedDuty(elapsed, remaining));

  // 7. Периодический отчёт в сеть
  if (now - g_lastReport > REPORT_INTERVAL_MS) {
    g_lastReport = now;
    reportPosition();
  }
}

// ---------------------------------------------------------------------------
//  Калибровка: полностью закрыть, затем полностью открыть и замерить время
// ---------------------------------------------------------------------------
static void calibrate() {
  Serial.println("[CAL] калибровка началась");
  led(20, 12, 0);
  g_state = ST_CALIBRATING;

  const uint32_t HARD_LIMIT = 30000;   // абсолютный предел одного прохода, мс

  // Фаза 1 — в положение «закрыто»
  uint32_t t0 = millis();
  motorDrive(DIR_CLOSE, DUTY_RUN);
  while (millis() - t0 < HARD_LIMIT) {
    if (limitClosedHit()) break;
    if (millis() - t0 > INRUSH_IGNORE_MS && motorCurrent() > CURRENT_STALL_A) break;
    delay(5);
  }
  motorStop();
  delay(400);

  // Фаза 2 — в положение «открыто», засекаем время
  t0 = millis();
  motorDrive(DIR_OPEN, DUTY_RUN);
  while (millis() - t0 < HARD_LIMIT) {
    if (limitOpenHit()) break;
    if (millis() - t0 > INRUSH_IGNORE_MS && motorCurrent() > CURRENT_STALL_A) break;
    delay(5);
  }
  uint32_t measured = millis() - t0;
  motorStop();

  if (measured > 1000) {
    g_travelMs = measured;
    prefs.begin("openwindow", false);
    prefs.putULong("travelMs", g_travelMs);
    prefs.end();
    Serial.printf("[CAL] полный ход = %lu мс, сохранено\n", (unsigned long)g_travelMs);
  } else {
    Serial.println("[CAL] не удалось измерить — оставляем прежнее значение");
  }

  g_posPct = 0.0f;             // после калибровки окно открыто
  g_state  = ST_IDLE;
  reportPosition();
  led(0, 8, 0);
}

// ---------------------------------------------------------------------------
//  Zigbee-колбэки: только выставляют флаги
// ---------------------------------------------------------------------------
static void cbOpen()                  { g_reqOpen  = true; }
static void cbClose()                 { g_reqClose = true; }
static void cbStop()                  { g_reqStop  = true; }
static void cbGoToLift(uint8_t pct)   { g_reqGotoPct = pct; g_reqGoto = true; }

// ---------------------------------------------------------------------------
//  Кнопка BOOT: короткое нажатие — открыть/закрыть,
//               3 с — калибровка, 10 с — сброс к заводским (выход из сети)
// ---------------------------------------------------------------------------
static void handleButton() {
  static uint32_t pressedAt = 0;
  static bool     wasDown   = false;
  bool down = (digitalRead(PIN_BUTTON) == LOW);

  if (down && !wasDown) { pressedAt = millis(); }

  if (!down && wasDown) {
    uint32_t held = millis() - pressedAt;
    if (held > 10000) {
      Serial.println("[BTN] сброс к заводским настройкам, выходим из сети…");
      led(30, 0, 30);
      delay(300);
      Zigbee.factoryReset();
    } else if (held > 3000) {
      calibrate();
    } else if (held > 40) {
      if (g_state == ST_MOVING) stopMotion("кнопка");
      else                      startMotion(g_posPct > 50 ? 0 : 100);
    }
  }
  wasDown = down;
}

// ---------------------------------------------------------------------------
//  setup()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== OpenWindow — Zigbee window opener ===");

  // --- ШИМ на мотор ---
  ledcAttach(PIN_MOTOR_IN1, PWM_FREQ_HZ, PWM_RESOLUTION);
  ledcAttach(PIN_MOTOR_IN2, PWM_FREQ_HZ, PWM_RESOLUTION);
  motorStop();

  // --- Входы ---
  pinMode(PIN_LIMIT_CLOSED, INPUT_PULLUP);
  pinMode(PIN_LIMIT_OPEN,   INPUT_PULLUP);
  pinMode(PIN_BUTTON,       INPUT_PULLUP);

  inaInit();

  // --- Восстанавливаем калибровку и позицию ---
  prefs.begin("openwindow", true);
  g_travelMs = prefs.getULong("travelMs", DEFAULT_TRAVEL_MS);
  g_posPct   = prefs.getUChar("pos", 100);
  prefs.end();
  Serial.printf("[NVS] полный ход %lu мс, позиция %.0f%%\n",
                (unsigned long)g_travelMs, g_posPct);

  // --- Zigbee-эндпоинт ---
  zbCover.setManufacturerAndModel(ZB_MANUFACTURER, ZB_MODEL);
  zbCover.setCoveringType(AWNING);                 // ближайший тип для откидного окна
  zbCover.setConfigStatus(
      /*operational*/            true,
      /*online*/                 true,
      /*commands_reversed*/      false,
      /*lift_closed_loop*/       true,
      /*tilt_closed_loop*/       false,
      /*lift_encoder_controlled*/false,
      /*tilt_encoder_controlled*/false);
  zbCover.setMode(/*motor_reversed*/false, /*calibration*/false,
                  /*maintenance*/false, /*leds_on*/false);
  zbCover.setLimits(/*open lift*/0, /*closed lift*/STROKE_MM, /*open tilt*/0, /*closed tilt*/0);

  zbCover.onOpen(cbOpen);
  zbCover.onClose(cbClose);
  zbCover.onStop(cbStop);
  zbCover.onGoToLiftPercentage(cbGoToLift);

  Zigbee.addEndpoint(&zbCover);

  Serial.println("[ZB] запуск стека…");
  if (!Zigbee.begin(ZB_ROLE)) {
    Serial.println("[ZB] стек не стартовал — перезагрузка");
    delay(1000);
    ESP.restart();
  }

  Serial.print("[ZB] подключение к сети");
  while (!Zigbee.connected()) {
    Serial.print('.');
    led(20, 20, 0);
    delay(150);
    led(0, 0, 0);
    delay(150);
  }
  Serial.println(" готово");
  led(0, 8, 0);

  reportPosition();
}

// ---------------------------------------------------------------------------
//  loop()
// ---------------------------------------------------------------------------
void loop() {
  handleButton();

#if DEBUG_CURRENT
  static uint32_t lastDbg = 0;
  if (millis() - lastDbg > 200) {
    lastDbg = millis();
    Serial.printf("I = %.2f A  pos = %.1f%%\n", motorCurrent(), g_posPct);
  }
#endif

  // Команды из сети
  if (g_reqStop)  { g_reqStop = false; if (g_state == ST_MOVING) stopMotion("команда STOP"); }
  if (g_reqOpen)  { g_reqOpen = false;  g_state = ST_IDLE; startMotion(0);   }
  if (g_reqClose) { g_reqClose = false; g_state = ST_IDLE; startMotion(100); }
  if (g_reqGoto)  { g_reqGoto = false;  g_state = ST_IDLE; startMotion(g_reqGotoPct); }

  if (g_state == ST_MOVING) {
    updateMotion();
  } else {
    // Сохраняем позицию в NVS не чаще раза в 5 с и только когда стоим
    static uint32_t lastSave = 0;
    static uint8_t  lastSaved = 255;
    uint8_t p = (uint8_t)lroundf(g_posPct);
    if (p != lastSaved && millis() - lastSave > 5000) {
      prefs.begin("openwindow", false);
      prefs.putUChar("pos", p);
      prefs.end();
      lastSaved = p;
      lastSave  = millis();
    }
  }

  delay(5);
}
