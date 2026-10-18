#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include "ELMduino.h"
#include "RTClib.h"
#include <NimBLEDevice.h>
#include <memory>
#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

std::unique_ptr<ELM327> myELM327;
RTC_PCF8563 rtc;
Adafruit_SSD1306 display(128, 64, &Wire, -1);

const char* ssid = "WiFi_OBDII";

// UUID сервиса и характеристики (можно сгенерировать свои)
#define SERVICE_UUID "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcdefab-1234-5678-1234-abcdefabcdef"

struct TimeMessage {
    char value[20];
};
QueueHandle_t timeQueue;
QueueHandle_t elmInitQueue;

#define DEBUG_PORT Serial
#define pushButton_pin 13

IPAddress server(192, 168, 0, 10);
// ELMduino busy-waits inside begin(). Yield while waiting for network data
// so the ESP32 idle task (and its watchdog) can run during initialization.
class ElmClient : public WiFiClient {
public:
    int available() override {
        const int bytes = WiFiClient::available();
        if (bytes == 0) vTaskDelay(1);
        return bytes;
    }
};
ElmClient client;

static const unsigned char PROGMEM eng_temp [] PROGMEM = {
    0x01, 0x00, 0x01, 0x00, 0x03, 0xc0, 0x01, 0x00, 0x01, 0x00,
    0x01, 0xc0, 0x01, 0x00, 0x01, 0xc0, 0x01, 0x00, 0x07, 0xc0,
    0xfe, 0xfe, 0xff, 0xfe, 0x03, 0x80, 0x3f, 0xfc, 0x3f, 0xf8
};

static const unsigned char PROGMEM fuel_tank [] PROGMEM = {
    0x1f, 0xe0, 0x1f, 0xe0, 0x18, 0xf8, 0x1f, 0xfc, 0x1f, 0xfc,
    0x18, 0x6c, 0x1b, 0x6c, 0x1f, 0xec, 0x1f, 0xec, 0x1f, 0xec,
    0x1f, 0xfc, 0x1e, 0x7c, 0x18, 0x60, 0x3f, 0xf0, 0x3f, 0xf0
};

typedef enum {
    COOLANT_TEMP,
    VOLTAG,
    FUEL
} obd_pid_states;

obd_pid_states obd_state = COOLANT_TEMP;

float e_temp = 0;
float f_lvl = 0;
float b_volt = 0;
float f_lvl_tmp = 0;

int global_counter = 0;

float fuel_array[10] = {
    0.0, 0.0, 0.0, 0.0, 0.0,
    0.0, 0.0, 0.0, 0.0, 0.0
};

char date_buf[10];
char time_buf[10];

int mode = 0;
int mods_count = 2;
enum ElmState { ELM_IDLE, ELM_WIFI_CONNECTING, ELM_INITIALIZING, ELM_READY, ELM_RETRY };
ElmState elm_state = ELM_IDLE;
constexpr uint32_t WIFI_TIMEOUT_MS = 10000;
constexpr uint32_t ELM_RETRY_MS = 5000;
constexpr uint32_t ELM_DATA_TIMEOUT_MS = 15000;
uint32_t elmStateSince = 0;
uint32_t lastObdSuccess = 0;
uint32_t lastDisplayUpdate = 0;
bool displayDirty = true;
bool coolantValid = false;
bool voltageValid = false;
bool fuelValid = false;


void init_display()
{
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 allocation failed");

        for (;;) ;
    }

    display.clearDisplay();
    display.setCursor(35, 30);
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.println("NEXIA");
    display.display();

    delay(2000);
}


// Only this task accesses the client/library until its result is received.
// ELMduino initialization is blocking; keep it out of the UI loop.
void elm_connect_task(void *)
{
    myELM327.reset(new (std::nothrow) ELM327());
    bool connected = false;
    if (myELM327 && client.connect(server, 35000, 1000)) {
        connected = myELM327->begin(client, false, 2000, '5');
    }
    if (!connected) {
        client.stop();
    }
    xQueueSend(elmInitQueue, &connected, portMAX_DELAY);
    vTaskDelete(nullptr);
}


void retry_elm()
{
    client.stop();
    elm_state = ELM_RETRY;
    elmStateSince = millis();
    coolantValid = voltageValid = fuelValid = false;
    displayDirty = true;
    Serial.println("ELM disconnected; retry in 5 seconds");
}


