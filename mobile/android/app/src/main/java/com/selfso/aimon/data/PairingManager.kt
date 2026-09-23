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

class PairingManager(private val context: Context? = null) {

    private val inMemoryMap = mutableMapOf<String, Any>()

    private val prefs: SharedPreferences? by lazy {
        if (context == null) null
        else {
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
    }

    private val _config = MutableStateFlow(loadConfig())
    val config: StateFlow<ConnectionConfig> = _config.asStateFlow()

    private fun loadConfig(): ConnectionConfig {
        val host = prefs?.getString("host", "127.0.0.1") ?: (inMemoryMap["host"] as? String ?: "127.0.0.1")
        val port = prefs?.getInt("port", 3883) ?: (inMemoryMap["port"] as? Int ?: 3883)
        val secret = prefs?.getString("secret", "") ?: (inMemoryMap["secret"] as? String ?: "")
        val token = prefs?.getString("token", "") ?: (inMemoryMap["token"] as? String ?: "")
        val isPaired = (prefs?.getBoolean("is_paired", false) ?: (inMemoryMap["is_paired"] as? Boolean ?: false)) && token.isNotEmpty()

        return ConnectionConfig(
            host = host,
            port = port,
            secret = secret,
            token = token,
            isPaired = isPaired
        )
    }

    fun getDeviceId(): String {
        var id = prefs?.getString("device_id", null) ?: (inMemoryMap["device_id"] as? String)
        if (id == null) {
            id = "android-" + UUID.randomUUID().toString().take(12)
            prefs?.edit()?.putString("device_id", id)?.apply() ?: run { inMemoryMap["device_id"] = id }
        }
        return id
    }

    fun getDeviceName(): String {
        return try {
            "${Build.MANUFACTURER} ${Build.MODEL}".trim().ifEmpty { "Android Test Client" }
        } catch (_: Throwable) {
            "Android Test Client"
        }
    }

    fun savePairing(host: String, port: Int, secret: String, token: String) {
        prefs?.edit()
            ?.putString("host", host)
            ?.putInt("port", port)
            ?.putString("secret", secret)
            ?.putString("token", token)
            ?.putBoolean("is_paired", true)
            ?.apply() ?: run {
                inMemoryMap["host"] = host
                inMemoryMap["port"] = port
                inMemoryMap["secret"] = secret
                inMemoryMap["token"] = token
                inMemoryMap["is_paired"] = true
            }

        _config.value = ConnectionConfig(
            host = host,
            port = port,
            secret = secret,
            token = token,
            isPaired = true
        )
    }

    fun clearPairing() {
        prefs?.edit()
            ?.remove("token")
            ?.remove("secret")
            ?.putBoolean("is_paired", false)
            ?.apply() ?: run {
                inMemoryMap.remove("token")
                inMemoryMap.remove("secret")
                inMemoryMap["is_paired"] = false
            }

        _config.value = _config.value.copy(
            secret = "",
            token = "",
            isPaired = false
        )
    }
}
