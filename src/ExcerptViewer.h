#pragma once
#include <M5EPD.h>
#include "DataStore.h"
#include "PowerPolicy.h"

extern PowerPolicy powerPolicy;

#ifndef READING_DIAG_LOG
#define READING_DIAG_LOG 1
#endif

#if READING_DIAG_LOG
#define READING_DIAG(fmt, ...) Serial.printf("[READING][%lu] " fmt "\n", millis(), ##__VA_ARGS__)
#else
#define READING_DIAG(fmt, ...) ((void)0)
#endif

class ExcerptViewer {
public:
    ExcerptViewer(M5EPD_Canvas &canvas, DataStore &store)
        : _canvas(canvas), _store(store),
          _currentIndex(0), _subPage(0), _subPageCount(1),
          _touching(false), _touchStartX(0), _touchLastX(0),
          _autoSwitchTarget(0),
          _portrait(true), _screenW(540), _screenH(960),
          _waitFingerUp(false), _powerSave(true),
          _loopCount(0), _loopWrappedLastSwitch(false) {}

    void begin(bool portrait = true, bool powerSave = true) {
        Serial.println("EV_BEGIN: start");
        _currentIndex = 0;
        _subPage = 0;
        _loopCount = 0;
        _loopWrappedLastSwitch = false;
        _touching = false;
        _waitFingerUp = false;
        _powerSave = powerSave;
        _configureOrientation(portrait, true);
        Serial.printf("EV_BEGIN: excerptCount=%d sortRule=%d portrait=%d powerSave=%d\n",
                      _store.getExcerptCount(), _store.getReviewSettings().sortRule,
                      _portrait ? 1 : 0, _powerSave ? 1 : 0);
        if (_store.getReviewSettings().sortRule == 1) {
            Serial.println("EV_BEGIN: buildShuffledOrder");
            _store.buildShuffledOrder();
            Serial.println("EV_BEGIN: buildShuffledOrder done");
        }
        Serial.println("EV_BEGIN: scheduleNextSwitch");
        _scheduleNextSwitch();
        Serial.println("EV_BEGIN: calling _renderCurrent");
        _renderCurrent();
        if (_powerSave) {
            Serial.println("EV_BEGIN: power-save arm shutdown");
            _shutdownForNextPage(true);
            // Returned from shutdown means power-off did not happen (USB power).
            _scheduleNextSwitch();
        }
        Serial.println("EV_BEGIN: done");
    }

    void beginFromState(int index, int subPage, bool portrait = true, bool powerSave = true) {
        Serial.printf("EV_BEGIN_STATE: index=%d subPage=%d portrait=%d powerSave=%d\n",
                      index, subPage, portrait ? 1 : 0, powerSave ? 1 : 0);
        _currentIndex = index;
        _subPage = subPage;
        _loopWrappedLastSwitch = false;
        _touching = false;
        _waitFingerUp = false;
        _powerSave = powerSave;
        _configureOrientation(portrait, true);
        Serial.printf("EV_BEGIN_STATE: excerptCount=%d sortRule=%d\n",
                      _store.getExcerptCount(), _store.getReviewSettings().sortRule);
        if (_store.getReviewSettings().sortRule == 1) {
            Serial.println("EV_BEGIN_STATE: buildShuffledOrder");
            _store.buildShuffledOrder();
            Serial.println("EV_BEGIN_STATE: buildShuffledOrder done");
        }
        Serial.println("EV_BEGIN_STATE: scheduleNextSwitch");
        _scheduleNextSwitch();
        Serial.println("EV_BEGIN_STATE: calling _renderCurrent");
        _renderCurrent();
        if (_powerSave) {
            Serial.println("EV_BEGIN_STATE: power-save arm shutdown");
            _shutdownForNextPage(true);
            // Returned from shutdown means power-off did not happen (USB power).
            _scheduleNextSwitch();
        }
        Serial.println("EV_BEGIN_STATE: done");
    }