void init_elm()
{
    const uint32_t now = millis();
    if (elm_state == ELM_INITIALIZING) {
        bool connected;
        if (xQueueReceive(elmInitQueue, &connected, 0) == pdTRUE) {
            if (connected && WiFi.status() == WL_CONNECTED) {
                elm_state = ELM_READY;
                obd_state = COOLANT_TEMP;
                lastObdSuccess = millis();
                displayDirty = true;
                Serial.println("ELM ready");
            } else {
                retry_elm();
            }
        }
        return;
    }
    if (elm_state == ELM_READY) {
        if (WiFi.status() != WL_CONNECTED || !client.connected() ||
            now - lastObdSuccess >= ELM_DATA_TIMEOUT_MS) {
            retry_elm();
        }
        return;
    }
    if (elm_state == ELM_RETRY && now - elmStateSince < ELM_RETRY_MS) {
        return;
    }
    if (elm_state == ELM_IDLE || elm_state == ELM_RETRY) {
        // Start on the first visit to the OBD screen, then maintain the connection.
        if (elm_state == ELM_IDLE && mode != 1) {
            return;
        }
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid);
        elm_state = ELM_WIFI_CONNECTING;
        elmStateSince = now;
        displayDirty = true;
        Serial.println("Connecting to ELM Wi-Fi");
    }
    if (elm_state == ELM_WIFI_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            elm_state = ELM_INITIALIZING;
            if (xTaskCreate(elm_connect_task, "elm-connect", 6144, nullptr, 1, nullptr) != pdPASS) {
                retry_elm();
            }
        } else if (now - elmStateSince >= WIFI_TIMEOUT_MS) {
            WiFi.disconnect();
            retry_elm();
        }
    }
}


void init_rtc()
{
    if (!rtc.begin()) {
        Serial.flush();

        while (1) {
            delay(10);
        }
    }

    if (rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        rtc.start();
    }
}


void draw_stat()
{
    display.clearDisplay();
    display.setTextColor(WHITE);
    if (elm_state != ELM_READY) {
        display.setTextSize(1);
        display.setCursor(0, 20);
        display.println(elm_state == ELM_RETRY ? "ELM unavailable" : "Connecting to ELM...");
        display.println("Clock and BLE active");
        display.display();
        return;
    }

    display.setCursor(30, 0);
    display.setTextSize(2);
    if (voltageValid) display.print(b_volt, 1);
    else display.print("--");
    display.print("V");

    display.drawBitmap(0, 20, fuel_tank, 15, 15, WHITE);

    display.setCursor(25, 20);
    display.setTextSize(2);
    display.setTextColor(WHITE);
    if (fuelValid) display.print(f_lvl, 1);
    else display.print("--");
    display.println(" L");

    display.drawBitmap(0, 40, eng_temp, 15, 15, WHITE);

    display.setCursor(25, 40);
    if (coolantValid) display.print(e_temp, 0);
    else display.print("--");
    display.print(" C");
    display.write(0xF7);

    display.display();
}


void draw_date_time()
{
    DateTime now = rtc.now();

    display.clearDisplay();

    display.setTextSize(4);
    display.setCursor(5, 16);

    snprintf(
        time_buf,
        sizeof(time_buf),
        "%02d:%02d",
        now.hour(),
        now.minute()
    );

    display.println(time_buf);

    display.setCursor(5, 50);
    display.setTextSize(2);

    snprintf(
        date_buf,
        sizeof(date_buf),
        "%02d.%02d.%02d",
        now.day(),
        now.month(),
        now.year() % 100
    );

    display.println(date_buf);
    display.display();
}


void update_f_lvl()
{
    float array_sum = 0;

    if (fuel_array[0] == 0.0 && fuel_array[9] == 0.0) {
        for (int counter = 0; counter < 10; counter++) {
            fuel_array[counter] = f_lvl_tmp;
        }
    } else {
        fuel_array[global_counter] = f_lvl_tmp;

        if (global_counter == 9) {
            global_counter = 0;
        } else {
            global_counter++;
        }
    }

    for (int counter = 0; counter < 10; counter++) {
        array_sum += fuel_array[counter];
    }

    f_lvl = array_sum / 10;
}


void update_fuel()
{
    float fuel_level = myELM327->processPID(34, 4805, 1, 1);

    if (myELM327->nb_rx_state == ELM_SUCCESS) {
        f_lvl_tmp = (fuel_level / 255.0) * 50.0;
        update_f_lvl();
        fuelValid = true;
        lastObdSuccess = millis();
        obd_state = COOLANT_TEMP;
    } else if (myELM327->nb_rx_state != ELM_GETTING_MSG) {
        fuelValid = false;
        myELM327->printError();
        obd_state = COOLANT_TEMP;
    }
}


