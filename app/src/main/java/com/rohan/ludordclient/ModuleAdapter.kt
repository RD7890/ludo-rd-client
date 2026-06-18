package com.rohan.ludordclient

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

@Suppress("DEPRECATION")
class ModuleAdapter(
    private val modules: MutableList<ModuleItem>,
    private val onToggle: (ModuleItem) -> Unit,
    private val onSlider: (ModuleItem, Int) -> Unit
) : RecyclerView.Adapter<ModuleAdapter.VH>() {

    inner class VH(view: View) : RecyclerView.ViewHolder(view) {
        val tvTitle: TextView = view.findViewById(R.id.tvModuleTitle)
        val tvDesc: TextView = view.findViewById(R.id.tvModuleDesc)
        val switch: Switch = view.findViewById(R.id.switchModule)
        val seekBar: SeekBar = view.findViewById(R.id.seekBarModule)
        val tvSliderValue: TextView = view.findViewById(R.id.tvSliderValue)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_module, parent, false)
        return VH(view)
    }

    override fun onBindViewHolder(holder: VH, position: Int) {
        val module = modules[position]
        holder.tvTitle.text = module.title
        holder.tvDesc.text = module.description

        if (module.type == ModuleType.TOGGLE) {
            holder.switch.visibility = View.VISIBLE
            holder.seekBar.visibility = View.GONE
            holder.tvSliderValue.visibility = View.GONE
            holder.switch.isChecked = module.isEnabled
            holder.switch.setOnCheckedChangeListener { _, checked ->
                module.isEnabled = checked
                onToggle(module)
            }
        } else {
            holder.switch.visibility = View.GONE
            holder.seekBar.visibility = View.VISIBLE
            holder.tvSliderValue.visibility = View.VISIBLE
            holder.seekBar.max = module.sliderMax
            holder.seekBar.min = module.sliderMin
            holder.seekBar.progress = module.sliderValue
            holder.tvSliderValue.text = if (module.sliderValue == 0) "Random" else module.sliderValue.toString()
            holder.seekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar, progress: Int, fromUser: Boolean) {
                    if (fromUser) {
                        module.sliderValue = progress
                        holder.tvSliderValue.text = if (progress == 0) "Random" else progress.toString()
                        onSlider(module, progress)
                    }
                }
                override fun onStartTrackingTouch(sb: SeekBar) {}
                override fun onStopTrackingTouch(sb: SeekBar) {}
            })
        }
    }

    override fun getItemCount() = modules.size
}