    // Returns true when user wants to go back to HOME
    bool update() {
        if (_powerSave) {
            // Extreme power-save mode: no button/touch processing, timer only.
            if ((int32_t)(millis() - _autoSwitchTarget) >= 0) {
                _loopWrappedLastSwitch = false;
                _shutdownForNextPage(true);
                // If shutdown fails (e.g. USB powered), fall back to normal next.
                _next(true);
                _scheduleNextSwitch();
            }
            return false;
        }

        M5.update();

        // Interactive mode auto-switch
        if ((int32_t)(millis() - _autoSwitchTarget) >= 0) {
            _loopWrappedLastSwitch = false;
            _next(true);
            _scheduleNextSwitch();
        }

        // Physical buttons
        if (M5.BtnP.wasPressed()) {
            _store.clearReadingState();
            _restorePortrait();
            return true; // back to HOME
        }
        if (M5.BtnR.wasPressed()) {
            _loopWrappedLastSwitch = false;
            _next();
            _scheduleNextSwitch();
        }
        if (M5.BtnL.wasPressed()) {
            _loopWrappedLastSwitch = false;
            _prev();
            _scheduleNextSwitch();
        }

        // Touch
        if (M5.TP.available()) {
            M5.TP.update();
            if (M5.TP.isFingerUp()) {
                _waitFingerUp = false;
                if (_touching) {
                    _touching = false;
                    int16_t deltaX = (int16_t)_touchLastX - (int16_t)_touchStartX;
                    if (deltaX < -80) {
                        _loopWrappedLastSwitch = false;
                        _next();
                        _scheduleNextSwitch();
                    } else if (deltaX > 80) {
                        _loopWrappedLastSwitch = false;
                        _prev();
                        _scheduleNextSwitch();
                    }
                }
            } else {
                // Two-finger touch → toggle orientation
                if (M5.TP.getFingerNum() >= 2 && !_waitFingerUp) {
                    _waitFingerUp = true;
                    _touching = false;
                    _toggleOrientation();
                    M5.TP.flush();
                    return false;
                }

                // Single-finger swipe
                tp_finger_t finger = M5.TP.readFinger(0);
                _touchLastX = finger.x;
                if (!_touching) {
                    _touching = true;
                    _touchStartX = finger.x;
                }
            }
        }

        return false;
    }

    uint32_t millisUntilAutoSwitch() const {
        int32_t remain = (int32_t)(_autoSwitchTarget - millis());
        return remain > 0 ? (uint32_t)remain : 0;
    }

    bool isLoopWrappedLastSwitch() const { return _loopWrappedLastSwitch; }
    uint32_t getLoopCount() const { return _loopCount; }
    bool isPowerSaveMode() const { return _powerSave; }

private:
    M5EPD_Canvas &_canvas;
    DataStore &_store;

    int _currentIndex;
    int _subPage;
    int _subPageCount;

    // Touch
    bool _touching;
    uint16_t _touchStartX, _touchLastX;

    // Auto-switch timer
    unsigned long _autoSwitchTarget;

    // Orientation
    bool _portrait;                   // true=竖屏(540x960), false=横屏(960x540)
    int _screenW, _screenH;
    bool _waitFingerUp;
    bool _powerSave;
    uint32_t _loopCount;
    bool _loopWrappedLastSwitch;

    static const int MARGIN = 40;          // Top and side margins
    static const int CONTENT_GAP = 15;     // Gap between content bottom and first divider
    static const int BOOK_ZONE_H = 65;     // Book divider to screen bottom (center book name in this zone)
    static const int IDEA_ZONE_H = 80;     // Idea divider to book divider (when idea exists)

    // --- Orientation helpers ---

    void _configureOrientation(bool portrait, bool forceRecreate = false) {
        bool changed = (_portrait != portrait);
        _portrait = portrait;
        _screenW = _portrait ? 540 : 960;
        _screenH = _portrait ? 960 : 540;
        if (!forceRecreate && !changed) {
            return;
        }
        _canvas.deleteCanvas();
        uint16_t rotation = _portrait ? 90 : 0;
        M5.EPD.SetRotation(rotation);
        M5.TP.SetRotation(rotation);
        _canvas.createCanvas(_screenW, _screenH);
        _canvas.createRender(24, 256);
        _canvas.createRender(32, 256);
    }

