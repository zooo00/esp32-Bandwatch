# Instructions for opencode (and any non-Claude agent)

Read `CLAUDE.md` first (project rules, hard-won board rules, build/flash/run), then `docs/BACKLOG.md` (open work)
and `docs/DEVELOPER.md` (design). Do not duplicate them here.

## Do not
- Run `esptool`, toggle DTR/RTS, or open the serial port with `dtr=False, rts=False` - it reboots the board
  (CLAUDE.md rules 1-3). Only the owner flashes or talks to the board unless told otherwise in the prompt.
- Bump the Arduino core, or change the `libnet80211` patch offsets (rule 8, 10).
- Force-push, rewrite or squash history, move or delete tags, or create release tags. Releases follow the workflow
  in CLAUDE.md and are done by the owner. If a history change seems needed, stop and give the owner the command.
- Leave temp git worktrees, build outputs or scratch files behind; remove worktrees when done.

## Do
- Run `tests/run_offline.sh` before committing (host + firmware tiers; no board needed).
- After any firmware change report static RAM (build output "Global variables use N bytes") and the delta against
  the previous build; the firmware test gates it.
- Commit with your own identity (the owner launches you with `GIT_AUTHOR_*`/`GIT_COMMITTER_*` set to opencode) and
  end each commit message with `Co-Authored-By: opencode <noreply@opencode.ai>`.
- Say plainly at the end of the session what you changed and whether anything is unpushed.

## One-time setup per clone
`cp tools/pre-push .git/hooks/pre-push && chmod +x .git/hooks/pre-push` - refuses non-fast-forward pushes and
branch deletions and runs the host tests. Hooks are not cloned with the repo, so repeat this on every machine.
