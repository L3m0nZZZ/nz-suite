package com.shoubo.tv.config

import android.content.Context
import android.os.Environment
import android.os.FileObserver
import android.os.Handler
import android.os.Looper
import com.shoubo.tv.util.Logs
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * 配置管理（文档 4.7）：
 * 1. 优先读 /sdcard/守播/config.json（U盘/电脑直接替换即生效，改完自动热加载）；
 * 2. 读不到时退到应用专属目录，都没有则把 APK 内置默认配置落盘；
 * 3. 解析失败自动备份坏文件（config.broken.json）并回退默认配置；
 * 4. m3u_file 指定的播放列表解析后追加到频道表（group-title 作为省份分组）；
 * 5. 应用内的添加频道/收藏/切台都会完整写回 config.json（channels + favorites + current_channel_id）。
 */
class ConfigRepository(context: Context) {

    companion object {
        const val DIR_NAME = "守播"
        private const val FILE_NAME = "config.json"
        private const val ASSET_NAME = "config_default.json"
    }

    private val appContext = context.applicationContext
    private val primaryDir = File(Environment.getExternalStorageDirectory(), DIR_NAME)
    private val fallbackDir = appContext.getExternalFilesDir(null) ?: appContext.filesDir
    private val configFile = File(primaryDir, FILE_NAME)
    private val fallbackFile = File(fallbackDir, FILE_NAME)

    private val main = Handler(Looper.getMainLooper())
    private var observer: FileObserver? = null
    private var suppressNext = false
    private var loadedFile: File = configFile
    private var rawText = ""

    var config: Config = Models.parse("""{"channels":[],"current_channel_id":0}""")
        private set

    var onConfigChanged: (() -> Unit)? = null

    fun load() {
        val primaryText = readFile(configFile)
        val fallbackText = if (primaryText == null) readFile(fallbackFile) else null

        val text: String = when {
            primaryText != null -> {
                loadedFile = configFile
                primaryText
            }
            fallbackText != null -> {
                loadedFile = fallbackFile
                fallbackText
            }
            else -> {
                val def = readAsset()
                if (primaryDir.isDirectory || primaryDir.mkdirs()) writeText(configFile, def)
                writeText(fallbackFile, def)
                loadedFile = if (configFile.exists()) configFile else fallbackFile
                def
            }
        }
        rawText = text

        config = try {
            Models.parse(text)
        } catch (e: Exception) {
            Logs.event("异常", "配置解析失败，已回退默认配置：" + e.message)
            backupBroken(text)
            Models.parse(readAsset())
        }

        val fromM3u = loadM3u()
        if (fromM3u.isNotEmpty()) {
            config = config.copy(channels = config.channels + fromM3u)
            Logs.event("配置", "从 m3u 导入 ${fromM3u.size} 个频道")
        }

        Logs.event(
            "配置",
            "加载完成：${config.channels.size} 个频道，当前ID ${config.currentChannelId}，收藏 ${config.favorites.size}，文件 ${loadedFile.absolutePath}"
        )
        startWatch()
    }

    /** 完整写回：应用内改动（添加频道/收藏/切台）持久化到 config.json */
    fun persist() {
        try {
            val obj = try {
                JSONObject(rawText.ifEmpty { "{}" })
            } catch (_: Exception) {
                JSONObject()
            }
            val arr = JSONArray()
            for (c in config.channels) {
                val o = JSONObject()
                o.put("id", c.id)
                o.put("name", c.name)
                o.put("primary_url", c.primaryUrl)
                if (c.backupUrl.isNotEmpty()) o.put("backup_url", c.backupUrl)
                o.put("category", c.category)
                o.put("player", c.player)
                if (c.headers.isNotEmpty()) {
                    val h = JSONObject()
                    for ((k, v) in c.headers) h.put(k, v)
                    o.put("headers", h)
                }
                arr.put(o)
            }
            obj.put("channels", arr)
            obj.put("current_channel_id", config.currentChannelId)
            val fa = JSONArray()
            for (f in config.favorites) fa.put(f)
            obj.put("favorites", fa)
            val out = obj.toString(2)
            suppressNext = true
            loadedFile.writeText(out, Charsets.UTF_8)
            rawText = out
        } catch (e: Exception) {
            Logs.event("异常", "保存配置失败：" + e.message)
        }
    }