    void _toggleOrientation() {
        _portrait = !_portrait;
        _configureOrientation(_portrait, true);
        _subPage = 0;
        _renderCurrent();
    }

    void _restorePortrait() {
        if (!_portrait) {
            _portrait = true;
            _canvas.deleteCanvas();
            M5.EPD.SetRotation(90);
            M5.TP.SetRotation(90);
            _screenW = 540;
            _screenH = 960;
            _canvas.createCanvas(_screenW, _screenH);
            _canvas.createRender(24, 256);
            _canvas.createRender(32, 256);
        }
    }

    // --- Text width estimation ---

    int _estimateTextWidth(const char *str, int fontSize) {
        int width = 0;
        while (*str) {
            if ((*str & 0x80) == 0) {          // ASCII: ~half fontSize
                width += fontSize / 2;
                str += 1;
            } else if ((*str & 0xE0) == 0xC0) { // 2-byte UTF-8
                width += fontSize;
                str += (str[1] ? 2 : 1);
            } else if ((*str & 0xF0) == 0xE0) { // 3-byte UTF-8 (中文)
                width += fontSize;
                str += (str[1] && str[2] ? 3 : 1);
            } else {                             // 4-byte UTF-8
                width += fontSize;
                str += (str[1] && str[2] && str[3] ? 4 : 1);
            }
        }
        return width;
    }

    // --- Navigation ---

    void _next(bool wrapAtEnd = false) {
        int nextIndex = _currentIndex;
        int nextSubPage = _subPage;
        _computeNextPosition(wrapAtEnd, nextIndex, nextSubPage);

        if (nextIndex == _currentIndex && nextSubPage == _subPage) {
            return;
        }

        bool wrapped = wrapAtEnd &&
                       (_store.getExcerptCount() > 0) &&
                       (_currentIndex == _store.getExcerptCount() - 1) &&
                       (_subPage + 1 >= _subPageCount) &&
                       (nextIndex == 0 && nextSubPage == 0);

        _currentIndex = nextIndex;
        _subPage = nextSubPage;
        if (wrapped) {
            _loopCount++;
            _loopWrappedLastSwitch = true;
        }
        _renderCurrent();
    }

    void _prev() {
        // If on sub-page > 0, go back
        if (_subPage > 0) {
            _subPage--;
            _renderCurrent();
            return;
        }
        // Go to previous excerpt
        if (_currentIndex > 0) {
            _currentIndex--;
            _subPage = 0;
            _renderCurrent();
        }
    }

    int _resolveIndex(int position) const {
        if (_store.getReviewSettings().sortRule == 1) {
            return _store.getShuffledIndex(position);
        }
        return position;
    }

    void _scheduleNextSwitch() {
        uint32_t intervalMs = (uint32_t)_store.getAutoSwitchMinutes() * 60UL * 1000UL;
        _autoSwitchTarget = millis() + intervalMs;
    }

    void _computeNextPosition(bool wrapAtEnd, int &nextIndex, int &nextSubPage) const {
        nextIndex = _currentIndex;
        nextSubPage = _subPage;

        int excerptCount = _store.getExcerptCount();
        if (excerptCount <= 0) {
            nextIndex = 0;
            nextSubPage = 0;
            return;
        }

        if (_subPage + 1 < _subPageCount) {
            nextSubPage = _subPage + 1;
            return;
        }

        if (_currentIndex + 1 < excerptCount) {
            nextIndex = _currentIndex + 1;
            nextSubPage = 0;
            return;
        }

        if (wrapAtEnd) {
            nextIndex = 0;
            nextSubPage = 0;
        }
    }

