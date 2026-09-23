/*
 * PairingManager.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.data

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.UUID

class PairingManager(private val context: Context) {

    private val prefs: SharedPreferences by lazy {
        try {
            val masterKey = MasterKey.Builder(context)
                .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
                .build()

            EncryptedSharedPreferences.create(
                context,
                "aimon_secure_prefs",
                masterKey,
                EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
                EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
            )
        } catch (e: Exception) {
            context.getSharedPreferences("aimon_fallback_prefs", Context.MODE_PRIVATE)
        }
    }

    private val _config = MutableStateFlow(loadConfig())
    val config: StateFlow<ConnectionConfig> = _config.asStateFlow()

    private fun loadConfig(): ConnectionConfig {
        val host = prefs.getString("host", "127.0.0.1") ?: "127.0.0.1"
        val port = prefs.getInt("port", 3883)
        val secret = prefs.getString("secret", "") ?: ""
        val token = prefs.getString("token", "") ?: ""
        val isPaired = prefs.getBoolean("is_paired", false) && token.isNotEmpty()

        return ConnectionConfig(
            host = host,
            port = port,
            secret = secret,
            token = token,
            isPaired = isPaired
        )
    }

    fun getDeviceId(): String {
        var id = prefs.getString("device_id", null)
        if (id == null) {
            id = "android-" + UUID.randomUUID().toString().take(12)
            prefs.edit().putString("device_id", id).apply()
        }
        return id
    }

    fun getDeviceName(): String {
        return "${Build.MANUFACTURER} ${Build.MODEL}"
    }

    fun savePairing(host: String, port: Int, secret: String, token: String) {
        prefs.edit()
            .putString("host", host)
            .putInt("port", port)
            .putString("secret", secret)
            .putString("token", token)
            .putBoolean("is_paired", true)
            .apply()

        _config.value = ConnectionConfig(
            host = host,
            port = port,
            secret = secret,
            token = token,
            isPaired = true
        )
    }

    fun clearPairing() {
        prefs.edit()
            .remove("token")
            .remove("secret")
            .putBoolean("is_paired", false)
            .apply()

        _config.value = _config.value.copy(
            secret = "",
            token = "",
            isPaired = false
        )
    }
}
