#pragma once

#include <stddef.h>
#include <stdint.h>

struct LocalTimeParts {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
};

inline bool parseLocalTime(const char *text, size_t length, LocalTimeParts &result)
{
    if (!text || length != 19) return false;
    for (size_t i = 0; i < length; ++i) {
        const char separator = (i == 4 || i == 7) ? '-' :
                               (i == 10) ? ' ' :
                               (i == 13 || i == 16) ? ':' : '\0';
        if (separator ? text[i] != separator : (text[i] < '0' || text[i] > '9')) return false;
    }
    const auto pair = [text](size_t offset) -> uint8_t {
        return (text[offset] - '0') * 10 + text[offset + 1] - '0';
    };
    LocalTimeParts value = {
        static_cast<uint16_t>(pair(0) * 100 + pair(2)),
        pair(5), pair(8), pair(11), pair(14), pair(17)
    };
    if (value.year < 2000 || value.year > 2099 || value.month < 1 || value.month > 12 ||
        value.hour > 23 || value.minute > 59 || value.second > 59) return false;
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint8_t maxDay = days[value.month - 1];
    if (value.month == 2 && value.year % 4 == 0) ++maxDay; // Valid range is 2000–2099.
    if (value.day < 1 || value.day > maxDay) return false;
    result = value;
    return true;
}

inline bool parseFirmwareSize(const char *text, size_t length, uint32_t limit, uint32_t &result)
{
    if (!text || !length || length > 10) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const uint8_t digit = text[i] - '0';
        if (value > limit / 10 || (value == limit / 10 && digit > limit % 10)) return false;
        value = value * 10 + digit;
    }
    if (!value) return false;
    result = value;
    return true;
}
