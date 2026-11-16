# hashstash design

**hashstash is a version control system for AI/SI agentic coding.** It versions code
at the level of functions, methods, classes and structs instead of files, keeps that
code in a content-addressed database,
and speaks a git-like vocabulary: `commit`, `push`, `pull`, `branch`, `merge`.
Normal source files are *generated* from the database whenever they're needed: to
build, to run, or to push to GitHub.

It is language-agnostic. Python comes first, then C and C++.

For a visual overview open [`docs/index.html`](index.html). For step-by-step use by
hand (build, edit, test, hub, publish, agents), open [`docs/guide.html`](guide.html).
This file is the technical design.

## 1. Programs

| Program  | Side   | Role |
|----------|--------|------|
| `hs`     | client | The command line. Works on a project's local store (`.hs/`), syncs with a hub, and runs a local MCP server (`hs mcp`) over stdio. |
| `hshub`  | server | The shared global store: GitHub for hashstash, run on your own machine or in the cloud. Hosts many projects; serves the sync protocol, the lock service and an MCP endpoint over HTTP. |

Both are thin shells over one C++ library, `libhashstash`, so the CLI, the local MCP
server and the hub all run the same code.

## 2. Working model: the store is the source

Code lives in the database. There is no checked-out working tree of source files.

1. `hs edit net.http.parse_header` locks the definition and writes it to a scratch
   file (`.hs/scratch/<lang file>`), or an agent calls the MCP tool `hs_edit_def`
   with new source text.
2. `hs update` parses the scratch file, stores each changed definition under its
   new hash, and repoints its name. Nothing is ever overwritten: old hashes remain
   in history.
3. `hs commit -m "..."` records the new namespace state.
4. `hs render` generates source files into an output folder for compilers, test
   runners, or `git`.

Compilers need files on disk. A single file can be piped in (`gcc -x c -`), but
`#include`, Python `import` and build systems all expect a real folder tree. So
`render` is the bridge, and it is incremental: only files whose content changed are
rewritten, so make, ninja and CMake rebuild the minimum. A later option is a
virtual filesystem (ProjFS on Windows, FUSE on Linux) that shows the store as a live,
read-only source tree, generated on demand.

## 3. Data model

Everything is an immutable **object** stored by hash. The hash is BLAKE3-256 over a
canonical byte encoding, shown in base32 (`#k3v9q2mz…`, usually shortened to 10
characters).

