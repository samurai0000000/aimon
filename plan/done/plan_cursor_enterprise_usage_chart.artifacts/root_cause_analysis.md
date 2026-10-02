# Root Cause Analysis: Missing Cursor Graph on aimon Dashboard

- **Target**: `http://builder.selfso.com:3883/#quotas`
- **Component**: Cursor Card (`#cursor-spend-chart`, `renderCursorSpend` in `web/app.js`)
- **Date**: 2026-10-02

## 1. Observed Behavior
On the `#quotas` subpanel, the Cursor card's "Cumulative Spend" section displays empty horizontal grid lines ($400, $300, $200, $0) with no trendline, no shaded area, and no date axis.

## 2. Technical Findings

### Frontend (`web/app.js`)
In lines 390–398:
```javascript
const history = cr.daily_spend || [];
if (history.length === 0) {
    if (areaPath) areaPath.setAttribute('d', '');
    if (linePath) linePath.setAttribute('d', '');
    if (dotsGroup) dotsGroup.innerHTML = '';
    if (datesAxis) datesAxis.innerHTML = '';
    return;
}
```
When `cr.daily_spend` is empty, the function clears the SVG paths and immediately aborts rendering.

### Backend (`/api/status` & `CursorCollector.cxx`)
When querying `api2.cursor.sh`:
- Endpoint `/auth/usage-summary` returns:
  - `membershipType`: `"enterprise"`
  - `individualUsage.overall`: `{"used": 12206, "limit": 120000, "remaining": 107794}`
  - `teamUsage.onDemand.used`: `0`
- Endpoint `/aiserver.v1.DashboardService/GetDailySpendByCategory` returns:
  `{"effectiveLimitCents": 120000}` (Status 200, but **no `dailySpend` key**).

### Underlying Database State
`aimon`'s SQLite database (`~/.config/aimon/history.db`) on `builder` is continuously logging samples:
```text
provider: cursor
metric_key: fast_requests_used
metric_value: 12206.0
metric_limit: 120000.0
```
However, the UI and API model only attempt to graph dollar spend (`daily_spend`), ignoring the logged fast request timeseries.

## 3. Recommended Remediation
Implement dual-mode rendering in `CursorCollector` and `web/app.js`:
- Enterprise / Pooled accounts: Graph the **Fast Requests Burn-down** curve based on historical `fast_requests_used` samples.
- Pro accounts with on-demand spend: Maintain the **Cumulative Dollar Spend** curve.
