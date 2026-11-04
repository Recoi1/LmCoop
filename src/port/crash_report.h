#pragma once
// Crash reports (docs/CRASH_REPORTS.md): when a run crashes, its log and facts are kept as their own folder
// before the next start rotates them away, and the recovery prompt (recovery.cpp) can send them to the
// developer's crash inbox (server/crash-inbox) in one click. A failed upload stays saved and is offered again.
//
// What is sent: the crashed session's log (scrubbed by crash_redact.h) with its error stack, the game version and
// build id, Windows and graphics card, hosting or joining, the active mods (names and versions) and an optional
// description. Never sent: passwords, addresses, personal folders, saves, game files.
#include <filesystem>
#include <string>
#include <vector>

namespace port::crash {

// recomp registers what it knows about a run (the session mode, the mods): "key=value" lines,
// "mod=<kind>|<name>|<version>". Called at start and again once the game is running.
using SessionProvider = std::string (*)();
void setSessionProvider(SessionProvider p);
void writeSession();      // last_run_session.txt from the provider

const char* buildId();    // the git commit this build was made from ("unknown" without one)

// Keeps the crashed run's log and session facts as <dataDir>/crash_reports/<time>-<id>/ (nothing to keep: empty).
std::filesystem::path preserve(const std::string& reason, const std::string& version, const std::string& build);

struct Pending {
    std::filesystem::path dir;
    std::string id;
};
std::vector<Pending> pending();  // saved reports not yet sent, newest first

struct Outcome {
    bool ok = false;
    int number = 0;      // the report number the inbox answered with
    std::string error;
};
Outcome send(const Pending& report, const std::string& description);

// True when the developer has not set up an inbox yet (the address file is readable and empty): the prompt
// then does not offer to send. Offline or unknown counts as set up, so a failed send is kept and retried.
bool notSetUp();
// "" when sending is possible, else why not (no inbox address yet, unsupported system).
std::string unavailableReason();
// A small window for the optional description (Windows). False: cancelled.
bool askDescription(std::string& text);

}  // namespace port::crash
