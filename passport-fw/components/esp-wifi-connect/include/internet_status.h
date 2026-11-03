#pragma once
enum class InternetStatus { Offline, Checking, Online, CaptivePortal, NoInternet };
inline InternetStatus wifi_http_status(int status, bool content) {
    if (status == 204) return InternetStatus::Online;
    if ((status >= 300 && status < 400) || (status == 200 && content)) return InternetStatus::CaptivePortal;
    return InternetStatus::NoInternet;
}
