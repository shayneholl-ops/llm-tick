# Issue tracker: GitHub

Issues and specs for this repo live as GitHub issues on `shayneholl-ops/llm-tick`.
Use the `gh` CLI for all operations; it infers the repo from `git remote -v`.

## Conventions

- **Create an issue**: `gh issue create --title "..." --body "..."`
- **Read an issue**: `gh issue view <number> --comments`
- **List issues**: `gh issue list --state open --json number,title,body,labels`
- **Comment**: `gh issue comment <number> --body "..."`
- **Label**: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --comment "..."`

## Blocking edges

Use GitHub's **native issue dependencies** — they render in the UI and are queryable:

```
# blocker's numeric DATABASE id (not the #number, not node_id)
gh api repos/shayneholl-ops/llm-tick/issues/<n> --jq .id

gh api --method POST \
  repos/shayneholl-ops/llm-tick/issues/<child>/dependencies/blocked_by \
  -F issue_id=<blocker-db-id>
```

The live gate is `issue_dependencies_summary.blocked_by` (open blockers only).

## Public repo — sanitise before publishing

**This repo is PUBLIC.** Issues are world-readable, so tickets and comments must use
**generic placeholders**, never the real deployment values:

| Never write | Write instead |
|---|---|
| the real server LAN IP | "the Server's IP" |
| the real model-host IP | "the model host" |
| the real SSID / hostname | "the home network", "the Server hostname" |
| the Ledger's absolute path | "the Ledger path" |
| the weather API key or its location | "the weather key" |

`secrets.h` is gitignored and must stay that way: never paste its contents into an
issue, and never paste serial output that contains credentials.

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as
feature requests; `/triage` reads this flag.)_
