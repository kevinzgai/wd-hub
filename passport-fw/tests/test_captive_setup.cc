#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include "apps/home/home_wifi_selection.h"
#include "captive_dns.h"

struct Ap { uint8_t ssid[33]; int8_t rssi; };
Ap ap(const char* name, int rssi) {
    Ap result{};
    std::strncpy(reinterpret_cast<char*>(result.ssid), name, 32);
    result.rssi = rssi;
    return result;
}

int main() {
    const std::vector<SsidItem> saved{{"Absent", "old"}, {"Weak", "weak-key"}, {"Strong", "strong-key"}};
    const std::vector<Ap> aps{ap("Weak", -80), ap("Strong", -60), ap("Strong", -20), ap("Unknown", -10)};
    auto selected = home_wifi_candidates(saved, aps);
    assert(selected.size() == 2 && selected[0].ssid == "Strong" && selected[1].ssid == "Weak");
    assert(selected[0].password == "strong-key");
    selected = home_wifi_candidates(saved, aps, "Absent");
    assert(selected.size() == 1 && selected[0].ssid == "Absent");
    assert(home_wifi_candidates(saved, aps, "Unknown").empty());
    assert(home_wifi_candidates(saved, std::vector<Ap>{}).size() == saved.size());
    assert(home_wifi_candidates(saved, std::vector<Ap>{ap("Unknown", -10)}).size() == saved.size());
    assert(home_wifi_candidates(std::vector<SsidItem>{}, aps).empty());
    const std::string full(32, 's');
    const std::vector<SsidItem> full_saved{{full, "key"}};
    const std::vector<Ap> full_aps{ap(full.c_str(), -30)};
    assert(home_wifi_candidates(full_saved, full_aps).size() == 1);

    const uint8_t query[] = {0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 3, 'o', 's', 'x', 0, 0, 1, 0, 1};
    const uint8_t ip[] = {192, 168, 4, 1};
    std::array<uint8_t, 512> packet{};
    auto reset = [&] { packet.fill(0); std::memcpy(packet.data(), query, sizeof(query)); };
    reset();
    assert(captive_dns_reply(packet.data(), sizeof(query), packet.size(), ip) == sizeof(query) + 16);
    assert(packet[7] == 1 && !std::memcmp(packet.data() + sizeof(query) + 12, ip, 4));
    reset(); packet[18] = 28;
    assert(captive_dns_reply(packet.data(), sizeof(query), packet.size(), ip) == sizeof(query));
    assert(packet[7] == 0 && packet[18] == 28);
    reset(); packet[10] = 0; packet[11] = 1;
    assert(captive_dns_reply(packet.data(), 512, packet.size(), ip) == sizeof(query) + 16);
    assert(packet[11] == 0); // EDNS additions are removed, not copied ahead of answers.
    for (size_t n = 0; n < sizeof(query); ++n) {
        reset(); assert(captive_dns_reply(packet.data(), n, packet.size(), ip) == 0);
    }
    reset(); assert(captive_dns_reply(packet.data(), sizeof(query), sizeof(query) + 15, ip) == 0);
    reset(); packet[12] = 0xc0;
    assert(captive_dns_reply(packet.data(), sizeof(query), packet.size(), ip) == 0);
    reset(); packet[5] = 2;
    assert(captive_dns_reply(packet.data(), sizeof(query), packet.size(), ip) == 0);
    std::cout << "Captive DNS bounds/IPv6 and saved network selection: PASS\n";
}