    void _shutdownForNextPage(bool wrapAtEnd) {
        unsigned long settleStartMs = millis();
        READING_DIAG("SETTLE start before shutdown");
        m5epd_err_t settleRet = M5.EPD.CheckAFSR();
        unsigned long settleElapsedMs = millis() - settleStartMs;
        if (settleRet != M5EPD_OK) {
            READING_DIAG("SETTLE timeout ret=%d elapsed=%lums -> skip shutdown",
                         (int)settleRet, settleElapsedMs);
            _store.clearReadingState();
            return;
        }
        READING_DIAG("SETTLE done elapsed=%lums", settleElapsedMs);

        int nextIndex = _currentIndex;
        int nextSubPage = _subPage;
        _computeNextPosition(wrapAtEnd, nextIndex, nextSubPage);

        if (!_store.saveReadingState(nextIndex, nextSubPage, _portrait, _powerSave)) {
            Serial.println("READING: save state failed, skip shutdown");
            return;
        }
        uint32_t intervalSec = (uint32_t)_store.getAutoSwitchMinutes() * 60UL;
        READING_DIAG("SHUTDOWN arm nextIndex=%d nextSubPage=%d intervalSec=%lu",
                     nextIndex, nextSubPage, (unsigned long)intervalSec);

        M5.shutdown(intervalSec);
        // Returned from shutdown means power-off did not happen (e.g. USB powered).
        // Clear stale state to avoid false "RTC wake" fast-path on next cold boot.
        READING_DIAG("SHUTDOWN returned, clear saved state");
        _store.clearReadingState();
    }

    // --- Rendering ---

    void _renderCurrent() {
        Serial.println("EV_RENDER: start");
        powerPolicy.boostForRender();

        int actualIndex = _resolveIndex(_currentIndex);
        Serial.printf("EV_RENDER: currentIndex=%d actualIndex=%d\n", _currentIndex, actualIndex);
        const ExcerptInfo *exc = _store.getExcerpt(actualIndex);
        if (!exc) {
            Serial.println("EV_RENDER: excerpt is NULL, abort");
            powerPolicy.idleAfterRender();
            return;
        }
        Serial.printf("EV_RENDER: exc->content=%s bookId=%d\n",
                      exc->content ? "non-null" : "NULL", exc->bookId);
        if (!exc->content) {
            Serial.println("EV_RENDER: content is NULL, showing error");
            _canvas.fillCanvas(0);
            _canvas.setTextColor(15);
            _canvas.setTextSize(28);
            _canvas.drawString("内容加载失败", 160, _screenH / 2 - 30);
            _canvas.setTextSize(24);
            _canvas.setTextColor(8);
            _canvas.drawString("请稍后重试", 190, _screenH / 2 + 20);
            _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            powerPolicy.idleAfterRender();
            return;
        }

        const BookInfo *book = _store.findBook(exc->bookId);
        Serial.printf("EV_RENDER: book=%s\n", book ? "found" : "NULL");

        // Bottom-up layout: fixed zones from screen bottom
        //   [screen bottom] ← BOOK_ZONE_H → [book divider] ← IDEA_ZONE_H → [idea divider] ← CONTENT_GAP → [content]
        bool hasIdea = (exc->idea && strlen(exc->idea) > 0);
        int bottomZoneH = BOOK_ZONE_H + (hasIdea ? IDEA_ZONE_H : 0);
        int contentH = _screenH - MARGIN - CONTENT_GAP - bottomZoneH;

        // Determine sub-page count
        Serial.printf("EV_RENDER: calling _calcSubPages contentH=%d\n", contentH);
        _subPageCount = _calcSubPages(exc->content, contentH);
        Serial.printf("EV_RENDER: subPageCount=%d subPage=%d\n", _subPageCount, _subPage);

        _canvas.fillCanvas(0);

        // Render excerpt text for current sub-page
        int contentW = _screenW - 2 * MARGIN;

        _canvas.setTextArea(MARGIN, MARGIN, contentW, contentH);
        _canvas.setTextWrap(true, false);
        _canvas.setTextColor(15);
        _canvas.setTextSize(32);
        _canvas.setTextLineSpace(16);
        _canvas.setCursor(MARGIN, MARGIN);

        // Find offset for current sub-page
        size_t offset = _getSubPageOffset(exc->content, contentH, _subPage);
        _canvas.print(exc->content + offset);

        // --- Bottom area (fixed positions, calculated bottom-up) ---
        int bookDividerY = _screenH - BOOK_ZONE_H;

        // Idea section (between idea divider and book divider)
        if (hasIdea && _subPage == _subPageCount - 1) {
            int ideaDividerY = bookDividerY - IDEA_ZONE_H;
            _canvas.fillRect(MARGIN, ideaDividerY, contentW, 1, 8);
            int ideaTextY = ideaDividerY + 15;
            int ideaTextH = IDEA_ZONE_H - 15 - 5;
            _canvas.setTextColor(15);
            _canvas.setTextSize(24);
            _canvas.setTextArea(MARGIN, ideaTextY, contentW, ideaTextH);
            _canvas.setCursor(MARGIN, ideaTextY);
            _canvas.print(exc->idea);
        }

        // Book divider + book name (always at the same position)
        _canvas.fillRect(MARGIN, bookDividerY, contentW, 1, 8);

        // Vertically center book name between divider and screen bottom
        int belowDivider = BOOK_ZONE_H - 1;
        int bookNameY = bookDividerY + 1 + (belowDivider - 24) / 2;

        _canvas.setTextColor(15);
        _canvas.setTextSize(24);
        _canvas.setTextArea(0, 0, _screenW, _screenH);
        if (book) {
            char bookLine[256];
            snprintf(bookLine, sizeof(bookLine), "\xE3\x80\x8A%s\xE3\x80\x8B", book->name);
            int strW = _estimateTextWidth(bookLine, 24);
            int bx = (_screenW - strW) / 2;
            if (bx < MARGIN) bx = MARGIN;
            _canvas.drawString(bookLine, bx, bookNameY);
        }
        _canvas.setTextColor(15);
        Serial.println("EV_RENDER: pushing canvas");
        _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        Serial.println("EV_RENDER: done");
        powerPolicy.idleAfterRender();
    }

