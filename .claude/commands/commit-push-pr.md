---
allowed-tools: Bash(git status:*), Bash(git diff:*), Bash(git add:*), Bash(git commit:*), Bash(git branch:*), Bash(git bundle:*)
description: Commit work in this fork and refresh its bundle (does NOT push to upstream OrcaSlicer)
---

## ⛔ Read this before doing anything

**`origin` in this repo is `github.com/OrcaSlicer/OrcaSlicer` — the public upstream project,
not Petko's.** The previous version of this command ran `git push origin` and `gh pr create`,
which would have opened a public pull request against upstream OrcaSlicer from a personal fork,
with all confirmation text suppressed. Do not do that.

| Remote | Points at | Push? |
|---|---|---|
| `origin` | `OrcaSlicer/OrcaSlicer` (upstream) | **never** |
| `imagemap` | `sentientstardust-dev/OrcaSlicer-ImageMap` | not Petko's |
| `waveoverhangs` | `dennisklappe/OrcaSlicer-WaveOverhangs` | not Petko's |

**Petko has no remote of his own for this fork.** His work is the local `petkos-orca` branch
(52 commits ahead of upstream), backed up at `E:\Backups\petkos-orca-stage1.bundle`.

## Context

- Current git status: !`git status`
- Current branch: !`git branch --show-current`

## Your task

1. Review the status and `git diff HEAD`.
2. Commit to the **`petkos-orca`** branch with a clear message. Never commit to `main`.
3. **Stop there — do not push, do not open a PR.**
4. Refresh the backup, since there is no remote:
   `git bundle create E:\Backups\petkos-orca-stage1.bundle --all`, then `git bundle verify` it.
5. Tell Petko it is committed and bundled, and that this fork still has no remote of his own —
   offer to create `QuarkSplit/petkos-orca` if he wants real off-machine backup.

This is a `blob:none` partial clone, so a bundle does not carry file contents for unfetched
objects. Run `git fetch --refetch` first if the bundle must stand alone.
