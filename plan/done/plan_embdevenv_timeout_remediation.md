# Debugging `embdevenv` Subsystem Timeout

The recent firmware update on `n1-655-pro` (kernel `6.1.112-linuxkit-5ca3b9972c27-custom-dirty`) appears to have disrupted the telemetry/hardware bridge daemon (`embdevenv`) running on the node. Because the `aimon` MCP Gateway is reporting `Timed out waiting for subsystem 'embdevenv'`, it is highly likely that the `embdevenv` daemon on `n1-655-pro` either failed to start on boot, crashed when attempting to open the UART ports, or cannot establish a TCP connection back to the `aimon` hub.

## Proposed Diagnostic Steps

I recommend a methodical top-down approach to isolate the communication break:

### Phase 1: Verify `embdevenv` Process State
- SSH into `n1-655-pro` and check if the `embdevenv` daemon is running (`ps aux | grep embdevenv` or checking the relevant systemd/init service).
- If it is not running, attempt to start it manually to observe any immediate crash logs or panics.

### Phase 2: Inspect Hardware UART Initialization (Kernel Level)
- Run `dmesg | grep ttyS` on `n1-655-pro` to confirm that `/dev/ttyS1` and `/dev/ttyS2` were actually initialized by the new kernel. 
- A common issue with custom kernel builds is missing serial drivers or different device node numbering, which would cause `embdevenv` to crash when attempting to bind to them.

### Phase 3: Review `embdevenv` Daemon Logs
- Inspect the daemon logs (e.g., `/var/log/embdevenv.log` or `journalctl -u embdevenv`) on `n1-655-pro` for any connection errors to the `aimon` hub's IP/port, or permission denied errors on the TTY devices.

### Phase 4: Network and TCP Gateway Checks
- Check if `n1-655-pro` can ping the `aimon` host (`builder`).
- Check the `aimon` logs on `builder` to see if `embdevenv` is actively attempting to connect but failing authentication or dropping immediately.

## Verification Plan

Once the root cause is identified and the daemon is restored:
- We will verify that `aimon` sees the subsystem via the `TcpGateway`.
- We will re-run the Phase 1 verification step: sending test strings to `/dev/ttyS1` and `/dev/ttyS2` and validating the output via the `embdevenv_console_expect` MCP tool.
