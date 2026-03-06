#pragma once
#include <ArduinoJson.h>
#include <SD.h>

// In-memory book info
struct BookInfo {
    int id;
    char name[128];
    char author[64];
};

// In-memory excerpt info
struct ExcerptInfo {
    int id;
    int bookId;
    char *content;   // allocated via ps_malloc (lazy-loaded for split format)
    char *idea;      // allocated via ps_malloc (lazy-loaded for split format)
    char chapter[64];
    uint32_t fileOffset; // byte offset in excerpts.jsonl (0 if legacy)
    uint16_t lineLen;    // line length in excerpts.jsonl (0 if legacy)
};

// Review settings from Android sync
struct ReviewSettings {
    int sortRule;           // 0 = ORDER, 1 = RANDOM
    int sortOrder;          // 0 = ASC (旧→新), 1 = DESC (新→旧)
    int autoSwitchMinutes;  // auto-switch interval in minutes, default 10
};

class DataStore {
public:
    DataStore() : _books(nullptr), _bookCount(0),
                  _excerpts(nullptr), _excerptCount(0),
                  _shuffledIndices(nullptr), _shuffledCount(0),
                  _loadedIdx(-1), _lazyMode(false) {
        _reviewSettings.sortRule = 1; // default RANDOM
        _reviewSettings.sortOrder = 0; // default ASC
        _reviewSettings.autoSwitchMinutes = _sanitizeAutoSwitchMinutes(10);
    }

    ~DataStore() { freeData(); }

    // Ensure /xmnote directory exists
    void begin() {
        if (!SD.exists("/xmnote")) {
            SD.mkdir("/xmnote");
        }
    }

    // --- WiFi config ---

    bool loadWiFiConfig(char *ssid, size_t ssidLen, char *password, size_t passLen) {
        if (!SD.exists("/xmnote/wifi.json")) return false;
        File f = SD.open("/xmnote/wifi.json", FILE_READ);
        if (!f) return false;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) return false;

        const char *s = doc["ssid"];
        const char *p = doc["password"];
        if (!s) return false;

        strlcpy(ssid, s, ssidLen);
        strlcpy(password, p ? p : "", passLen);
        return true;
    }

    bool saveWiFiConfig(const char *ssid, const char *password) {
        SD.remove("/xmnote/wifi.json");
        File f = SD.open("/xmnote/wifi.json", FILE_WRITE);
        if (!f) return false;

        JsonDocument doc;
        doc["ssid"] = ssid;
        doc["password"] = password;
        serializeJson(doc, f);
        f.close();
        return true;
    }

    // --- Excerpt data ---

    // Save raw JSON body directly to SD card (legacy single-file format)
    bool saveDataRaw(const String &body) {
        // Remove split-format files to avoid confusion
        SD.remove("/xmnote/meta.json");
        SD.remove("/xmnote/excerpts.jsonl");
        SD.remove("/xmnote/data.json");
        File f = SD.open("/xmnote/data.json", FILE_WRITE);
        if (!f) return false;
        f.print(body);
        f.close();
        return true;
    }

    // Save meta (books + reviewSettings) to separate file
    bool saveMetaRaw(const String &body) {
        SD.remove("/xmnote/meta.json");
        File f = SD.open("/xmnote/meta.json", FILE_WRITE);
        if (!f) return false;
        f.print(body);
        f.close();
        return true;
    }

    // Load data: prefer split format (meta.json + excerpts.jsonl), fallback to legacy data.json
    bool loadData() {
        freeData();
        if (SD.exists("/xmnote/meta.json")) {
            return _loadSplitFormat();
        }
        if (SD.exists("/xmnote/data.json")) {
            return _loadLegacyFormat();
        }
        return false;
    }

