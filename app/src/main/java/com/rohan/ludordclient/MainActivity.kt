package com.rohan.ludordclient

import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView

class MainActivity : AppCompatActivity() {

    private val conn = ModConnection()
    private val handler = Handler(Looper.getMainLooper())
    private lateinit var adapter: ModuleAdapter
    private lateinit var tvStatus: TextView
    private lateinit var btnLaunch: Button

    private val modules = mutableListOf(
        ModuleItem("red_win",    "🔴 Red Always Wins",   "Red player wins instantly",        type = ModuleType.TOGGLE),
        ModuleItem("force_dice", "🎲 Force Dice Value",   "Lock dice to specific number",     type = ModuleType.SLIDER, sliderMin = 0, sliderMax = 6),
        ModuleItem("ai_dumb",   "🤖 AI Dumb Mode",      "AI makes worst moves possible",    type = ModuleType.TOGGLE),
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tvStatus)
        btnLaunch = findViewById(R.id.btnLaunch)
        val rvModules: RecyclerView = findViewById(R.id.rvModules)

        adapter = ModuleAdapter(modules,
            onToggle = { handleToggle(it) },
            onSlider = { mod, value -> handleSlider(mod, value) }
        )
        rvModules.layoutManager = LinearLayoutManager(this)
        rvModules.adapter = adapter

        btnLaunch.setOnClickListener { launchGame() }

        tryConnect()
    }

    private fun tryConnect() {
        Thread {
            val ok = conn.connect()
            handler.post {
                if (ok) {
                    tvStatus.text = "● Connected (Live Mode)"
                    tvStatus.setTextColor(getColor(R.color.green))
                } else {
                    tvStatus.text = "● Offline (Config Mode)"
                    tvStatus.setTextColor(getColor(R.color.yellow))
                }
            }
        }.start()
    }

    private fun handleToggle(module: ModuleItem) {
        when (module.id) {
            "red_win" -> ModConfig.redAlwaysWins = module.isEnabled
            "ai_dumb" -> ModConfig.aiDumbMode    = module.isEnabled
        }
        ModConfig.saveToPrefs(this)

        if (conn.isConnected) {
            Thread {
                when (module.id) {
                    "red_win" -> conn.setRedWin(module.isEnabled)
                    "ai_dumb" -> conn.setAiDumb(module.isEnabled)
                }
            }.start()
        }
    }

    private fun handleSlider(module: ModuleItem, value: Int) {
        when (module.id) {
            "force_dice" -> ModConfig.forceDice = value
        }
        ModConfig.saveToPrefs(this)
        if (conn.isConnected) {
            Thread { conn.setDice(value) }.start()
        }
    }

    private fun launchGame() {
        ModConfig.saveToPrefs(this)
        val intent = packageManager.getLaunchIntentForPackage("com.ludo.king")
        if (intent != null) {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            intent.putExtra("LUDO_CLIENT_ENABLED", true)
            startActivity(intent)
        } else {
            Toast.makeText(this, "Game not installed!", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        conn.disconnect()
    }
}
