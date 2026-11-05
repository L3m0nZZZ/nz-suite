package com.shoubo.radio

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
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.media3.ui.PlayerView
import com.shoubo.radio.config.Channel
import com.shoubo.radio.config.ConfigRepository
import com.shoubo.radio.keepalive.KeepAliveService
import com.shoubo.radio.net.NetworkMonitor
import com.shoubo.radio.net.RemoteSync
import com.shoubo.radio.player.PlayerManager
import com.shoubo.radio.util.Logs
import com.shoubo.radio.util.Stats
import java.text.Collator
import java.util.Locale

/**
 * 全屏播放 + 两级频道侧边栏：
 * - 播放画面零遮挡：无任何常驻按钮；点屏幕任意处打开侧栏，点侧栏以外区域关闭；
 * - 一级分类（收藏 / 央视 / 山东 / 内蒙古 / 河南 / 陕西…）在左栏，点击切换；
 * - 二级地区分组在右栏：市与区县平铺（德州、武城、威海、茌平…并列），分组头可展开/收起；
 * - 添加频道：一级分类「自定义」；长按频道收藏/取消收藏。
 */
class MainActivity : Activity(), PlayerManager.Ui {

    private lateinit var repo: ConfigRepository
    private lateinit var network: NetworkMonitor
    private lateinit var player: PlayerManager
    private lateinit var remoteSync: RemoteSync
    private lateinit var statusView: TextView
    private lateinit var waitView: TextView
    private lateinit var sidebar: LinearLayout
    private lateinit var catList: LinearLayout
    private lateinit var channelList: LinearLayout

    private val main = Handler(Looper.getMainLooper())
    private var memoryReceiver: BroadcastReceiver? = null
    private val hideStatusRun = Runnable { statusView.visibility = View.GONE }
    private var sidebarOpen = false

