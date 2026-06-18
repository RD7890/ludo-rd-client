package com.rohan.ludordclient

import android.content.Context
import java.io.File

object ModConfig {
    var redAlwaysWins: Boolean = false
    var forceDice: Int = 0
    var aiDumbMode: Boolean = false
    var speedMultiplier: Int = 1

    private val hookConfigDir  = "/sdcard/LudoRD"
    private val hookConfigPath = "$hookConfigDir/config.conf"

    fun saveToPrefs(context: Context) {
        context.getSharedPreferences("ludo_mods", Context.MODE_PRIVATE).edit().apply {
            putBoolean("red_always_wins", redAlwaysWins)
            putInt("force_dice", forceDice)
            putBoolean("ai_dumb", aiDumbMode)
            putInt("speed_multiplier", speedMultiplier)
        }.apply()

        writeHookConfig()
    }

    fun loadFromPrefs(context: Context) {
        val prefs = context.getSharedPreferences("ludo_mods", Context.MODE_PRIVATE)
        redAlwaysWins   = prefs.getBoolean("red_always_wins", false)
        forceDice       = prefs.getInt("force_dice", 0)
        aiDumbMode      = prefs.getBoolean("ai_dumb", false)
        speedMultiplier = prefs.getInt("speed_multiplier", 1)
    }

    private fun writeHookConfig() {
        try {
            File(hookConfigDir).mkdirs()
            val config = buildString {
                appendLine("force_dice=$forceDice")
                appendLine("red_always_wins=${if (redAlwaysWins) 1 else 0}")
                appendLine("ai_dumb=${if (aiDumbMode) 1 else 0}")
                appendLine("speed_multiplier=$speedMultiplier")
            }
            File(hookConfigPath).writeText(config)
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }
}