    // Calculate how many sub-pages a text needs given available height
    int _calcSubPages(const char *text, int contentH) {
        if (!text || strlen(text) == 0) return 1;

        int contentW = _screenW - 2 * MARGIN;
        size_t textLen = strlen(text);
        size_t offset = 0;
        int pages = 0;

        Serial.printf("EV_CALC: textLen=%d contentW=%d contentH=%d\n", (int)textLen, contentW, contentH);

        while (offset < textLen) {
            _canvas.fillCanvas(0);
            _canvas.setTextArea(MARGIN, MARGIN, contentW, contentH);
            _canvas.setTextWrap(true, false);
            _canvas.setTextSize(32);
            _canvas.setTextLineSpace(16);
            _canvas.setTextColor(15);
            _canvas.setCursor(MARGIN, MARGIN);
            _canvas.print(text + offset);

            uint32_t exceed = _canvas.getExceedOffset();
            pages++;
            Serial.printf("EV_CALC: page=%d offset=%d exceed=%d\n", pages, (int)offset, (int)exceed);
            if (exceed == 0) break; // all text fits
            offset += exceed;
        }

        Serial.printf("EV_CALC: total pages=%d\n", pages);
        return pages > 0 ? pages : 1;
    }

    // Get byte offset for a given sub-page
    size_t _getSubPageOffset(const char *text, int contentH, int targetPage) {
        if (targetPage == 0) return 0;

        int contentW = _screenW - 2 * MARGIN;
        size_t textLen = strlen(text);
        size_t offset = 0;

        for (int p = 0; p < targetPage && offset < textLen; p++) {
            _canvas.fillCanvas(0);
            _canvas.setTextArea(MARGIN, MARGIN, contentW, contentH);
            _canvas.setTextWrap(true, false);
            _canvas.setTextSize(32);
            _canvas.setTextLineSpace(16);
            _canvas.setTextColor(15);
            _canvas.setCursor(MARGIN, MARGIN);
            _canvas.print(text + offset);

            uint32_t exceed = _canvas.getExceedOffset();
            if (exceed == 0) break;
            offset += exceed;
        }

        return offset;
    }
};
