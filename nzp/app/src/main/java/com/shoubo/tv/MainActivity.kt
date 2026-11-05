package com.shoubo.tv

import android.app.Activity
import android.app.AlertDialog
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.media3.ui.PlayerView
import com.shoubo.tv.config.Channel
import com.shoubo.tv.config.ConfigRepository
import com.shoubo.tv.keepalive.KeepAliveService
import com.shoubo.tv.net.NetworkMonitor
import com.shoubo.tv.player.PlayerManager
import com.shoubo.tv.util.Logs
import com.shoubo.tv.util.Stats
import java.text.Collator
import java.util.Locale

/**
 * 全屏播放 + 频道侧边栏（文档 4.8）：
 * - 极简：全屏画面 + 左下角半透明状态栏，5秒无操作自动隐藏；
 * - 侧边栏：收藏 + 按省份分组（category 字段），长按频道收藏/取消；
 * - 添加频道：侧边栏「＋添加」输入名称和 m3u8 地址即可，写入 config.json；
 * - 防误触：channel_lock=true 时点击列表不切台（长按收藏仍可用）；
 * - 自愈不受切台影响：切台后自愈引擎继续守护新频道。
 */
class MainActivity : Activity(), PlayerManager.Ui {

    private lateinit var repo: ConfigRepository
    private lateinit var network: NetworkMonitor
    private lateinit var player: PlayerManager
    private lateinit var statusView: TextView
    private lateinit var waitView: TextView
    private lateinit var sidebar: LinearLayout
    private lateinit var channelList: LinearLayout

