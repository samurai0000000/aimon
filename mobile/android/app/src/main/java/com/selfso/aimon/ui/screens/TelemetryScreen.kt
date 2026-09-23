/*
 * TelemetryScreen.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.selfso.aimon.data.TelemetryOverview
import com.selfso.aimon.ui.theme.*

@Composable
fun TelemetryScreen(
    telemetry: TelemetryOverview?,
    onRefresh: () -> Unit
) {
    if (telemetry == null) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                CircularProgressIndicator(color = CyanAccent)
                Spacer(modifier = Modifier.height(16.dp))
                Text(text = "Loading agent telemetry...", color = TextSecondary, fontSize = 14.sp)
            }
        }
        return
    }

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = Icons.Default.Speed,
                        contentDescription = null,
                        tint = CyanAccent,
                        modifier = Modifier.size(24.dp)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = "Agent Execution Telemetry",
                        fontSize = 20.sp,
                        fontWeight = FontWeight.Bold,
                        color = TextPrimary
                    )
                }
                IconButton(onClick = onRefresh) {
                    Icon(
                        imageVector = Icons.Default.Refresh,
                        contentDescription = "Refresh",
                        tint = CyanAccent
                    )
                }
            }
        }

        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                KpiCard(
                    title = "Active Sessions",
                    value = "${telemetry.activeSessions}",
                    sub = "live agents",
                    valueColor = if (telemetry.activeSessions > 0) GreenAccent else TextSecondary,
                    modifier = Modifier.weight(1f)
                )
                KpiCard(
                    title = "Total Tool Calls",
                    value = String.format("%,d", telemetry.totalToolCalls),
                    sub = "invocations (24h)",
                    valueColor = CyanAccent,
                    modifier = Modifier.weight(1f)
                )
            }
        }

        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                val avgSec = String.format("%.2f", telemetry.avgTurnLatencyMs / 1000.0)
                KpiCard(
                    title = "Avg Step Duration",
                    value = "$avgSec s",
                    sub = "turn latency",
                    modifier = Modifier.weight(1f)
                )
                KpiCard(
                    title = "Tool Error Rate",
                    value = "${String.format("%.1f", telemetry.errorRatePct)}%",
                    sub = if (telemetry.errorRatePct == 0.0) "all healthy" else "failures",
                    valueColor = if (telemetry.errorRatePct > 5.0) RedAccent else if (telemetry.errorRatePct > 0.0) AmberAccent else GreenAccent,
                    modifier = Modifier.weight(1f)
                )
            }
        }

        item {
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = CardDefaults.cardColors(containerColor = DarkSurface)
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(
                        text = "System Gateway Health",
                        fontSize = 15.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = TextPrimary
                    )
                    Spacer(modifier = Modifier.height(12.dp))

                    HealthRow(name = "aimon Core Daemon", status = "OPERATIONAL", statusColor = GreenAccent)
                    Spacer(modifier = Modifier.height(8.dp))
                    HealthRow(name = "Mobile SSE Event Stream", status = "CONNECTED", statusColor = CyanAccent)
                    Spacer(modifier = Modifier.height(8.dp))
                    HealthRow(name = "Agent Approval Interceptor", status = "ARMED", statusColor = GreenAccent)
                }
            }
        }
    }
}

@Composable
private fun HealthRow(name: String, status: String, statusColor: Color) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .background(Color.Black.copy(alpha = 0.2f))
            .padding(horizontal = 12.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(text = name, fontSize = 13.sp, color = TextPrimary)
        Badge(containerColor = statusColor.copy(alpha = 0.2f)) {
            Text(
                text = status,
                color = statusColor,
                fontSize = 11.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
            )
        }
    }
}

@Composable
private fun KpiCard(
    title: String,
    value: String,
    sub: String,
    modifier: Modifier = Modifier,
    valueColor: Color = TextPrimary
) {
    Card(
        modifier = modifier,
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = DarkSurface)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(text = title, fontSize = 12.sp, color = TextSecondary)
            Spacer(modifier = Modifier.height(6.dp))
            Text(
                text = value,
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
                color = valueColor
            )
            Spacer(modifier = Modifier.height(2.dp))
            Text(text = sub, fontSize = 11.sp, color = TextSecondary)
        }
    }
}