| Object | Holds |
|--------|-------|
| **Def** | One definition: language, kind (`function`, `method`, `class`, `struct`, `enum`, `typedef`, `macro`), its source text, its normalized hash, and its child crumbtrail if it is a container (a class holds its methods). |
| **Glue** | Source text that isn't a definition: imports, `#include`s, top-level statements, blank lines and comments between definitions. |
| **Fragment** | A piece of a large definition stored on its own, placed back into its parent at a crumb. |
| **Crumbtrail** | The ordered placement list for a file (or for a container's body): a sequence of Def, Glue and Fragment references. |
| **Namespace** | The tree of a project: qualified names to Def hashes, file paths to crumbtrails, plus each definition's lineage ID. |
| **Commit** | A namespace hash, parent commit hashes, author, time and message. |

### Two hashes per definition

* The **content hash** covers the exact text, comments and formatting included. It
  is the object's identity.
* The **normalized hash** covers the token stream with whitespace and comments
  removed. When two versions share it, diffs say "formatting/comments only".

A renamed definition keeps its content hash, so rename detection is free.

### Lineage IDs

A definition's hash changes on every edit. Locks, history and "who changed this"
need an identity that doesn't, so every definition gets a **lineage ID** (a UUID) when it is
first stored. Edits and renames keep it.

### Labels and ordering

Every definition carries the labels **project → file → qualified name**. Its place
in the file is recorded by its file's crumbtrail. Rendering walks the crumbtrail in
order and writes each piece's exact text, so the generated file is byte-for-byte
the same every time.

**Round-trip guarantee:** importing a file and rendering it again gives identical
bytes. The test suite checks this against every fixture and against real projects.

### Crumbs and fragments

A definition over the size limit gets a warning and a suggestion to split it into
helper functions. That is the preferred fix. For a large function that can't be
refactored yet, it can be *fragmented*: a section is cut out, stored as a Fragment,
and replaced in the parent by a **crumb**, a one-line marker comment:

```python
    # @hs:crumb 7f3a2c9e  validate headers
```

Each fragment can be locked and edited on its own. When files are rendered, every
crumb is replaced by its fragment's text, so generated code never contains markers
(unless `render --keep-crumbs` is passed for debugging).

## 4. Index and graphs

Derived tables are maintained in each store and on the hub, keyed by content hash so
each definition is analyzed only once, ever.

* **Name index:** qualified name, short name, kind, file and language, with SQLite
  FTS5 full-text search over names, signatures and docstrings.
* **Call graph:** for every definition, the call sites and references found by
  tree-sitter queries. Each is resolved to a definition when it can be (same file,
  then imports and includes, then a unique project-wide name), or kept as external
  or unresolved.
* **Execution graph:** the call graph walked from entry points (`main`,
  `if __name__ == "__main__"`, test functions, exported API).

Queries, available to `hs graph` and MCP alike: callers, callees, reachable from X
to depth N, call paths from A to B, unused definitions, cycles, and impact (everything
that depends on X). Results come back as JSON for agents and as text for people.

## 5. Size policy

Per-project config, `.hs/config.toml`:

```toml
[size]
warn_lines  = 60     # warn above this
max_lines   = 0      # refuse above this (0 = never refuse)
```

Every update or commit that stores an oversized definition reports a warning, in the
CLI and in the MCP response:

```json
{"warnings":[{"name":"app.load_config","lines":143,"limit":60,
  "suggestion":"split into helper functions, or fragment with hs_fragment"}]}
```

## 5a. Tests per definition

Every function and method, standalone or inside a class, must have test cases
designed for it. The namespace keeps, for each definition's lineage, the **set** of
test definitions linked to it: many tests per definition, and a test may cover
several definitions (a test may build whatever higher-level objects it needs).

* A definition with no linked tests is a warning on every update, commit, verify
  and MCP response (`[tests] require = "warn"`), or an error (`"block"`).
* `hs import` and `hs tests autolink` link tests that call a definition directly;
  `hs tests link` adds more by hand.
* The **suite hash** ties a definition to its tests: BLAKE3 over the definition's
  content hash plus the content hashes of all its linked tests. It changes when the
  code or any of its tests change, and test results are recorded against it.

## 5b. Verification and the push gate

`hs verify` checks the policy (tests linked, size limits) for every definition
the hub has not seen yet, meaning changes since the branch's last pushed or pulled
commit (everything, before the first push). It then generates the source tree, runs
the linked tests (`[tests] python = "python -m pytest -q {tests}"`), and records the
result against the namespace hash. Commits are local; **a push only accepts verified
commits.**

* **Impact testing.** Verify also follows the call graph upwards and runs the tests
  of every caller up to `[tests] impact_depth` levels (default 3), so changing
  `add` also runs the tests of `mul`, which calls it.
* **Where tests run.** Each Python test file runs from its own project root: the
  nearest folder with `pytest.ini`, `pyproject.toml`, `setup.cfg` or `tox.ini`, else
  the folder above its package and any `tests/` folder. Nested projects (such as
  fixtures) therefore import correctly.
* **Fresh code.** Bytecode caches in the generated tree are removed and
  `PYTHONDONTWRITEBYTECODE` is set. Python only notices a stale `.pyc` when a file's
  size or modification second changes, which a quick one-character edit may not do.
* **One line per function.** Results are summarised per function ("18/20 passed,
  2 failed"), and only failing tests are listed one by one (`-v` lists all). The same
  summary appears in `hs tests summary`, `hs show`, the review pages and MCP
  (`hs_test_summary`, `hs_show`, `hs_tests`). A function's numbers come from the
  verification of that exact state first. A test's outcome also depends on the code
  it calls, so a result from another state (say, with a broken helper) is used only
  for tests that weren't run on this one. Verification reports travel with pushes
  and pulls, so the client, the hub and every clone show the same numbers.

## 5e. Reviews and the review flag

Every commit carries a **review flag**, kept current in the `commit_review` table:

| Flag | Meaning |
|---|---|
| `unreviewed` | nobody has reviewed it |
| `ai` (AI reviewed) | only AI agents reviewed it |
| `human` (human reviewed / approved) | a person looked too; `human_approved` says whether they approved |

* **Reviews never block anything.** Commit, verify, push and pull work the same
  whatever the flag says. The flag records who looked.
* **Who counts as what.** Reviews through MCP (`hs_review`) are AI reviews. Reviews
  on the review pages are a person's. `hs review` in a terminal is a person's, unless
  it runs under an AI agent (Claude Code and others are detected; `HS_REVIEWER=human|ai`
  overrides). On a hub, `--review-token` makes the pages ask for a token before
  recording a person's review, so only people you give it to can mark commits human
  reviewed.
* **A review** has a reviewer, a verdict (`approved`, `changes_requested`, `comment`),
  a summary (required unless approving) and optional line comments. Reviews are
  append-only records keyed by id. A push sends the ones made locally, a pull fetches
  the ones made elsewhere, and a new review is sent to `origin` straight away.
* **Where approval is offered.** `[review] approval = "both"` (`local`, `hub`, `both`
  or `off`) sets where commits waiting for a person are listed: the local pages
  (`hs ui`), the hub's pages, or both.
* `hs push` reports how much of what it pushed a person has reviewed, and the hub logs
  every accepted push (`push_log`) so its pages can show each push's review state.

## 5f. Review pages

`hs ui` serves review pages for the local store on `http://127.0.0.1:7421`, and
`hshub serve` serves the same pages for every hosted project at `/ui`:

* **Overview:** commits with their verification and review flags, what is waiting for
  a person, uncommitted changes, functions with failing tests, locks and (on a hub)
  pushes with their review state.
* **Commit:** line diffs per function, module code and deleted code, each with its
  test summary, then the reviews and a form to approve, request changes or comment.
* **Tests:** every function with its summary, failing ones first.
* **Definition:** code, each linked test's result, notes, context, callers and history.

With a hub token set, a browser enters it once and keeps it in a cookie.

## 5c. Lifecycle history and rollback

Every step that changes a branch's working state is recorded: import, update,
note, context, test links, commit, verify, rollback. `hs rollback @<id>` restores
any recorded state *as a new step*, so the latest state is never lost and the two can
be compared (`hs diff --from @12 --to work`). `[history] keep = 500` sets how many steps
are kept per branch.

Commits require a description of what changed (`-m`), and each commit also stores a
generated per-definition change list.

## 5d. Comment regions and context

Comments about what code does are stored **beside** the code, not in its text:

* A **note** covers a line region of a definition (regions may overlap). Edits move
  the region with the code; a note whose lines were all removed is marked stale.
* A definition's **context history** records why it exists and why it changed: the
  original request (`origin`), design snippets (`design`, with a source such as
  `docs/DESIGN.md#locks`), and every change reason (`change`, recorded from
  `hs update -m` or the MCP `reason` argument).

`hs context <name>` (MCP `hs_context`) returns everything an agent needs in one
document: code, signature, notes, context history, linked tests and suite hash,
callers, callees, commit history and lock state.

Notes and context are versioned objects attached to the lineage in the namespace,
so they follow renames, roll back with history and sync with pushes.

## 6. Locks

* Locks are held per **lineage ID**, or per fragment ID inside a big definition.
* They are **hard**: the hub refuses pushes that change a definition someone else
  has locked. A local store enforces its own locks the same way.
* They are **leases**: each lock expires (default 15 minutes) unless renewed, so a
  crashed agent can't hold code forever. Any activity by the holder renews it.
* `hs lock`, `hs unlock` and `hs locks` (list), with matching MCP tools.
* Opening a definition for editing (`hs edit`, MCP `hs_edit_def`) takes its lock.
  Edits happen in the local store; syncing with a hub carries the locks with it.
* A lock is held until the definition is **committed**, not until the whole feature
  is done, so locks stay short.

### Work items, requests and events

hashstash exists so that many agents can work on one codebase at once. Waiting on a
lock is the thing to avoid, so locks carry intent and agents queue work, not themselves.

* **Work items.** Every lock is taken for a work item: `hs work start FEAT-12 --title
  "..." [--kind feature|bug|chore] [--priority N]`. Others see it on the lock
  ("locked by agent-2 for FEAT-12"). An agent without one gets an ad-hoc chore item,
  so nobody is forced through ceremony. Default priority: bug 2, feature 1, chore 0.
* **Change requests.** An agent that needs a definition someone else holds logs what
  it needs (`hs request <name> --need "..." [--test-file f --test-code file]`, MCP
  `hs_request`) and carries on. The holder is told. While the request is open, the
  holder's commit of that definition is refused (`[work] requests = "block"`, or
  `"warn"`), so it takes the work on: accept it (the requester's test code is added
  and linked to the definition, so the verify gate holds the change to both agents'
  tests), make the change, verify, and commit with `--resolves <id>`. Or decline with a
  reason, which queues the requester for the lock instead. The request is **done**
  when the resolving commit reaches the shared branch (the push is accepted by the
  hub, or the commit itself when there is no hub), and the requester is told, with
  the commit to pull.
* **Lock requests.** `hs request <name> --lock` queues for the lock. When it is released,
  or its lease runs out, it goes to the first queued request (highest priority, then
  oldest). A lock released while change requests are still open goes to the first
  requester ("returned"), and the remaining requests now wait on the new holder, so
  nothing stalls on an agent that went away.
* **Events.** Each of these steps appends an event for the agent concerned
  (`request_new`, `request_accepted`, `request_declined`, `request_cancelled`,
  `request_done`, `lock_granted`). How an agent hears of them is up to the agent:
  * `hs inbox [--wait S] [--follow]` and MCP `hs_inbox` read them since the agent's
    cursor. `--wait` long-polls the hub. The exit code is 0 when something arrived and
    3 when nothing did, so a background `hs inbox --wait 600` (or a Claude Code
    Monitor on `hs inbox --follow`) wakes the agent exactly when there is news.
  * `hs mcp --channel` pushes each event into the session as a
    `notifications/claude/channel` message (Claude Code channels, a research preview,
    stdio only, enabled with `--dangerously-load-development-channels server:<name>`).
    Other MCP clients ignore it.
  * `hs subscribe <url>`: the hub posts the agent's events to a webhook, in order,
    retrying until the endpoint answers 2xx.
  * At scale, the same event log can feed a message broker (Kafka, NATS, Redis
    Streams, ActiveMQ) as one more delivery adapter. It is not needed below many
    dozens of agents.
* Identity is by agent name (`HS_AGENT`, `--as`, or one per MCP session), so an agent
  that should keep its inbox across restarts sets `HS_AGENT`. On a hub with access
  keys, the identity is the worker key's work item instead (below).
* All of this sits behind one interface (`Coordination`, `src/ops/coord.h`): SQL on a
  project store (a local project shared by several agents, or the hub's SQLite
  backend), or the hub over HTTP for a client whose project has one. Every change is
  one transaction, and nothing lives in server memory.

### Access keys: who may change what

Agents are replaced and restarted with fresh contexts all the time, so an agent's
name proves nothing. On a hub, the right to change a project comes from keys, with a
person at the root:

1. **People** are added on the hub machine (`hshub user add <name>`), so whoever
   controls that machine is the root of trust. Once a hub has an account, its pages
   need signing in (name and password, a session cookie that is HttpOnly and
   SameSite=Strict, 12 hours).
2. **Orchestrator keys.** A signed-in person opens a project's *Access* page and
   creates an orchestrator key valid for a chosen number of days (1 to 366). It is shown
   once and handed to the orchestrator agent in its environment (`HS_ORCH_KEY`), not
   in a chat. `hshub key orchestrator <project> --days N` does the same on the hub
   machine.
3. **Worker keys.** The orchestrator runs `hs key issue FEAT-12 --title "..."
   [--scope "src/net/**"] [--hours 8] [--out file]` for each piece of work and gives the
   key to the agent doing it (`HS_WORK_KEY`, or the MCP tool `hs_use_key`). The work
   item is created if new. A worker key never outlives its orchestrator key.
4. **The key is the identity.** Whoever presents a worker key acts as its work item:
   the agent name, lock holder, requester and inbox all become the work item. When an
   agent is replaced, the orchestrator issues the new one a key for the same work item,
   and it carries on with the same locks, requests and events. Whatever identity a
   request names is overwritten with the key's, so one agent can't act as another.
5. **The hub enforces it.** Creating the first orchestrator key switches the project
   to requiring keys (a person can switch it off on the Access page). From then on,
   anything that changes the project needs a valid key: locking, unlocking, editing
   through the hub's MCP tools, requests, work items, subscriptions and pushes. Reading
   stays open to anyone with the hub token. A worker key's scope (comma-separated globs
   over file paths or `file:qualified.name`) limits which definitions it may lock, edit
   or change in a push.
