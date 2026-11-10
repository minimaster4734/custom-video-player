package com.leandro.customplayer

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.view.View
import android.widget.ArrayAdapter
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.graphics.drawable.DrawableCompat
import androidx.documentfile.provider.DocumentFile
import com.leandro.customplayer.databinding.ActivityMainBinding
import java.io.File

/**
 * Library screen: settings at the top left, folder picker at the top right,
 * and the videos of the chosen folder as a list. Tapping one opens the
 * player. (Same layout conventions as the audio player this app is modelled
 * on.)
 */
class MainActivity : AppCompatActivity() {

    private class Entry(val name: String, val uri: Uri)

    private lateinit var binding: ActivityMainBinding
    private lateinit var settings: AppSettings
    private lateinit var prefs: PlaybackPrefs
    private var entries: List<Entry> = emptyList()

    private val legacyBrowser = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { r ->
        r.data?.getStringExtra(LegacyFileBrowserActivity.EXTRA_PATH)?.let { loadLegacyFolder(it) }
    }

    private val safFolder = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            loadSafFolder(uri)
        }
    }

    private val singleFile = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) play(uri)
    }

    private val storagePermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) {
        restoreLastFolder()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        settings = AppSettings(this)
        prefs = PlaybackPrefs(this)

        val gear = ContextCompat.getDrawable(this, R.drawable.ic_settings)!!.mutate()
        DrawableCompat.setTint(gear, Color.WHITE)
        binding.toolbar.navigationIcon = gear
        binding.toolbar.navigationContentDescription = getString(R.string.settings)
        binding.toolbar.setNavigationOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        binding.toolbar.setTitle(R.string.app_name)
        binding.toolbar.setTitleTextColor(Color.WHITE)
        binding.toolbar.inflateMenu(R.menu.main_menu)
        binding.toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.action_open_folder -> { pickFolder(); true }
                R.id.action_open_file -> { singleFile.launch(arrayOf("video/*", "application/octet-stream")); true }
                else -> false
            }
        }

        binding.videoList.setOnItemClickListener { _, _, position, _ ->
            entries.getOrNull(position)?.let { play(it.uri) }
        }

        // Legacy folders need the storage permission before they can be listed.
        val last = prefs.lastFolder
        if (last != null && !last.startsWith("content:") && !hasStoragePermission()) {
            storagePermission.launch(Manifest.permission.READ_EXTERNAL_STORAGE)
        } else {
            restoreLastFolder()
        }
    }

    private fun hasStoragePermission() =
        ContextCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE) ==
            PackageManager.PERMISSION_GRANTED

    private fun restoreLastFolder() {
        val last = prefs.lastFolder ?: return showEntries(emptyList())
        if (last.startsWith("content:")) loadSafFolder(Uri.parse(last)) else loadLegacyFolder(last)
    }

    private fun pickFolder() {
        if (settings.getPickerMode() == AppSettings.PICKER_LEGACY) {
            legacyBrowser.launch(Intent(this, LegacyFileBrowserActivity::class.java))
        } else {
            safFolder.launch(null)
        }
    }

    private fun loadLegacyFolder(path: String) {
        prefs.lastFolder = path
        val files = File(path).listFiles { f -> f.isFile && VideoFiles.isVideoName(f.name) }
            ?.sortedBy { it.name.lowercase() } ?: emptyList()
        showEntries(files.map { Entry(it.name, Uri.fromFile(it)) })
    }

    private fun loadSafFolder(tree: Uri) {
        prefs.lastFolder = tree.toString()
        val dir = DocumentFile.fromTreeUri(this, tree)
        val files = dir?.listFiles()
            ?.filter { f ->
                f.isFile && (VideoFiles.isVideoName(f.name ?: "") || (f.type ?: "").startsWith("video/"))
            }
            ?.sortedBy { (it.name ?: "").lowercase() } ?: emptyList()
        showEntries(files.map { Entry(it.name ?: "?", it.uri) })
    }

    private fun showEntries(list: List<Entry>) {
        entries = list
        binding.videoList.adapter = ArrayAdapter(
            this, R.layout.list_item_video, android.R.id.text1, list.map { it.name }
        )
        binding.emptyText.visibility = if (list.isEmpty()) View.VISIBLE else View.GONE
    }

    private fun play(uri: Uri) {
        startActivity(Intent(this, PlayerActivity::class.java).setData(uri))
    }
}
