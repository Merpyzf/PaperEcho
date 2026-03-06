#include <M5EPD.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include "DataStore.h"
#include "WiFiSetup.h"
#include "SyncServer.h"
#include "HomePage.h"
#include "ExcerptViewer.h"
#include "PowerPolicy.h"

// Application state machine
enum AppState {
    STATE_BOOT,
    STATE_WIFI_SETUP,
    STATE_HOME,
    STATE_READING
};

M5EPD_Canvas canvas(&M5.EPD);
DataStore dataStore;
WiFiSetup *wifiSetup = nullptr;
SyncServer *syncServer = nullptr;
HomePage *homePage = nullptr;
ExcerptViewer *excerptViewer = nullptr;
PowerPolicy powerPolicy;

AppState appState = STATE_BOOT;
unsigned long gLastHomeActivityMs = 0;
static const uint8_t BM8563_CTRL_STATUS2_REG = 0x01;
static const uint8_t BM8563_TIMER_FLAG_MASK = 0x04;      // TF
static const uint8_t BM8563_ALARM_FLAG_MASK = 0x08;      // AF
static const uint8_t BM8563_TIMER_IRQ_ENABLE_MASK = 0x01; // TIE

void renderSyncScreen(M5EPD_Canvas &canvas, int count);
void updateSyncCount(M5EPD_Canvas &canvas, int count);
void enterWiFiSetup();
void enterHome();
void enterReading(int fromIndex = -1, int fromSubPage = 0, bool portrait = true, bool powerSave = true);

bool consumeRtcTimerWakeFlag() {
    uint8_t reg01 = M5.RTC.readReg(BM8563_CTRL_STATUS2_REG);
    if (reg01 == 0xFF) {
        Serial.println("BOOT: BM8563 reg01 read 0xFF, retry after Wire.begin");
        Wire.begin(21, 22, (uint32_t)400000U);
        delay(2);
        reg01 = M5.RTC.readReg(BM8563_CTRL_STATUS2_REG);
        Serial.printf("BOOT: BM8563 reg01 retry=0x%02X\n", reg01);
        if (reg01 == 0xFF) {
            Serial.println("BOOT: BM8563 reg01 still 0xFF, treat as non-timer wake");
            M5.RTC.disableIRQ();
            return false;
        }
    }

    bool timerFlag = (reg01 & BM8563_TIMER_FLAG_MASK) != 0;
    bool alarmFlag = (reg01 & BM8563_ALARM_FLAG_MASK) != 0;
    bool timerIrqEnabled = (reg01 & BM8563_TIMER_IRQ_ENABLE_MASK) != 0;
    Serial.printf("BOOT: BM8563 reg01=0x%02X TF=%d AF=%d TIE=%d\n",
                  reg01, timerFlag ? 1 : 0, alarmFlag ? 1 : 0, timerIrqEnabled ? 1 : 0);

    // Always clear and disable stale IRQ bits on boot.
    M5.RTC.disableIRQ();
    uint8_t regAfter = M5.RTC.readReg(BM8563_CTRL_STATUS2_REG);
    Serial.printf("BOOT: BM8563 reg01 after disableIRQ=0x%02X\n", regAfter);
    return timerFlag;
}

bool initHardware() {
    M5.begin(true, false, true, true);
    M5.EPD.SetRotation(90);
    M5.TP.SetRotation(90);
    // Note: EPD.Clear is NOT called here — deferred until we know if this is an RTC wake
    canvas.createCanvas(540, 960);

    // Manual SD init
    SPI.begin(14, 13, 12, 4);
    if (!SD.begin(4, SPI, 20000000)) {
        M5.EPD.Clear(true);
        canvas.setTextSize(3);
        canvas.drawString("SD 卡读取失败，请检查后重启", 40, 200);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        return false;
    }

    // Load TTF font
    canvas.loadFont("/font.ttf", SD);
    canvas.createRender(24, 256);
    canvas.createRender(20, 256);
    canvas.createRender(22, 256);
    canvas.createRender(26, 256);
    canvas.createRender(28, 256);
    canvas.createRender(32, 256);
    canvas.createRender(36, 256);

    return true;
}

