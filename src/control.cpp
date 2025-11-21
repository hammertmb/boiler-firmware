// FW-VERSION: V12.3-common-pilot — control.cpp

#include <Arduino.h>
#include <math.h>

#include "control.h"
#include "vfd.h"
#include "sensors.h"
#include "logging.h"
#include "profiles.h"   // g_cfg, g_common, g_boilerMode, MODE_*

// ===== Реализация удержания ступени ПЧ (ручной тест) =========================

static int8_t   s_forcedPcStep  = -1;
static uint32_t s_forcedPcUntil = 0;

void controlForcePcStep(int8_t step, uint32_t hold_ms)
{
    if (step >= 0 && step <= 7) {
        s_forcedPcStep  = step;
        s_forcedPcUntil = millis() + hold_ms;
        applySpeed((uint8_t)step);   // включаем ступень немедленно
    } else {
        s_forcedPcStep  = -1;
        s_forcedPcUntil = 0;
    }
}

void controlClearPcStep()
{
    s_forcedPcStep  = -1;
    s_forcedPcUntil = 0;
}

bool controlIsPcStepForced()
{
    if (s_forcedPcStep < 0) return false;

    // истёк таймер?
    if ((int32_t)(millis() - s_forcedPcUntil) >= 0) {
        s_forcedPcStep = -1;
        return false;
    }
    return true;
}

// ===== ШНЕК: внутренние переменные =========================================

static uint16_t aug_Ton   = 0;   // мс
static uint16_t aug_Toff  = 0;   // мс
static uint32_t aug_tMark = 0;
static bool     aug_isOn  = false;

// ===== Расчёт средней подачи шнека (простой вариант 3.1 без ступеней) ========
// Q = Q100 · duty · K
// duty = Ton / (Ton + Toff)

float calcAugerFeedKgPerHour(float q100_kgph,
                             float k_material,
                             float ton_s,
                             float toff_s)
{
    if (q100_kgph <= 0.0f)  return 0.0f;
    if (k_material <= 0.0f) return 0.0f;
    if (ton_s <= 0.0f)      return 0.0f;

    const float period_s = ton_s + toff_s;
    if (period_s <= 0.0f)  return 0.0f;

    const float duty = ton_s / period_s;
    return q100_kgph * duty * k_material;
}

// ТЕКУЩАЯ подача топлива в кг/ч:
// - считаем по активному профилю g_cfg (тон/тофф того режима, который сейчас выбран);
// - если котёл реально стоит (OFF/ALARM или скорость 0) → 0.0.
float getCurrentFuelFeedKgPerHour()
{
    // Котёл не работает → реальной подачи нет
    if (g_boilerMode == MODE_OFF || g_boilerMode == MODE_ALARM || g_speed_mode == 0) {
        return 0.0f;
    }

    // Берём времена шнека из активного профиля (режим, выбранный в UI)
    float ton_s  = g_cfg.ton_ui_sec;
    float toff_s = g_cfg.toff_ui_sec;

    if (ton_s <= 0.0f) {
        return 0.0f;
    }
    if (toff_s < 0.0f) {
        toff_s = 0.0f;
    }

    return calcAugerFeedKgPerHour(
        g_common.q100_kgph,
        g_common.k_material,
        ton_s,
        toff_s
    );
}

// ===== Клампы минимальных длительностей (для авто-режимов) ===================

static inline float clampTon(float s)
{
    const float mn = g_common.aug_min_ton_ms / 1000.0f;
    if (mn <= 0.0f) return s;
    if (s > 0.0f && s < mn) return mn;
    return s;
}

static inline float clampToff(float s)
{
    const float mn = g_common.aug_min_toff_ms / 1000.0f;
    if (mn <= 0.0f) return s;
    if (s > 0.0f && s < mn) return mn;
    return s;
}

// ===== Расчёт duty для шнека по доле мощности P [0..1] (авто-контур) ========

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

    // ещё раз следим за минимальной паузой
    float minToff_s = g_common.aug_min_toff_ms / 1000.0f;
    if (toff_s < minToff_s) {
        toff_s = minToff_s;
    }

    aug_Ton  = (uint16_t)roundf(ton_s  * 1000.0f);
    aug_Toff = (uint16_t)roundf(toff_s * 1000.0f);
}

// ===== Коммутация шнека (привязка к железу) =================================

static void auger_hwSwitch(bool /*on*/)
{
    // Здесь должна быть твоя реальная команда на реле шнека.
    // Пример:
    //   setRelay(PIN_RELAY_AUGER, on);
}

// ===== Тик шнека: реализует TON/TOFF по aug_Ton/aug_Toff =====================

static void auger_tick()
{
  Serial.printf("auger_on=%d Ton=%u Toff=%u Δ=%lu\n", aug_isOn, aug_Ton, aug_Toff, millis() - aug_tMark);

    const uint32_t now = millis();

    if (!aug_isOn) {
        // Сейчас пауза → ждём окончания паузы
        if (now - aug_tMark >= aug_Toff) {
            aug_isOn  = true;
            aug_tMark = now;
            auger_hwSwitch(true);
        }
    } else {
        // Сейчас шнек крутится → ждём окончания работы
        if (now - aug_tMark >= aug_Ton) {
            aug_isOn  = false;
            aug_tMark = now;
            auger_hwSwitch(false);
        }
    }
}

