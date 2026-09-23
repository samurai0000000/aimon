/*
 * PairingScreen.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.selfso.aimon.data.ConnectionConfig
import com.selfso.aimon.ui.theme.*

@Composable
fun PairingScreen(
    config: ConnectionConfig,
    onPair: (String, Int, String) -> Unit,
    onUnpair: () -> Unit
) {
    var host by remember { mutableStateOf(config.host) }
    var portStr by remember { mutableStateOf(config.port.toString()) }
    var secret by remember { mutableStateOf(config.secret) }
    var isLoading by remember { mutableStateOf(false) }
    var errorMessage by remember { mutableStateOf<String?>(null) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text(
            text = if (config.isPaired) "Device Paired" else "Pair with aimon Daemon",
            fontSize = 22.sp,
            fontWeight = FontWeight.Bold,
            color = TextPrimary
        )

        Spacer(modifier = Modifier.height(8.dp))

        Text(
            text = if (config.isPaired)
                "Linked to ${config.host}:${config.port}"
            else
                "Scan the QR code from the Web Dashboard or enter the 128-bit pairing secret manually.",
            fontSize = 13.sp,
            color = TextSecondary,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center
        )

        Spacer(modifier = Modifier.height(24.dp))

        if (config.isPaired) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = CardDefaults.cardColors(containerColor = DarkSurface)
            ) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(text = "Daemon Gateway", fontSize = 12.sp, color = TextSecondary)
                    Text(
                        text = "http://${config.host}:${config.port}",
                        fontSize = 15.sp,
                        fontFamily = FontFamily.Monospace,
                        color = CyanAccent
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(text = "Security Token", fontSize = 12.sp, color = TextSecondary)
                    Text(
                        text = "••••••••••••••••" + config.token.takeLast(6),
                        fontSize = 13.sp,
                        fontFamily = FontFamily.Monospace,
                        color = TextPrimary
                    )
                }
            }

            Spacer(modifier = Modifier.height(24.dp))

            Button(
                onClick = onUnpair,
                colors = ButtonDefaults.buttonColors(containerColor = RedAccent.copy(alpha = 0.25f)),
                shape = RoundedCornerShape(12.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Unpair Device", color = RedAccent, fontWeight = FontWeight.Bold)
            }
        } else {
            OutlinedTextField(
                value = host,
                onValueChange = { host = it },
                label = { Text("Daemon Host / IP") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = CyanAccent,
                    focusedLabelColor = CyanAccent
                )
            )

            Spacer(modifier = Modifier.height(12.dp))

            OutlinedTextField(
                value = portStr,
                onValueChange = { portStr = it },
                label = { Text("Port (default 3883)") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = CyanAccent,
                    focusedLabelColor = CyanAccent
                )
            )

            Spacer(modifier = Modifier.height(12.dp))

            OutlinedTextField(
                value = secret,
                onValueChange = { secret = it },
                label = { Text("128-bit Pairing Secret") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = CyanAccent,
                    focusedLabelColor = CyanAccent
                )
            )

            Spacer(modifier = Modifier.height(24.dp))

            Button(
                onClick = {
                    val p = portStr.toIntOrNull() ?: 3883
                    isLoading = true
                    errorMessage = null
                    onPair(host, p, secret)
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(48.dp),
                colors = ButtonDefaults.buttonColors(containerColor = CyanAccent),
                shape = RoundedCornerShape(12.dp),
                enabled = !isLoading && secret.isNotEmpty()
            ) {
                if (isLoading) {
                    CircularProgressIndicator(color = Color.Black, modifier = Modifier.size(20.dp))
                } else {
                    Text("Pair Now", color = Color.Black, fontWeight = FontWeight.Bold)
                }
            }
        }
    }
}
