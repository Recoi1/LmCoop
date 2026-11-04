# LmCoop crash inbox

Receives crash reports from the game and files each one as an issue in a **private** GitHub repository you
review. Players send with one click and need no GitHub account. Runs free on Cloudflare Workers.

```
game  --POST /report (JSON, already scrubbed)-->  this Worker  --GitHub REST (token stays here)-->  private repo issue
                                                       |
                                                       +-- full report kept in KV for 90 days
```

## What is in a report

Log of the crashed session and its error stack, game version and build id, Windows and graphics card, whether
hosting or joining (and over what), active mods (name, version), an optional description. The game removes
passwords, IP addresses, Steam ids, invite codes, user names and personal folders before sending; saves and game
files are never read. See `src/port/crash_report.cpp`.

## Deploy (once)

1. Make a private repo for the issues, for example `Recoi1/lmcoop-crash-reports`.
2. GitHub > Settings > Developer settings > Fine-grained tokens: access to **only that repo**, permission
   **Issues: read and write**, nothing else.
3. In this folder:
   ```
   npx wrangler login
   npx wrangler kv namespace create REPORTS      # paste the id into wrangler.toml
   npx wrangler secret put GITHUB_TOKEN          # paste the token here, and only here
   npx wrangler deploy
   ```
4. Put the Worker's address in `data/crash_inbox.json` of the game repo (`{"url": "https://<name>.<you>.workers.dev/report"}`).
   The game reads that file from GitHub when a player presses Send, so no new release is needed to move the inbox.

The token lives in the Worker secret only. It is not in the game, in the repo or in `wrangler.toml`.

## Limits and safety

* 300 KB per report, 8 reports per address per hour, `schema: 1` only; unknown fields are dropped.
* The log is written into a fenced code block that it cannot close, so a report cannot inject markdown.
* A retry of a report already stored gets its number back and creates nothing twice (the game keeps failed uploads
  and retries them).
* If GitHub is unreachable the report is still stored and the player still gets a number.

## Test

`npm test` (the Worker's logic with a fake store and a fake GitHub). `node dev-server.js 8790` runs the same code
locally without GitHub: start the game with `LM_CRASH_ENDPOINT=http://127.0.0.1:8790/report`.
