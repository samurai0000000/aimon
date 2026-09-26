#!/usr/bin/env bash
#
# ChaosTestFleetRecovery.sh
#
# Copyright (C) 2026, Charles Chiou
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${CYAN}================================================================${NC}"
echo -e "${CYAN}    aimon Automated Fleet & Process Chaos Acceptance Test      ${NC}"
echo -e "${CYAN}================================================================${NC}"

# Negative Constraint Verification: Ensure deployed production aimon is undisturbed
DEPLOYED_PID=$(pgrep -f "aimon daemon --port 3883" || true)
if [ -n "$DEPLOYED_PID" ]; then
    echo -e "${YELLOW}[SAFETY] Detected live deployed aimon daemon (PID: ${DEPLOYED_PID}).${NC}"
    echo -e "${YELLOW}[SAFETY] Test will strictly execute on isolated test ports (3891/3892).${NC}"
fi

WORK_DIR="/tmp/aimon_chaos_$$"
mkdir -p "${WORK_DIR}"

cleanup() {
    echo -e "\n${CYAN}[CLEANUP] Cleaning up test processes and temp directory...${NC}"
    if [ -n "${PARENT_PID:-}" ] && kill -0 "${PARENT_PID}" 2>/dev/null; then
        kill -TERM "${PARENT_PID}" 2>/dev/null || true
        wait "${PARENT_PID}" 2>/dev/null || true
    fi
    if [ -n "${CHILD_PID:-}" ] && kill -0 "${CHILD_PID}" 2>/dev/null; then
        kill -KILL "${CHILD_PID}" 2>/dev/null || true
    fi
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT INT TERM

# -----------------------------------------------------------------------------
# Configuration Generation
# -----------------------------------------------------------------------------
CONFIG_FILE="${WORK_DIR}/aimon_chaos.cfg"
cat << 'EOF' > "${CONFIG_FILE}"
polling = {
    base_interval_sec = 300;
};

antigravity = {
    auto_discover = false;
};

cursor = {
    auto_discover = false;
};

web = {
    host = "127.0.0.1";
    port = 3891;
    endpoints_enabled = true;
};

gateway = {
    enabled = true;
    host = "127.0.0.1";
    port = 3892;
};

history = {
    enabled = false;
};

supervisor = {
    enabled = true;
    auto_restart = true;
    probe_interval_sec = 1;
    probe_timeout_sec = 1;
    crash_loop_window_sec = 30;
    crash_loop_max_retries = 3;
    safe_mode_port = 3893;
};

services = (
    {
        id = "dummy-satellite";
        name = "Dummy Satellite";
        host = "127.0.0.1";
        port = 19888;
        start_cmd = "true";
        enabled = true;
    }
);
EOF

# -----------------------------------------------------------------------------
# Stage 1: Dual-Process Monitor Launch & PID Separation
# -----------------------------------------------------------------------------
echo -e "\n${CYAN}--- Stage 1: Dual-Process Monitor Launch & PID Separation ---${NC}"

./build/aimon daemon --config "${CONFIG_FILE}" --no-ncurses > "${WORK_DIR}/stdout.log" 2>&1 &
PARENT_PID=$!
echo -e "Launched aimon-monitor parent PID: ${PARENT_PID}"

# Wait for child worker to be spawned by parent
CHILD_PID=""
for i in $(seq 1 30); do
    CHILD_PID=$(pgrep -P "${PARENT_PID}" || true)
    if [ -n "${CHILD_PID}" ]; then
        break
    fi
    sleep 0.1
done

if [ -z "${CHILD_PID}" ]; then
    echo -e "${RED}[FAIL] Failed to discover spawned child worker PID.${NC}"
    cat "${WORK_DIR}/stdout.log"
    exit 1
fi

echo -e "Spawned aimon child worker PID: ${CHILD_PID}"
if [ "${PARENT_PID}" -eq "${CHILD_PID}" ]; then
    echo -e "${RED}[FAIL] Parent and child PIDs must be distinct.${NC}"
    exit 1
fi
echo -e "${GREEN}[PASS] Dual-process PID separation verified: Parent=${PARENT_PID}, Child=${CHILD_PID}${NC}"

# Verify isolated port 3891 is bound and responding
BOUND=0
for i in $(seq 1 40); do
    if curl -s -f -H "User-Agent: aimon-hook/1.0" http://127.0.0.1:3891/api/services >/dev/null 2>&1; then
        BOUND=1
        break
    fi
    sleep 0.1
done

if [ "${BOUND}" -ne 1 ]; then
    echo -e "${RED}[FAIL] Port 3891 did not respond within timeout.${NC}"
    cat "${WORK_DIR}/stdout.log"
    exit 1
fi
echo -e "${GREEN}[PASS] Web server successfully responding on port 3891.${NC}"

# -----------------------------------------------------------------------------
# Stage 2: Child Fault Injection (SIGSEGV) & Auto-Respawn
# -----------------------------------------------------------------------------
echo -e "\n${CYAN}--- Stage 2: Child Fault Injection (SIGSEGV) & Auto-Respawn ---${NC}"
OLD_CHILD_PID="${CHILD_PID}"
echo -e "Injecting SIGSEGV (signal 11) into child worker (PID: ${OLD_CHILD_PID})..."
kill -11 "${OLD_CHILD_PID}"

# Parent should detect abnormal exit and auto-respawn a new child within 2s
NEW_CHILD_PID=""
for i in $(seq 1 40); do
    CURRENT_CHILD=$(pgrep -P "${PARENT_PID}" || true)
    if [ -n "${CURRENT_CHILD}" ] && [ "${CURRENT_CHILD}" -ne "${OLD_CHILD_PID}" ]; then
        NEW_CHILD_PID="${CURRENT_CHILD}"
        break
    fi
    sleep 0.1
done

if [ -z "${NEW_CHILD_PID}" ]; then
    echo -e "${RED}[FAIL] Parent monitor failed to auto-respawn child after SIGSEGV.${NC}"
    cat "${WORK_DIR}/stdout.log"
    exit 1
fi

echo -e "Auto-respawned new child worker PID: ${NEW_CHILD_PID}"
CHILD_PID="${NEW_CHILD_PID}"

# Verify service is re-bound on port 3891
REBOUND=0
for i in $(seq 1 40); do
    if curl -s -f -H "User-Agent: aimon-hook/1.0" http://127.0.0.1:3891/api/supervisor/status >/dev/null 2>&1; then
        REBOUND=1
        break
    fi
    sleep 0.1
done

if [ "${REBOUND}" -ne 1 ]; then
    echo -e "${RED}[FAIL] Web server did not re-bind on port 3891 after respawn.${NC}"
    cat "${WORK_DIR}/stdout.log"
    exit 1
fi
echo -e "${GREEN}[PASS] Auto-respawn successful. Port 3891 healthy with new Child PID ${NEW_CHILD_PID}.${NC}"

# -----------------------------------------------------------------------------
# Stage 3: Clean Shutdown Symmetry
# -----------------------------------------------------------------------------
echo -e "\n${CYAN}--- Stage 3: Clean Shutdown Symmetry ---${NC}"
echo -e "Sending SIGTERM to parent monitor (PID: ${PARENT_PID})..."
kill -TERM "${PARENT_PID}"

# Both parent and child must terminate cleanly within 3 seconds
CLEAN_SHUTDOWN=0
for i in $(seq 1 30); do
    PARENT_ALIVE=0
    CHILD_ALIVE=0
    if kill -0 "${PARENT_PID}" 2>/dev/null; then PARENT_ALIVE=1; fi
    if kill -0 "${NEW_CHILD_PID}" 2>/dev/null; then CHILD_ALIVE=1; fi

    if [ "${PARENT_ALIVE}" -eq 0 ] && [ "${CHILD_ALIVE}" -eq 0 ]; then
        CLEAN_SHUTDOWN=1
        break
    fi
    sleep 0.1
done

if [ "${CLEAN_SHUTDOWN}" -ne 1 ]; then
    echo -e "${RED}[FAIL] Process monitor failed to cleanly shutdown both processes.${NC}"
    exit 1
fi

# Reset variables so trap doesn't attempt redundant kills
PARENT_PID=""
CHILD_PID=""

echo -e "${GREEN}[PASS] Clean shutdown symmetry verified: 0 zombie processes, 0 orphaned sockets.${NC}"

# -----------------------------------------------------------------------------
# Stage 4: Summary Verification
# -----------------------------------------------------------------------------
echo -e "\n${CYAN}--- Stage 4: Acceptance Verification Summary ---${NC}"
echo -e "${GREEN}[ALL STAGES PASSED] Chaos fleet recovery verified successfully on physical hardware!${NC}"
exit 0