6. **Keys end.** They expire, a person can revoke any key on the Access page, an
   orchestrator can revoke the worker keys (`hs key revoke <id>`), and revoking an
   orchestrator key stops every key it issued.

Keys are `hsk_<id>_<secret>`. The hub stores only a hash of the secret and compares it
in constant time. Passwords are salted and stretched (200,000 rounds of BLAKE3).
Secrets come from the operating system's secure generator. The client sends the key
as the `X-HS-Key` header on every hub call: `HS_WORK_KEY`, else `HS_ORCH_KEY`.
`hs key` commands meant for the orchestrator prefer `HS_ORCH_KEY`.

Limits, stated plainly. Keys protect a project on a hub. A project without a hub has
no referee: every agent on the machine can write its `.hs` folder. A key is a bearer
secret, so an agent that can read another agent's environment can use its key, and
scopes and short lifetimes are what limit the damage. A later step can bind
orchestrator keys to a key pair (challenge and signature) so a copied secret alone is
not enough.

## 7. Diffs and merges

* `hs diff` reports changes per definition: added, removed, renamed, formatting-only
  or changed. Changed definitions show a line-by-line unified diff of their text,
  like a GitHub diff but grouped by function. Added and removed definitions show
  their full text, and module-level code (imports, constants, code between
  definitions) gets its own line diff per file, with definitions shown as
  one-line placeholders.
