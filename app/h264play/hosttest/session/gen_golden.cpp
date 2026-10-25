// XCloud360 stage 3 — gera os corpos/cabeçalhos "golden" exatamente como o
// green-nx (src/core/session.cpp, tier P720 = o que o capture-rtp usa) os
// serializa com nlohmann::json (std::map => chaves em ordem alfabética).
// O teste do host (session_test.c) compara o C do Xbox byte a byte com isto.
// Construção dos objetos copiada de ~/green-nx/src/core/session.cpp (GPL-3.0).
// Uso: g++ -std=c++17 -I ~/green-nx/vendor gen_golden.cpp -o gen && ./gen > golden.h
#include <cstdio>
#include <string>
#include <vector>
#include "json.hpp"
using nlohmann::json;

static void out(const char *name, const std::string &s) {
    std::printf("#define GOLDEN_%s \"", name);
    for (char c : s) {
        if (c == '\\') std::printf("\\\\");
        else if (c == '"') std::printf("\\\"");
        else if (c == '\r') std::printf("\\r");
        else if (c == '\n') std::printf("\\n");
        else std::putchar(c);
    }
    std::printf("\"\n");
}

int main() {
    std::printf("/* AUTO-GERADO por gen_golden.cpp (nlohmann::json do green-nx). Nao editar. */\n");
    const char *os = "android";  // QualityTier::P720
    json info = {
        {"appInfo",
         {{"env",
           {{"clientAppId", "www.xbox.com"},
            {"clientAppType", "browser"},
            {"clientAppVersion", "26.1.97"},
            {"clientSdkVersion", "10.3.7"},
            {"httpEnvironment", "prod"},
            {"sdkInstallId", ""}}}}},
        {"dev",
         {{"hw", {{"make", "Microsoft"}, {"model", "unknown"}, {"sdktype", "web"}}},
          {"os", {{"name", os}, {"ver", "22631.2715"}, {"platform", "desktop"}}},
          {"displayInfo",
           {{"dimensions", {{"widthInPixels", 1280}, {"heightInPixels", 720}}},
            {"pixelDensity", {{"dpiX", 1}, {"dpiY", 1}}}}},
          {"browser", {{"browserName", "chrome"}, {"browserVersion", "140.0.3485.54"}}}}},
    };
    out("DEVICE_INFO", "X-MS-Device-Info: " + info.dump());

    json play = {
        {"clientSessionId", ""},
        {"titleId", "FORZAHORIZON5"},
        {"systemUpdateGroup", ""},
        {"settings",
         {{"nanoVersion", "V3;WebrtcTransport.dll"},
          {"enableOptionalDataCollection", false},
          {"enableTextToSpeech", false},
          {"highContrast", 0},
          {"locale", "en-US"},
          {"useIceConnection", false},
          {"timezoneOffsetMinutes", 120},
          {"sdkType", "web"},
          {"osName", os}}},
        {"serverId", ""},
        {"fallbackRegionNames", json::array()},
    };
    out("PLAY", play.dump());

    out("CONNECT", json{{"userToken", "PASSPORT-FAKE"}}.dump());

    std::string offer = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\na=ice-ufrag:abCD\r\na=setup:actpass\r\n";
    json sdp = {
        {"messageType", "offer"},
        {"sdp", offer},
        {"requestId", "1"},
        {"configuration",
         {{"chatConfiguration",
           {{"bytesPerSample", 2},
            {"expectedClipDurationMs", 20},
            {"format", {{"codec", "opus"}, {"container", "webm"}}},
            {"numChannels", 1},
            {"sampleFrequencyHz", 24000}}},
          {"chat", {{"minVersion", 1}, {"maxVersion", 1}}},
          {"control", {{"minVersion", 1}, {"maxVersion", 3}}},
          {"input", {{"minVersion", 1}, {"maxVersion", 9}}},
          {"message", {{"minVersion", 1}, {"maxVersion", 1}}},
          {"reliableinput", {{"minVersion", 9}, {"maxVersion", 9}}},
          {"unreliableinput", {{"minVersion", 9}, {"maxVersion", 9}}}}},
    };
    out("SDP", sdp.dump());

    std::vector<std::string> cands = {
        "candidate:1 1 UDP 2122317823 192.168.15.60 50000 typ host",
        "candidate:2 1 UDP 1686052607 200.100.50.25 50000 typ srflx raddr 192.168.15.60 rport 50000"};
    std::string ufrag = "abCD";
    json list = json::array();
    for (const std::string &c : cands) {
        json entry = {{"candidate", c}, {"sdpMid", "0"}, {"sdpMLineIndex", 0}, {"usernameFragment", ufrag}};
        list.push_back(entry.dump());
    }
    list.push_back(json{{"candidate", "a=end-of-candidates"}, {"sdpMid", "0"}, {"sdpMLineIndex", 0},
                        {"usernameFragment", ufrag}}.dump());
    out("ICE", json{{"messageType", "iceCandidate"}, {"candidate", list}}.dump());
    return 0;
}
