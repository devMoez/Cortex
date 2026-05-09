#!/usr/bin/env python3
"""
Cortex MCP Server

Exposes Cortex's architectural awareness as MCP tools so any compatible
AI tool (Claude, Cursor, Continue, Ultron) can call them natively.

Workflow the AI should follow:
  1. cortex_summary       — at the start of every session
  2. cortex_preflight     — before touching any file
  3. <write the code>
  4. cortex_verify        — after writing, to confirm nothing was missed

Usage:
  python mcp_server.py               (stdio mode — for MCP clients)
  python mcp_server.py --check       (verify Cortex is reachable)
"""

import sys
import json
import requests
from mcp.server.fastmcp import FastMCP

# ── Config ───────────────────────────────────────────────────────────────────

# Override with env var: CORTEX_URL=http://my-server:8080
import os
CORTEX_URL = os.environ.get("CORTEX_URL", "http://localhost:8080")
TIMEOUT    = 15  # seconds

# ── MCP Server ───────────────────────────────────────────────────────────────

mcp = FastMCP(
    "cortex",
    instructions=(
        "Architectural awareness engine for your codebase. "
        "Gives AI tools full dependency maps, blast radius analysis, "
        "and a ruleset to follow before and after every code change. "
        "Always call cortex_summary first, cortex_preflight before editing, "
        "and cortex_verify after writing."
    )
)

# ── HTTP helper ──────────────────────────────────────────────────────────────

def cortex(method: str, endpoint: str, body=None) -> dict:
    """Forward a call to the running Cortex HTTP API."""
    try:
        url = f"{CORTEX_URL}{endpoint}"
        if method == "GET":
            r = requests.get(url, timeout=TIMEOUT)
        else:
            r = requests.post(url, json=body, timeout=TIMEOUT)
        r.raise_for_status()
        return r.json()
    except requests.exceptions.ConnectionError:
        return {
            "error": "Cortex is offline.",
            "fix":   "Run internal_brain/mind.exe then cortex.exe"
        }
    except requests.exceptions.Timeout:
        return {"error": "Cortex timed out — it may be scanning a large codebase. Try again."}
    except requests.exceptions.HTTPError as e:
        if e.response is not None and e.response.status_code == 404:
            return {
                "error": f"Endpoint {endpoint} not found.",
                "fix":   "Recompile and restart cortex.exe — you may be running an old binary."
            }
        return {"error": str(e)}
    except Exception as e:
        return {"error": str(e)}


def cortex_delete(endpoint: str) -> dict:
    try:
        r = requests.delete(f"{CORTEX_URL}{endpoint}", timeout=TIMEOUT)
        r.raise_for_status()
        return r.json()
    except requests.exceptions.ConnectionError:
        return {"error": "Cortex is offline."}
    except Exception as e:
        return {"error": str(e)}


def cortex_patch(endpoint: str, body: dict) -> dict:
    try:
        r = requests.patch(f"{CORTEX_URL}{endpoint}", json=body, timeout=TIMEOUT)
        r.raise_for_status()
        return r.json()
    except requests.exceptions.ConnectionError:
        return {"error": "Cortex is offline."}
    except Exception as e:
        return {"error": str(e)}


def fmt(data) -> str:
    return json.dumps(data, indent=2)

# ── Tools ────────────────────────────────────────────────────────────────────

@mcp.tool()
def cortex_summary() -> str:
    """
    Get an instant architectural snapshot of the current codebase.

    Returns:
    - file_count: how many files Cortex has mapped
    - active_rule_count: how many rules are currently enforced
    - active_stack: which rule preset is active (fullstack, frontend-only, etc.)
    - last_scan: when the dependency graph was last updated
    - watcher_running: whether live sync is active
    - most_depended_on: the 5 files everything else relies on — touch these carefully
    - exception_count: how many active rule exceptions exist

    ALWAYS call this at the start of a session before making any changes.
    It tells you the shape of the project and what you need to be careful about.
    """
    return fmt(cortex("GET", "/summary"))


@mcp.tool()
def cortex_preflight(file: str) -> str:
    """
    Pre-flight check for a single file you are about to modify.

    Returns:
    - blast_radius: every file that imports or depends on this one
    - blast_radius_count: how many files will be affected
    - rules: all active rules you must follow for this change
    - purpose: what this file does (if documented)
    - symbols: functions and exports defined in this file
    - warning: explicit warning if dependent files exist

    CALL THIS BEFORE MODIFYING ANY FILE.
    If blast_radius is non-empty, you must update every file in that list
    in the same change. Partial updates leave the codebase in a broken state.

    Args:
        file: Path to the file you are about to modify (relative or absolute path)
    """
    return fmt(cortex("POST", "/preflight", {"file": file}))


@mcp.tool()
def cortex_preflight_batch(files: list[str]) -> str:
    """
    Pre-flight check for multiple files at once.

    Use this when you already know you will be touching several files —
    it returns blast radius and rules for all of them in a single call
    instead of calling cortex_preflight once per file.

    Returns a list of preflight results, one per file.

    Args:
        files: List of file paths you are about to modify
    """
    return fmt(cortex("POST", "/preflight/batch", files))