private:
    // Split-format loader: meta.json (small) + excerpts.jsonl (only store offsets, lazy-load content)
    bool _loadSplitFormat() {
        // 1. Parse meta.json
        File metaFile = SD.open("/xmnote/meta.json", FILE_READ);
        if (!metaFile) return false;

        JsonDocument metaDoc;
        DeserializationError err = deserializeJson(metaDoc, metaFile);
        metaFile.close();
        if (err) {
            Serial.printf("meta.json parse error: %s\n", err.c_str());
            return false;
        }

        _parseBooks(metaDoc);
        _parseReviewSettings(metaDoc);

        // 2. Scan excerpts.jsonl — record file offsets only, don't store content
        if (!SD.exists("/xmnote/excerpts.jsonl")) return true;

        File excFile = SD.open("/xmnote/excerpts.jsonl", FILE_READ);
        if (!excFile) return true;

        size_t fileSize = excFile.size();
        if (fileSize == 0) { excFile.close(); return true; }

        // Estimate capacity: ~400 bytes per line on average
        int capacity = fileSize / 400;
        if (capacity < 64) capacity = 64;

        _excerpts = (ExcerptInfo *)ps_malloc(sizeof(ExcerptInfo) * capacity);
        if (!_excerpts) {
            Serial.println("Failed to allocate excerpts array");
            excFile.close();
            return false;
        }

        // 4KB read buffer + 2KB line buffer for parsing metadata
        const size_t READ_BUF_SIZE = 4096;
        uint8_t *readBuf = (uint8_t *)ps_malloc(READ_BUF_SIZE);
        size_t lineBufCap = 2048;
        char *lineBuf = (char *)ps_malloc(lineBufCap);
        size_t lineLen = 0;

        if (!readBuf || !lineBuf) {
            Serial.println("Failed to allocate read buffers");
            if (readBuf) free(readBuf);
            if (lineBuf) free(lineBuf);
            free(_excerpts); _excerpts = nullptr;
            excFile.close();
            return false;
        }

        int i = 0;
        unsigned long startMs = millis();
        size_t lineStartPos = 0;  // file offset where current line starts
        size_t chunkStart = 0;    // file offset where current chunk starts

        Serial.printf("Loading excerpts: fileSize=%d, capacity=%d, free PSRAM=%d\n",
                       fileSize, capacity, ESP.getFreePsram());

        while (excFile.available()) {
            size_t bytesRead = excFile.read(readBuf, READ_BUF_SIZE);
            for (size_t b = 0; b < bytesRead; b++) {
                uint8_t ch = readBuf[b];
                if (ch == '\n') {
                    if (lineLen > 0) {
                        lineBuf[lineLen] = '\0';

                        // Grow excerpts array if needed
                        if (i >= capacity) {
                            int newCap = capacity * 2;
                            ExcerptInfo *newArr = (ExcerptInfo *)ps_realloc(_excerpts, sizeof(ExcerptInfo) * newCap);
                            if (!newArr) {
                                Serial.printf("realloc failed at %d excerpts\n", i);
                                goto done;
                            }
                            _excerpts = newArr;
                            capacity = newCap;
                        }

                        // Parse only id, bookId, chapter — skip content/idea to save PSRAM
                        JsonDocument lineDoc;
                        if (!deserializeJson(lineDoc, lineBuf, lineLen)) {
                            _excerpts[i].id = lineDoc["id"] | 0;
                            _excerpts[i].bookId = lineDoc["bookId"] | 0;
                            strlcpy(_excerpts[i].chapter, lineDoc["chapter"] | "", sizeof(_excerpts[i].chapter));
                            _excerpts[i].content = nullptr;
                            _excerpts[i].idea = nullptr;
                            _excerpts[i].fileOffset = (uint32_t)lineStartPos;
                            _excerpts[i].lineLen = (uint16_t)lineLen;
                            i++;

                            if (i % 2000 == 0) {
                                Serial.printf("  ... indexed %d excerpts, free PSRAM=%d\n",
                                              i, ESP.getFreePsram());
                            }
                        }
                    }
                    lineLen = 0;
                    lineStartPos = chunkStart + b + 1;
                } else {
                    // Grow line buffer if needed
                    if (lineLen + 1 >= lineBufCap) {
                        size_t newCap = lineBufCap * 2;
                        char *newBuf = (char *)ps_realloc(lineBuf, newCap);
                        if (!newBuf) {
                            Serial.println("lineBuf realloc failed");
                            goto done;
                        }
                        lineBuf = newBuf;
                        lineBufCap = newCap;
                    }
                    lineBuf[lineLen++] = (char)ch;
                }
            }
            chunkStart += bytesRead;
        }

        // Handle last line without trailing newline
        if (lineLen > 0) {
            lineBuf[lineLen] = '\0';
            if (i >= capacity) {
                int newCap = capacity * 2;
                ExcerptInfo *newArr = (ExcerptInfo *)ps_realloc(_excerpts, sizeof(ExcerptInfo) * newCap);
                if (newArr) { _excerpts = newArr; capacity = newCap; }
            }
            if (i < capacity) {
                JsonDocument lineDoc;
                if (!deserializeJson(lineDoc, lineBuf, lineLen)) {
                    _excerpts[i].id = lineDoc["id"] | 0;
                    _excerpts[i].bookId = lineDoc["bookId"] | 0;
                    strlcpy(_excerpts[i].chapter, lineDoc["chapter"] | "", sizeof(_excerpts[i].chapter));
                    _excerpts[i].content = nullptr;
                    _excerpts[i].idea = nullptr;
                    _excerpts[i].fileOffset = (uint32_t)lineStartPos;
                    _excerpts[i].lineLen = (uint16_t)lineLen;
                    i++;
                }
            }
        }

done:
        free(readBuf);
        free(lineBuf);
        excFile.close();
        _excerptCount = i;
        _lazyMode = true;

        Serial.printf("Indexed %d books, %d excerpts in %lums, free PSRAM=%d (split/lazy)\n",
                       _bookCount, _excerptCount, millis() - startMs, ESP.getFreePsram());
        return true;
    }

    // Lazy-load content/idea for one excerpt from excerpts.jsonl
    void _ensureContentLoaded(int index) const {
        if (_loadedIdx == index) return;

        // Free previously loaded content
        if (_loadedIdx >= 0 && _loadedIdx < _excerptCount) {
            free(_excerpts[_loadedIdx].content);
            _excerpts[_loadedIdx].content = nullptr;
            free(_excerpts[_loadedIdx].idea);
            _excerpts[_loadedIdx].idea = nullptr;
        }

        File f = SD.open("/xmnote/excerpts.jsonl", FILE_READ);
        if (!f) { _loadedIdx = -1; return; }

        f.seek(_excerpts[index].fileOffset);
        uint16_t len = _excerpts[index].lineLen;
        char *buf = (char *)ps_malloc(len + 1);
        if (!buf) { f.close(); _loadedIdx = -1; return; }

        f.readBytes(buf, len);
        buf[len] = '\0';
        f.close();

        JsonDocument doc;
        if (!deserializeJson(doc, buf, len)) {
            const char *c = doc["content"] | "";
            size_t cLen = strlen(c);
            _excerpts[index].content = (char *)ps_malloc(cLen + 1);
            if (_excerpts[index].content) memcpy(_excerpts[index].content, c, cLen + 1);

            const char *idea = doc["idea"] | "";
            size_t iLen = strlen(idea);
            _excerpts[index].idea = (char *)ps_malloc(iLen + 1);
            if (_excerpts[index].idea) memcpy(_excerpts[index].idea, idea, iLen + 1);
        }
        free(buf);
        _loadedIdx = index;
    }

    // Legacy single-file loader (for backward compat with old /api/sync)
    bool _loadLegacyFormat() {
        File f = SD.open("/xmnote/data.json", FILE_READ);
        if (!f) return false;

        size_t fileSize = f.size();
        if (fileSize == 0) { f.close(); return false; }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            Serial.printf("JSON parse error: %s\n", err.c_str());
            return false;
        }

        _parseBooks(doc);
        _parseReviewSettings(doc);

        // Parse excerpts
        JsonArray excArr = doc["excerpts"];
        _excerptCount = excArr.size();
        if (_excerptCount > 0) {
            _excerpts = (ExcerptInfo *)ps_malloc(sizeof(ExcerptInfo) * _excerptCount);
            int i = 0;
            for (JsonObject e : excArr) {
                _excerpts[i].id = e["id"] | 0;
                _excerpts[i].bookId = e["bookId"] | 0;

                const char *c = e["content"] | "";
                size_t cLen = strlen(c);
                _excerpts[i].content = (char *)ps_malloc(cLen + 1);
                memcpy(_excerpts[i].content, c, cLen + 1);

                const char *idea = e["idea"] | "";
                size_t iLen = strlen(idea);
                _excerpts[i].idea = (char *)ps_malloc(iLen + 1);
                memcpy(_excerpts[i].idea, idea, iLen + 1);

                strlcpy(_excerpts[i].chapter, e["chapter"] | "", sizeof(_excerpts[i].chapter));
                _excerpts[i].fileOffset = 0;
                _excerpts[i].lineLen = 0;
                i++;
            }
        }

        Serial.printf("Loaded %d books, %d excerpts (legacy format)\n", _bookCount, _excerptCount);
        return true;
    }

    void _parseBooks(JsonDocument &doc) {
        JsonArray booksArr = doc["books"];
        _bookCount = booksArr.size();
        if (_bookCount > 0) {
            _books = (BookInfo *)ps_malloc(sizeof(BookInfo) * _bookCount);
            int i = 0;
            for (JsonObject b : booksArr) {
                _books[i].id = b["id"] | 0;
                strlcpy(_books[i].name, b["name"] | "", sizeof(_books[i].name));
                strlcpy(_books[i].author, b["author"] | "", sizeof(_books[i].author));
                i++;
            }
        }
    }

    void _parseReviewSettings(JsonDocument &doc) {
        _reviewSettings.sortRule = 1; // default RANDOM
        _reviewSettings.sortOrder = 0; // default ASC
        _reviewSettings.autoSwitchMinutes = _sanitizeAutoSwitchMinutes(10);
        if (doc["reviewSettings"].is<JsonObject>()) {
            JsonObject rs = doc["reviewSettings"];
            _reviewSettings.sortRule = rs["sortRule"] | 1;
            _reviewSettings.sortOrder = rs["sortOrder"] | 0;
            _reviewSettings.autoSwitchMinutes = _sanitizeAutoSwitchMinutes(rs["autoSwitchMinutes"] | 10);
        }
    }

