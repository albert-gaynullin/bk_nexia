#include "DevicePortal.h"
#include "LocalTime.h"
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <new>

namespace {
constexpr uint32_t IDLE_TIMEOUT_MS = 10 * 60 * 1000;
const char *PORTAL_HEADERS[] = {"Origin"};
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="ru"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Nexia</title>
<style>body{font:17px system-ui;max-width:600px;margin:32px auto;padding:0 18px;color:#182630;background:#f3f6f8}
section{background:white;border-radius:14px;padding:20px;margin:18px 0}button,input{font:inherit;max-width:100%;margin:8px 0}
button{padding:10px 16px;border:0;border-radius:8px;background:#176a86;color:white}#result{white-space:pre-wrap}</style>
<h1>Nexia</h1><p>Настройка времени и обновление прошивки</p>
<p>Через 10 минут бездействия устройство вернётся к часам.</p>
<section><h2>Дата и время</h2><p id="clock">Чтение часов…</p>
<button id="sync" onclick="syncTime()">Установить время телефона / ПК</button>
<p>Будет записано местное время этого устройства. Проверьте его часы и часовой пояс.</p>
<p id="result" role="status"></p></section>
<section><h2>Обновление прошивки</h2><p>Выберите только файл приложения <b>bk.ino.bin</b>.
Не отключайте питание во время записи.</p>
<form method="post" action="/update" enctype="multipart/form-data" onsubmit="return prepare(this)">
<input type="file" name="firmware" accept=".bin" required><br><button>Загрузить прошивку</button></form>
<p>После успешной проверки устройство перезапустится. Загрузка прошивки не меняет таблицу разделов.</p></section>
<button onclick="restart()">Вернуться к часам</button>
<script>
const pad=n=>String(n).padStart(2,'0');
function stamp(d){return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`}
async function readClock(){try{const r=await fetch('/time');document.getElementById('clock').textContent=await r.text()}
catch(e){document.getElementById('clock').textContent='Нет связи с устройством'}}
async function syncTime(){const b=document.getElementById('sync');b.disabled=true;
try{const r=await fetch('/time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'time='+encodeURIComponent(stamp(new Date()))});
document.getElementById('result').textContent=await r.text();if(r.ok)await readClock()}
catch(e){document.getElementById('result').textContent='Нет связи: проверьте подключение к Wi-Fi'}finally{b.disabled=false}}
function prepare(form){const f=form.elements.firmware.files[0];if(!f)return false;
if(!/\.bin$/i.test(f.name)||/\.(merged|bootloader|partitions)\.bin$/i.test(f.name)){alert('Нужен файл приложения bk.ino.bin');return false}
form.action='/update?size='+f.size;form.querySelector('button').disabled=true;return true}
async function restart(){try{const r=await fetch('/restart',{method:'POST'});alert(await r.text())}catch(e){alert('Проверьте подключение к Wi-Fi')}}
readClock();</script></html>)HTML";
}

bool DevicePortal::begin(bool rtcReady, void (*onStatus)())
{
    _rtcReady = rtcReady;
    _onStatus = onStatus;
    _lastActivity = millis();
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0))) {
        notify("AP ERROR");
        return false;
    }
    const uint64_t chip = ESP.getEfuseMac();
    snprintf(_ssid, sizeof(_ssid), "Nexia-Setup-%04X", static_cast<unsigned>(chip & 0xFFFF));
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    for (size_t i = 0; i < sizeof(_password) - 1; ++i) {
        _password[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    if (!WiFi.softAP(_ssid, _password, 1, false, 1)) {
        notify("AP ERROR");
        return false;
    }
    _origin = "http://" + WiFi.softAPIP().toString();
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (next && running && next->address != running->address) _maxSize = next->size;
    _server.reset(new (std::nothrow) WebServer(80));
    if (!_server) {
        notify("SERVER ERROR");
        return false;
    }
    _server->collectHeaders(PORTAL_HEADERS, 1);
    _server->on("/", HTTP_GET, [this]() {
        if (!authorize()) return;
        _server->sendHeader("Cache-Control", "no-store");
        _server->send_P(200, "text/html; charset=utf-8", PAGE);
    });
    _server->on("/time", HTTP_GET, [this]() { serveTime(); });
    _server->on("/time", HTTP_POST, [this]() { setTime(); });
    _server->on("/restart", HTTP_POST, [this]() {
        if (!authorize(true)) return;
        _server->send(200, "text/plain; charset=utf-8", "Перезапуск. Отпустите кнопку устройства.");
        _rebootPending = true;
        _rebootSince = millis();
        notify("RESTARTING");
    });
    _server->on("/update", HTTP_POST, [this]() { finishUpload(); }, [this]() { handleUpload(); });
    _server->onNotFound([this]() { _server->send(404, "text/plain", "Not found"); });
    _server->begin();
    notify(_maxSize ? "READY" : "NO OTA SLOT");
    return true;
}

bool DevicePortal::authorize(bool checkOrigin)
{
    if (!_server->authenticate("admin", _password)) {
        _server->requestAuthentication();
        return false;
    }
    if (checkOrigin && _server->header("Origin") != _origin) {
        _server->send(403, "text/plain; charset=utf-8", "Откройте страницу по адресу с OLED и повторите действие.");
        return false;
    }
    if (_rebootPending) {
        _server->send(409, "text/plain; charset=utf-8", "Устройство перезапускается.");
        return false;
    }
    _lastActivity = millis();
    return true;
}

void DevicePortal::serveTime()
{
    if (!authorize()) return;
    _server->sendHeader("Cache-Control", "no-store");
    if (!_rtcReady) {
        _server->send(503, "text/plain; charset=utf-8", "RTC не найден. Обновление прошивки доступно.");
        return;
    }
    const DateTime now = _rtc.now();
    if (!now.isValid()) {
        _server->send(503, "text/plain; charset=utf-8", "Время RTC недостоверно. Установите время телефона.");
        return;
    }
    char value[20];
    snprintf(value, sizeof(value), "%04d-%02d-%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
    _server->send(200, "text/plain; charset=utf-8", value);
}

void DevicePortal::setTime()
{
    if (!authorize(true)) return;
    if (!_rtcReady) {
        _server->send(503, "text/plain; charset=utf-8", "RTC не найден: проверьте подключение часов.");
        return;
    }
    const String text = _server->arg("time");
    LocalTimeParts value;
    if (!parseLocalTime(text.c_str(), text.length(), value)) {
        _server->send(400, "text/plain; charset=utf-8", "Неверная дата. Допустимы годы 2000–2099 и формат YYYY-MM-DD HH:MM:SS.");
        return;
    }
    const DateTime requested(value.year, value.month, value.day, value.hour, value.minute, value.second);
    _rtc.adjust(requested);
    _rtc.start();
    const DateTime actual = _rtc.now();
    if (!actual.isValid() || actual.unixtime() - requested.unixtime() > 2) {
        _server->send(500, "text/plain; charset=utf-8", "Не удалось подтвердить запись в RTC. Проверьте подключение часов.");
        return;
    }
    _server->send(200, "text/plain; charset=utf-8", "Время записано в RTC.");
    notify("TIME SET");
}

void DevicePortal::notify(const char *status)
{
    _status = status;
    if (_onStatus) _onStatus();
}

void DevicePortal::fail(const char *message)
{
    if (Update.isRunning()) Update.abort();
    _error = message;
    _fileComplete = false;
    notify("UPLOAD ERROR");
}

void DevicePortal::resetRequest()
{
    _fileSeen = false;
    _fileComplete = false;
    _uploadAuthorized = false;
}

void DevicePortal::handleUpload()
{
    HTTPUpload &upload = _server->upload();
    if (upload.status == UPLOAD_FILE_START) {
        if (_fileSeen) {
            fail("За один запрос можно загрузить только один файл.");
            return;
        }
        _fileSeen = true;
        _fileComplete = false;
        _error = nullptr;
        _received = _expected = 0;
        // Do not send an HTTP response while the multipart body is being parsed.
        _uploadAuthorized = _server->authenticate("admin", _password) &&
                            _server->header("Origin") == _origin && !_rebootPending;
        if (!_uploadAuthorized) return;
        _lastActivity = millis();
        String filename = upload.filename;
        filename.toLowerCase();
        if (upload.name != "firmware" || !filename.endsWith(".bin") ||
            filename.endsWith(".merged.bin") || filename.endsWith(".bootloader.bin") ||
            filename.endsWith(".partitions.bin")) {
            fail("Выберите файл приложения bk.ino.bin.");
            return;
        }
        if (!_maxSize) {
            fail("Нет отдельного OTA-раздела. Требуется прошивка по USB с таблицей разделов для OTA.");
            return;
        }
        // The query string is available before multipart form fields are parsed.
        const String size = _server->arg("size");
        if (!parseFirmwareSize(size.c_str(), size.length(), _maxSize, _expected) || _expected < 32) {
            fail("Недопустимый размер файла: проверьте, помещается ли он в OTA-раздел.");
            return;
        }
        if (!Update.begin(_expected, U_FLASH)) {
            fail("Не удалось начать запись прошивки.");
            return;
        }
        notify("UPLOADING");
    } else if (upload.status == UPLOAD_FILE_WRITE && _uploadAuthorized && !_error) {
        if (!Update.isRunning() || upload.currentSize > _expected - _received ||
            Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            fail("Ошибка записи: неподходящий образ, неверный размер или ошибка flash.");
            return;
        }
        _received += upload.currentSize;
        _lastActivity = millis();
        if (_lastActivity - _lastProgress >= 250) {
            _lastProgress = _lastActivity;
            notify("UPLOADING");
        }
    } else if (upload.status == UPLOAD_FILE_END && _uploadAuthorized && !_error) {
        _fileComplete = _received == _expected && upload.totalSize == _expected;
        if (!_fileComplete) fail("Файл получен не полностью. Повторите загрузку.");
        // Activate only in the final POST handler, after the entire request is parsed.
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        fail("Передача прервана. Повторите загрузку.");
        resetRequest();
    }
}

void DevicePortal::finishUpload()
{
    if (!authorize(true)) {
        if (Update.isRunning()) Update.abort();
        resetRequest();
        return;
    }
    if (!_fileSeen || !_fileComplete || !_uploadAuthorized || _error) {
        const char *message = _error ? _error : "Прошивка не получена. Выберите файл и повторите загрузку.";
        fail(message);
        _server->send(400, "text/plain; charset=utf-8", message);
    } else if (!Update.end()) {
        fail("Проверка образа не пройдена. Устройство продолжит работу с прежней прошивкой.");
        _server->send(400, "text/plain; charset=utf-8", _error);
    } else {
        _server->sendHeader("Connection", "close");
        _server->send(200, "text/plain; charset=utf-8", "Прошивка проверена и записана. Устройство перезапускается; отпустите кнопку.");
        _rebootPending = true;
        _rebootSince = millis();
        notify("RESTARTING");
    }
    resetRequest();
}

void DevicePortal::loop()
{
    if (_server) _server->handleClient();
    // WebServer parses a whole multipart request synchronously. A running Update
    // here means parsing returned without the final handler (e.g. a disconnect).
    // Never carry a partially parsed request over to the next HTTP request.
    if (Update.isRunning()) {
        fail("Запрос загрузки не завершён. Повторите загрузку.");
        resetRequest();
    }
    const uint32_t now = millis();
    if (_rebootPending && now - _rebootSince >= 1000) ESP.restart();
    if (!_rebootPending && !Update.isRunning() && now - _lastActivity >= IDLE_TIMEOUT_MS) ESP.restart();
}
