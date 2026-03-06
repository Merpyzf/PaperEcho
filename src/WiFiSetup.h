#pragma once
#include <M5EPD.h>
#include <WiFi.h>
#include "DataStore.h"

class WiFiSetup {
public:
    enum Result { RESULT_NONE, RESULT_CONNECTED, RESULT_BACK };

    WiFiSetup(M5EPD_Canvas &canvas, DataStore &store)
        : _canvas(canvas), _store(store), _passwordCanvas(&M5.EPD),
          _state(STATE_SCAN), _selectedNet(-1), _networkCount(0),
          _passwordLen(0), _shiftOn(false),
          _needFullRedraw(true), _prevPasswordLen(0),
          _touching(false), _passwordCanvasReady(false),
          _cursorIdx(0), _listOffset(0) {
        _password[0] = '\0';
    }

    ~WiFiSetup() {
        _releasePasswordCanvas();
    }

    // Call once to start WiFi setup flow
    void begin() {
        _state = STATE_SCAN;
        _needFullRedraw = true;
        _cursorIdx = 0;
        _listOffset = 0;
        _scan();
        _drawNetworkList();
    }

    // Call in loop, returns RESULT_CONNECTED when done
    Result update() {
        M5.update();

        // Button handling
        if (_state == STATE_LIST) {
            if (M5.BtnR.wasPressed()) {
                int totalItems = _networkCount + 1; // networks + rescan
                if (_cursorIdx < totalItems - 1) {
                    _cursorIdx++;
                    // Scroll down if cursor goes below visible area
                    if (_cursorIdx >= _listOffset + MAX_VISIBLE) {
                        _listOffset = _cursorIdx - MAX_VISIBLE + 1;
                    }
                    _drawNetworkList();
                }
                return RESULT_NONE;
            }
            if (M5.BtnL.wasPressed()) {
                if (_cursorIdx > 0) {
                    _cursorIdx--;
                    // Scroll up if cursor goes above visible area
                    if (_cursorIdx < _listOffset) {
                        _listOffset = _cursorIdx;
                    }
                    _drawNetworkList();
                }
                return RESULT_NONE;
            }
            if (M5.BtnP.wasPressed()) {
                return _selectCurrent();
            }
        } else if (_state == STATE_PASSWORD) {
            if (M5.BtnP.wasPressed()) {
                return _tryConnect(_password, _passwordLen);
            }
            if (M5.BtnL.wasPressed()) {
                _state = STATE_LIST;
                _drawNetworkList();
                return RESULT_NONE;
            }
        }

        if (M5.TP.available()) {
            M5.TP.update();

            if (M5.TP.isFingerUp()) {
                if (_touching) {
                    Result r = _handleTap(_touchLastX, _touchLastY);
                    if (r != RESULT_NONE) return r;
                }
                _touching = false;
            } else if (M5.TP.getFingerNum() > 0) {
                tp_finger_t finger = M5.TP.readFinger(0);
                _touchLastX = finger.x;
                _touchLastY = finger.y;
                if (!_touching) {
                    _touching = true;
                }
            }
        }

        return RESULT_NONE;
    }

private:
    enum State { STATE_SCAN, STATE_LIST, STATE_PASSWORD, STATE_CONNECTING };

    M5EPD_Canvas &_canvas;
    DataStore &_store;
    M5EPD_Canvas _passwordCanvas;
    State _state;

    // Network list
    struct NetInfo {
        char ssid[33];
        int32_t rssi;
        bool encrypted;
    };
    NetInfo _networks[10];
    int _networkCount;
    int _selectedNet;

    // Cursor navigation
    int _cursorIdx;    // 0 ~ _networkCount (last = rescan)
    int _listOffset;   // scroll offset
    static const int MAX_VISIBLE = 8;

    // Password input
    char _password[65];
    int _passwordLen;
    bool _shiftOn;

    // Drawing state
    bool _needFullRedraw;
    int _prevPasswordLen;

    // Touch
    bool _touching;
    uint16_t _touchLastX, _touchLastY;
    bool _passwordCanvasReady;

    // Keyboard layout constants
    static const int KB_X = 20;
    static const int KB_WIDTH = 500;
    static const int KB_ROWS = 5;
    static const int KB_BOTTOM_MARGIN = 20;
    static const int SCREEN_H = 960;
    static const int KEY_W = 48;
    static const int KEY_H = 56;
    static const int KEY_GAP = 2;
    static const int ROW_GAP = 4;

    // SSID max display chars (to prevent overflow)
    static const int SSID_MAX_DISPLAY = 24;