public:

    void freeData() {
        if (_excerpts) {
            for (int i = 0; i < _excerptCount; i++) {
                if (_excerpts[i].content) free(_excerpts[i].content);
                if (_excerpts[i].idea) free(_excerpts[i].idea);
            }
            free(_excerpts);
            _excerpts = nullptr;
        }
        if (_books) {
            free(_books);
            _books = nullptr;
        }
        _bookCount = 0;
        _excerptCount = 0;
        _loadedIdx = -1;
        _lazyMode = false;
        if (_shuffledIndices) { free(_shuffledIndices); _shuffledIndices = nullptr; }
        _shuffledCount = 0;
        _reviewSettings.sortRule = 1;
        _reviewSettings.sortOrder = 0;
        _reviewSettings.autoSwitchMinutes = _sanitizeAutoSwitchMinutes(10);
    }

    // Find book by ID
    const BookInfo *findBook(int bookId) const {
        for (int i = 0; i < _bookCount; i++) {
            if (_books[i].id == bookId) return &_books[i];
        }
        return nullptr;
    }

    // Accessors
    int getBookCount() const { return _bookCount; }
    int getExcerptCount() const { return _excerptCount; }
    const ExcerptInfo *getExcerpt(int index) const {
        if (index < 0 || index >= _excerptCount) return nullptr;
        if (_lazyMode) _ensureContentLoaded(index);
        return &_excerpts[index];
    }

    // SD free space in KB
    uint32_t getSDFreeKB() const {
        uint64_t total = SD.totalBytes();
        uint64_t used = SD.usedBytes();
        return (uint32_t)((total - used) / 1024);
    }

    // Review settings
    const ReviewSettings &getReviewSettings() const { return _reviewSettings; }
    int getAutoSwitchMinutes() const { return _sanitizeAutoSwitchMinutes(_reviewSettings.autoSwitchMinutes); }

    void buildShuffledOrder() {
        if (_shuffledIndices) { free(_shuffledIndices); _shuffledIndices = nullptr; }
        _shuffledCount = _excerptCount;
        if (_shuffledCount == 0) return;
        _shuffledIndices = (int *)ps_malloc(sizeof(int) * _shuffledCount);
        for (int i = 0; i < _shuffledCount; i++) _shuffledIndices[i] = i;
        // Fisher-Yates shuffle using ESP32 hardware RNG
        for (int i = _shuffledCount - 1; i > 0; i--) {
            int j = esp_random() % (i + 1);
            int tmp = _shuffledIndices[i];
            _shuffledIndices[i] = _shuffledIndices[j];
            _shuffledIndices[j] = tmp;
        }
    }

    int getShuffledIndex(int position) const {
        if (!_shuffledIndices || position < 0 || position >= _shuffledCount) return position;
        return _shuffledIndices[position];
    }

    // --- Reading state persistence (for shutdown/wake resume) ---

    bool saveReadingState(int currentIndex, int subPage, bool portrait = true, bool powerSave = true) {
        SD.remove("/xmnote/reading.json");
        File f = SD.open("/xmnote/reading.json", FILE_WRITE);
        if (!f) return false;
        JsonDocument doc;
        doc["index"] = currentIndex;
        doc["subPage"] = subPage;
        doc["portrait"] = portrait;
        doc["powerSave"] = powerSave;
        serializeJson(doc, f);
        f.close();
        return true;
    }

    bool loadReadingState(int &currentIndex, int &subPage, bool &portrait, bool &powerSave) {
        if (!SD.exists("/xmnote/reading.json")) return false;
        File f = SD.open("/xmnote/reading.json", FILE_READ);
        if (!f) return false;
        JsonDocument doc;
        if (deserializeJson(doc, f)) { f.close(); return false; }
        f.close();
        currentIndex = doc["index"] | 0;
        subPage = doc["subPage"] | 0;
        portrait = doc["portrait"] | true;
        powerSave = doc["powerSave"] | true;
        return true;
    }

    void clearReadingState() {
        SD.remove("/xmnote/reading.json");
    }

private:
    static int _sanitizeAutoSwitchMinutes(int minutes) {
        if (minutes <= 0) return 10;
        if (minutes > 1440) return 1440;
        return minutes;
    }

    BookInfo *_books;
    int _bookCount;
    mutable ExcerptInfo *_excerpts;
    int _excerptCount;
    ReviewSettings _reviewSettings;
    int *_shuffledIndices;
    int _shuffledCount;
    mutable int _loadedIdx;  // which excerpt currently has content loaded (-1 = none)
    bool _lazyMode;          // true for split format (lazy-load content from file)
};
