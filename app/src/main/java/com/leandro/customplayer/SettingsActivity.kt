package com.leandro.customplayer

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import com.leandro.customplayer.databinding.ActivitySettingsBinding

class SettingsActivity : AppCompatActivity() {

    private lateinit var binding: ActivitySettingsBinding
    private lateinit var settings: AppSettings

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        supportActionBar?.title = getString(R.string.settings)

        settings = AppSettings(this)

        when (settings.getTheme()) {
            AppSettings.THEME_LIGHT -> binding.radioThemeLight.isChecked = true
            AppSettings.THEME_DARK -> binding.radioThemeDark.isChecked = true
            else -> binding.radioThemeSystem.isChecked = true
        }
        binding.themeGroup.setOnCheckedChangeListener { _, checkedId ->
            val mode = when (checkedId) {
                R.id.radioThemeLight -> AppSettings.THEME_LIGHT
                R.id.radioThemeDark -> AppSettings.THEME_DARK
                else -> AppSettings.THEME_SYSTEM
            }
            settings.setTheme(mode)
            recreate()
        }

        when (settings.getPickerMode()) {
            AppSettings.PICKER_LEGACY -> binding.radioPickerLegacy.isChecked = true
            else -> binding.radioPickerSaf.isChecked = true
        }
        binding.pickerGroup.setOnCheckedChangeListener { _, checkedId ->
            val mode = when (checkedId) {
                R.id.radioPickerLegacy -> AppSettings.PICKER_LEGACY
                else -> AppSettings.PICKER_SAF
            }
            settings.setPickerMode(mode)
        }
    }

    override fun onSupportNavigateUp(): Boolean {
        finish()
        return true
    }
}