    fun saveCurrentChannel(id: Long) {
        config = config.copy(currentChannelId = id)
        persist()
    }

    /** 应用内添加频道：名称 + 直播流地址，默认归入「自定义」分组 */
    fun addChannel(name: String, url: String): Channel {
        val newId = (config.channels.maxOfOrNull { it.id } ?: 0L) + 1
        val ch = Channel(newId, name, url, "", "auto", emptyMap(), "自定义")
        config = config.copy(channels = config.channels + ch)
        persist()
        Logs.event("配置", "手动添加频道：$name ($url)")
        return ch
    }

    /** 长按收藏/取消收藏，返回收藏后的状态 */
    fun toggleFavorite(id: Long): Boolean {
        val s = config.favorites.toMutableSet()
        val nowFav = if (s.contains(id)) {
            s.remove(id)
            false
        } else {
            s.add(id)
            true
        }
        config = config.copy(favorites = s)
        persist()
        return nowFav
    }

    // ---------- 热加载 ----------

    private fun startWatch() {
        if (observer != null) return
        val parent = loadedFile.parentFile ?: return
        observer = object : FileObserver(parent.absolutePath, CLOSE_WRITE or MOVED_TO) {
            override fun onEvent(event: Int, path: String?) {
                if (path != null && path != FILE_NAME) return
                main.removeCallbacks(reloadRun)
                main.postDelayed(reloadRun, 800)
            }
        }.also { it.startWatching() }
    }

    private val reloadRun = Runnable {
        if (suppressNext) {
            suppressNext = false
        } else {
            Logs.event("配置", "检测到配置文件变化，热加载")
            load()
            onConfigChanged?.invoke()
        }
    }

    // ---------- m3u 导入 ----------

    private fun loadM3u(): List<Channel> {
        val path = try {
            JSONObject(rawText).optString("m3u_file", "")
        } catch (_: Exception) {
            ""
        }
        if (path.isEmpty()) return emptyList()
        return parseM3u(File(path))
    }

    private fun parseM3u(f: File): List<Channel> {
        if (!f.exists()) return emptyList()
        val channels = mutableListOf<Channel>()
        var name = ""
        var group = ""
        var id = 1000L
        return try {
            f.readLines(Charsets.UTF_8).forEach { raw ->
                val line = raw.trim()
                when {
                    line.startsWith("#EXTINF") -> {
                        name = line.substringAfterLast(',').trim()
                        group = Regex("group-title=\"([^\"]*)\"").find(line)?.groupValues?.get(1)?.trim() ?: ""
                    }
                    line.isNotEmpty() && !line.startsWith("#") -> {
                        if (name.isEmpty()) {
                            name = line.substringAfterLast('/').substringBefore('.').trim()
                        }
                        channels += Channel(id++, name, line, "", "auto", emptyMap(), group)
                        name = ""
                        group = ""
                    }
                }
            }
            channels
        } catch (e: Exception) {
            Logs.event("异常", "m3u 导入失败：" + e.message)
            emptyList()
        }
    }

    // ---------- IO 工具 ----------

    private fun readFile(f: File): String? = try {
        if (f.exists() && f.length() > 0) f.readText(Charsets.UTF_8) else null
    } catch (_: Exception) {
        null
    }

    private fun writeText(f: File, text: String) = try {
        f.parentFile?.mkdirs()
        f.writeText(text, Charsets.UTF_8)
        true
    } catch (e: Exception) {
        Logs.event("异常", "写入 ${f.absolutePath} 失败：" + e.message)
        false
    }

    private fun readAsset(): String = try {
        appContext.assets.open(ASSET_NAME).bufferedReader(Charsets.UTF_8).use { it.readText() }
    } catch (_: Exception) {
        """{"channels":[],"current_channel_id":0,"lock":{"channel_lock":false}}"""
    }

    private fun backupBroken(text: String) {
        try {
            loadedFile.parentFile?.let { parent ->
                File(parent, "config.broken.json").writeText(text, Charsets.UTF_8)
            }
        } catch (_: Exception) {
        }
    }
}
