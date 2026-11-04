// Tests for the crash-report scrubber (src/port/crash_redact.h): what must go, and what must stay.
// cl /std:c++20 /EHsc /I src tools\recomp\crash_redact_test.cpp
#include "port/crash_redact.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using port::crash::redact;
using port::crash::Scrub;

static int failures = 0;
static void check(const char* what, const std::string& got, const std::string& want) {
    if (got != want) {
        std::printf("FAIL %s\n  got : %s\n  want: %s\n", what, got.c_str(), want.c_str());
        failures++;
    }
}
static void gone(const char* what, const std::string& got, const char* secret) {
    if (got.find(secret) != std::string::npos) {
        std::printf("FAIL %s: still contains \"%s\"\n  %s\n", what, secret, got.c_str());
        failures++;
    }
}

int main() {
    Scrub sc;
    sc.literals = {{"Geart Smith", "<user>"}, {"DESKTOP-ABC123", "<pc>"}, {"Jester", "<name>"}};

    // personal paths
    check("windows path", redact("data folder C:\\Users\\geart\\AppData\\Roaming\\Luigi's Mansion Recompiled (57)", sc),
          "data folder %USERPROFILE%\\AppData\\Roaming\\Luigi's Mansion Recompiled (57)");
    check("forward slashes", redact("C:/Users/geart/Downloads/x.zip", sc), "%USERPROFILE%/Downloads/x.zip");
    check("name with a space", redact("C:\\Users\\Geart Smith\\Desktop", sc), "%USERPROFILE%\\Desktop");
    check("linux", redact("/home/bob/.local/share/x and /Users/alice/Library", sc), "~/.local/share/x and ~/Library");
    check("not a user folder", redact("C:\\Program Files\\LmCoop\\LmCoop.exe", sc), "C:\\Program Files\\LmCoop\\LmCoop.exe");

    // literals
    check("pc name", redact("host DESKTOP-abc123 ok", sc), "host <pc> ok");
    check("player name", redact("[online] partner: Jester", sc), "[online] partner: <name>");
    check("short needle ignored", redact("an ab cd", Scrub{{{"ab", "X"}}}), "an ab cd");

    // addresses
    check("ipv4", redact("connected to 26.77.249.119:28555 and 127.0.0.1", sc), "connected to <ip>:28555 and <ip>");
    check("versions stay", redact("running 1.6.0, driver 32.0.32015.7029, build 10.0.26200", sc), "running 1.6.0, driver 32.0.32015.7029, build 10.0.26200");
    check("not an ip", redact("999.1.1.1 and 1.2.3.4.5", sc), "999.1.1.1 and 1.2.3.4.5");
    check("ipv6", redact("peer fe80::1ff:fe23:4567:890a and 2001:db8::1", sc), "peer <ip> and <ip>");
    check("clock times stay", redact("at 12:34:56 and 01:02", sc), "at 12:34:56 and 01:02");

    // identities and secrets
    check("steam id", redact("Caching Steam ID:  76561198966754591 [API loaded no]", sc), "Caching Steam ID:  <steamid> [API loaded no]");
    check("other 17-digit number stays", redact("value 12345678901234567", sc), "value 12345678901234567");
    check("password", redact("password=hunter2 next", sc), "password=<hidden> next");
    check("password json", redact("{\"password\": \"s3cret\", \"x\": 1}", sc), "{\"password=<hidden>\", \"x\": 1}");
    check("token", redact("Authorization token: abc.def", sc), "Authorization token=<hidden>");
    check("url creds", redact("fetch https://user:pw@example.com/path", sc), "fetch https://<creds>@example.com/path");
    check("turn creds", redact("ice turn:lmcoop:SECRET@relay.example.com:3478", sc), "ice turn:<creds>@relay.example.com:3478");
    check("email", redact("contact me@example.com now", sc), "contact <email> now");
    check("invite code", redact("[online] lobby 5ARG6E: waiting for a player", sc), "[online] lobby <code>: waiting for a player");
    check("invite code line", redact("Invite code: F3ZMWX", sc), "Invite code: <code>");

    // the useful part of a crash survives
    const std::string stack =
        "=== exception 0xC0000005 at 00007FF62EE588D3 (LmCoop.exe+0x1EC88D3), rsp 00000000E56A4000 ===\n"
        "  #0 recomp::online::frame (online.cpp:402)\n[aurora::gx] TEV samples texcoord 0 without a texgen\n";
    check("stack untouched", redact(stack, sc), stack);

    gone("mixed", redact("C:\\Users\\geart\\x 26.1.2.3 password=pw 76561198966754591 Jester me@a.bc", sc), "geart");
    gone("mixed", redact("C:\\Users\\geart\\x 26.1.2.3 password=pw 76561198966754591 Jester me@a.bc", sc), "26.1.2.3");
    gone("mixed", redact("C:\\Users\\geart\\x 26.1.2.3 password=pw 76561198966754591 Jester me@a.bc", sc), "76561198966754591");

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("crash_redact_test: all passed\n");
    return 0;
}