void update_voltag()
{
    float tmp_b_volt = myELM327->batteryVoltage();

    if (myELM327->nb_rx_state == ELM_SUCCESS) {
        b_volt = tmp_b_volt;
        voltageValid = true;
        lastObdSuccess = millis();
        obd_state = FUEL;
    } else if (myELM327->nb_rx_state != ELM_GETTING_MSG) {
        voltageValid = false;
        myELM327->printError();
        obd_state = FUEL;
    }
}


void update_coolant_temp()
{
    float tmp_e_temp = myELM327->engineCoolantTemp();

    if (myELM327->nb_rx_state == ELM_SUCCESS) {
        e_temp = tmp_e_temp;
        coolantValid = true;
        lastObdSuccess = millis();
        obd_state = VOLTAG;
    } else if (myELM327->nb_rx_state != ELM_GETTING_MSG) {
        coolantValid = false;
        myELM327->printError();
        obd_state = VOLTAG;
    }
}


void update_stats()
{
    switch (obd_state) {
        case COOLANT_TEMP: {
            update_coolant_temp();
            break;
        }

        case VOLTAG: {
            update_voltag();
            break;
        }

        case FUEL: {
            update_fuel();
            break;
        }
    }
}


void changeMod()
{
    static int lastReading = HIGH;
    static int stableState = HIGH;
    static uint32_t changedAt = 0;
    const int reading = digitalRead(pushButton_pin);
    const uint32_t now = millis();
    if (reading != lastReading) {
        changedAt = now;
        lastReading = reading;
    }
    if (now - changedAt >= 40 && reading != stableState) {
        stableState = reading;
        if (stableState == LOW) {
            mode = (mode + 1) % mods_count;
            displayDirty = true;
        }
    }
}


class TimeCallback : public NimBLECharacteristicCallbacks
{
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &) override
    {
        auto value = pChar->getValue();
        if (value.length() == 19) {
            for (size_t i = 0; i < 19; ++i) {
                const char expected = (i == 4 || i == 7) ? '-' :
                                      (i == 10) ? ' ' :
                                      (i == 13 || i == 16) ? ':' : '\0';
                if (expected ? value[i] != expected : (value[i] < '0' || value[i] > '9')) {
                    return;
                }
            }
            TimeMessage message = {};
            memcpy(message.value, value.c_str(), 19);
            xQueueOverwrite(timeQueue, &message);
        }
    }
};

static TimeCallback timeCallback;


void setup()
{
    DEBUG_PORT.begin(115200);

    init_display();
    init_rtc();

    pinMode(pushButton_pin, INPUT_PULLUP);

    timeQueue = xQueueCreate(1, sizeof(TimeMessage));
    elmInitQueue = xQueueCreate(1, sizeof(bool));
    if (!timeQueue || !elmInitQueue) {
        Serial.println("Queue allocation failed");
        ESP.restart();
        return;
    }

    NimBLEDevice::init("NexiaBLE"); // имя, которое увидит iPhone

    NimBLEServer *pServer = NimBLEDevice::createServer();
    pServer->advertiseOnDisconnect(true);

    NimBLEService *pService = pServer->createService(SERVICE_UUID);

    NimBLECharacteristic *pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::WRITE,
        sizeof(TimeMessage::value) - 1
    );

    pCharacteristic->setCallbacks(&timeCallback);

    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();

    pAdvertising->setName("NexiaBLE");
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->enableScanResponse(true);

    NimBLEDevice::startAdvertising();
}


void loop()
{
    changeMod();
    init_elm();
    if (elm_state == ELM_READY) {
        // Finish pending requests even when the user switches to the clock.
        update_stats();
    }

    TimeMessage message;
    if (xQueueReceive(timeQueue, &message, 0) == pdTRUE) {
        // Ожидаем строку вида: "2025-08-21 22:15:00"
        int year, month, day, hour, minute, second;

        if (sscanf(
            message.value,
            "%4d-%2d-%2d %2d:%2d:%2d",
            &year,
            &month,
            &day,
            &hour,
            &minute,
            &second
        ) == 6 && year >= 2000 && year <= 2099 &&
            month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
            hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 &&
            second >= 0 && second <= 59) {
            DateTime newTime(year, month, day, hour, minute, second);
            if (newTime.isValid()) {
                rtc.adjust(newTime);
                displayDirty = true;
            }
        }
    }
    const uint32_t now = millis();
    const uint32_t displayInterval = mode == 0 ? 1000 : 250;
    if (displayDirty || now - lastDisplayUpdate >= displayInterval) {
        if (mode == 0) draw_date_time();
        else draw_stat();
        displayDirty = false;
        lastDisplayUpdate = now;
    }
    delay(1);
}
