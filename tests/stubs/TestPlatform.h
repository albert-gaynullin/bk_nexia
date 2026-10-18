#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>

#define PROGMEM

class String {
public:
    String() = default;
    String(const char *value) : text(value ? value : "") {}
    String(std::string value) : text(std::move(value)) {}
    const char *c_str() const { return text.c_str(); }
    size_t length() const { return text.length(); }
    bool endsWith(const char *suffix) const {
        const std::string end(suffix);
        return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
    }
    void toLowerCase() { std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); }); }
    bool operator==(const String &other) const { return text == other.text; }
    bool operator!=(const String &other) const { return !(*this == other); }
    friend String operator+(const String &a, const String &b) { return a.text + b.text; }
    std::string text;
};

inline uint32_t testMillis = 0;
inline uint32_t millis() { return testMillis; }
struct FakeESP {
    int restarts = 0;
    uint64_t getEfuseMac() const { return 0x1234; }
    void restart() { ++restarts; }
};
inline FakeESP ESP;
inline uint32_t esp_random() { static uint32_t value = 0; return ++value; }

class IPAddress {
public:
    IPAddress(int a, int b, int c, int d) { char value[20]; snprintf(value, sizeof(value), "%d.%d.%d.%d", a,b,c,d); text=value; }
    String toString() const { return text; }
    String text;
};
constexpr int WIFI_AP = 2;
struct FakeWiFi {
    bool success = true;
    int clients = 0;
    String network, password;
    void mode(int) {}
    bool softAPConfig(IPAddress, IPAddress, IPAddress) { return success; }
    bool softAP(const char *name, const char *key, int, bool, int maxClients) {
        network=name; password=key; clients=maxClients; return success;
    }
    IPAddress softAPIP() const { return {192,168,4,1}; }
};
inline FakeWiFi WiFi;

class DateTime {
public:
    DateTime(uint16_t year=2026,uint8_t month=1,uint8_t day=1,uint8_t hour=0,uint8_t minute=0,uint8_t second=0)
        : y(year),m(month),d(day),h(hour),mi(minute),s(second) {}
    uint16_t year() const { return y; }
    uint8_t month() const { return m; }
    uint8_t day() const { return d; }
    uint8_t hour() const { return h; }
    uint8_t minute() const { return mi; }
    uint8_t second() const { return s; }
    bool isValid() const { return y >= 2000 && y <= 2099 && m >= 1 && m <= 12 && d >= 1 && d <= 31 && h < 24 && mi < 60 && s < 60; }
    // A monotonic test clock; calendar validation is tested against the production parser.
    uint32_t unixtime() const { return (((y-2000)*372U+(m-1)*31U+d)*24U+h)*3600U+mi*60U+s; }
private:
    uint16_t y;
    uint8_t m,d,h,mi,s;
};
class RTC_PCF8563 {
public:
    bool writeWorks = true;
    int writes = 0;
    DateTime value;
    DateTime now() { return value; }
    void adjust(const DateTime &next) { ++writes; if (writeWorks) value=next; }
    void start() {}
};

struct esp_partition_t { uint32_t address, size; };
inline esp_partition_t nextPartition{0x150000,1310720}, runningPartition{0x10000,1310720};
inline bool partitionPresent = true;
inline const esp_partition_t *esp_ota_get_next_update_partition(void*) { return partitionPresent ? &nextPartition : nullptr; }
inline const esp_partition_t *esp_ota_get_running_partition() { return &runningPartition; }

enum HTTPMethod { HTTP_GET, HTTP_POST };
enum HTTPUploadStatus { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
    HTTPUploadStatus status = UPLOAD_FILE_START;
    String name = "firmware", filename = "bk.ino.bin";
    uint8_t buf[1436] = {};
    size_t currentSize = 0, totalSize = 0;
};
class WebServer {
public:
    using Handler = std::function<void()>;
    struct Route { Handler handler, upload; };
    inline static WebServer *instance = nullptr;
    explicit WebServer(int) { instance=this; }
    ~WebServer() { instance=nullptr; }
    std::map<std::pair<std::string,HTTPMethod>,Route> routes;
    std::map<std::string,String> args;
    bool authenticated = true;
    String origin = "http://192.168.4.1";
    int response = 0;
    String body;
    HTTPUpload file;
    bool authenticate(const char*,const char*) { return authenticated; }
    void requestAuthentication() { response=401; }
    void collectHeaders(const char**,size_t) {}
    String header(const char*) const { return origin; }
    void sendHeader(const char*,const char*) {}
    void send(int code,const char*,const String &value) { response=code;body=value; }
    void send_P(int code,const char *type,const char *value) { send(code,type,value); }
    void on(const char *path,HTTPMethod method,Handler handler) { routes[{path,method}]={handler,{}}; }
    void on(const char *path,HTTPMethod method,Handler handler,Handler upload) { routes[{path,method}]={handler,upload}; }
    void onNotFound(Handler) {}
    String arg(const char *name) { return args[name]; }
    HTTPUpload &upload() { return file; }
    void begin() {}
    void handleClient() {}
    void request(const char *path,HTTPMethod method) { response=0; routes.at({path,method}).handler(); }
    void chunk(HTTPUploadStatus status,size_t size=0) {
        file.status=status;
        if (status==UPLOAD_FILE_START) { file.currentSize=0;file.totalSize=0; }
        if (status==UPLOAD_FILE_WRITE) { file.currentSize=size;file.totalSize+=size; }
        routes.at({"/update",HTTP_POST}).upload();
    }
};

constexpr int U_FLASH = 0;
struct FakeUpdate {
    bool running=false,beginWorks=true,writeWorks=true,imageValid=true,activated=false;
    int begins=0,ends=0,aborts=0;
    size_t expected=0,received=0;
    bool begin(size_t size,int) { ++begins;expected=size;received=0;running=beginWorks;return running; }
    bool isRunning() const { return running; }
    size_t write(uint8_t*,size_t size) { if(!writeWorks)return 0;received+=size;return size; }
    bool end() { ++ends;bool ok=running&&imageValid&&received==expected;running=false;activated=ok;return ok; }
    void abort() { ++aborts;running=false; }
};
inline FakeUpdate Update;
