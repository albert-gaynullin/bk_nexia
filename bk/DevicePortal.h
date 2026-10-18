#pragma once

#include <Arduino.h>
#include <RTClib.h>
#include <WebServer.h>
#include <memory>

// Created only in the physical, boot-time maintenance mode.
class DevicePortal {
public:
    explicit DevicePortal(RTC_PCF8563 &rtc) : _rtc(rtc) {}
    bool begin(bool rtcReady, void (*onStatus)());
    void loop();
    const char *ssid() const { return _ssid; }
    const char *password() const { return _password; }
    const char *status() const { return _status; }
    uint8_t progress() const { return _expected ? static_cast<uint8_t>(100ULL * _received / _expected) : 0; }

private:
    bool authorize(bool checkOrigin = false);
    void serveTime();
    void setTime();
    void handleUpload();
    void finishUpload();
    void fail(const char *message);
    void notify(const char *status);
    void resetRequest();

    RTC_PCF8563 &_rtc;
    std::unique_ptr<WebServer> _server;
    void (*_onStatus)() = nullptr;
    char _ssid[20] = {};
    char _password[13] = {};
    String _origin;
    const char *_status = "STARTING";
    const char *_error = nullptr;
    uint32_t _maxSize = 0, _expected = 0, _received = 0;
    uint32_t _lastActivity = 0, _lastProgress = 0, _rebootSince = 0;
    bool _rtcReady = false, _fileSeen = false, _fileComplete = false;
    bool _uploadAuthorized = false, _rebootPending = false;
};
