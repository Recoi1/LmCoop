# Automated crash fixes

Crash reports (CRASH_REPORTS.md) arrive as issues in a private GitHub repository. This pipeline investigates each new
crash and, when it is confident, opens a **draft** pull request with a fix. It never merges, releases or edits
anything by itself: a person reads every change.

```
crash issue (private repo)  -->  workflow: crash-fix.yml  -->  briefing  -->  agent (Claude)  -->  gate  -->  draft PR (public repo)
   label sig-<hash>                 check out main + the crashed build       reads and edits       checks it     or: analysis on the issue
```

## Steps

1. **Group.** The inbox gives each report a signature (`sig-<hash>`, `server/crash-inbox/worker.js`): the exception code
   and the faulting function names (stable across builds) or, without symbols, the faulting address and version. The
   same crash from another player becomes a comment on the open issue, not a second issue, so a fix is tried once.
2. **Briefing** (`tools/crash/prepare_context.py`). Build commit, signature, the stack turned into function names with
   the build's debug symbols (`LmCoop-<version>.pdb.zip`, attached to each release) and `llvm-symbolizer`, mods, and the
   end of the log. Everything from the player's PC is marked UNTRUSTED DATA.
3. **Agent** (`tools/crash/fix-prompt.md`). Claude Code, non-interactive, with main checked out and the crashed commit
   next to it. It can only read and edit files: **no shell, no network, no secrets except its own model key**. It writes
   `analysis.md` (private) and, if it changed code, `summary.md` (the pull request text).
4. **Gate** (`tools/crash/check_patch.py`, run from a copy made before the agent started). The change is refused unless:
   only `src/`, `tools/`, `server/`, `docs/` are touched (never `.github/`, packaging, build files, dependencies, the
   crash inbox address, binaries, or the crash tools themselves); at most 8 files and 400 added lines; and no added line
   contains an address, user path, e-mail, Steam id, secret-looking text, or a line copied from the report. The pull
   request text is checked the same way (`--scan-text`); a refused summary is replaced by a plain one.
5. **Result.** Gate passed: a draft PR (`auto-fix` label) in the public repo and a comment on the private issue with the
   analysis. Gate refused or no change: the analysis is posted on the issue with the label `needs-human`.

The PR's own CI build (the normal `Build` workflow) is the compile check; the agent runs nothing.

## Why it is built this way

The report text is written by a stranger's PC. A log line or a note could be crafted to steer a model. So the agent has
nothing to steal or run, and what it leaves behind is judged by a script it cannot edit, then by you.

## Setting it up (once)

You need: the crash inbox deployed (`server/crash-inbox/README.md`), an Anthropic API key, and two repositories (the
private one for reports, the public LmCoop).

1. Private repo: Settings > Secrets and variables > Actions: `ANTHROPIC_API_KEY`, and `LMCOOP_TOKEN` = a fine-grained
   token for **Recoi1/LmCoop only** with Contents read and write, Pull requests read and write.
2. Copy `server/crash-inbox/crash-fix.workflow.yml` to `.github/workflows/crash-fix.yml` in the private repo.
3. Create the labels: `gh label create needs-human -R <private repo>` and `gh label create auto-fix -R Recoi1/LmCoop`.
4. On each release attach the symbols the build produces (the `symbols` artifact of the Windows build):
   `gh release upload v<version> LmCoop-<version>.pdb.zip`. Without them the briefing has raw addresses only and the
   agent is much less likely to find the cause.
5. Try it on one report: Actions > Crash fix (draft) > Run workflow > the issue number.

## What it will and will not do

It can fix a null check, a bad index, an unchecked size, a race it can see in the code. It will not (and is told not to)
guess at a crash inside the game module, a graphics driver or a mod's data: those get an analysis and `needs-human`.
Expect many reports to end that way. A run costs model usage (up to 60 agent turns).

## Tested here, and not

Tested: the grouping and signatures (13 tests), the briefing against the real build's PDB with `llvm-symbolizer`, the gate
(10 cases: allowed change, workflow edit, build-file edit, IP, user path, secret, copied report line, binary, size,
nothing), the text check, and the workflow's YAML. **Not run:** the workflow itself and the agent step, which need your
secrets, and the `claude` command's flags (`-p`, `--allowedTools`, `--permission-mode`, `--add-dir`, `--max-turns`) were
written from memory of its documentation, so check them on the first run.
