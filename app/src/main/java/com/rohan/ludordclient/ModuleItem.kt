package com.rohan.ludordclient

data class ModuleItem(
    val id: String,
    val title: String,
    val description: String,
    var isEnabled: Boolean = false,
    val type: ModuleType = ModuleType.TOGGLE,
    var sliderValue: Int = 0,
    val sliderMin: Int = 0,
    val sliderMax: Int = 6
)

enum class ModuleType { TOGGLE, SLIDER }