bool tryAutoConnect() {
    char ssid[33], password[65];
    if (!dataStore.loadWiFiConfig(ssid, sizeof(ssid), password, sizeof(password))) {
        return false;
    }

    canvas.fillCanvas(0);
    canvas.setTextSize(26);
    canvas.setTextColor(15);
    canvas.drawString("正在连接 WiFi...", 40, 200);
    canvas.drawString(ssid, 40, 250);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    int timeout = 20; // 10 seconds
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
        M5.update();
        delay(500);
        timeout--;
    }

    return WiFi.status() == WL_CONNECTED;
}

void enterWiFiSetup() {
    appState = STATE_WIFI_SETUP;
    powerPolicy.apply(PowerPolicy::PROFILE_WIFI_SETUP);
    if (!wifiSetup) {
        wifiSetup = new WiFiSetup(canvas, dataStore);
    }
    wifiSetup->begin();
}

void enterHome() {
    appState = STATE_HOME;
    powerPolicy.apply(PowerPolicy::PROFILE_HOME_IDLE);
    gLastHomeActivityMs = millis();

    // Ensure WiFi reconnects when returning from reading mode.
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
    }

    // Start HTTP server if not already running
    if (!syncServer) {
        syncServer = new SyncServer(dataStore);
        syncServer->begin();
    }

    if (!homePage) {
        homePage = new HomePage(canvas, dataStore);
    }
    homePage->render();
}

void enterReading(int fromIndex, int fromSubPage, bool portrait, bool powerSave) {
    unsigned long enterStartMs = millis();
#if HOME_DIAG_LOG
    Serial.printf("[STATE][%lu] ENTER_READING begin fromIndex=%d fromSubPage=%d portrait=%d powerSave=%d excerptCount=%d\n",
                  enterStartMs, fromIndex, fromSubPage, portrait ? 1 : 0, powerSave ? 1 : 0, dataStore.getExcerptCount());
#endif
    Serial.println("ENTER_READING: start");
    appState = STATE_READING;

    // Turn off WiFi in reading mode
    if (syncServer) {
        Serial.println("ENTER_READING: stopping sync server");
        syncServer->stop();
        delete syncServer;
        syncServer = nullptr;
        Serial.println("ENTER_READING: sync server stopped");
    }

    // Turn off WiFi first (while CPU is still fast), then apply reading profile
    // which lowers CPU to 40MHz. The ExcerptViewer::begin() call below does
    // heavyweight work (shuffle, TTF rendering) that is too slow at 40MHz,
    // so we defer the profile switch until after initialization completes.
    Serial.println("ENTER_READING: turning off WiFi");
    WiFi.mode(WIFI_OFF);
    Serial.println("ENTER_READING: WiFi off");

    if (!excerptViewer) {
        Serial.println("ENTER_READING: creating ExcerptViewer");
        excerptViewer = new ExcerptViewer(canvas, dataStore);
        Serial.println("ENTER_READING: ExcerptViewer created");
    }

    if (fromIndex >= 0) {
        Serial.printf("ENTER_READING: calling beginFromState(%d, %d, %d)\n", fromIndex, fromSubPage, portrait ? 1 : 0);
        excerptViewer->beginFromState(fromIndex, fromSubPage, portrait, powerSave);
    } else {
        Serial.println("ENTER_READING: calling begin()");
        excerptViewer->begin(portrait, powerSave);
    }

    // Now that initialization and first render are done, apply reading power policy
    Serial.println("ENTER_READING: applying power policy");
    powerPolicy.apply(PowerPolicy::PROFILE_READING_ACTIVE);
    Serial.println("ENTER_READING: done");
#if HOME_DIAG_LOG
    Serial.printf("[STATE][%lu] ENTER_READING done elapsed=%lums state=%d powerSave=%d\n",
                  millis(), millis() - enterStartMs, (int)appState, powerSave ? 1 : 0);
#endif
}

// Sync progress area constants
static const int SYNC_COUNT_X = 120;
static const int SYNC_COUNT_Y = 490;
static const int SYNC_COUNT_W = 300;
static const int SYNC_COUNT_H = 40;

