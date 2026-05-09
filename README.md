# Cortex

Cortex is an architectural awareness engine that sits between you and your AI coding tools.

The problem it solves is simple but painful: AI models like Claude and Gemini are genuinely smart, but they code like someone who just joined your team today and hasn't read anything. They touch one file and break three others. They add a UI button with no backend wired up. They redesign a page and silently delete features that were already working. They don't ask. They don't check. They just write.

Cortex fixes this. Before your AI touches a single file, Cortex gives it a complete picture of the codebase — which files depend on what, what will break if something changes, and a set of rules it must follow. After the AI writes, Cortex verifies that nothing was missed. The AI still does the coding. Cortex makes it code like a senior engineer who actually knows the project.

---

## How It Works

The flow is straightforward:

**Before coding** — AI calls `/preflight` with the file it's about to touch. Cortex returns the blast radius (every file that depends on it), the file's purpose and symbols, and all active rules the AI must follow for this change.

**During coding** — AI has full architectural context. It knows what will break, what rules apply, and what already exists in the codebase.

**After coding** — AI calls `/verify_change` with the list of files it modified. Cortex checks whether every file in the blast radius was updated. If something was missed, the response tells the AI exactly which files it skipped.

This loop turns a reckless vibe-coder into something closer to a production engineer.

---

## Architecture

Cortex runs as two processes:

**Internal Brain** (`internal_brain/mind.cpp`) — The analysis engine. Runs on port 9090. Owns the dependency graph, rules, stacks, exceptions, and the live file watcher. This is the part that actually knows your codebase.

**Cortex Core** (`cortex.cpp`) — The API gateway. Runs on port 8080. Proxies requests to the brain, handles archetype expansion, and is the single endpoint your AI tools talk to.

Both are written in C++17 with no external runtime dependencies. The only libraries are single-header: `cpp-httplib` for HTTP and `nlohmann/json` for state management.

---

## The Rules System

Cortex ships with 34 rules across 8 categories. These are the rules AI tools consistently violate in practice.

**Preflight rules** — things AI must check before writing anything:
- Check blast radius before touching any file
- Check whether a similar feature already exists before creating a new one
- Verify backend exists before adding frontend that depends on it
- Read existing features before redesigning — ask what to keep, remove, or merge
- Warn before breaking changes to shared interfaces (renamed exports, changed signatures)
- Ask one clarifying question when the instruction is ambiguous

**Coding rules** — how AI must write code:
- No dummy data, fake API responses, or placeholder UI
- Update every file in the blast radius in the same change set
- Full-stack features must be complete — frontend, backend, and data layer together
- No debug statements or console.log in production code
- No commented-out code blocks
- No hardcoded URLs, ports, or environment-specific values

**Exception rules** — error handling AI always skips:
- Every async operation must handle errors
- Never expose raw errors or stack traces to users
- Every async operation must have a loading state
- Every list or data view must handle the empty state
- Validate all inputs at the boundary
- API errors must return a consistent shape

**Security rules**:
- No hardcoded credentials or secrets anywhere in code
- No sensitive data in frontend or client bundles
- Sanitize all user-generated content before rendering

**Database rules**:
- No destructive operations without explicit user confirmation
- Migrations must be reversible
- No raw SQL with unparameterized user input

**UI rules**:
- Never silently remove existing features
- Merge old and new design — don't start from scratch unless asked
- Interactive elements must disable during async operations
- Mobile responsiveness is not optional

**Architecture rules**:
- No circular dependencies
- Shared logic lives in shared files — never duplicated
- Don't add a package if one already exists in the project

**Verification rules**:
- Blast radius coverage must be complete
- Self-check before declaring a task done
- Every new file must have a stated, single purpose

Rules are stored in `internal_brain/rules.json`. You can add, remove, toggle, or update any rule via the API. Changes persist immediately.

---

## Stacks

Different projects need different rules. A frontend-only project doesn't need database migration rules. A Python project doesn't need JS-specific rules.

Cortex ships with five preset stacks:

| Stack | What It Does |
|---|---|
| `fullstack` | All rules active — React, Node, database |
| `frontend-only` | Disables backend and database rules |
| `backend-only` | Disables UI rules |
| `python` | Disables JS/TS and UI-specific rules |
| `minimal` | Only the hard-blocking rules — for demos and prototyping |

Activate with `POST /stack/frontend-only`. Deactivate with `POST /stack/none`.

You can also define your own stacks in `internal_brain/stacks.json` by specifying which rule IDs to enable or disable.

---

## Live File Watcher

Once Cortex starts, it watches your codebase continuously. Every 2 seconds it checks whether any tracked file has changed (using filesystem timestamps — not polling file content). If a file changed, only that file gets re-analyzed. The blast radius recalculates automatically.

Every 10 seconds it also scans for new files added to the project.

This means the dependency graph and blast radius are always current. When AI calls `/preflight` mid-session, it gets fresh data, not stale startup data.

The watcher adds no meaningful overhead. Checking a timestamp is a single kernel `stat()` call. On a 1,000-file project, the 2-second tick takes under a millisecond.

You can also trigger a manual rescan with `POST /rescan`.

---

## Exceptions

Sometimes AI needs to knowingly break a rule. Maybe you asked for a quick demo with fake data. Maybe you're explicitly removing a feature and the blast radius warning is expected.

