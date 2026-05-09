@echo off
setlocal

echo.
echo  Cortex MCP Installer
echo  ─────────────────────────────────────────────────────
echo.

:: Install Python dependencies
echo [1/2] Installing Python dependencies...
pip install -r "%~dp0requirements.txt" --quiet
if %ERRORLEVEL% NEQ 0 (
    echo FAIL: pip install failed. Make sure Python is in your PATH.
    exit /b 1
)
echo       Done.
echo.

:: Register with Claude Code
echo [2/2] Registering Cortex MCP with Claude Code...
claude mcp add cortex -- python "%~dp0mcp_server.py"
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo FAIL: Could not register with Claude Code.
    echo       Make sure the Claude Code CLI is installed and in your PATH.
    echo.
    echo       You can also register manually by adding this to your MCP config:
    echo.
    echo       {
    echo         "mcpServers": {
    echo           "cortex": {
    echo             "command": "python",
    echo             "args": ["%~dp0mcp_server.py"]
    echo           }
    echo         }
    echo       }
    exit /b 1
)
echo       Done.
echo.
echo  ─────────────────────────────────────────────────────
echo  Cortex MCP is registered. Start a new Claude Code session
echo  and you will see these tools available:
echo.
echo    cortex_summary          — call at session start
echo    cortex_preflight        — call before editing any file
echo    cortex_preflight_batch  — call before editing multiple files
echo    cortex_verify           — call after writing code
echo    cortex_rules            — get active ruleset
echo    cortex_add_exception    — acknowledge a known rule violation
echo    cortex_activate_stack   — switch rule profile for this project
echo    cortex_toggle_rule      — enable/disable a specific rule
echo    cortex_rescan           — force a rescan
echo    cortex_map              — full dependency map
echo    cortex_circular_deps    — circular dependency report
echo.
echo  Make sure mind.exe and cortex.exe are running before using the tools.
echo.
endlocal
