#pragma once
#include <M5EPD.h>
#include <WiFi.h>
#include "DataStore.h"

#ifndef HOME_DIAG_LOG
#define HOME_DIAG_LOG 1
#endif

#if HOME_DIAG_LOG
#define HOME_DIAG_PRINT(evtId, fmt, ...) \
    Serial.printf("[HOME][%lu][E%lu] " fmt "\n", millis(), (unsigned long)(evtId), ##__VA_ARGS__)
#else
#define HOME_DIAG_PRINT(evtId, fmt, ...) ((void)0)
#endif

class HomePage {
public:
    enum Action { ACTION_NONE, ACTION_READ, ACTION_WIFI_SETUP };

    HomePage(M5EPD_Canvas &canvas, DataStore &store)
        : _canvas(canvas), _store(store),
          _touching(false), _touchLastX(0), _touchLastY(0),
          _btnPressed(false), _btnPressTime(0),
          _startPortrait(true), _powerSaveMode(true),
          _diagEventSeq(0), _lastDiagEventId(0),
          _lastBatteryCheckMs(0), _lowBatteryHits(0),
          _batteryMvFiltered(0), _batteryFilterInited(false),
          _lastBatteryPct(0), _batteryPctValid(false) {}

    void render() {
        if (_checkAndHandleLowBattery(true)) return;

        _canvas.fillCanvas(0);
        _canvas.setTextColor(15);
        _canvas.setTextDatum(TL_DATUM);

        int cx = 270; // center x

        // Battery
        uint32_t mv = _readBatteryMvFiltered();
        int pct = -1;
        if (mv > 0) {
            int targetPct = _batteryPercentFromMv(mv);
            pct = _smoothBatteryPercent(targetPct);
        } else if (_batteryPctValid) {
            pct = _lastBatteryPct;
        }

        // Battery icon outline
        _canvas.drawRect(388, 30, 40, 20, 15);
        _canvas.fillRect(428, 36, 4, 8, 15);
        // White background inside, then black fill proportional to pct
        _canvas.fillRect(390, 32, 36, 16, 0);
        int fillW = (pct >= 0) ? (36 * pct / 100) : 0;
        if (fillW > 0) _canvas.fillRect(390, 32, fillW, 16, 15);
        // Percentage text
        char batStr[8];
        if (pct >= 0) {
            snprintf(batStr, sizeof(batStr), "%d%%", pct);
        } else {
            snprintf(batStr, sizeof(batStr), "--%%");
        }
        _canvas.setTextSize(24);
        _canvas.drawString(batStr, 438, 30);

        // Title
        _canvas.setTextSize(36);
        int titleW = 4 * 36;
        _canvas.drawString("纸间书摘", cx - titleW / 2, 80);

        _canvas.setTextSize(24);
        _canvas.setTextColor(8);
        int subW = 6 * 24;
        _canvas.drawString("书摘回顾看板", cx - subW / 2, 132);

        // Divider
        _canvas.fillRect(40, 180, 460, 2, 8);

        int cardX = 36;
        int cardW = 468;
        int excerptCount = _store.getExcerptCount();
        int bookCount = _store.getBookCount();
        const ReviewSettings &rs = _store.getReviewSettings();
        const char *modeStr;
        if (rs.sortRule == 1) {
            modeStr = "随机回顾";
        } else {
            modeStr = rs.sortOrder == 0 ? "顺序回顾(旧→新)" : "顺序回顾(新→旧)";
        }

        // Overview card
        _canvas.drawRoundRect(cardX, 206, cardW, 196, 12, 8);
        _canvas.setTextColor(15);
        _canvas.setTextSize(24);
        if (WiFi.status() == WL_CONNECTED) {
            String ssid = WiFi.SSID();
            _canvas.drawString("WiFi: " + ssid, 56, 228);
            _canvas.drawString("IP:   " + WiFi.localIP().toString(), 56, 262);
        } else {
            _canvas.drawString("WiFi: 未连接", 56, 244);
        }
        char stats[64];
        snprintf(stats, sizeof(stats), "书摘总数: %d 条 · %d 本书", excerptCount, bookCount);
        _canvas.drawString(stats, 56, 304);
        char modeDisplay[64];
        snprintf(modeDisplay, sizeof(modeDisplay), "回顾方式: %s", modeStr);
        _canvas.drawString(modeDisplay, 56, 338);
        char intervalDisplay[64];
        snprintf(intervalDisplay, sizeof(intervalDisplay), "自动切换间隔: %d 分钟", _store.getAutoSwitchMinutes());
        _canvas.drawString(intervalDisplay, 56, 372);

        // Mode card
        _canvas.drawRoundRect(cardX, 426, cardW, 166, 12, 8);
        _canvas.setTextColor(8);
        _canvas.setTextSize(22);
        _canvas.drawString("阅读模式", 56, 448);
        _drawChoiceButton(_powerBtnX, _powerBtnY, _choiceBtnW, _choiceBtnH,
                          "省电模式", _powerSaveMode, true);
        _drawChoiceButton(_interactiveBtnX, _interactiveBtnY, _choiceBtnW, _choiceBtnH,
                          "交互模式", !_powerSaveMode, true);
        _canvas.setTextColor(15);

        // Start actions
        bool canStart = excerptCount > 0;
        _drawChoiceButton(_startPortraitBtnX, _startBtnY, _startBtnW, _startBtnH,
                          "竖屏回顾", false, canStart);
        _drawChoiceButton(_startLandscapeBtnX, _startBtnY, _startBtnW, _startBtnH,
                          "横屏回顾", false, canStart);
        if (!canStart) {
            _canvas.setTextColor(8);
            _canvas.setTextSize(20);
            _canvas.drawString("尚无书摘，请先在 App 中同步数据", 56, 726);
            _canvas.setTextColor(15);
        }

        // Usage hint
        _canvas.setTextColor(8);
        _canvas.setTextSize(20);
        if (WiFi.status() == WL_CONNECTED) {
            _canvas.drawString("在纸间书摘 App 中同步数据到此设备", 40, 804);
        } else {
            _canvas.drawString("请先连接 WiFi", 40, 804);
        }
        _canvas.drawString("长按中键重新设置 WiFi", 40, 836);
        _canvas.drawString("省电回顾中按 RESET 返回首页", 40, 868);
        _canvas.setTextColor(15);

        _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    }

    uint32_t getLastEventId() const { return _lastDiagEventId; }
    bool getStartPortrait() const { return _startPortrait; }
    bool isPowerSaveMode() const { return _powerSaveMode; }

    // Call in loop, returns action
    Action update() {
        M5.update();
        if (_checkAndHandleLowBattery(false)) return ACTION_NONE;

        int excerptCount = _store.getExcerptCount();

        // Check center button long press for WiFi setup
        if (M5.BtnP.isPressed()) {
            if (!_btnPressed) {
                _btnPressed = true;
                _btnPressTime = millis();
                uint32_t eventId = _nextDiagEventId();
                HOME_DIAG_PRINT(eventId, "BTN_CENTER_DOWN excerptCount=%d powerSave=%d",
                                excerptCount, _powerSaveMode ? 1 : 0);
            } else if (millis() - _btnPressTime > 2000) {
                _btnPressed = false;
                uint32_t eventId = _nextDiagEventId();
                HOME_DIAG_PRINT(eventId, "BTN_CENTER_LONG_PRESS duration=%lu -> ACTION_WIFI_SETUP",
                                millis() - _btnPressTime);
                return ACTION_WIFI_SETUP;
            }
        } else {
            if (_btnPressed) {
                unsigned long pressDuration = millis() - _btnPressTime;
                _btnPressed = false;
                uint32_t eventId = _nextDiagEventId();
                HOME_DIAG_PRINT(eventId, "BTN_CENTER_UP duration=%lu excerptCount=%d",
                                pressDuration, excerptCount);
                // Short press = enter reading if excerpts exist
                if (excerptCount > 0) {
                    _startPortrait = true;
                    uint32_t actionEventId = _nextDiagEventId();
                    HOME_DIAG_PRINT(actionEventId,
                                    "ACTION_READ source=button portrait=%d powerSave=%d",
                                    _startPortrait ? 1 : 0, _powerSaveMode ? 1 : 0);
                    return ACTION_READ;
                }
            }
        }

        // Touch handling
        if (M5.TP.available()) {
            uint32_t eventId = _nextDiagEventId();
            HOME_DIAG_PRINT(eventId, "TP_AVAILABLE");
            M5.TP.update();

            if (M5.TP.isFingerUp()) {
                if (_touching) {
                    bool canStart = excerptCount > 0;
                    bool hitPowerMode = _contains(_touchLastX, _touchLastY, _powerBtnX, _powerBtnY, _choiceBtnW, _choiceBtnH);
                    bool hitInteractiveMode = _contains(_touchLastX, _touchLastY, _interactiveBtnX, _interactiveBtnY, _choiceBtnW, _choiceBtnH);
                    bool hitPortraitStart = canStart && _contains(_touchLastX, _touchLastY, _startPortraitBtnX, _startBtnY, _startBtnW, _startBtnH);
                    bool hitLandscapeStart = canStart && _contains(_touchLastX, _touchLastY, _startLandscapeBtnX, _startBtnY, _startBtnW, _startBtnH);

                    uint32_t upEventId = _nextDiagEventId();
                    HOME_DIAG_PRINT(upEventId,
                                    "TP_UP x=%d y=%d hitModeP=%d hitModeI=%d hitPortrait=%d hitLandscape=%d excerptCount=%d",
                                    _touchLastX, _touchLastY,
                                    hitPowerMode ? 1 : 0, hitInteractiveMode ? 1 : 0,
                                    hitPortraitStart ? 1 : 0, hitLandscapeStart ? 1 : 0,
                                    excerptCount);

                    if (hitPowerMode && !_powerSaveMode) {
                        _powerSaveMode = true;
                        uint32_t modeEvt = _nextDiagEventId();
                        HOME_DIAG_PRINT(modeEvt, "MODE_CHANGED powerSave=1");
                        render();
                    } else if (hitInteractiveMode && _powerSaveMode) {
                        _powerSaveMode = false;
                        uint32_t modeEvt = _nextDiagEventId();
                        HOME_DIAG_PRINT(modeEvt, "MODE_CHANGED powerSave=0");
                        render();
                    } else if (hitPortraitStart || hitLandscapeStart) {
                        _startPortrait = hitPortraitStart;
                        uint32_t actionEventId = _nextDiagEventId();
                        HOME_DIAG_PRINT(actionEventId,
                                        "ACTION_READ source=touch portrait=%d powerSave=%d",
                                        _startPortrait ? 1 : 0, _powerSaveMode ? 1 : 0);
                        _touching = false;
                        return ACTION_READ;
                    }
                }
                _touching = false;
            } else {
                uint8_t fingerNum = M5.TP.getFingerNum();
                if (fingerNum > 0) {
                    tp_finger_t finger = M5.TP.readFinger(0);
                    _touchLastX = finger.x;
                    _touchLastY = finger.y;
                    if (!_touching) {
                        _touching = true;
                        uint32_t downEventId = _nextDiagEventId();
                        HOME_DIAG_PRINT(downEventId,
                                        "TP_DOWN x=%d y=%d fingers=%d excerptCount=%d",
                                        _touchLastX, _touchLastY, fingerNum, excerptCount);
                    }
                }
            }
        }

        return ACTION_NONE;
    }

private:
    static const uint16_t LOW_BATTERY_MV = 3400;
    static const uint16_t BATTERY_RECOVER_MV = 3500;
    static const uint32_t BATTERY_CHECK_INTERVAL_MS = 30000;
    static const uint16_t BATTERY_RAW_MIN_MV = 2500;
    static const uint16_t BATTERY_RAW_MAX_MV = 5000;
    static const uint8_t BATTERY_FILTER_ALPHA_PCT = 25; // EMA alpha=0.25
    static const uint8_t BATTERY_PCT_STEP_MAX = 2;      // max +/-2% per update

    struct BatterySocPoint {
        uint16_t mv;
        uint8_t pct;
    };

    M5EPD_Canvas &_canvas;
    DataStore &_store;

    bool _touching;
    uint16_t _touchLastX, _touchLastY;

    // Center button long press
    bool _btnPressed;
    unsigned long _btnPressTime;
    bool _startPortrait;
    bool _powerSaveMode;
    static const int _powerBtnX = 60;
    static const int _interactiveBtnX = 280;
    static const int _powerBtnY = 500;
    static const int _interactiveBtnY = 500;
    static const int _choiceBtnW = 200;
    static const int _choiceBtnH = 64;
    static const int _startPortraitBtnX = 60;
    static const int _startLandscapeBtnX = 280;
    static const int _startBtnY = 644;
    static const int _startBtnW = 200;
    static const int _startBtnH = 76;
    uint32_t _diagEventSeq;
    uint32_t _lastDiagEventId;
    unsigned long _lastBatteryCheckMs;
    uint8_t _lowBatteryHits;
    uint32_t _batteryMvFiltered;
    bool _batteryFilterInited;
    int _lastBatteryPct;
    bool _batteryPctValid;

    uint32_t _nextDiagEventId() {
        _lastDiagEventId = ++_diagEventSeq;
        return _lastDiagEventId;
    }

    bool _contains(int x, int y, int bx, int by, int bw, int bh) const {
        return x >= bx && x <= bx + bw && y >= by && y <= by + bh;
    }

    void _drawChoiceButton(int x, int y, int w, int h, const char *label, bool selected, bool enabled) {
        uint16_t borderColor = enabled ? 8 : 7;
        uint16_t fillColor = selected ? 15 : 0;
        uint16_t textColor = selected ? 0 : (enabled ? 15 : 8);

        if (selected) {
            _canvas.fillRoundRect(x, y, w, h, 12, fillColor);
        } else {
            _canvas.fillRoundRect(x, y, w, h, 12, 0);
        }
        _canvas.drawRoundRect(x, y, w, h, 12, borderColor);
        _canvas.setTextColor(textColor);
        _canvas.setTextSize(26);
        _canvas.setTextDatum(MC_DATUM);
        _canvas.drawString(label, x + w / 2, y + h / 2 + 2);
        _canvas.setTextDatum(TL_DATUM);
        _canvas.setTextColor(15);
    }

    bool _checkAndHandleLowBattery(bool force) {
        unsigned long now = millis();
        if (!force && now - _lastBatteryCheckMs < BATTERY_CHECK_INTERVAL_MS) {
            return false;
        }
        _lastBatteryCheckMs = now;

        uint32_t mv = _readBatteryMvFiltered();
        if (mv == 0) return false;

        if (mv < LOW_BATTERY_MV) {
            if (_lowBatteryHits < 255) _lowBatteryHits++;
        } else if (mv >= BATTERY_RECOVER_MV) {
            _lowBatteryHits = 0;
        }

        if (_lowBatteryHits < 2) return false;

        _canvas.fillCanvas(0);
        _canvas.setTextSize(32);
        _canvas.setTextColor(15);
        _canvas.drawString("电量过低，即将关机...", 80, 450);
        _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        delay(3000);
        M5.shutdown();
        return true;
    }

    uint32_t _readBatteryMvFiltered() {
        uint32_t rawMv = M5.getBatteryVoltage();
        bool validRaw = (rawMv >= BATTERY_RAW_MIN_MV && rawMv <= BATTERY_RAW_MAX_MV);

        if (!validRaw) {
            return _batteryFilterInited ? _batteryMvFiltered : 0;
        }

        if (!_batteryFilterInited) {
            _batteryMvFiltered = rawMv;
            _batteryFilterInited = true;
            return _batteryMvFiltered;
        }

        _batteryMvFiltered =
            (_batteryMvFiltered * (100 - BATTERY_FILTER_ALPHA_PCT) + rawMv * BATTERY_FILTER_ALPHA_PCT) / 100;
        return _batteryMvFiltered;
    }

    int _batteryPercentFromMv(uint32_t mv) const {
        // OCV-like lookup table for 1-cell Li-ion battery (compile-time calibration).
        static const BatterySocPoint table[] = {
            {3400, 0},
            {3500, 5},
            {3600, 10},
            {3650, 15},
            {3700, 20},
            {3740, 30},
            {3780, 40},
            {3830, 50},
            {3900, 60},
            {3950, 70},
            {4020, 80},
            {4100, 90},
            {4200, 100},
        };
        const size_t count = sizeof(table) / sizeof(table[0]);
        if (count == 0) return 0;
        if (mv <= table[0].mv) return table[0].pct;
        if (mv >= table[count - 1].mv) return table[count - 1].pct;

        for (size_t i = 1; i < count; i++) {
            if (mv <= table[i].mv) {
                uint32_t mvLo = table[i - 1].mv;
                uint32_t mvHi = table[i].mv;
                uint32_t pctLo = table[i - 1].pct;
                uint32_t pctHi = table[i].pct;
                uint32_t range = mvHi - mvLo;
                if (range == 0) return (int)pctHi;

                uint32_t offset = mv - mvLo;
                uint32_t pct = pctLo + (offset * (pctHi - pctLo) + range / 2) / range;
                if (pct > 100) pct = 100;
                return (int)pct;
            }
        }
        return table[count - 1].pct;
    }

    int _smoothBatteryPercent(int targetPct) {
        targetPct = constrain(targetPct, 0, 100);
        if (!_batteryPctValid) {
            _lastBatteryPct = targetPct;
            _batteryPctValid = true;
            return targetPct;
        }

        int delta = targetPct - _lastBatteryPct;
        if (delta > BATTERY_PCT_STEP_MAX) delta = BATTERY_PCT_STEP_MAX;
        if (delta < -(int)BATTERY_PCT_STEP_MAX) delta = -(int)BATTERY_PCT_STEP_MAX;
        _lastBatteryPct = constrain(_lastBatteryPct + delta, 0, 100);
        return _lastBatteryPct;
    }
};