@mcp.tool()
def cortex_verify(files_changed: list[str]) -> str:
    """
    Verify that your change is architecturally complete.

    Call this AFTER writing code, with every file you modified.

    Cortex checks the blast radius of your changes and reports whether
    all dependent files were included. If any were missed, the response
    lists exactly which files need to be updated before the task is done.

    Response fields:
    - passed: true means your change is complete; false means files were missed
    - violations: files in the blast radius that you did not update
    - acknowledged: violations covered by active exceptions (not errors)
    - changed_files: the list you submitted, echoed back for confirmation

    Do not declare a task done until passed is true.

    Args:
        files_changed: Every file you modified in this change
    """
    return fmt(cortex("POST", "/verify_change", files_changed))


@mcp.tool()
def cortex_rules(category: str = "") -> str:
    """
    Get the active rules for this project.

    Rules tell you exactly what you must do (and must not do) when coding here.
    They cover: pre-flight checks, coding standards, error handling, security,
    database safety, UI behavior, architecture constraints, and verification.

    Only active rules are enforced. Inactive rules are visible but skipped.

    Args:
        category: Filter by category. Options:
                  preflight, coding, exception, security, database,
                  ui, architecture, verification
                  Leave empty to get all rules.
    """
    rules = cortex("GET", "/rules")
    if isinstance(rules, list) and category:
        rules = [r for r in rules if r.get("category") == category]
    return fmt(rules)


@mcp.tool()
def cortex_add_exception(rule_id: str, reason: str, file: str = "") -> str:
    """
    Register a one-time exception for a rule you are knowingly violating.

    Use this when the user explicitly asked for something that breaks a rule.
    Examples:
    - User says "build a quick demo with fake data" → add exception for code_001
    - User says "just remove that sidebar, I don't need it" → add exception for ui_001

    The exception is consumed automatically the next time cortex_verify runs.
    It does not carry over to future changes. The reason is logged for audit purposes.

    Args:
        rule_id:  The rule ID to exempt, e.g. "code_001", "ui_001", "verify_001"
        reason:   Why this exception is justified — be specific
        file:     Optional — scope the exception to one specific file only
    """
    body = {"rule_id": rule_id, "reason": reason}
    if file:
        body["file"] = file
    return fmt(cortex("POST", "/exceptions", body))


@mcp.tool()
def cortex_activate_stack(stack: str) -> str:
    """
    Switch the active rule stack to match the project type.

    A stack toggles which rules are active without permanently deleting anything.
    Switching stacks resets all rules and applies the stack's preset.

    Available stacks:
    - fullstack:      All rules on — React, Node, database
    - frontend-only:  Disables backend and database rules
    - backend-only:   Disables UI rules
    - python:         Disables JS/TS and UI-specific rules
    - minimal:        Only hard-blocking rules — good for quick prototypes
    - none:           Clears any active stack, restores all rules to active

    Args:
        stack: One of the stack names above
    """
    return fmt(cortex("POST", f"/stack/{stack}", {}))


@mcp.tool()
def cortex_toggle_rule(rule_id: str, active: bool) -> str:
    """
    Enable or disable a specific rule without switching stacks.

    Use this when you need to turn off one rule for this project
    without changing the overall stack.

    Args:
        rule_id: The rule ID, e.g. "ui_004", "code_005"
        active:  True to enable, False to disable
    """
    return fmt(cortex_patch(f"/rules/{rule_id}", {"active": active}))


@mcp.tool()
def cortex_rescan() -> str:
    """
    Force an immediate incremental rescan of the codebase.

    Cortex automatically rescans every 2 seconds for changed files and
    every 10 seconds for new files. Call this when you have just made
    several changes and want the blast radius data current right now,
    before calling cortex_preflight or cortex_verify.

    Returns the updated summary after the rescan completes.
    """
    return fmt(cortex("POST", "/rescan", {}))


@mcp.tool()
def cortex_map() -> str:
    """
    Get the full file dependency map for the codebase.

    Returns every tracked file with its symbols (functions, exports) and
    its direct dependencies (files it imports).

    Use this when you need to understand the overall structure of the project
    before deciding where to add a new feature or how files relate to each other.
    For large codebases, prefer cortex_preflight on specific files instead.
    """
    return fmt(cortex("GET", "/map"))


@mcp.tool()
def cortex_circular_deps() -> str:
    """
    Detect circular dependencies in the codebase.

    Returns a list of dependency cycles — groups of files that import
    each other in a loop. Circular dependencies cause unpredictable
    initialization order, hard-to-debug errors, and bundler failures.

    Call this after adding new imports to verify you have not created a cycle.
    """
    return fmt(cortex("GET", "/analyze_deps"))

# ── Entry point ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if "--check" in sys.argv:
        print("Checking Cortex connection...")
        result = cortex("GET", "/summary")
        if "error" in result:
            print(f"FAIL: {result['error']}")
            if "fix" in result:
                print(f"Fix:  {result['fix']}")
            sys.exit(1)
        else:
            print(f"OK   — {result.get('file_count', '?')} files mapped, "
                  f"{result.get('active_rule_count', '?')} active rules, "
                  f"watcher={'on' if result.get('watcher_running') else 'off'}")
            sys.exit(0)
    mcp.run()
