# Core branches (SquirrelBIOS)

These two long-lived branches are **core**. Treat them like product
trunks, not feature branches.

| Branch | Role |
|--------|------|
| `main` | Shipping firmware. PRs land here. Releases tag from here. |
| `klingon-font` | Permanent joke twin of `main`. pIqaD VGA (and, later, all-Klingon COM1 per [`KLINGON_ASCII_PURGE.md`](KLINGON_ASCII_PURGE.md)). **Never merge into `main`.** Refresh by merging `main` *into* `klingon-font`. |

Everything else (`cursor/*`, release prep, etc.) is disposable relative to
these two. Agents must not delete either core branch and must not open a
PR that merges `klingon-font` (or `klingon-font*`) into `main`.

## What agents can enforce (already)

- `AGENTS.md`: never merge `klingon-font*`; never delete branches; never tag.
- This file + README joke callout: `klingon-font` is core, parallel to `main`.
- Refresh flow: `git merge origin/main` while checked out on `klingon-font`,
  then push `origin/klingon-font` (no PR to `main`).

## What only the owner can enforce (GitHub)

Cloud / Cursor agent tokens **cannot** read or write branch protection
(HTTP 403). Sarah has to flip this in the repo settings once.

### Recommended: one repository ruleset for both cores

GitHub → **squirelworks/squirrelbios** → **Settings** → **Rules** →
**Rulesets** → **New branch ruleset**:

1. **Name:** `core-branches` (or similar).
2. **Enforcement:** Active.
3. **Target branches:** include by name
   - `main`
   - `klingon-font`
4. **Rules** (match whatever `main` already uses, at least):
   - Restrict deletions
   - Block force pushes
   - (Optional) Require a pull request before merging — useful for `main`;
     for `klingon-font` you may prefer **allowing** direct pushes from the
     owner / agents that refresh from `main`, so either:
     - omit “require PR” for this shared ruleset and keep PR-required only
       on a **second** ruleset that targets only `main`, or
     - require PRs on both and refresh `klingon-font` via PRs into itself
       (base = `klingon-font`, head = a refresh branch).
   - Require status checks to pass: the `make-test` workflow check name(s)
     used on `main`.
5. **Bypass** (optional): only the owner account, for emergencies.

### Classic branch protection (alternate)

**Settings** → **Branches** → add (or edit) a rule for `klingon-font` with
the same toggles as `main`: no force push, no delete, required checks.

### Also worth doing

- Under **Branches**, ensure default branch stays `main`.
- Do **not** set `klingon-font` as default.
- Optional: GitHub **Rules** → block creation of new branches matching
  `klingon-font-*` if you want a single joke trunk (agents already must
  not merge those into `main`).

## After protection is on

Paste a short note on this branch (e.g. under HANDOFF “Where things are”
on the next `klingon-font` refresh) that both cores are ruleset-protected,
so future agents stop asking.
