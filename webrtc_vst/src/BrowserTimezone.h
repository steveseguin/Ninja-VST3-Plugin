#pragma once
#include <algorithm>
#include <ctime>

namespace webrtc_vst {
inline int browserTimezoneOffset() {
    const auto westOffset = [](std::time_t instant, int* daylight) {
        std::tm local{}, utc{};
#ifdef _WIN32
        localtime_s(&local, &instant);
        gmtime_s(&utc, &instant);
#else
        localtime_r(&instant, &local);
        gmtime_r(&instant, &utc);
#endif
        if (daylight) *daylight = local.tm_isdst > 0;
        int diff = (local.tm_hour - utc.tm_hour) * 60 + local.tm_min - utc.tm_min;
        const int days = local.tm_yday - utc.tm_yday;
        if (days == 1 || days < -1) diff += 1440;
        else if (days == -1 || days > 1) diff -= 1440;
        return -diff;
    };
    int daylight = 0;
    const int current = westOffset(std::time(nullptr), &daylight);
    // lib.js getTimezone uses February 1900 and 1906. Windows CRT cannot
    // convert those dates, so retain the OS standard offset in that case.
    int standard = current + (daylight ? 60 : 0);
    bool valid = false;
    for (const int year : {1900, 1906}) {
        std::tm date{};
        date.tm_year = year - 1900;
        date.tm_mon = 1;
        date.tm_mday = 1;
        date.tm_isdst = -1;
        const auto instant = std::mktime(&date);
        if (instant == std::time_t(-1)) continue;
        const auto offset = westOffset(instant, nullptr);
        standard = valid ? std::max(standard, offset) : offset;
        valid = true;
    }
    return current < standard ? current + 60 : current;
}
}