    private val main = Handler(Looper.getMainLooper())
    private var memoryReceiver: BroadcastReceiver? = null
    private val hideStatusRun = Runnable { statusView.visibility = View.GONE }
    private var sidebarOpen = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_main)
        statusView = findViewById(R.id.statusBar)
        waitView = findViewById(R.id.waitMessage)
        sidebar = findViewById(R.id.sidebar)
        channelList = findViewById(R.id.channelList)

        repo = ConfigRepository(this)
        repo.load()
        network = NetworkMonitor(this)
        player = PlayerManager(this, repo, network)
        player.ui = this
        player.attach(findViewById(R.id.playerView))

        repo.onConfigChanged = {
            main.post {
                player.onConfigChanged()
                if (sidebarOpen) buildSidebar()
                showStatus()
            }
        }

        findViewById<View>(R.id.root).setOnClickListener {
            if (statusView.visibility == View.VISIBLE) hideStatus() else showStatus()
        }
        findViewById<View>(R.id.btnChannels).setOnClickListener { toggleSidebar() }
        findViewById<View>(R.id.btnAdd).setOnClickListener { showAddDialog() }

        // 内存超限广播（KeepAliveService 发出）
        val filter = IntentFilter(ACTION_MEMORY_HIGH)
        memoryReceiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                player.hardRebuild("内存水位超限（广播）")
            }
        }
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(memoryReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(memoryReceiver, filter)
        }

        if (intent?.getStringExtra(EXTRA_RESTART_REASON) == "crash") {
            Logs.event("恢复", "崩溃后自动重启完成，恢复播放")
        }
        showStatus()
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        if (intent?.getStringExtra(EXTRA_RESTART_REASON) == "crash") {
            Logs.event("恢复", "崩溃后自动重启完成（onNewIntent），恢复播放")
            player.hardRebuild("崩溃自启")
        }
    }

    override fun onStart() {
        super.onStart()
        network.start()
        player.start()
        startServiceSafe()
        KeepAliveService.scheduleDailyRestart(this)
    }

    override fun onDestroy() {
        super.onDestroy()
        memoryReceiver?.let {
            try {
                unregisterReceiver(it)
            } catch (_: Exception) {
            }
        }
        memoryReceiver = null
        main.removeCallbacks(hideStatusRun)
        player.releaseAll()
        network.stop()
    }

    private fun startServiceSafe() {
        try {
            val i = Intent(this, KeepAliveService::class.java)
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(i) else startService(i)
        } catch (e: Exception) {
            Logs.event("异常", "启动前台服务失败：" + e.message)
        }
    }

    // ---------- 频道侧边栏 ----------

    private fun toggleSidebar() {
        sidebarOpen = !sidebarOpen
        if (sidebarOpen) {
            buildSidebar()
            sidebar.visibility = View.VISIBLE
            sidebar.translationX = -sidebarWidth()
            sidebar.animate().translationX(0f).setDuration(180).start()
        } else {
            sidebar.animate().translationX(-sidebarWidth()).setDuration(180)
                .withEndAction { sidebar.visibility = View.GONE }.start()
        }
    }

    private fun sidebarWidth(): Float =
        (if (sidebar.width > 0) sidebar.width else dp(300)).toFloat()

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    /** 分组顺序：收藏在最前，然后 央视/卫视/省份，自定义和其他垫底 */
    private fun catRank(c: String): Int = when (c) {
        "央视" -> 0
        "卫视" -> 1
        "自定义" -> 8
        "其他" -> 9
        else -> 5
    }

    private fun buildSidebar() {
        channelList.removeAllViews()
        val cfg = repo.config
        if (cfg.channels.isEmpty()) {
            channelList.addView(groupHeader("还没有频道"))
            channelList.addView(
                hintView("点击右上角「＋添加」，或在 /sdcard/守播/config.json 里配置")
            )
            return
        }

        val collator = Collator.getInstance(Locale.CHINA)
        val favChannels = cfg.channels.filter { it.id in cfg.favorites }
        val normal = cfg.channels.filter { it.id !in cfg.favorites }

        if (favChannels.isNotEmpty()) {
            channelList.addView(groupHeader("★ 收藏"))
            favChannels.forEach { channelList.addView(channelRow(it)) }
        }

        val grouped = LinkedHashMap<String, MutableList<Channel>>()
        for (c in normal) {
            val key = c.category.ifBlank { "其他" }
            grouped.getOrPut(key) { mutableListOf() }.add(c)
        }
        grouped.keys
            .sortedWith(compareBy({ catRank(it) }, { collator.getCollationKey(it) }))
            .forEach { g ->
                channelList.addView(groupHeader(g))
                grouped[g]!!.forEach { channelList.addView(channelRow(it)) }
            }
    }

    private fun groupHeader(title: String): View {
        val tv = TextView(this)
        tv.text = "  $title"
        tv.setTextColor(ACCENT)
        tv.textSize = 13f
        tv.typeface = android.graphics.Typeface.DEFAULT_BOLD
        tv.setPadding(dp(10), dp(9), dp(10), dp(7))
        tv.setBackgroundColor(0xFF0E2233.toInt())
        return tv
    }

    private fun hintView(text: String): View {
        val tv = TextView(this)
        tv.text = text
        tv.setTextColor(0x88FFFFFF.toInt())
        tv.textSize = 13f
        tv.setPadding(dp(16), dp(12), dp(12), dp(12))
        return tv
    }

    private fun channelRow(ch: Channel): View {
        val tv = TextView(this)
        val isFav = ch.id in repo.config.favorites
        val isCur = ch.id == player.currentChannelId()
        tv.text = (if (isFav) "★ " else "") + ch.name
        tv.setTextColor(if (isCur) ACCENT else 0xFFE6EDF3.toInt())
        tv.textSize = 15f
        tv.setPadding(dp(16), dp(11), dp(10), dp(11))
        if (isCur) tv.setBackgroundColor(0x334FC3F7)
        tv.maxLines = 1

        tv.setOnClickListener {
            if (repo.config.lock.channelLock) {
                Toast.makeText(this, "频道已锁定（config.json → lock.channel_lock 可关闭）", Toast.LENGTH_SHORT).show()
            } else {
                player.switchTo(ch.id)
                if (sidebarOpen) buildSidebar()
                showStatus()
            }
        }
        tv.setOnLongClickListener {
            val fav = repo.toggleFavorite(ch.id)
            Toast.makeText(this, if (fav) "已收藏「${ch.name}」" else "已取消收藏「${ch.name}」", Toast.LENGTH_SHORT).show()
            if (sidebarOpen) buildSidebar()
            true
        }
        return tv
    }

    /** 应用内添加频道：名称 + m3u8 地址 → 写入 config.json 的「自定义」分组 */
    private fun showAddDialog() {
        val v = layoutInflater.inflate(R.layout.dialog_add_channel, null)
        val nameEdit = v.findViewById<EditText>(R.id.editName)
        val urlEdit = v.findViewById<EditText>(R.id.editUrl)
        AlertDialog.Builder(this)
            .setTitle("添加频道")
            .setView(v)
            .setPositiveButton("添加") { _, _ ->
                val name = nameEdit.text.toString().trim()
                val url = urlEdit.text.toString().trim()
                when {
                    name.isEmpty() -> Toast.makeText(this, "请填写频道名称", Toast.LENGTH_SHORT).show()
                    !url.startsWith("http") -> Toast.makeText(this, "地址必须以 http(s):// 开头", Toast.LENGTH_SHORT).show()
                    else -> {
                        repo.addChannel(name, url)
                        Toast.makeText(this, "已添加「$name」，点击列表即可播放", Toast.LENGTH_SHORT).show()
                        if (sidebarOpen) buildSidebar()
                    }
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    // ---------- PlayerManager.Ui ----------

    override fun onModeChanged(mode: PlayerManager.Mode) {
        runOnUiThread {
            waitView.visibility =
                if (mode == PlayerManager.Mode.WAITING_NETWORK) View.VISIBLE else View.GONE
            showStatus()
        }
    }

    override fun onTick() {
        runOnUiThread { refreshStatus() }
    }

    // ---------- 状态栏 ----------

    private fun showStatus() {
        statusView.visibility = View.VISIBLE
        refreshStatus()
        main.removeCallbacks(hideStatusRun)
        main.postDelayed(hideStatusRun, AUTO_HIDE_MS)
    }

    private fun hideStatus() {
        statusView.visibility = View.GONE
    }

    private fun refreshStatus() {
        val sb = StringBuilder()
        when (player.mode) {
            PlayerManager.Mode.PLAYING ->
                sb.append("🔴 直播中 · ").append(player.channelName())
            PlayerManager.Mode.RETRYING ->
                sb.append("⚠ 信号中断 · 第 ").append(player.failures())
                    .append(" 次重试 · ").append(player.nextRetryInSec()).append(" 秒后自动重连")
            PlayerManager.Mode.WAITING_NETWORK ->
                sb.append("⚠ 网络已断开，正在等待恢复…")
        }
        sb.append("\n运行 ").append(Stats.runDurationText())
            .append(" · 今日断线 ").append(Stats.getDisconnects())
            .append(" · 卡死 ").append(Stats.getStalls())
            .append(" · 网络 ").append(if (network.online) "正常" else "断开")
        statusView.text = sb
    }

    // ---------- 按键 ----------

    private fun switchRelative(delta: Int) {
        if (repo.config.lock.channelLock) return
        val list = repo.config.channels
        if (list.isEmpty()) return
        val idx = list.indexOfFirst { it.id == player.currentChannelId() }
        val next = ((if (idx < 0) 0 else idx + delta) + list.size) % list.size
        player.switchTo(list[next].id)
        if (sidebarOpen) buildSidebar()
        showStatus()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        return when (keyCode) {
            // 返回键：侧边栏开着先关侧边栏，否则退到后台不退出
            KeyEvent.KEYCODE_BACK -> {
                if (sidebarOpen) toggleSidebar() else moveTaskToBack(true)
                true
            }
            KeyEvent.KEYCODE_MENU -> {
                toggleSidebar()
                true
            }
            KeyEvent.KEYCODE_CHANNEL_UP -> {
                switchRelative(-1)
                true
            }
            KeyEvent.KEYCODE_CHANNEL_DOWN -> {
                switchRelative(1)
                true
            }
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> {
                if (statusView.visibility == View.VISIBLE) hideStatus() else showStatus()
                true
            }
            else -> super.onKeyDown(keyCode, event)
        }
    }

    companion object {
        const val EXTRA_RESTART_REASON = "restart_reason"
        const val ACTION_MEMORY_HIGH = "com.shoubo.tv.action.MEMORY_HIGH"
        private const val AUTO_HIDE_MS = 5000L
        private val ACCENT = 0xFF4FC3F7.toInt()
    }
}
