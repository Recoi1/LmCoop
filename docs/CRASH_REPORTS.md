# Crash reports

When LmCoop crashes, the next start offers to send a crash report to the developer in one click. Players need no
GitHub account.

## What the player sees

1. The game crashes.
2. They start LmCoop again: *"LmCoop crashed last time. Send a crash report to Recoi1?"* with **Send report**,
   **Start normally**, and the existing recovery choices.
3. **Send report** opens a small window for an optional note, then uploads.
4. *"Report sent. Report number: #123"*.

The prompt only offers Send when an inbox is set up (below). Older saved reports that never got through are sent
too, each as its own report.

## What is in a report

* The crashed session's log, with the error and stack (`=== crash: ... ===`); at most the first 6 KB and last 144 KB.
* Game version and build id (the git commit).
* Windows and graphics card, taken from the lines the renderer logs at start.
* Whether the session was hosting or joining, and over what (direct, internet, Steam).
* Active mods: texture packs, loose-file mods, a character pack (name and version).
* The player's optional note.

**Removed before sending** (`src/port/crash_redact.h`, tested by `tools/recomp/crash_redact_test.cpp`): passwords and
tokens, IP addresses (v4 and v6), Steam ids, invite codes, e-mail addresses, credentials in URLs (`user:pass@`,
`turn:user:pass@`), the user name in paths (`C:\Users\<name>` becomes `%USERPROFILE%`), the PC name, the player's chosen
name, and the program's own data folders. Never read at all: memory card saves and game files.

## Keeping reports

`recovery.cpp` calls `crash::preserve` at the start of the next run, **before** that run replaces `previous_run.log`:
the crashed run's log, session facts and reason are copied to `<dataDir>/crash_reports/<time>-<id>/`. A successful
upload adds `sent.txt` (the number); a failed one leaves the folder as it is and it is offered again. The newest 20
folders are kept. The saved copy on the player's PC is the raw log; scrubbing happens only on what is sent.

## The inbox (what you deploy)

`server/crash-inbox/`: a small Cloudflare Worker (free plan) that stores each report and, depending on which secrets are set,
posts it to a private Discord channel (`DISCORD_WEBHOOK`, the log attached, mentions off) and/or opens an issue in a
**private** GitHub repository (`GITHUB_TOKEN` + `GITHUB_REPO`). Secrets live only in the Worker; the game holds no
credential. Live now with Discord only. See `server/crash-inbox/README.md`.

The game finds the Worker's address in `data/crash_inbox.json` (read from GitHub when a player presses Send), so the
inbox can be moved without a release. **Until `url` is filled in, the prompt does not offer to send.**

## Fixing them

New crash issues are grouped by signature and investigated automatically, producing a draft pull request for review:
see CRASH_FIXES.md.

## Testing

* `server/crash-inbox`: `npm test` (the Worker with a fake store and a fake GitHub).
* `tools/recomp/crash_redact_test.cpp`: the scrubber.
* End to end: `node server/crash-inbox/dev-server.js 8790` and start the game with
  `LM_CRASH_ENDPOINT=http://127.0.0.1:8790/report` (plain http is only accepted to this machine). Test aids:
  `LM_CRASH_AUTO=send` sends without any window, `LM_CRASH_DESC` is the note.

## Limits

* Windows only so far (the upload uses WinHTTP; elsewhere the prompt does not offer it).
* Only crashes are reported, not hangs or runs that were just closed.
* The session facts (mods, hosting or joining) are written when the game starts and again once it runs; a crash
  during start-up may carry the earlier snapshot.
