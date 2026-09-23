/*
 * AimonApp.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.ui

import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Analytics
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.NotificationsActive
import androidx.compose.material.icons.filled.PieChart
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.selfso.aimon.data.AimonRepository
import com.selfso.aimon.data.PairingManager
import com.selfso.aimon.ui.screens.*
import com.selfso.aimon.ui.theme.*
import kotlinx.coroutines.launch

sealed class Screen(val route: String, val title: String, val icon: ImageVector) {
    object Quotas : Screen("quotas", "Quotas", Icons.Default.PieChart)
    object Approvals : Screen("approvals", "Approvals", Icons.Default.NotificationsActive)
    object Telemetry : Screen("telemetry", "Telemetry", Icons.Default.Analytics)
    object Pairing : Screen("pairing", "Pairing", Icons.Default.Lock)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AimonApp(
    repository: AimonRepository,
    pairingManager: PairingManager
) {
    val navController = rememberNavController()
    val scope = rememberCoroutineScope()

    val config by pairingManager.config.collectAsState()
    val quotaStatus by repository.quotaStatus.collectAsState()
    val pendingApprovals by repository.pendingApprovals.collectAsState()
    val telemetry by repository.telemetry.collectAsState()
    val isConnected by repository.isConnected.collectAsState()

    val items = listOf(
        Screen.Quotas,
        Screen.Approvals,
        Screen.Telemetry,
        Screen.Pairing
    )

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        text = "aimon",
                        color = TextPrimary
                    )
                },
                actions = {
                    Badge(
                        containerColor = if (isConnected) GreenAccent.copy(alpha = 0.2f) else RedAccent.copy(alpha = 0.2f)
                    ) {
                        Text(
                            text = if (isConnected) "ONLINE" else "OFFLINE",
                            color = if (isConnected) GreenAccent else RedAccent,
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = DarkBackground)
            )
        },
        bottomBar = {
            NavigationBar(containerColor = DarkBackground) {
                val navBackStackEntry by navController.currentBackStackEntryAsState()
                val currentRoute = navBackStackEntry?.destination?.route

                items.forEach { screen ->
                    NavigationBarItem(
                        icon = {
                            if (screen == Screen.Approvals && pendingApprovals.isNotEmpty()) {
                                BadgedBox(badge = { Badge { Text("${pendingApprovals.size}") } }) {
                                    Icon(screen.icon, contentDescription = screen.title)
                                }
                            } else {
                                Icon(screen.icon, contentDescription = screen.title)
                            }
                        },
                        label = { Text(screen.title) },
                        selected = currentRoute == screen.route,
                        colors = NavigationBarItemDefaults.colors(
                            selectedIconColor = CyanAccent,
                            selectedTextColor = CyanAccent,
                            unselectedIconColor = TextSecondary,
                            unselectedTextColor = TextSecondary,
                            indicatorColor = CyanAccent.copy(alpha = 0.15f)
                        ),
                        onClick = {
                            navController.navigate(screen.route) {
                                popUpTo(navController.graph.findStartDestination().id) {
                                    saveState = true
                                }
                                launchSingleTop = true
                                restoreState = true
                            }
                        }
                    )
                }
            }
        },
        containerColor = DarkBackground
    ) { innerPadding ->
        NavHost(
            navController = navController,
            startDestination = if (config.isPaired) Screen.Quotas.route else Screen.Pairing.route,
            modifier = Modifier.padding(innerPadding)
        ) {
            composable(Screen.Quotas.route) {
                QuotasScreen(
                    quotaStatus = quotaStatus,
                    onRefresh = { scope.launch { repository.refreshQuotas() } }
                )
            }
            composable(Screen.Approvals.route) {
                ApprovalsScreen(
                    pendingApprovals = pendingApprovals,
                    onDecision = { id, dec -> scope.launch { repository.submitDecision(id, dec) } },
                    onRefresh = { scope.launch { repository.refreshApprovals() } }
                )
            }
            composable(Screen.Telemetry.route) {
                TelemetryScreen(
                    telemetry = telemetry,
                    onRefresh = { scope.launch { repository.refreshTelemetry() } }
                )
            }
            composable(Screen.Pairing.route) {
                PairingScreen(
                    config = config,
                    onPair = { host, port, secret ->
                        scope.launch {
                            val res = repository.pairWithDaemon(host, port, secret)
                            if (res.isSuccess) {
                                navController.navigate(Screen.Quotas.route) {
                                    popUpTo(0)
                                }
                            }
                        }
                    },
                    onUnpair = {
                        pairingManager.clearPairing()
                    }
                )
            }
        }
    }
}
