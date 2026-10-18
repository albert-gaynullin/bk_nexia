# Локальные проверки веб-интерфейса

Тесты компилируют настоящий [DevicePortal.cpp](../bk/DevicePortal.cpp) и
[LocalTime.h](../bk/LocalTime.h) с имитациями HTTP-сервера, RTC, Wi-Fi и записи
flash. Аппаратные API заменяются заголовками из `stubs/`; исходники прошивки
для тестов не копируются и не переписываются.

В Developer PowerShell / Developer Command Prompt для Visual Studio, из корня
репозитория:

```powershell
New-Item -ItemType Directory -Force .test-build | Out-Null
cl /nologo /EHsc /std:c++17 /Itests/stubs /Ibk tests/portal_tests.cpp bk/DevicePortal.cpp /Fe:.test-build/portal_tests.exe /Fo:.test-build/
.\.test-build\portal_tests.exe
```

С GCC или Clang (C++17):

```sh
mkdir -p .test-build
c++ -std=c++17 -Itests/stubs -Ibk tests/portal_tests.cpp bk/DevicePortal.cpp -o .test-build/portal_tests
.test-build/portal_tests
```

JavaScript встроенной страницы проверяется Node.js без дополнительных пакетов:

```sh
node tests/browser_tests.js
```

Проверяются календарные даты и переполнение размера, чтение/установка времени,
ошибки RTC, авторизация и Origin, успешная OTA-запись, обрыв/таймаут,
неполный или слишком большой файл, несколько файлов, ошибки проверки образа,
повторные попытки и перезапуск только после завершения запроса.

Эти тесты проверяют логику обработчиков. Они не проверяют реализацию HTTP,
Wi-Fi, flash и проверку образа в ESP-IDF; для этого нужны сборка под ESP32 и
проверка на устройстве по [OTA.md](../OTA.md).
