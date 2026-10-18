#include "DevicePortal.h"
#include "LocalTime.h"
#include "TestPlatform.h"
#include <cassert>
#include <iostream>

struct Fixture {
    RTC_PCF8563 rtc;
    DevicePortal portal{rtc};
    Fixture(bool rtcReady=true) {
        testMillis=0;ESP={};WiFi={};Update={};partitionPresent=true;
        nextPartition={0x150000,1310720};runningPartition={0x10000,1310720};
        assert(portal.begin(rtcReady,nullptr));
    }
    WebServer &web() { return *WebServer::instance; }
    void start(const char *size="64") { web().args["size"]=size;web().chunk(UPLOAD_FILE_START); }
    void complete() { web().chunk(UPLOAD_FILE_WRITE,64);web().chunk(UPLOAD_FILE_END); }
    void finish() { web().request("/update",HTTP_POST); }
};

int main()
{
    LocalTimeParts time{2001,1,1,0,0,0};
    for (const char *valid : {"2000-02-29 00:00:00","2028-02-29 12:34:56","2099-12-31 23:59:59"}) {
        assert(parseLocalTime(valid,strlen(valid),time));
    }
    for (const char *bad : {"","1999-12-31 00:00:00","2100-01-01 00:00:00","2026-02-29 00:00:00",
         "2026-04-31 00:00:00","2026-00-01 00:00:00","2026-01-00 00:00:00","2026-13-01 00:00:00",
         "2026-01-01 24:00:00","2026-01-01 00:60:00","2026-01-01 00:00:60","2026/01/01 00:00:00",
         "2026-01-01T00:00:00","2026-01-01 0:00:00","2026-01-01 00:00:00Z"}) {
        assert(!parseLocalTime(bad,strlen(bad),time));
    }
    uint32_t size=0;
    assert(parseFirmwareSize("1310720",7,1310720,size)&&size==1310720);
    assert(parseFirmwareSize("4294967295",10,UINT32_MAX,size)&&size==UINT32_MAX);
    for (const char *bad : {"","0","-1","1310721","123x","4294967296","99999999999999999999"}) {
        assert(!parseFirmwareSize(bad,strlen(bad),1310720,size));
    }
    {
        Fixture f;assert(strlen(f.portal.password())==12);assert(WiFi.clients==1);
        f.web().request("/",HTTP_GET);assert(f.web().response==200);
        f.web().args["time"]="2028-02-29 12:34:56";f.web().request("/time",HTTP_POST);
        assert(f.web().response==200&&f.rtc.writes==1&&f.rtc.value.year()==2028);
        f.web().request("/time",HTTP_GET);assert(f.web().body=="2028-02-29 12:34:56");
        f.web().args["time"]="2026-02-29 12:34:56";f.web().request("/time",HTTP_POST);
        assert(f.web().response==400&&f.rtc.writes==1);
        f.rtc.writeWorks=false;f.web().args["time"]="2029-01-01 00:00:00";
        f.web().request("/time",HTTP_POST);assert(f.web().response==500);
    }
    {
        Fixture f(false);f.web().args["time"]="2028-02-29 12:34:56";
        f.web().request("/time",HTTP_POST);assert(f.web().response==503&&f.rtc.writes==0);
        f.start();f.complete();f.finish();assert(Update.activated); // OTA works without RTC.
    }
    {
        Fixture f;f.web().authenticated=false;f.start();f.complete();f.finish();
        assert(f.web().response==401&&Update.begins==0&&!Update.activated);
        f.web().args["time"]="2028-02-29 12:34:56";f.web().request("/time",HTTP_POST);
        assert(f.web().response==401&&f.rtc.writes==0);
    }
    {
        Fixture f;f.web().origin="http://other.example";f.start();f.complete();f.finish();
        assert(f.web().response==403&&Update.begins==0&&!Update.activated);
        f.web().request("/restart",HTTP_POST);assert(f.web().response==403&&ESP.restarts==0);
    }
    {
        Fixture f;f.start();f.complete();assert(Update.running&&Update.ends==0&&!Update.activated);
        f.finish();assert(f.web().response==200&&Update.ends==1&&Update.activated&&ESP.restarts==0);
        testMillis=999;f.portal.loop();assert(ESP.restarts==0);
        testMillis=1000;f.portal.loop();assert(ESP.restarts==1);
    }
    {
        Fixture f;f.start();f.web().chunk(UPLOAD_FILE_WRITE,32);f.web().chunk(UPLOAD_FILE_END);f.finish();
        assert(f.web().response==400&&Update.ends==0&&!Update.activated&&!Update.running);
        f.start();f.complete();f.finish();assert(Update.activated); // Retry after incomplete upload.
    }
    {
        Fixture f;f.start();f.web().chunk(UPLOAD_FILE_WRITE,65);f.finish();
        assert(f.web().response==400&&!Update.activated&&Update.aborts==1);
    }
    {
        Fixture f;f.start();f.web().chunk(UPLOAD_FILE_WRITE,32);f.web().chunk(UPLOAD_FILE_ABORTED);
        assert(!Update.running&&!Update.activated);
        f.start();f.complete();f.finish();assert(Update.activated); // Retry after disconnect.
    }
    {
        Fixture f;f.start();f.complete();f.web().chunk(UPLOAD_FILE_START);f.finish();
        assert(f.web().response==400&&Update.ends==0&&!Update.activated); // Multiple files.
    }
    {
        Fixture f;Update.imageValid=false;f.start();f.complete();f.finish();
        assert(f.web().response==400&&!Update.activated&&ESP.restarts==0);
    }
    {
        Fixture f;Update.beginWorks=false;f.start();f.finish();
        assert(f.web().response==400&&!Update.activated&&ESP.restarts==0);
    }
    {
        Fixture f;Update.writeWorks=false;f.start();f.complete();f.finish();
        assert(f.web().response==400&&!Update.activated&&!Update.running);
    }
    for (const char *filename : {"bk.ino.merged.bin","bk.ino.bootloader.bin","bk.ino.partitions.bin","bk.ino.elf","bk.ino.bin.gz"}) {
        Fixture f;f.web().file.filename=filename;f.start();f.finish();assert(Update.begins==0&&!Update.activated);
    }
    for (const char *badSize : {"0","31","1310721","-1","999999999999999999999"}) {
        Fixture f;f.start(badSize);f.finish();assert(Update.begins==0&&!Update.activated);
    }
    {
        Fixture f;partitionPresent=false; // Recheck with a fresh portal without a second partition.
        RTC_PCF8563 rtc;DevicePortal unavailable(rtc);assert(unavailable.begin(true,nullptr));
        f.start();f.finish();assert(Update.begins==0&&!Update.activated);
    }
    {
        Fixture f;f.finish();assert(f.web().response==400&&Update.ends==0&&ESP.restarts==0);
    }
    {
        Fixture f;f.start();f.web().chunk(UPLOAD_FILE_WRITE,32);testMillis=30000;f.portal.loop();
        assert(!Update.running&&!Update.activated&&ESP.restarts==0);
        f.start();f.complete();f.finish();assert(Update.activated);
    }
    {
        Fixture f;f.start();f.complete();f.portal.loop(); // Parser returned without final POST handler.
        assert(!Update.running&&!Update.activated);
        f.finish();assert(f.web().response==400&&Update.ends==0); // A new empty request cannot activate it.
        f.start();f.complete();f.finish();assert(Update.activated);
    }
    {
        Fixture f;testMillis=UINT32_MAX-1000;f.web().request("/",HTTP_GET);
        testMillis=598998;f.portal.loop();assert(ESP.restarts==0);
        testMillis=598999;f.portal.loop();assert(ESP.restarts==1); // Idle timer across millis rollover.
    }
    {
        Fixture f;testMillis=600000;f.portal.loop();assert(ESP.restarts==1); // Idle exit.
    }
    {
        Fixture f;f.web().request("/restart",HTTP_POST);assert(f.web().response==200);
        testMillis=1000;f.portal.loop();assert(ESP.restarts==1);
    }
    std::cout << "PASS: calendar, size overflow, time read/write, RTC failures, auth, origin, OTA success, incomplete/oversized/aborted/multiple/invalid files, retry, timeouts, deferred reboot\n";
}
