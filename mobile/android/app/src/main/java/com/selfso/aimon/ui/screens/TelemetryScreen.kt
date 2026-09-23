/*
 * TelemetryScreen.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
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
            CircularProgressIndicator(color = CyanAccent)
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
            Text(
                text = "Agent Execution Telemetry",
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold,
                color = TextPrimary
            )
        }

        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                KpiCard(
                    title = "Active Sessions",
                    value = "${telemetry.activeSessions}",
                    sub = "running",
                    modifier = Modifier.weight(1f)
                )
                KpiCard(
                    title = "Total Tool Calls",
                    value = "${telemetry.totalToolCalls}",
                    sub = "calls (24h)",
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
                    sub = "failures",
                    valueColor = if (telemetry.errorRatePct > 5.0) AmberAccent else GreenAccent,
                    modifier = Modifier.weight(1f)
                )
            }
        }
    }
}

@Composable
private fun KpiCard(
    title: String,
    value: String,
    sub: String,
    modifier: Modifier = Modifier,
    valueColor: androidx.compose.ui.graphics.Color = TextPrimary
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
