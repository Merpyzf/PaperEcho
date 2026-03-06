#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

// M5Paper GPIO pins for wakeup sources
#define PP_TOUCH_INT_PIN   GPIO_NUM_36
#define PP_KEY_LEFT_PIN    GPIO_NUM_37
#define PP_KEY_PUSH_PIN    GPIO_NUM_38
#define PP_KEY_RIGHT_PIN   GPIO_NUM_39

class PowerPolicy {
public:
    enum Profile {
        PROFILE_WIFI_SETUP,
        PROFILE_HOME_IDLE,
        PROFILE_HOME_SYNCING,
        PROFILE_READING_ACTIVE
    };

    PowerPolicy() : _current(PROFILE_HOME_IDLE), _initialized(false) {}

    void apply(Profile profile) {
        if (_initialized && profile == _current) return;

        switch (profile) {
            case PROFILE_WIFI_SETUP:
                setCpuFrequencyMhz(160);
                _fixSerial();
                _ensureWifiStaMode();
                WiFi.setSleep(false);
                break;

            case PROFILE_HOME_IDLE:
                setCpuFrequencyMhz(80);
                _fixSerial();
                _ensureWifiStaMode();
                WiFi.setSleep(true);
                break;

            case PROFILE_HOME_SYNCING:
                setCpuFrequencyMhz(160);
                _fixSerial();
                _ensureWifiStaMode();
                WiFi.setSleep(false);
                break;

            case PROFILE_READING_ACTIVE:
                // Turn off WiFi BEFORE lowering CPU — WiFi.mode(WIFI_OFF) is
                // heavyweight and triggers a WDT reset at 40 MHz.
                WiFi.mode(WIFI_OFF);
                setCpuFrequencyMhz(40);
                _fixSerial();
                break;
        }

        _current = profile;
        _initialized = true;
    }

    Profile current() const { return _current; }

    // Enter light sleep until touch/button input or timer expires.
    // maxMs: maximum sleep duration in milliseconds (0 = no timer, wake on input only).
    // Returns immediately if maxMs < 10.
    void sleepUntilInput(uint32_t maxMs) {
        if (maxMs < 10) return;

        // Configure GPIO wakeup: all active-low (touch INT + 3 buttons)
        gpio_wakeup_enable(PP_TOUCH_INT_PIN, GPIO_INTR_LOW_LEVEL);
        gpio_wakeup_enable(PP_KEY_LEFT_PIN,  GPIO_INTR_LOW_LEVEL);
        gpio_wakeup_enable(PP_KEY_PUSH_PIN,  GPIO_INTR_LOW_LEVEL);
        gpio_wakeup_enable(PP_KEY_RIGHT_PIN, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        // Configure timer wakeup
        esp_sleep_enable_timer_wakeup((uint64_t)maxMs * 1000ULL);

        // Enter light sleep — CPU halts, GPIO interrupt state preserved,
        // returns here on wakeup
        esp_light_sleep_start();

        // Disable wakeup sources to avoid affecting subsequent deep sleep (M5.shutdown)
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    }

    // Temporarily boost CPU to 80MHz for rendering operations
    void boostForRender() {
        if (_current == PROFILE_READING_ACTIVE) {
            setCpuFrequencyMhz(80);
            _fixSerial();
        }
    }

    // Enter light sleep with timer wakeup only.
    void sleepTimerOnly(uint32_t maxMs) {
        if (maxMs < 10) return;
        esp_sleep_enable_timer_wakeup((uint64_t)maxMs * 1000ULL);
        esp_light_sleep_start();
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }

    // Restore idle CPU frequency after rendering
    void idleAfterRender() {
        if (_current == PROFILE_READING_ACTIVE) {
            setCpuFrequencyMhz(40);
            _fixSerial();
        }
    }

private:
    Profile _current;
    bool _initialized;

    void _ensureWifiStaMode() {
        if (WiFi.getMode() == WIFI_OFF) {
            WiFi.mode(WIFI_STA);
        }
    }

    // Re-sync UART baud rate after CPU frequency change
    void _fixSerial() {
        Serial.updateBaudRate(115200);
    }
};