* MCP `hs_commit` returns the line diff it committed (capped at 300 lines, the rest
  listed by name), so an agent can confirm each commit is what it intended. `hs_show`
  returns any commit in full.
* Merges are three-way per definition. A conflict only exists when both sides
  changed the *same* definition; inside it, a line-level three-way merge resolves
  most of those automatically.

## 8. Sync

Like git, sync is a hash exchange. The client and hub compare commit
hashes, work out which objects the other side lacks, and transfer only those. A push
is a compare-and-swap of the branch head: it succeeds only if the hub's head is still
the one the client started from (a fast-forward), otherwise the client must pull and
merge first.

Received objects are staged until all their dependencies have arrived, so a
half-finished transfer never leaves a broken object in the store.

### Protocol

JSON over HTTP at `<hub>/api/p/<project>/<op>`, with an optional bearer token
(`hshub --token` or `HS_HUB_TOKEN` on the hub, `HS_TOKEN` on the client):

| op | purpose |
|----|---------|
| `create`, `refs` | make a project; read branch heads |
| `missing` | given hashes, return the ones the hub lacks |
| `put_objects` / `get_objects` | upload or download objects in batches (base64) |
| `reachable` | objects reachable from a head, stopping at what the client already has |
| `push` | move a branch head: compare-and-swap from `old` to `new` |
| `lock`, `unlock`, `locks` | the shared lock service |
| `put_reviews`, `reviews` | send reviews made on a client; fetch reviews after a sequence number |
| `verification` | verification reports (test results) for one namespace or a batch |

