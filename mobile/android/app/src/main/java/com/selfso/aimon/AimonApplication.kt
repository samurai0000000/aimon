/*
 * AimonApplication.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import android.os.Build
import com.selfso.aimon.data.AimonRepository
import com.selfso.aimon.data.PairingManager

class AimonApplication : Application() {

    lateinit var pairingManager: PairingManager
        private set

    lateinit var repository: AimonRepository
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this

        pairingManager = PairingManager(this)
        repository = AimonRepository(this, pairingManager)

        createNotificationChannels()
    }

    private fun createNotificationChannels() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val approvalChannel = NotificationChannel(
                CHANNEL_APPROVALS,
                "Action Approvals",
                NotificationManager.IMPORTANCE_HIGH
            ).apply {
                description = "High-priority action confirmation alerts from Antigravity and Cursor IDE"
                enableVibration(true)
                setShowBadge(true)
            }

            val serviceChannel = NotificationChannel(
                CHANNEL_SERVICE,
                "Background Connection",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Persistent daemon gateway WebSocket connection"
                setShowBadge(false)
            }

            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(approvalChannel)
            nm.createNotificationChannel(serviceChannel)
        }
    }

    companion object {
        const val CHANNEL_APPROVALS = "aimon_approvals"
        const val CHANNEL_SERVICE = "aimon_service"

        lateinit var instance: AimonApplication
            private set
    }
}
