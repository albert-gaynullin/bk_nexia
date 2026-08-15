#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include "ELMduino.h"
#include "RTClib.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

ELM327 myELM327;
RTC_PCF8563 rtc;
Adafruit_SSD1306 display(128, 64, &Wire, -1);

const char* ssid = "WiFi_OBDII";

// UUID сервиса и характеристики (можно сгенерировать свои)
#define SERVICE_UUID "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcdefab-1234-5678-1234-abcdefabcdef"

BLECharacteristic *pCharacteristic;
bool newTimeReceived = false;
String receivedTime = "";

#define DEBUG_PORT Serial
#define pushButton_pin 13

IPAddress server(192, 168, 0, 10);
WiFiClient client;

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
int elm_state = 0;

unsigned long button_time = 0;
unsigned long last_button_time = 0;


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


void init_elm()
{
    WiFi.mode(WIFI_AP);
    WiFi.begin(ssid);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }

    if (client.connect(server, 35000)) {
        Serial.println("connected");
        elm_state = 1;
    } else {
        Serial.println("connection failed");

        while (1);
    }

    myELM327.begin(client, false, 2000, '5');
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

    display.setCursor(30, 0);
    display.setTextSize(2);
    display.print(b_volt, 1);
    display.print("V");

    display.drawBitmap(0, 20, fuel_tank, 15, 15, WHITE);

    display.setCursor(25, 20);
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.print(f_lvl, 1);
    display.println(" L");

    display.drawBitmap(0, 40, eng_temp, 15, 15, WHITE);

    display.setCursor(25, 40);
    display.print(e_temp, 0);
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

    sprintf(
        time_buf,
        "%02d:%02d",
        now.hour(),
        now.minute()
    );

    display.println(time_buf);

    display.setCursor(5, 50);
    display.setTextSize(2);

    sprintf(
        date_buf,
        "%02d.%02d.%02d",
        now.day(),
        now.month(),
        now.year()
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
    float fuel_level = myELM327.processPID(34, 4805, 1, 1);

    if (myELM327.nb_rx_state == ELM_SUCCESS) {
        f_lvl_tmp = (fuel_level / 255.0) * 50.0;
        update_f_lvl();
        obd_state = COOLANT_TEMP;
    } else if (myELM327.nb_rx_state != ELM_GETTING_MSG) {
        myELM327.printError();
        obd_state = COOLANT_TEMP;
    }
}


void update_voltag()
{
    float tmp_b_volt = myELM327.batteryVoltage();

    if (myELM327.nb_rx_state == ELM_SUCCESS) {
        b_volt = tmp_b_volt;
        obd_state = FUEL;
    } else if (myELM327.nb_rx_state != ELM_GETTING_MSG) {
        myELM327.printError();
        obd_state = FUEL;
    }
}


void update_coolant_temp()
{
    float tmp_e_temp = myELM327.engineCoolantTemp();

    if (myELM327.nb_rx_state == ELM_SUCCESS) {
        e_temp = (uint32_t)tmp_e_temp;
        obd_state = VOLTAG;
    } else if (myELM327.nb_rx_state != ELM_GETTING_MSG) {
        myELM327.printError();
        obd_state = VOLTAG;
    }
}


void update_stats()
{
    switch (obd_state) {
        case COOLANT_TEMP: {
            update_coolant_temp();
            draw_stat();
            break;
        }

        case VOLTAG: {
            update_voltag();
            draw_stat();
            break;
        }

        case FUEL: {
            update_fuel();
            draw_stat();
            break;
        }
    }
}


void changeMod()
{
    button_time = millis();

    if (button_time - last_button_time > 500) {
        if (mode < mods_count - 1) {
            mode++;
        } else {
            mode = 0;
        }

        last_button_time = button_time;
    }
}


class TimeCallback : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pChar)
    {
        receivedTime = String(pChar->getValue().c_str());

        if (receivedTime.length() > 0) {
            newTimeReceived = true;
        }
    }
};


void setup()
{
    DEBUG_PORT.begin(115200);

    init_display();
    init_rtc();

    pinMode(pushButton_pin, INPUT_PULLUP);

    attachInterrupt(pushButton_pin, changeMod, FALLING);

    //init_elm();

    BLEDevice::init("NexiaBLE"); // имя, которое увидит iPhone

    BLEServer *pServer = BLEDevice::createServer();

    BLEService *pService = pServer->createService(SERVICE_UUID);

    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );

    pCharacteristic->setCallbacks(new TimeCallback());

    pService->start();

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();

    pAdvertising->addServiceUUID(SERVICE_UUID);

    BLEDevice::startAdvertising();
}


void loop()
{
    if (mode == 0) {
        draw_date_time();
        delay(1000);
    } else if (mode == 1) {
        if (elm_state == 1) {
            update_stats();
        } else {
            init_elm();
        }
    }

    if (newTimeReceived) {
        newTimeReceived = false;

        // Ожидаем строку вида: "2025-08-21 22:15:00"
        int year, month, day, hour, minute, second;

        if (sscanf(
            receivedTime.c_str(),
            "%d-%d-%d %d:%d:%d",
            &year,
            &month,
            &day,
            &hour,
            &minute,
            &second
        ) == 6) {
            rtc.adjust(
                DateTime(
                    year,
                    month,
                    day,
                    hour,
                    minute,
                    second
                )
            );

            draw_date_time();
        }
    }
}