// ===== Инициализация управления ==============================================

void controlInit()
{
    // безопасное состояние старта
    applySpeed(0);          // вентилятор/ПЧ в ноль

    aug_isOn  = false;
    aug_Ton   = (uint16_t)g_common.aug_min_ton_ms;
    aug_Toff  = (uint16_t)g_common.aug_min_toff_ms;
    aug_tMark = millis();
    auger_hwSwitch(false);

    logMsg("controlInit()");
}

// ===== Основной цикл управления ==============================================

void controlTick()
{
    // 1. Ручное удержание ступени ПЧ (тест PC-00..07)
    if (controlIsPcStepForced()) {
        // удерживаем выбранную ступень; основной цикл не перетирает выходы
        return;
    }

    // 2. Защита по максимальной температуре подачи
    {
        float ts = getTsupply();
        if (!isnan(ts) && ts > g_common.T_supply_max) {
            applySpeed(0);
            g_boilerMode = MODE_ALARM;
            logMsg("АВАРИЯ: превышение T подачи");
        }
    }

    // 3. Логика режимов
    switch (g_boilerMode) {

        case MODE_PILOT:
        {
            // ФИТИЛЬ: вентилятор — минимальная ступень; шнек — слабая подача

            // --- вентилятор ---
            float fan_pct = g_common.fan_steps[0];

            uint8_t best = 0;
            float   bd   = 1e9f;

            for (uint8_t i = 0; i < 8; ++i) {    // PC-00..07
                float d = fabsf(g_common.fan_steps[i] - fan_pct);
                if (d < bd) {
                    bd   = d;
                    best = i;
                }
            }

            const uint8_t desired = best + 1;    // applySpeed: 1..8

            if (g_speed_mode != desired &&
                millis() - g_lastSpeedChangeMs > 2000)
            {
                applySpeed(desired);
            }

            // --- шнек ---
            float Ppilot = g_common.auger_steps10[0] / 100.0f;
            if (Ppilot < 0.0f) Ppilot = 0.0f;
            if (Ppilot > 1.0f) Ppilot = 1.0f;

            auger_update(Ppilot);
        }
        break;

        case MODE_OFF:
            // Полный стоп: вентиляторы в ноль, шнек — в минимум
            applySpeed(0);
            auger_update(0.0f);
            break;

        case MODE_MANUAL:
        {
            // ------------------------------
            // РУЧНОЙ РЕЖИМ
            // ------------------------------
            // 1) Первичный воздух — жёстко по профилю (g_cfg.air_primary_pct_ui).
            // 2) Шнек — прямой TON/TOFF по g_cfg.ton_ui_sec / g_cfg.toff_ui_sec.
            //    Без PI, без анализа дымовых — ровно то, что пользователь задал.

            // --- 1. Вентилятор (через ПЧ) ---
            float fan_pct = g_cfg.air_primary_pct_ui;

            uint8_t best = 0;
            float   bd   = 1e9f;

            // Ищем ближайшую ступень PC-00..07 по таблице fan_steps[0..7]
            for (uint8_t i = 0; i < 8; ++i) {
                float d = fabsf(g_common.fan_steps[i] - fan_pct);
                if (d < bd) {
                    bd   = d;
                    best = i;
                }
            }

            const uint8_t desired = best + 1;    // applySpeed: 1..8

            if (g_speed_mode != desired &&
                millis() - g_lastSpeedChangeMs > 2000)
            {
                applySpeed(desired);
            }

            // --- 2. Шнек: прямой TON/TOFF из профиля ---
            float ton_s  = g_cfg.ton_ui_sec;
            float toff_s = g_cfg.toff_ui_sec;

            if (ton_s < 0.0f)  ton_s  = 0.0f;
            if (toff_s < 0.0f) toff_s = 0.0f;

            // Учитываем минимальные длительности, если заданы
            float minTon_s  = g_common.aug_min_ton_ms  / 1000.0f;
            float minToff_s = g_common.aug_min_toff_ms / 1000.0f;

            if (ton_s > 0.0f && ton_s < minTon_s)    ton_s  = minTon_s;
            if (toff_s > 0.0f && toff_s < minToff_s) toff_s = minToff_s;

            aug_Ton  = (uint16_t)roundf(ton_s  * 1000.0f);
            aug_Toff = (uint16_t)roundf(toff_s * 1000.0f);

            // Всё остальное (TON/TOFF) делает auger_tick() ниже.
        }
        break;

    #ifdef MODE_IGNITION
        case MODE_IGNITION:
            // Розжиг пока заглушка — оставляем место для авто-логики
            break;
    #endif

        case MODE_ALARM:
            // Авария: всё в ноль, шнек — минимум
            applySpeed(0);
            auger_update(0.0f);
            break;

        default:
            break;
    }

    // 4. Тикаем шнек (реализует TON/TOFF по aug_Ton/aug_Toff)
    auger_tick();
}
