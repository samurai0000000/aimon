/*
 * MainActivity.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.core.content.ContextCompat
import com.selfso.aimon.service.AimonWebSocketService
import com.selfso.aimon.ui.AimonApp
import com.selfso.aimon.ui.theme.AimonTheme
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        handleIntent(intent)
        startWebSocketService()

        setContent {
            AimonTheme {
                AimonApp(
                    repository = AimonApplication.instance.repository,
                    pairingManager = AimonApplication.instance.pairingManager
                )
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent?) {
        val data: Uri? = intent?.data
        if (data != null && data.scheme == "aimon" && data.host == "pair") {
            val host = data.getQueryParameter("host") ?: "127.0.0.1"
            val port = data.getQueryParameter("port")?.toIntOrNull() ?: 3883
            val secret = data.getQueryParameter("secret") ?: ""

            if (secret.isNotEmpty()) {
                CoroutineScope(Dispatchers.IO).launch {
                    AimonApplication.instance.repository.pairWithDaemon(host, port, secret)
                }
            }
        }
    }

    private fun startWebSocketService() {
        val serviceIntent = Intent(this, AimonWebSocketService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            ContextCompat.startForegroundService(this, serviceIntent)
        } else {
            startService(serviceIntent)
        }
    }
}