A push is accepted only when:
* the new commit's closure is complete on the hub;
* it is a fast-forward of the hub's head;
* it does not change a definition someone else holds a lock on;
* the head has passed verification (`hs verify`). The hub can also rerun the tests
  itself (`verify_on_push`).

After a push the hub refreshes its index and graphs, so `hs_graph` and the other
queries over MCP see the new code at once. `pull` downloads the reachable objects,
fast-forwards and brings the hub's locks down into the local store (`origin` column).
Merging diverged branches is the next milestone; for now `pull` refuses to proceed
when the two sides have diverged.

### Hub storage and scale

`hshub` is written against two interfaces, `HubBackend` (projects, jobs, sessions, access)
and `ProjectStore` (objects, refs, coordination, verification records, reviews, push
log), in `src/hub/backend.h`.
The first backend keeps one SQLite store per project, which suits one machine or a
small team. The interfaces are shaped for the way GitHub and similar hosts scale:

| data | small (now) | large |
|------|-------------|-------|
| objects (immutable, content-addressed) | SQLite per project | object store sharded by hash prefix (S3/GCS/Ceph, or a cluster of storage nodes like GitHub's Spokes); safe to cache and replicate anywhere because a hash never changes meaning |
| refs (branch heads) | SQLite | Postgres or another transactional database; a push is one compare-and-swap row update |
| coordination (locks, work items, requests, events) | SQLite | Postgres (the statements are portable), events optionally mirrored to a broker |
| MCP and sign-in sessions | `hub.db` | Postgres or Redis |
| people, access keys (hashes), settings | `hub.db` | Postgres |
| index and graphs | rebuilt per project | derived data: rebuilt by workers from the objects, then served from replicas or a search cluster |
| jobs (publish, verify) | `hub.db` queue | a real queue (Postgres `SKIP LOCKED`, SQS, Redis streams) feeding worker pools |

Because objects are immutable and every head move is a compare-and-swap, the HTTP
servers hold no state and can run behind a load balancer in any number. The only
thing that needs strong consistency is the ref and coordination tables.

**Workspaces.** Verify-on-push, publishing, the hub's MCP tools and the review pages
need a full project to run the ops library on. They get it from a `Workspace`
(`src/hub/workspace.h`), never from the backend directly. A backend whose storage
already is a hashstash project (SQLite) offers it through `native_repo()`. Any other
backend gets a cached workspace: a local project brought up to date from the
backend's objects, refs, verification reports, reviews and push log before each use,
with its own changes (commits and edits made through the hub's MCP tools, reviews made
on the pages) written back by compare-and-swap afterwards. `hshub serve --force-cache`
runs a SQLite hub that way, and the unit tests do too, so the path a scaled hub takes
is exercised all the time.

### Publishing to git ("nightly builds")

The hub can render a branch and commit it to an ordinary git repository, so GitHub
holds plain, buildable source files:

```toml
[publish]                       # in the hub project's .hs/config.toml
git_dir = "../publish-wc"      # working copy (cloned or created on first use)
remote  = "https://github.com/you/project.git"
branch  = "nightly"
on_push = true                  # publish after every accepted push
```

`hshub publish <project>` (or `hs publish` / `hs_publish` locally) renders the tree,
replaces the files in the working copy, commits with the hashstash commit messages
since the last publish and pushes. Recommended setup: publish on every verified push
to a dedicated branch such as `nightly`, and put `hshub publish` on a schedule
(Task Scheduler, cron) if you want one commit per day instead. Keep `main` for
releases you choose to promote.

**Keeping things out of git.** Some files belong in hashstash but not on GitHub: local
settings, internal notes, scratch tools, test data. `hs gitignore add <path|glob>
--reason "..."` (MCP `hs_git_ignore`) records a rule in the namespace itself, next to
the files and test links. It is committed, pushed and pulled like code, so every agent
and the hub's publisher see the same rules, and the review pages show when they change.
Patterns work like `.gitignore`: `secrets/` or `secrets` (a folder and everything in
it), `config/local.toml`, `*.env` (no slash: matches at any depth),
`tools/**/scratch_*.py`. Publishing leaves matching files out of the git working copy,
and a file published before it was ignored is removed from the branch on the next
publish. Git history that was already pushed still has it, so rotate any secret that
was published. `hs render` still writes everything, because a local build needs it.
A namespace without rules has no `git_ignore` key, so older hashes are unchanged.

## 9. Commands (`hs`)

```
Available now:
hs init [name]                   create a project store in .hs/
hs import <path> [--prefix dir]  bring in existing source code, index it, build the graphs
hs ls [prefix]                   list definitions
hs view <name>                   show a definition
hs find <text>                   search the index
hs context <name>                code, notes, context history, tests, graph, changes
hs edit <name>...                lock and open definitions in a scratch file
hs new <file>[:<Class>]          add a section for new code
hs update [-m reason]            store edits from the scratch file
hs rm <name>                     delete a definition
hs status                        uncommitted changes, held locks, open edits
hs diff [name] [--from r --to r] per-definition diff
hs commit -m <msg>               description required
hs log [name]                    project or per-definition history
hs history / rollback @<id>      lifecycle steps; restore without losing the latest
hs verify [--all] [-v]           policy checks and linked tests; required before push
hs show [commit]                 per-function line diffs, test summaries, reviews
hs review [commit] approve|changes|comment [-m text]
hs reviews                       review flags of recent commits
hs ui [--port 7421]              review pages in the browser
hs tests ...                     show / link / unlink / suggest / autolink / missing / summary
hs note <name> <a>-<b> <text>    comment on a region, kept beside the code
hs why <name> <text>             record origin / design / change context
hs lock / unlock / locks
hs work start <id> --title ... / work / work list / work done
hs request <name> --need ... [--test-file f --test-code file] | --lock
hs request accept|decline|cancel <id> / hs requests
hs commit ... --resolves <ids>   commit that makes requested changes
hs inbox [--wait S] [--follow]   events: requests, answers, granted locks, done
hs subscribe <url> / unsubscribe webhook delivery of your events
hs key [whoami]                  the access key you present (HS_WORK_KEY / HS_ORCH_KEY)
hs key issue <work> --title ... [--scope globs] [--hours N] [--out file]
                                 orchestrator: a worker key for one work item
hs key list / hs key revoke <id> orchestrator: the project's keys
hs graph callers|callees|reach|impact|path|unused|cycles|entry
hs render [--out dir] [--at ref] generate source files
hs mcp [--channel]               MCP server over stdio; --channel pushes events
hs remote add <name> <url> / hs remotes
hs clone <url> [dir]             copy a hub project into a new local store
hs push [remote]                 verified, fast-forward push; carries locks
hs pull [remote]                 fast-forward pull; brings hub locks down
hs publish [branch]              render and commit to the [publish] git repository
hs gitignore [list] / add <pattern> --reason ... / remove <pattern>
                                 paths kept in hashstash but never published to git

Later milestones:
hs branch / switch / merge
hs fragment <name> <lines>       cut a fragment out of a large definition
```

### Hub (`hshub`)

```
hshub serve --root <dir> [--port 7420] [--token T] [--review-token R] [--cache dir] [--force-cache]
                                  HTTP sync, locks, MCP at /mcp/<project>, review pages at /ui
hshub create <project> --root <dir>
hshub list --root <dir>
hshub publish <project> --root <dir> [--branch b]
hshub user add <name> --root <dir> [--password p]   someone who can sign in to the pages
hshub key orchestrator <project> --root <dir> [--days 30] [--label l]
```

The pages at `/ui/p/<project>/access` list a project's keys, create orchestrator
keys, revoke keys, and switch key protection on or off.

## 10. MCP tools

The same tool set is served by `hs mcp` (stdio, local project) and by `hshub`
(HTTP, hosted projects). Tools describe themselves, so agents need no imported skill:

`hs_status`, `hs_find`, `hs_list`, `hs_view`, `hs_context`, `hs_graph`, `hs_lock`,
`hs_unlock`, `hs_locks`, `hs_work`, `hs_request`, `hs_requests`,
`hs_request_respond`, `hs_inbox`, `hs_use_key`, `hs_edit_def`, `hs_add_def`, `hs_delete_def`, `hs_tests`,
`hs_link_tests`, `hs_unlink_tests`, `hs_missing_tests`, `hs_note`, `hs_why`, `hs_diff`,
`hs_verify`, `hs_commit`, `hs_show`, `hs_review`, `hs_reviews`, `hs_test_summary`,
`hs_log`, `hs_history`, `hs_rollback`, `hs_render`, `hs_remotes`, `hs_pull`,
`hs_push`, `hs_publish`, `hs_git_ignore`.
Coming with later milestones: `hs_fragment`, `hs_merge`.

An agent with only the local MCP server can therefore do the whole cycle: edit,
verify, commit, push and pull. The hub's MCP endpoint (`POST /mcp/<project>`,
session in the `Mcp-Session-Id` header) serves the same tools against the hosted
copy, for agents that work without a local store. On a project with access keys, only
the reading tools work without an `X-HS-Key` header, and a worker key's scope is
checked against the `name`, `names` and `file` arguments.

