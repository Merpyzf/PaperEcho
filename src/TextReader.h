#pragma once
#include <M5EPD.h>

class TextReader {
public:
    TextReader(M5EPD_Canvas &canvas, uint8_t fontSize = 32, uint8_t margin = 30)
        : _canvas(canvas), _fontSize(fontSize), _margin(margin),
          _lineSpace(fontSize / 2),
          _portrait(true), _text(nullptr), _textLen(0),
          _totalPages(0), _currentPage(0),
          _screenW(540), _screenH(960),
          _touching(false), _touchStartX(0), _touchStartY(0),
          _touchLastX(0), _touchLastY(0) {}

    void loadText(const char *text, size_t len) {
        _text = text;
        _textLen = len;
        _buildPages();
    }

    void setPortrait(bool portrait) {
        if (_portrait == portrait) return;
        _portrait = portrait;
        _applyOrientation();
    }

    void toggleOrientation() {
        _portrait = !_portrait;
        _applyOrientation();
    }

    void renderCurrentPage() {
        _renderPage(_currentPage);
    }

    bool nextPage() {
        if (_currentPage + 1 >= _totalPages) return false;
        _currentPage++;
        _renderPage(_currentPage);
        return true;
    }

    bool prevPage() {
        if (_currentPage <= 0) return false;
        _currentPage--;
        _renderPage(_currentPage);
        return true;
    }

    void handleTouch() {
        if (!M5.TP.available()) return;

        if (M5.TP.isFingerUp()) {
            if (_touching) {
                _touching = false;
                int16_t deltaX = (int16_t)_touchLastX - (int16_t)_touchStartX;
                if (deltaX < -SWIPE_THRESHOLD) {
                    nextPage();
                } else if (deltaX > SWIPE_THRESHOLD) {
                    prevPage();
                }
            }
            return;
        }

        M5.TP.update();
        tp_finger_t finger = M5.TP.readFinger(0);
        _touchLastX = finger.x;
        _touchLastY = finger.y;

        if (!_touching) {
            _touching = true;
            _touchStartX = finger.x;
            _touchStartY = finger.y;
        }
    }

    int getCurrentPage() { return _currentPage + 1; }
    int getTotalPages()  { return _totalPages; }

private:
    M5EPD_Canvas &_canvas;
    uint8_t _fontSize;
    uint8_t _margin;
    uint8_t _lineSpace;
    bool _portrait;

    const char *_text;
    size_t _textLen;

    static const int MAX_PAGES = 500;
    size_t _pageOffsets[MAX_PAGES];
    int _totalPages;
    int _currentPage;

    int _screenW, _screenH;

    // Touch gesture state
    bool _touching;
    uint16_t _touchStartX, _touchStartY;
    uint16_t _touchLastX, _touchLastY;
    static const int SWIPE_THRESHOLD = 80;

    void _updateScreenSize() {
        if (_portrait) {
            _screenW = 540;
            _screenH = 960;
        } else {
            _screenW = 960;
            _screenH = 540;
        }
    }

    void _applyOrientation() {
        _canvas.deleteCanvas();

        uint16_t rotation = _portrait ? 90 : 0;
        M5.EPD.SetRotation(rotation);
        M5.TP.SetRotation(rotation);

        _updateScreenSize();
        _canvas.createCanvas(_screenW, _screenH);
        _canvas.createRender(_fontSize, 256);

        _buildPages();

        // Clamp current page to valid range after re-pagination
        if (_currentPage >= _totalPages) {
            _currentPage = _totalPages > 0 ? _totalPages - 1 : 0;
        }
        _renderPage(_currentPage);
    }

    void _buildPages() {
        if (!_text || _textLen == 0) {
            _totalPages = 0;
            return;
        }

        _updateScreenSize();

        int contentW = _screenW - 2 * _margin;
        int contentH = _screenH - 2 * _margin - 40; // reserve 40px for page number

        _pageOffsets[0] = 0;
        size_t offset = 0;
        int page = 0;

        while (offset < _textLen && page < MAX_PAGES) {
            _canvas.fillCanvas(0);
            _canvas.setTextArea(_margin, _margin, contentW, contentH);
            _canvas.setTextWrap(true, false);
            _canvas.setTextSize(_fontSize);
            _canvas.setTextLineSpace(_lineSpace);
            _canvas.setTextColor(15);
            _canvas.setCursor(_margin, _margin);
            _canvas.print(_text + offset);

            uint32_t rendered = _canvas.getExceedOffset();
            if (rendered == 0) {
                break; // All remaining text fit on this page
            }

            offset += rendered;
            page++;
            if (page < MAX_PAGES) {
                _pageOffsets[page] = offset;
            }
        }

        _totalPages = page + 1;
    }

    void _renderPage(int page) {
        if (page < 0 || page >= _totalPages) return;

        int contentW = _screenW - 2 * _margin;
        int contentH = _screenH - 2 * _margin - 40;

        _canvas.fillCanvas(0);
        _canvas.setTextArea(_margin, _margin, contentW, contentH);
        _canvas.setTextWrap(true, false);
        _canvas.setTextColor(15);
        _canvas.setTextSize(_fontSize);
        _canvas.setTextLineSpace(_lineSpace);
        _canvas.setCursor(_margin, _margin);
        _canvas.print(_text + _pageOffsets[page]);

        // Draw page number at bottom center (built-in bitmap font)
        char pageStr[16];
        snprintf(pageStr, sizeof(pageStr), "%d/%d", page + 1, _totalPages);
        _canvas.setTextSize(2);
        int strW = strlen(pageStr) * 12; // approximate width for built-in font size 2
        _canvas.drawString(pageStr, (_screenW - strW) / 2, _screenH - _margin - 10);

        _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    }
};