    bool _ensurePasswordCanvas() {
        if (_passwordCanvasReady) return true;
        _passwordCanvas.createCanvas(460, 40);
        _passwordCanvasReady = true;
        return true;
    }

    void _ensureTextRenders() {
        // Freetype render cache is global in M5EPD/TFT_eSPI. Never load font on
        // temp canvases; only ensure required sizes exist here.
        if (!_canvas.isRenderExist(24)) {
            Serial.println("WIFI_UI: create render 24");
            _canvas.createRender(24, 256);
        }
        if (!_canvas.isRenderExist(26)) {
            Serial.println("WIFI_UI: create render 26");
            _canvas.createRender(26, 256);
        }
    }

    int _keyboardTopY() const {
        int keyboardH = KB_ROWS * KEY_H + (KB_ROWS - 1) * ROW_GAP;
        return SCREEN_H - keyboardH - KB_BOTTOM_MARGIN;
    }

    bool _resolveRowKeyIndex(uint16_t tx, int rowX, int keyCount, int &idx) const {
        int rowSpan = keyCount * (KEY_W + KEY_GAP) - KEY_GAP;
        if ((int)tx < rowX || (int)tx >= rowX + rowSpan) return false;

        int local = (int)tx - rowX;
        int step = KEY_W + KEY_GAP;
        int slot = local / step;
        int offset = local % step;
        if (slot < 0 || slot >= keyCount || offset >= KEY_W) return false;

        idx = slot;
        return true;
    }

    void _releasePasswordCanvas() {
        if (_passwordCanvasReady) {
            _passwordCanvas.deleteCanvas();
            _passwordCanvasReady = false;
        }
    }

    void _scan() {
        _state = STATE_SCAN;
        _canvas.fillCanvas(0);
        _canvas.setTextSize(32);
        _canvas.setTextColor(15);
        _canvas.drawString("正在扫描 WiFi...", 40, 200);
        _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);
        int n = WiFi.scanNetworks();