Edits through MCP require a `reason`, which becomes the definition's change context.
Responses carry warnings for definitions over the review size limit and for
definitions without tests.

## 11. Implementation

* C++20, CMake, Ninja. Windows (MinGW-w64 GCC and MSVC) and Linux (GCC, Clang).
* Libraries, all permissively licensed and fetched by CMake:
  SQLite (storage), tree-sitter with the Python, C and C++ grammars (parsing),
  BLAKE3 (hashing), nlohmann/json (MCP and wire format), cpp-httplib (hub and sync).
* Layout:

```
src/core/      hashing, objects, canonical encoding, store (SQLite)
src/lang/      language adapters over tree-sitter: python, c, cpp
src/ops/       import, render, edit/update, diff, merge, locks, graph, sync, reviews,
               coordination (work items, requests, events) and work (their operations)
src/mcp/       MCP protocol and tool definitions
src/cli/       hs
src/hub/       hub service, storage interfaces, SQLite backend, workspaces, access keys
src/server/    hshub (HTTP server, job worker)
src/web/       review pages (served by hs ui and hshub), sign-in and access pages (hshub)
tests/         unit tests, round-trip fixtures, smoke.sh, mcp_smoke.py, live_hub.sh, live_python.sh,
               live_access.sh
```

## 12. Milestones