    /** 当前一级分类；空串 = 收藏 */
    private var activeCat = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_main)
        statusView = findViewById(R.id.statusBar)
        waitView = findViewById(R.id.waitMessage)
        sidebar = findViewById(R.id.sidebar)
        catList = findViewById(R.id.catList)
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
                if (sidebarOpen) {
                    buildCatList()
                    buildChannelList()
                }
                showStatus()
            }
        }

        // 方案②：远程节目单自动同步（频道服务器地址在 config.json remote_config.server）
        remoteSync = RemoteSync(repo)
        remoteSync.listener = object : RemoteSync.Listener {
            override fun onRemoteChannelsApplied(count: Int) {
                main.post {
                    if (sidebarOpen) {
                        buildCatList()
                        buildChannelList()
                    }
                    Toast.makeText(this@MainActivity, "已同步服务器节目单（$count 台）", Toast.LENGTH_SHORT).show()
                    showStatus()
                }
            }
        }

        // 点屏幕任意处：开侧栏；侧栏已开时点外部区域 → 关闭
        findViewById<View>(R.id.root).setOnClickListener {
            if (sidebarOpen) closeSidebar() else openSidebar()
        }
        // PlayerView 自带 onClick 会消费事件导致 root click 不触发，
        // 这里覆盖成"打开/关闭侧栏"——直播无暂停需要，也避免被 PlayerView 拦截。
        findViewById<View>(R.id.playerView).setOnClickListener {
            if (sidebarOpen) closeSidebar() else openSidebar()
        }
        findViewById<View>(R.id.btnAdd).setOnClickListener { showAddDialog() }

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
        remoteSync.start()
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
        remoteSync.stop()
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

    // ---------- 侧边栏开合 ----------

    private fun openSidebar() {
        sidebarOpen = true
        // 若当前分类无效（如收藏为空 / 分类下没台）自动落到第一个有台的分类
        val cats = availableCats()
        if (activeCat != CAT_FAV && activeCat !in cats) {
            activeCat = cats.firstOrNull() ?: CAT_FAV
        }
        buildCatList()
        buildChannelList()
        sidebar.visibility = View.VISIBLE
        sidebar.translationX = -sidebarWidth()
        sidebar.animate().translationX(0f).setDuration(200).start()
    }

    private fun closeSidebar() {
        sidebarOpen = false
        sidebar.animate().translationX(-sidebarWidth()).setDuration(200)
            .withEndAction { sidebar.visibility = View.GONE }.start()
    }

    private fun toggleSidebar() {
        if (sidebarOpen) closeSidebar() else openSidebar()
    }

    private fun sidebarWidth(): Float =
        (if (sidebar.width > 0) sidebar.width else dp(540)).toFloat()

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    // ---------- 一级分类 ----------

    private fun catRank(c: String): Int = when (c) {
        "央视" -> 0
        "卫视" -> 1
        "山东" -> 2
        "内蒙古" -> 3
        "河南" -> 4
        "陕西" -> 5
        "安徽" -> 6
        "广东" -> 7
        "自定义" -> 8
        "其他" -> 9
        else -> 10
    }

    /** 一级分类列表（不含收藏行，收藏单独插最前） */
    private fun availableCats(): List<String> {
        val collator = Collator.getInstance(Locale.CHINA)
        val cats = repo.config.channels.map { it.category.ifBlank { "其他" } }
            .filter { it.isNotEmpty() }.toMutableSet()
        return cats.sortedWith(compareBy({ catRank(it) }, { collator.getCollationKey(it) }))
    }

    private fun buildCatList() {
        catList.removeAllViews()
        val cats = availableCats()

        // 收藏
        val fav = repo.config.channels.filter { it.id in repo.config.favorites }
        if (fav.isNotEmpty() || cats.isNotEmpty()) {
            catList.addView(catRow(CAT_FAV, "★ 收藏", fav.size))
        }
        cats.forEach { c ->
            val n = repo.config.channels.count { it.category == c || (c == "其他" && it.category.isBlank()) }
            catList.addView(catRow(c, c, n))
        }
        if (cats.isEmpty() && fav.isEmpty()) {
            val h = hintView("还没有频道\n点击右上角「＋添加」，\n或在 /sdcard/NZ广播/config.json 配置")
            catList.addView(h)
        }
    }

    private fun catRow(cat: String, label: String, count: Int): View {
        val tv = TextView(this)
        val active = cat == activeCat
        tv.text = if (count > 0) "$label\n$count 个台" else label
        tv.setTextColor(if (active) 0xFF6EC1FF.toInt() else 0xFFB8C7D9.toInt())
        tv.textSize = 15f
        tv.setTypeface(null, if (active) android.graphics.Typeface.BOLD else android.graphics.Typeface.NORMAL)
        tv.gravity = android.view.Gravity.CENTER_VERTICAL
        tv.setPadding(dp(14), dp(12), dp(8), dp(12))
        tv.setBackgroundColor(if (active) 0x264FC3F7 else 0x00000000)
        tv.setOnClickListener {
            activeCat = cat
            buildCatList()
            buildChannelList()
        }
        return tv
    }

    // ---------- 二级地区分组频道 ----------

    private fun regionLabel(c: Channel): String =
        c.region.ifBlank { c.category.ifBlank { "其他" } }

    private fun buildChannelList() {
        channelList.removeAllViews()
        val cfg = repo.config

        // 收藏
        if (activeCat == CAT_FAV) {
            val fav = cfg.channels.filter { it.id in cfg.favorites }
            if (fav.isEmpty()) {
                channelList.addView(hintView("还没有收藏。\n打开某个分类，长按频道即可收藏。"))
                return
            }
            channelList.addView(sectionHeader("我的收藏", fav.size))
            fav.forEach { channelList.addView(channelRow(it, true)) }
            return
        }

        // 普通分类：按地区分组
        val list = cfg.channels
            .filter {
                val c = it.category.ifBlank { "其他" }
                if (activeCat == "其他") c == "其他" else c == activeCat
            }
        if (list.isEmpty()) {
            channelList.addView(hintView("该分类暂无频道。\n点右上角「＋添加」，或在 /sdcard/NZ广播/config.json 里配置。"))
            return
        }

        val collator = Collator.getInstance(Locale.CHINA)
        val grouped = LinkedHashMap<String, MutableList<Channel>>()
        for (c in list) grouped.getOrPut(regionLabel(c)) { mutableListOf() }.add(c)
        val regions = grouped.keys.sortedWith { a, b -> collator.compare(a, b) }

        val containers = HashMap<String, View>()
        for (r in regions) {
            val chs = grouped[r]!!
            val header = regionHeader(r, chs.size, true)
            channelList.addView(header)
            // 分组内容容器（可折叠）
            val container = LinearLayout(this)
            container.orientation = LinearLayout.VERTICAL
            container.layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT)
            channelList.addView(container)
            containers[r] = container
            chs.forEach { container.addView(channelRow(it, false)) }
            header.setOnClickListener {
                val body = containers[r]
                if (body == null) return@setOnClickListener
                val open = body.visibility == View.VISIBLE
                body.visibility = if (open) View.GONE else View.VISIBLE
                header.text = (if (open) "▸" else "▾") + " $r · " + chs.size
            }
        }
    }

    /** 地区分组头：点击展开/收起 */
    private fun regionHeader(title: String, count: Int, expanded: Boolean): TextView {
        val tv = TextView(this)
        tv.text = (if (expanded) "▾" else "▸") + " $title · $count"
        tv.setTextColor(0xFF6EC1FF.toInt())
        tv.textSize = 13f
        tv.setTypeface(null, android.graphics.Typeface.BOLD)
        tv.setPadding(dp(16), dp(8), dp(10), dp(6))
        tv.setBackgroundColor(0x1A0E2233)
        return tv
    }

    private fun sectionHeader(title: String, count: Int): View {
        val tv = TextView(this)
        tv.text = "  $title · $count"
        tv.setTextColor(ACCENT)
        tv.textSize = 13f
        tv.setTypeface(null, android.graphics.Typeface.BOLD)
        tv.setPadding(dp(10), dp(9), dp(10), dp(7))
        tv.setBackgroundColor(0xFF0E2233.toInt())
        return tv
    }

    private fun hintView(text: String): View {
        val tv = TextView(this)
        tv.text = text
        tv.setTextColor(0x88FFFFFF.toInt())
        tv.textSize = 13f
        tv.setPadding(dp(16), dp(16), dp(12), dp(12))
        return tv
    }

    private fun channelRow(ch: Channel, star: Boolean): View {
        val tv = TextView(this)
        val isFav = ch.id in repo.config.favorites
        val isCur = ch.id == player.currentChannelId()
        tv.text = (if (isFav) "★ " else "") + ch.name
        tv.setTextColor(if (isCur) ACCENT else 0xFFE6EDF3.toInt())
        tv.textSize = 15f
        tv.setPadding(dp(18), dp(11), dp(10), dp(11))
        if (isCur) tv.setBackgroundColor(0x334FC3F7)
        tv.maxLines = 1
        tv.setSingleLine(true)

        tv.setOnClickListener {
            if (repo.config.lock.channelLock) {
                Toast.makeText(this, "频道已锁定（config.json → lock.channel_lock 可关闭）", Toast.LENGTH_SHORT).show()
            } else {
                player.switchTo(ch.id)
                if (sidebarOpen) buildChannelList()
                showStatus()
            }
        }
        tv.setOnLongClickListener {
            val fav = repo.toggleFavorite(ch.id)
            Toast.makeText(this, if (fav) "已收藏「${ch.name}」" else "已取消收藏「${ch.name}」", Toast.LENGTH_SHORT).show()
            if (sidebarOpen) buildChannelList()
            true
        }
        return tv
    }

    /** 应用内添加频道：名称 + m3u8 地址 → 写入 config.json 的「自定义」分组 */
    private fun showAddDialog() {
        val v = layoutInflater.inflate(R.layout.dialog_add_channel, null)
        val nameEdit = v.findViewById<EditText>(R.id.editName)
        val urlEdit = v.findViewById<EditText>(R.id.editUrl)
        val dlg = AlertDialog.Builder(this)
            .setTitle("添加频道")
            .setView(v)
            .setPositiveButton("添加") { _, _ ->
                val name = nameEdit.text.toString().trim()
                val url = urlEdit.text.toString().trim()
                when {
                    name.isEmpty() -> Toast.makeText(this, "请填写频道名称", Toast.LENGTH_SHORT).show()
                    !url.startsWith("http") -> Toast.makeText(this, "地址必须以 http(s):// 开头", Toast.LENGTH_SHORT).show()
                    else -> {
                        repo.addChannel(name, url, "自定义", "自定义")
                        Toast.makeText(this, "已添加「$name」，点击列表即可播放", Toast.LENGTH_SHORT).show()
                        if (sidebarOpen) {
                            if (activeCat != "自定义") {
                                activeCat = "自定义"
                                buildCatList()
                            }
                            buildChannelList()
                        }
                    }
                }
            }
            .setNegativeButton("取消", null)
            .create()
        dlg.setOnShowListener {
            dlg.getButton(AlertDialog.BUTTON_POSITIVE).setTextColor(0xFF4FC3F7.toInt())
            dlg.getButton(AlertDialog.BUTTON_NEGATIVE).setTextColor(0xFFB8C7D9.toInt())
        }
        dlg.show()
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
        sb.append("\n🔊 ").append(player.audio.summary())
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
        if (sidebarOpen) buildChannelList()
        showStatus()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        return when (keyCode) {
            KeyEvent.KEYCODE_BACK -> {
                if (sidebarOpen) closeSidebar() else moveTaskToBack(true)
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
        const val ACTION_MEMORY_HIGH = "com.shoubo.radio.action.MEMORY_HIGH"
        private const val AUTO_HIDE_MS = 5000L
        private const val CAT_FAV = "★ 收藏"
        private val ACCENT = 0xFF4FC3F7.toInt()
    }
}