        _networkCount = 0;
        for (int i = 0; i < n && _networkCount < 10; i++) {
            // Skip duplicates
            bool dup = false;
            for (int j = 0; j < _networkCount; j++) {
                if (strcmp(_networks[j].ssid, WiFi.SSID(i).c_str()) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            strlcpy(_networks[_networkCount].ssid, WiFi.SSID(i).c_str(), 33);
            _networks[_networkCount].rssi = WiFi.RSSI(i);
            _networks[_networkCount].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            _networkCount++;
        }

        WiFi.scanDelete();
        _state = STATE_LIST;
        _cursorIdx = 0;
        _listOffset = 0;
    }

    // Truncate SSID for display, adding ".." if too long
    void _getDisplaySsid(const char *ssid, char *out, int maxChars) {
        int len = strlen(ssid);
        if (len <= maxChars) {
            strcpy(out, ssid);
        } else {
            strncpy(out, ssid, maxChars - 2);
            out[maxChars - 2] = '.';
            out[maxChars - 1] = '.';
            out[maxChars] = '\0';
        }
    }

    void _drawNetworkList() {
        _releasePasswordCanvas();
        _canvas.fillCanvas(0);
        _canvas.setTextDatum(TL_DATUM);
        _canvas.setTextColor(15);

        // Back button
        _canvas.fillRoundRect(16, 14, 64, 52, 10, 15);
        _canvas.drawLine(42, 26, 30, 40, 2, 0);
        _canvas.drawLine(30, 40, 42, 54, 2, 0);

        // Title
        _canvas.setTextSize(32);
        _canvas.drawString("选择 WiFi", 92, 30);

        // Divider
        _canvas.fillRect(40, 80, 460, 2, 8);

        int totalItems = _networkCount + 1; // networks + rescan
        int y = 110;

        for (int vi = 0; vi < MAX_VISIBLE && (_listOffset + vi) < totalItems; vi++) {
            int itemIdx = _listOffset + vi;
            bool highlighted = (itemIdx == _cursorIdx);

            if (itemIdx < _networkCount) {
                // Network item
                if (highlighted) {
                    _canvas.fillRoundRect(40, y, 460, 70, 8, 15);
                    _canvas.setTextColor(0);
                } else {
                    _canvas.drawRoundRect(40, y, 460, 70, 8, 10);
                    _canvas.fillRect(40, y + 8, 4, 54, 15); // left accent
                    _canvas.setTextColor(15);
                }

                // SSID with overflow protection
                char displaySsid[33];
                _getDisplaySsid(_networks[itemIdx].ssid, displaySsid, SSID_MAX_DISPLAY);
                _canvas.setTextSize(26);
                _canvas.drawString(displaySsid, 60, y + 20);

                _canvas.drawString(">", 472, y + 22);

                _canvas.setTextColor(15); // reset
            } else {
                // "Rescan" item (last item)
                if (highlighted) {
                    _canvas.fillRoundRect(40, y, 460, 70, 8, 15);
                    _canvas.setTextColor(0);
                } else {
                    _canvas.drawRoundRect(40, y, 460, 70, 8, 15);
                    _canvas.setTextColor(15);
                }
                _canvas.setTextSize(26);
                _canvas.drawString("重新扫描", 190, y + 20);
                _canvas.setTextColor(15); // reset
            }

            y += 80;
        }

        // Scroll indicators
        if (totalItems > MAX_VISIBLE) {
            _canvas.setTextSize(22);
            _canvas.setTextColor(8); // gray
            if (_listOffset > 0 && _listOffset + MAX_VISIBLE < totalItems) {
                _canvas.drawString("▲ ▼", 230, 760);
            } else if (_listOffset > 0) {
                _canvas.drawString("▲", 245, 760);
            } else if (_listOffset + MAX_VISIBLE < totalItems) {
                _canvas.drawString("▼", 245, 760);
            }
            _canvas.setTextColor(15);
        }

        _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    }

    // Unified select action for current cursor item
    Result _selectCurrent() {
        if (_cursorIdx < _networkCount) {
            _selectedNet = _cursorIdx;
            if (!_networks[_cursorIdx].encrypted) {
                return _tryConnect("", 0);
            }
            _state = STATE_PASSWORD;
            _passwordLen = 0;
            _password[0] = '\0';
            _shiftOn = false;
            _needFullRedraw = true;
            _drawPasswordPage();
            return RESULT_NONE;
        } else {
            // Rescan
            _scan();
            _drawNetworkList();
            return RESULT_NONE;
        }
    }

    void _drawPasswordPage() {
        _ensurePasswordCanvas();
        _ensureTextRenders();
        _canvas.fillCanvas(0);
        _canvas.setTextDatum(TL_DATUM);
        _canvas.setTextColor(15);

        // Back button
        _canvas.fillRoundRect(16, 14, 64, 52, 10, 15);
        _canvas.drawLine(42, 26, 30, 40, 2, 0);
        _canvas.drawLine(30, 40, 42, 54, 2, 0);

        // Network name
        _canvas.setTextSize(26);
        _canvas.drawString("WiFi: ", 80, 30);
        char displaySsid[33];
        _getDisplaySsid(_networks[_selectedNet].ssid, displaySsid, SSID_MAX_DISPLAY);
        _canvas.drawString(displaySsid, 190, 30);

        // Password input border
        _canvas.drawRoundRect(35, 65, 470, 46, 8, 15);

        // Password field
        _drawPasswordField();

        // Divider
        _canvas.fillRect(40, 120, 460, 2, 8);

        // Draw keyboard
        _drawKeyboard();

        _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        _needFullRedraw = false;
    }

    void _drawPasswordField() {
        // Clear password area
        _canvas.fillRect(40, 70, 460, 40, 0);

        _canvas.setTextSize(26);
        _canvas.setTextColor(15);

        _canvas.drawString(_password, 40, 75);
        // Cursor
        int cursorX = 40 + _passwordLen * 14;
        if (cursorX < 490) {
            _canvas.fillRect(cursorX, 75, 2, 30, 15);
        }
    }

    void _refreshPasswordField() {
        // Reuse one small canvas to avoid repeated font load and allocations.
        _ensurePasswordCanvas();
        _passwordCanvas.fillCanvas(0);
        _passwordCanvas.setTextDatum(TL_DATUM);
        _passwordCanvas.setTextSize(26);
        _passwordCanvas.setTextColor(15);

        _passwordCanvas.drawString(_password, 0, 5);

        int cursorX = _passwordLen * 14;
        if (cursorX < 450) {
            _passwordCanvas.fillRect(cursorX, 5, 2, 30, 15);
        }
        _passwordCanvas.pushCanvas(40, 70, UPDATE_MODE_GC16);
        _prevPasswordLen = _passwordLen;
    }

    void _drawKeyboard() {
        const char *row1 = _shiftOn ? "QWERTYUIOP" : "qwertyuiop";
        const char *row2 = _shiftOn ? "ASDFGHJKL" : "asdfghjkl";
        const char *row3 = _shiftOn ? "ZXCVBNM" : "zxcvbnm";

        int y = _keyboardTopY();

        // Row 0: numbers (always visible)
        _drawKeyRow("1234567890", 10, KB_X, y, KEY_W, KEY_H);
        y += KEY_H + ROW_GAP;

        // Row 1: 10 letter keys
        _drawKeyRow(row1, 10, KB_X, y, KEY_W, KEY_H);
        y += KEY_H + ROW_GAP;

        // Row 2: 9 keys, centered
        int row2X = KB_X + (KB_WIDTH - 9 * (KEY_W + KEY_GAP)) / 2;
        _drawKeyRow(row2, 9, row2X, y, KEY_W, KEY_H);
        y += KEY_H + ROW_GAP;

        // Row 3: Shift + 7 keys + Backspace
        if (_shiftOn) {
            _canvas.fillRoundRect(KB_X, y, 62, KEY_H, 6, 15);
            _canvas.setTextSize(24);
            _canvas.setTextColor(0);
            _canvas.drawString("SH", KB_X + 12, y + 16);
            _canvas.setTextColor(15);
        } else {
            _canvas.drawRoundRect(KB_X, y, 62, KEY_H, 6, 15);
            _canvas.setTextSize(24);
            _canvas.drawString("sh", KB_X + 12, y + 16);
        }

        int r3X = KB_X + 62 + KEY_GAP;
        _drawKeyRow(row3, 7, r3X, y, KEY_W, KEY_H);

        // Backspace key (filled - functional key style)
        int bkX = KB_X + KB_WIDTH - 62;
        _canvas.fillRoundRect(bkX, y, 62, KEY_H, 6, 15);
        _canvas.setTextSize(24);
        _canvas.setTextColor(0);
        _canvas.drawString("<-", bkX + 10, y + 16);
        _canvas.setTextColor(15);

        y += KEY_H + ROW_GAP;

        // Row 4: Space + OK
        int spaceW = KB_WIDTH - 100 - KEY_GAP;
        _canvas.drawRoundRect(KB_X, y, spaceW, KEY_H, 6, 15);

        // Connect button
        int connX = KB_X + KB_WIDTH - 100;
        _canvas.fillRoundRect(connX, y, 100, KEY_H, 6, 15);
        _canvas.setTextSize(24);
        _canvas.setTextColor(0);
        _canvas.drawString("连接", connX + 25, y + 16);
        _canvas.setTextColor(15);
    }

    void _drawKeyRow(const char *keys, int count, int x, int y, int w, int h) {
        char label[2] = {0, 0};
        for (int i = 0; i < count; i++) {
            int kx = x + i * (w + KEY_GAP);
            _canvas.drawRoundRect(kx, y, w, h, 6, 15);
            label[0] = keys[i];
            _canvas.setTextSize(24);
            _canvas.drawString(label, kx + 14, y + 16);
        }
    }

    Result _handleTap(uint16_t tx, uint16_t ty) {
        if (_state == STATE_LIST) {
            // Back button
            if (tx >= 0 && tx <= 90 && ty >= 0 && ty <= 76) {
                return RESULT_BACK;
            }

            // Check visible list items
            int y = 110;
            int totalItems = _networkCount + 1;
            for (int vi = 0; vi < MAX_VISIBLE && (_listOffset + vi) < totalItems; vi++) {
                if (ty >= y && ty < y + 70 && tx >= 40 && tx <= 500) {
                    _cursorIdx = _listOffset + vi;
                    return _selectCurrent();
                }
                y += 80;
            }
            return RESULT_NONE;
        }

        if (_state == STATE_PASSWORD) {
            // Back button - return to network list
            if (tx >= 0 && tx <= 90 && ty >= 0 && ty <= 76) {
                _state = STATE_LIST;
                _drawNetworkList();
                return RESULT_NONE;
            }
            return _handleKeyboardTap(tx, ty);
        }

        return RESULT_NONE;
    }

    Result _handleKeyboardTap(uint16_t tx, uint16_t ty) {
        int y = _keyboardTopY();

        // Row 0: numbers
        if (ty >= y && ty < y + KEY_H) {
            int idx = -1;
            if (_resolveRowKeyIndex(tx, KB_X, 10, idx)) {
                _appendChar("1234567890"[idx]);
                _refreshPasswordField();
            }
            return RESULT_NONE;
        }

        y += KEY_H + ROW_GAP;

        // Row 1: qwerty
        if (ty >= y && ty < y + KEY_H) {
            const char *row1 = _shiftOn ? "QWERTYUIOP" : "qwertyuiop";
            int idx = -1;
            if (_resolveRowKeyIndex(tx, KB_X, 10, idx)) {
                _appendChar(row1[idx]);
                if (_shiftOn) _shiftOn = false;
                _refreshPasswordField();
            }
            return RESULT_NONE;
        }

        y += KEY_H + ROW_GAP;

        // Row 2: asdf
        if (ty >= y && ty < y + KEY_H) {
            const char *row2 = _shiftOn ? "ASDFGHJKL" : "asdfghjkl";
            int row2X = KB_X + (KB_WIDTH - 9 * (KEY_W + KEY_GAP)) / 2;
            int idx = -1;
            if (_resolveRowKeyIndex(tx, row2X, 9, idx)) {
                _appendChar(row2[idx]);
                if (_shiftOn) _shiftOn = false;
                _refreshPasswordField();
            }
            return RESULT_NONE;
        }

        y += KEY_H + ROW_GAP;

        // Row 3: shift + letters + backspace
        if (ty >= y && ty < y + KEY_H) {
            // Shift key
            if (tx >= KB_X && tx < KB_X + 62) {
                _shiftOn = !_shiftOn;
                _needFullRedraw = true;
                _drawPasswordPage();
                return RESULT_NONE;
            }
            // Backspace key
            int bkX = KB_X + KB_WIDTH - 62;
            if (tx >= bkX && tx < bkX + 62) {
                if (_passwordLen > 0) {
                    _passwordLen--;
                    _password[_passwordLen] = '\0';
                    _refreshPasswordField();
                }
                return RESULT_NONE;
            }
            // Letter keys
            const char *row3 = _shiftOn ? "ZXCVBNM" : "zxcvbnm";
            int r3X = KB_X + 62 + KEY_GAP;
            int idx = -1;
            if (_resolveRowKeyIndex(tx, r3X, 7, idx)) {
                _appendChar(row3[idx]);
                if (_shiftOn) _shiftOn = false;
                _refreshPasswordField();
            }
            return RESULT_NONE;
        }

        y += KEY_H + ROW_GAP;

        // Row 4: space + OK
        if (ty >= y && ty < y + KEY_H) {
            // Connect button
            int connX = KB_X + KB_WIDTH - 100;
            if (tx >= connX && tx < connX + 100) {
                return _tryConnect(_password, _passwordLen);
            }
            // Space bar
            int spaceW = KB_WIDTH - 100 - KEY_GAP;
            if (tx >= KB_X && tx < KB_X + spaceW) {
                _appendChar(' ');
                _refreshPasswordField();
            }
            return RESULT_NONE;
        }

        return RESULT_NONE;
    }

    void _appendChar(char c) {
        if (_passwordLen < 64) {
            _password[_passwordLen++] = c;
            _password[_passwordLen] = '\0';
        }
    }

    Result _tryConnect(const char *password, int passLen) {
        _state = STATE_CONNECTING;

        // Show connecting message
        _canvas.fillCanvas(0);
        _canvas.setTextSize(32);
        _canvas.setTextColor(15);
        _canvas.drawString("连接中...", 40, 200);
        _canvas.setTextSize(26);
        _canvas.drawString(_networks[_selectedNet].ssid, 40, 260);
        _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

        WiFi.begin(_networks[_selectedNet].ssid, passLen > 0 ? password : nullptr);

        // Wait up to 15 seconds
        int timeout = 75; // 75 * 200ms = 15s
        while (WiFi.status() != WL_CONNECTED && timeout > 0) {
            M5.update();
            delay(200);
            timeout--;
        }

        if (WiFi.status() == WL_CONNECTED) {
            // Save config
            _store.saveWiFiConfig(_networks[_selectedNet].ssid, password);
            return RESULT_CONNECTED;
        }

        // Failed
        WiFi.disconnect();
        _canvas.fillCanvas(0);
        _canvas.setTextSize(26);
        _canvas.setTextColor(15);
        _canvas.drawString("连接失败", 40, 200);
        _canvas.drawString("请检查密码后重试", 40, 250);

        _canvas.drawRoundRect(180, 400, 180, 50, 8, 15);
        _canvas.drawString("返回", 240, 415);
        _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

        // Wait for tap to go back
        _waitForTap();

        _state = STATE_PASSWORD;
        _needFullRedraw = true;
        _drawPasswordPage();
        return RESULT_NONE;
    }

    void _waitForTap() {
        while (true) {
            M5.update();
            if (M5.TP.available() && M5.TP.isFingerUp()) {
                return;
            }
            if (M5.BtnP.wasPressed() || M5.BtnL.wasPressed() || M5.BtnR.wasPressed()) {
                return;
            }
            delay(50);
        }
    }
};