1. **Local core, Python.** `init`, `import`, `ls`, `view`, `find`, `render` with the
   round-trip guarantee, call graph and index, `edit`/`update`, `commit`, `log`,
   `diff`, size warnings, local locks, tests per definition, verify, lifecycle
   history and rollback, notes and context. *(done)*
2. **Agents.** `hs mcp` with the full tool set *(done)*; fragments and crumbs.
3. **Hub.** `hshub` with sync, hard locks over the network and MCP over HTTP
   *(done)*; branches and merge next.
4. **C and C++.** Adapter for functions, records, namespaces, templates and
   preprocessor blocks *(done)*; header/source pairing and overloads next.
5. **Git bridge.** Publish rendered branches to git *(done)*; incremental mirroring.
6. **Scale.** Object store, Postgres refs and locks, queue-backed workers behind the
   same interfaces.
7. **Graphical editor.** Shows the store as readable code with live graphs, edits through the same library.

### Possible additions (not scheduled)

* **Deeper Python coverage.** Track more Python code as individual definitions: module
  constants and assignments, nested functions, `if __name__ == "__main__"` blocks, type
  aliases, `@overload` groups; plus a coverage report per language showing what is
  tracked as definitions and what stays module code.
* **Python client library.** A pip-installable `hashstash` package so Python agents and
  scripts can use a hub directly (projects, locks, edits, requests, commit, push, inbox)
  without the `hs` command.
* **Python bindings.** The C++ core callable from Python: open a local store, query
  definitions and the call graph, edit and commit.
* **Hub discovery for agents.** A hub-wide MCP endpoint (`/mcp`) with tools to list and
  create projects, registered once in an agent's user-level MCP settings so every
  session finds the hub; optionally start the hub at logon.

## License

hashstash is free for noncommercial use under the PolyForm Noncommercial License 1.0.0
(`LICENSE.md`). Commercial use needs a separate license; see `LICENSING.md`.