void renderSyncScreen(M5EPD_Canvas &canvas, int count) {
    canvas.fillCanvas(0);
    canvas.setTextSize(36);
    canvas.setTextColor(15);
    canvas.setTextDatum(TC_DATUM);
    canvas.drawString("正在同步...", 270, 430);
    canvas.setTextSize(26);
    canvas.drawString("已接收 " + String(count) + " 条书摘", 270, SYNC_COUNT_Y);
    canvas.setTextDatum(TL_DATUM);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void updateSyncCount(M5EPD_Canvas &canvas, int count) {
    M5EPD_Canvas countCanvas(&M5.EPD);
    countCanvas.createCanvas(SYNC_COUNT_W, SYNC_COUNT_H);
    countCanvas.fillCanvas(0);
    countCanvas.setTextSize(26);
    countCanvas.setTextColor(15);
    countCanvas.setTextDatum(TC_DATUM);
    countCanvas.drawString("已接收 " + String(count) + " 条书摘", SYNC_COUNT_W / 2, 0);
    countCanvas.pushCanvas(SYNC_COUNT_X, SYNC_COUNT_Y, UPDATE_MODE_GC16);
    countCanvas.deleteCanvas();
}

void setup() {
    Serial.begin(115200);

    if (!initHardware()) return;

    // If reading state exists and is valid, resume reading directly to avoid
    // flashing loading/home between auto-switch wake cycles.
    bool hasReadingState = SD.exists("/xmnote/reading.json");
    esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    bool rtcTimerWake = consumeRtcTimerWakeFlag();
    bool dataLoaded = false;
    Serial.printf("BOOT: hasReadingState=%d wakeCause=%d rtcTimerWake=%d\n",
                  hasReadingState ? 1 : 0, (int)wakeCause, rtcTimerWake ? 1 : 0);

    if (hasReadingState) {
        dataStore.begin();
        dataLoaded = dataStore.loadData();

        int savedIndex, savedSubPage;
        bool savedPortrait, savedPowerSave;
        if (dataStore.loadReadingState(savedIndex, savedSubPage, savedPortrait, savedPowerSave)) {
            bool indexOk = (savedIndex >= 0 && savedIndex < dataStore.getExcerptCount());
            bool subPageOk = (savedSubPage >= 0);
            if (rtcTimerWake && indexOk && subPageOk) {
                Serial.printf("BOOT: resume reading success index=%d subPage=%d portrait=%d powerSave=%d\n",
                              savedIndex, savedSubPage, savedPortrait ? 1 : 0, savedPowerSave ? 1 : 0);
                dataStore.clearReadingState();
                enterReading(savedIndex, savedSubPage, savedPortrait, savedPowerSave);
                return;
            }
            if (!rtcTimerWake) {
                Serial.println("BOOT: reading state exists but wake is not RTC timer (likely RESET), skip resume");
            } else {
                Serial.printf("BOOT: resume reading invalid index=%d subPage=%d total=%d\n",
                              savedIndex, savedSubPage, dataStore.getExcerptCount());
            }
        } else {
            Serial.println("BOOT: reading state parse failed");
        }
        // reading.json existed but was invalid/stale.
        Serial.println("BOOT: clear stale reading state");
        dataStore.clearReadingState();
    }

    // Normal boot path
    M5.EPD.Clear(true);
    canvas.setTextSize(26);
    canvas.setTextColor(15);
    canvas.drawString("正在加载...", 200, 450);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

    if (!dataLoaded) {
        dataStore.begin();
        dataStore.loadData();
    }

    if (tryAutoConnect()) {
        enterHome();
    } else {
        enterWiFiSetup();
    }
}

void loop() {
    uint16_t loopDelayMs = 50;
    bool usedLightSleep = false;

    // Always handle HTTP requests when server is running
    if (syncServer) {
        syncServer->handleClient();
    }

    switch (appState) {
        case STATE_WIFI_SETUP: {
            WiFiSetup::Result r = wifiSetup->update();
            if (r == WiFiSetup::RESULT_CONNECTED) {
                enterHome();
            } else if (r == WiFiSetup::RESULT_BACK) {
                tryAutoConnect();
                enterHome();
            }
            loopDelayMs = 20;
            break;
        }

        case STATE_HOME: {
            // Sync progress display
            static bool wasSyncing = false;
            static int lastSyncCount = 0;
            bool syncingNow = (syncServer && syncServer->isSyncInProgress());
            unsigned long now = millis();
            if (gLastHomeActivityMs == 0) gLastHomeActivityMs = now;

            powerPolicy.apply(syncingNow ? PowerPolicy::PROFILE_HOME_SYNCING
                                         : PowerPolicy::PROFILE_HOME_IDLE);

            if (syncingNow) gLastHomeActivityMs = now;

            if (syncingNow) {
                int count = syncServer->getSyncExcerptCount();
                if (!wasSyncing) {
                    renderSyncScreen(canvas, count);
                    wasSyncing = true;
                    lastSyncCount = count;
                } else if (count != lastSyncCount) {
                    updateSyncCount(canvas, count);
                    lastSyncCount = count;
                }
            } else if (wasSyncing) {
                wasSyncing = false;
                lastSyncCount = 0;
            }

            // Check if sync server received new data
            if (syncServer && syncServer->hasNewData()) {
                dataStore.loadData();
                if (dataStore.getReviewSettings().sortRule == 1) {
                    dataStore.buildShuffledOrder();
                }
                homePage->render();
                gLastHomeActivityMs = now;
            }

            HomePage::Action action = homePage->update();
            if (action == HomePage::ACTION_READ) {
                uint32_t lastHomeEvt = homePage ? homePage->getLastEventId() : 0;
                bool startPortrait = homePage ? homePage->getStartPortrait() : true;
                bool startPowerSave = homePage ? homePage->isPowerSaveMode() : true;
#if HOME_DIAG_LOG
                Serial.printf("[STATE][%lu] HOME action=ACTION_READ lastHomeEvt=E%lu portrait=%d powerSave=%d excerptCount=%d\n",
                              millis(), (unsigned long)lastHomeEvt, startPortrait ? 1 : 0,
                              startPowerSave ? 1 : 0, dataStore.getExcerptCount());
#endif
                gLastHomeActivityMs = now;
                unsigned long readEnterStartMs = millis();
                enterReading(-1, 0, startPortrait, startPowerSave);
#if HOME_DIAG_LOG
                Serial.printf("[STATE][%lu] HOME->READ transition_done elapsed=%lums appState=%d\n",
                              millis(), millis() - readEnterStartMs, (int)appState);
#endif
            } else if (action == HomePage::ACTION_WIFI_SETUP) {
                uint32_t lastHomeEvt = homePage ? homePage->getLastEventId() : 0;
#if HOME_DIAG_LOG
                Serial.printf("[STATE][%lu] HOME action=ACTION_WIFI_SETUP lastHomeEvt=E%lu\n",
                              millis(), (unsigned long)lastHomeEvt);
#endif
                gLastHomeActivityMs = now;
                if (syncServer) {
                    syncServer->stop();
                    delete syncServer;
                    syncServer = nullptr;
                }
                WiFi.disconnect();
                enterWiFiSetup();
            }
            if (syncingNow) {
                loopDelayMs = 20;
            } else {
                // Keep HOME fully awake for stable touch detection on M5Paper.
                // Power saving remains primarily in reading mode.
                loopDelayMs = 20;
            }
            break;
        }

        case STATE_READING: {
            bool goBack = excerptViewer->update();
            if (goBack) {
                enterHome();
                break;
            }
            bool powerSaveMode = excerptViewer ? excerptViewer->isPowerSaveMode() : true;
            if (powerSaveMode) {
                uint32_t remain = excerptViewer ? excerptViewer->millisUntilAutoSwitch() : 0;
                if (remain > 20) {
                    uint32_t sleepMs = remain;
                    if (sleepMs > 5000) sleepMs = 5000;
                    powerPolicy.sleepTimerOnly(sleepMs);
                    usedLightSleep = true;
                } else {
                    loopDelayMs = 10;
                }
            } else {
                loopDelayMs = 20;
            }
            break;
        }

        default:
            loopDelayMs = 50;
            break;
    }

    if (!usedLightSleep) {
        delay(loopDelayMs);
    }
}