Add an exception before the change:

```
POST /exceptions
{"rule_id": "code_001", "reason": "User asked for demo with hardcoded data", "file": "src/Demo.tsx"}
```

When `verify_change` runs and hits that rule, it acknowledges the exception instead of flagging a violation. The exception is consumed — it doesn't carry over to the next change. Every acknowledged exception is logged with its reason, so there's a clear audit trail.

---

## API Reference

### Codebase Awareness
```
GET  /summary             Health snapshot — file count, active rules, last scan, most depended-on files
GET  /map                 Full file dependency map
GET  /analyze_deps        Circular dependency report (Tarjan SCC)
GET  /verify              Ghost file check — finds files in the map that no longer exist on disk
POST /rescan              Force a full incremental rescan
```

### Rules
```
GET    /rules             List all rules with active/inactive status
POST   /rules             Add a rule
PATCH  /rules/:id         Update a rule — use {"active": false} to disable
DELETE /rules/:id         Remove a rule
```

### Stacks
```
GET  /stack               Active stack + all available stacks
POST /stack/:name         Activate a stack (or "none" to clear)
```

### Exceptions
```
GET    /exceptions         Active exceptions
POST   /exceptions         Add an exception: {rule_id, reason, file?}
DELETE /exceptions/:id     Clear an exception manually
```

### Preflight & Verification
```
POST /preflight            Pre-flight for one file: {"file": "src/Sidebar.tsx"}
POST /preflight/batch      Pre-flight for multiple files: ["file1.ts", "file2.ts"]
POST /verify_change        Post-write check: ["file1.ts", "file2.ts"]
```

### History
```
GET /history              Timestamped event log
GET /meta_scan            Self-audit of system state
```

---

## Getting Started

**Prerequisites:** GCC 9+ or any C++17 compiler. On Windows, `ws2_32` for networking.

**Compile:**
```bash
g++ -std=c++17 internal_brain/mind.cpp -o internal_brain/mind.exe -lws2_32 -pthread
g++ -std=c++17 cortex.cpp              -o cortex.exe              -lws2_32 -pthread
```

**Run:**
```bash
# Start the brain first
./internal_brain/mind.exe

# Then the gateway
./cortex.exe
```

Cortex is now live on `http://localhost:8080`. The brain runs privately on port 9090.

On startup, Cortex scans your entire codebase, builds the dependency graph, loads your rules and stacks, and starts the live watcher. From that point on, the map stays current automatically.

**Point Cortex at any directory** by passing it as an argument — no recompiling needed:
```bash
./internal_brain/mind.exe C:\Users\you\projects\my-app
./cortex.exe
```

**Custom ports** (useful if 8080/9090 are taken):
```bash
./internal_brain/mind.exe C:\path\to\project --port 9191
./cortex.exe --port 8181 --brain-port 9191
```

**MCP server pointing at a remote Cortex instance:**
```bash
set CORTEX_URL=http://my-server:8080
python mcp_server.py
```

---

## MCP Integration

Cortex ships with a Python MCP server (`mcp_server.py`) that exposes all analysis as tools any MCP-compatible AI can call natively — Claude Code, Cursor, Continue, or your own assistant.

**Install and register with Claude Code:**
```bash
install_mcp.bat
```

**Or register manually:**
```bash
claude mcp add cortex -- python path/to/mcp_server.py
```

**Verify it can reach your running Cortex instance:**
```bash
python mcp_server.py --check
```

Once registered, Claude has these tools available automatically in every session:

| Tool | Purpose |
|---|---|
| `cortex_summary` | Session start — codebase health + most depended-on files |
| `cortex_preflight` | Before editing — blast radius + active rules for a file |
| `cortex_preflight_batch` | Before editing multiple files at once |
| `cortex_verify` | After writing — confirms blast radius is fully covered |
| `cortex_rules` | Get active ruleset, filter by category |
| `cortex_add_exception` | Acknowledge a known rule violation with reason |
| `cortex_activate_stack` | Switch rule profile for the project |
| `cortex_toggle_rule` | Enable or disable a specific rule |
| `cortex_rescan` | Force immediate rescan |
| `cortex_map` | Full dependency map |
| `cortex_circular_deps` | Circular dependency report |

**Point MCP at a remote Cortex instance** (for team or cloud use):
```bash
set CORTEX_URL=http://your-server:8080
python mcp_server.py
```

## What's Coming

**Ultron Integration** — Cortex ships as a default tool inside Ultron, a Jarvis-inspired coding assistant built on OpenCode. Every AI session in Ultron starts with a Cortex summary and has preflight/verify available throughout.

**GitHub Integration** — Connect a GitHub repo URL instead of a local path. Cortex clones, scans, and keeps it in sync. Foundation for hosted multi-user access.

---

## Project Structure

```
cortex.cpp                    API gateway (port 8080)
internal_brain/
  mind.cpp                    Analysis engine (port 9090)
  rules.json                  Rule definitions — edit to customize
  stacks.json                 Stack presets — edit to add your own
  mental_map.json             Dependency graph (auto-generated)
  history.json                Event log (auto-generated)
archetypes.json               Component pattern database
mapper.cpp                    Standalone codebase mapper utility
```

---

*Built to close the gap between AI that writes code and AI that understands a codebase.*
