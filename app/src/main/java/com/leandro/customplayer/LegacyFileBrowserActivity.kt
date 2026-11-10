package com.leandro.customplayer

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Environment
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.leandro.customplayer.databinding.ActivityLegacyBrowserBinding
import java.io.File

/**
 * Direct java.io.File browser -- works reliably on Android 7-9 (before
 * scoped storage), which is this app's actual target. On Android 10+ this
 * will likely see mostly-empty folders outside the app's own sandbox; the
 * system (SAF) picker is the one to use there instead.
 */
class LegacyFileBrowserActivity : AppCompatActivity() {

    private lateinit var binding: ActivityLegacyBrowserBinding
    private lateinit var settings: AppSettings
    private var currentDir: File = Environment.getExternalStorageDirectory()
    private var entries: List<File> = emptyList()

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) {
            refresh()
        } else {
            Toast.makeText(this, R.string.legacy_permission_denied, Toast.LENGTH_LONG).show()
            finish()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityLegacyBrowserBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)

        settings = AppSettings(this)
        settings.getLastLegacyPath()?.let { saved ->
            val f = File(saved)
            if (f.isDirectory) currentDir = f
        }

        binding.btnUseFolder.setOnClickListener {
            settings.setLastLegacyPath(currentDir.absolutePath)
            setResult(RESULT_OK, Intent().putExtra(EXTRA_PATH, currentDir.absolutePath))
            finish()
        }

        binding.fileList.setOnItemClickListener { _, _, position, _ ->
            val hasParent = currentDir.parentFile != null
            if (hasParent && position == 0) {
                currentDir = currentDir.parentFile!!
                refresh()
            } else {
                val idx = if (hasParent) position - 1 else position
                val entry = entries.getOrNull(idx) ?: return@setOnItemClickListener
                if (entry.isDirectory) {
                    currentDir = entry
                    refresh()
                }
            }
        }

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE)
            == PackageManager.PERMISSION_GRANTED
        ) {
            refresh()
        } else {
            permissionLauncher.launch(Manifest.permission.READ_EXTERNAL_STORAGE)
        }
    }

    override fun onSupportNavigateUp(): Boolean {
        finish()
        return true
    }

    private fun refresh() {
        supportActionBar?.title = currentDir.absolutePath

        val children = currentDir.listFiles { f ->
            f.isDirectory || f.name.let { n ->
                VideoFiles.isVideoName(n)
            }
        }?.sortedWith(compareBy({ !it.isDirectory }, { it.name.lowercase() })) ?: emptyList()
        entries = children

        val labels = mutableListOf<String>()
        if (currentDir.parentFile != null) labels.add(getString(R.string.up_one_level))
        labels.addAll(children.map { if (it.isDirectory) "\uD83D\uDCC1 ${it.name}" else it.name })

        binding.fileList.adapter = ArrayAdapter(
            this, R.layout.list_item_video, android.R.id.text1, labels
        )
    }

    companion object {
        const val EXTRA_PATH = "path"
    }
}
