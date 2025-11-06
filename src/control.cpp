// FW-VERSION: V12.3-common-pilot — control.cpp
#include <Arduino.h>
#include <math.h>

#include "control.h"
#include "vfd.h"
#include "sensors.h"
#include "logging.h"
#include "profiles.h"   // g_common, g_boilerMode, MODE_*
// ===== V10.1 build 2025-11-05 — boiler-firmware / fix/run-relay-debug =====
// Реализация удержания ступени ПЧ (ручной тест)
static int8_t   s_forcedPcStep  = -1;
static uint32_t s_forcedPcUntil = 0;

void controlForcePcStep(int8_t step, uint32_t hold_ms){
  if (step >= 0 && step <= 7) {
    s_forcedPcStep  = step;
    s_forcedPcUntil = millis() + hold_ms;
    applySpeed((uint8_t)step);   // включаем ступень немедленно
  } else {
    s_forcedPcStep  = -1;
    s_forcedPcUntil = 0;
  }
}

void controlClearPcStep(){
  s_forcedPcStep  = -1;
  s_forcedPcUntil = 0;
}

bool controlIsPcStepForced(){
  if (s_forcedPcStep < 0) return false;
  if ((int32_t)(millis() - s_forcedPcUntil) >= 0) { // истёк таймер
    s_forcedPcStep = -1;
    return false;
  }
  return true;
}


// ================= ШНЕК: внутренние переменные =================
static uint16_t aug_Ton   = 0;  // мс
static uint16_t aug_Toff  = 0;  // мс
static uint32_t aug_tMark = 0;
static bool     aug_isOn  = false;

float calcAugerFeedKgPerHour(float q100_kgph,
                             float k_material,
                             float ton_s,
                             float toff_s,
                             float step_frac)
{
  if (q100_kgph <= 0.0f)   return 0.0f;
  if (k_material <= 0.0f)  return 0.0f;
  if (step_frac <= 0.0f)   return 0.0f;
  if (ton_s <= 0.0f)       return 0.0f;

  const float period_s = ton_s + toff_s;
  if (period_s <= 0.0f)    return 0.0f;

  const float duty = ton_s / period_s;

  return q100_kgph * duty * step_frac * k_material;
}

static float currentAugerStepFraction()
{
  if (g_speed_mode == 0) {
    return 0.0f;
  }
  uint8_t idx = g_speed_mode ? (uint8_t)(g_speed_mode - 1) : 0;
  const uint8_t maxIdx = (uint8_t)(sizeof(g_common.auger_steps10) / sizeof(g_common.auger_steps10[0]));
  if (maxIdx == 0) {
    return 0.0f;
  }
  if (idx >= maxIdx) {
    idx = maxIdx - 1;
  }
  float value = g_common.auger_steps10[idx];
  if (value <= 0.0f) {
    return 0.0f;
  }
  float frac = value / 100.0f;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  return frac;
}

float getCurrentFuelFeedKgPerHour()
{
  const float ton_s  = aug_Ton  / 1000.0f;
  const float toff_s = aug_Toff / 1000.0f;
  return calcAugerFeedKgPerHour(g_common.q100_kgph,
                                g_common.k_material,
                                ton_s,
                                toff_s,
                                currentAugerStepFraction());
}

// ================= Клампы минимальных длительностей =================
static inline float clampTon(float s)  { const float mn = g_common.aug_min_ton_ms  / 1000.0f; return (s < mn) ? mn : s; }
static inline float clampToff(float s) { const float mn = g_common.aug_min_toff_ms / 1000.0f; return (s < mn) ? mn : s; }

// ================= Расчёт duty для шнека по доле P [0..1] =================
void auger_update(float P)
{
  if (isnan(P) || P <= 0.0f) {
    // «почти выкл.» — соблюдая минимумы
    aug_Ton = (uint16_t)g_common.aug_min_ton_ms;
    uint16_t pause = g_common.aug_min_toff_ms;
    if (pause < 3000) pause = 3000;  // чтобы реально была длинная пауза
    aug_Toff = pause;
    return;
  }
  if (P > 1.0f) P = 1.0f;

  const float Tcycle = 5.0f; // сек
  float ton_s  = clampTon (Tcycle * P);
  float toff_s = clampToff(Tcycle - ton_s);
  if (toff_s < (g_common.aug_min_toff_ms/1000.0f)) {
    toff_s = (g_common.aug_min_toff_ms/1000.0f);
  }

  aug_Ton  = (uint16_t)roundf(ton_s  * 1000.0f);
  aug_Toff = (uint16_t)roundf(toff_s * 1000.0f);
}

// ================= Коммутация шнека (под свои реле/ПЧ) =================
static void auger_hwSwitch(bool /*on*/)
{
  // Привяжи сюда свою реальную команду на шнек (реле/вход ПЧ).
  // setAuger(on);
}

static void auger_tick()
{
  const uint32_t now = millis();
  if (!aug_isOn) {
    if (now - aug_tMark >= aug_Toff) {
      aug_isOn  = true;
      aug_tMark = now;
      auger_hwSwitch(true);
    }
  } else {
    if (now - aug_tMark >= aug_Ton) {
      aug_isOn  = false;
      aug_tMark = now;
      auger_hwSwitch(false);
    }
  }
}

// ================= Инициализация управления (реализация controlInit) =================
void controlInit()
{
  // безопасное состояние старта
  applySpeed(0);                // вентилятор/ПЧ в ноль
  aug_isOn  = false;
  aug_Ton   = (uint16_t)g_common.aug_min_ton_ms;
  aug_Toff  = (uint16_t)g_common.aug_min_toff_ms;
  aug_tMark = millis();
  auger_hwSwitch(false);

  logMsg("controlInit()");
}

// ================= Основной цикл управления =================
void controlTick()
{
  // === приоритет удержания вручную выбранной ступени ПЧ ===
  if (controlIsPcStepForced()) {
    // удерживаем выбранную ступень; основной цикл не перетирает выходы
    return;
  }

  // Защита по максимальной температуре подачи
  {
    float ts = getTsupply();
    if (!isnan(ts) && ts > g_common.T_supply_max) {
      applySpeed(0);
      g_boilerMode = MODE_ALARM;
      logMsg("АВАРИЯ: превышение T подачи");
    }
  }

  switch (g_boilerMode)
  {
    case MODE_PILOT: {
      // ФИТИЛЬ: вентилятор — минимальная ступень; шнек — слабая подача
      float fan_pct = g_common.fan_steps[0];
      uint8_t best = 0; float bd = 1e9f;
      for (uint8_t i=0; i<8; ++i) { // PC-00..07
        float d = fabsf(g_common.fan_steps[i] - fan_pct);
        if (d < bd) { bd = d; best = i; }
      }
      const uint8_t desired = best + 1; // applySpeed: 1..8
      if (g_speed_mode != desired && millis() - g_lastSpeedChangeMs > 2000) {
        applySpeed(desired);
      }

      float Ppilot = g_common.auger_steps10[0] / 100.0f;
      if (Ppilot < 0) Ppilot = 0; if (Ppilot > 1) Ppilot = 1;
      auger_update(Ppilot);
    } break;

    case MODE_OFF:
      applySpeed(0);
      auger_update(0);
      break;

    case MODE_MANUAL:
      // Оставь твою реализацию ручного режима здесь
      break;

    #ifdef MODE_IGNITION
    case MODE_IGNITION:
      // Оставь твою реализацию розжига
      break;
    #endif

    case MODE_ALARM:
      applySpeed(0);
      auger_update(0);
      break;

    default:
      break;
  }

  // тикаем шнек
  auger_tick();
}
