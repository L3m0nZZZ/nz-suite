package com.shoubo.tv.net

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Handler
import android.os.Looper
import java.net.HttpURLConnection
import java.net.URL
import kotlin.concurrent.thread

/**
 * 网络层检测（文档 4.3）：
 * - 常态：ConnectivityManager 回调（onAvailable/onLost）；
 * - 主动探测：HTTP 204 双源 + ping 公网DNS，任一通过即在线（单 ping 有"假联网"，故多源互证）。
 * 探测结果回调发生在后台线程，调用方自行切线程。
 */
class NetworkMonitor(context: Context) {

    /** 网络由断转通时回调（主线程触发），用于"恢复即连"（文档 4.4） */
    var onRestored: (() -> Unit)? = null

    @Volatile
    var online: Boolean = true
        private set

    private val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val main = Handler(Looper.getMainLooper())
    private var callback: ConnectivityManager.NetworkCallback? = null
    private var registered = false

    fun start() {
        if (registered) return
        registered = true
        val request = NetworkRequest.Builder()
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                main.post { setOnline(true) }
            }

            override fun onLost(network: Network) {
                // 盒子一般是单网络（WiFi 或网线二选一），onLost 即断网；
                // 若设备有多网络，误报会被下一次主动探测纠正
                main.post { setOnline(false) }
            }
        }.also {
            cm.registerNetworkCallback(request, it)
        }
    }

    fun stop() {
        if (!registered) return
        registered = false
        callback?.let { cb ->
            try {
                cm.unregisterNetworkCallback(cb)
            } catch (_: Exception) {
            }
        }
        callback = null
    }

    fun probe(onResult: (Boolean) -> Unit) {
        thread(name = "net-probe") {
            val ok = http204("http://connect.rom.miui.com/generate_204")
                || http204("http://connectivitycheck.platform.hicloud.com/generate_204")
                || ping("114.114.114.114")
            main.post {
                setOnline(ok)
                onResult(ok)
            }
        }
    }

    private fun setOnline(v: Boolean) {
        val was = online
        online = v
        if (v && !was) onRestored?.invoke()
    }

    private fun http204(url: String): Boolean = try {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 3000
        conn.readTimeout = 3000
        conn.requestMethod = "GET"
        conn.useCaches = false
        val code = conn.responseCode
        try {
            conn.inputStream.close()
        } catch (_: Exception) {
        }
        code == 204
    } catch (_: Exception) {
        false
    }

    private fun ping(host: String): Boolean = try {
        val p = ProcessBuilder("ping", "-c", "1", "-W", "3", host).start()
        val ok = p.waitFor() == 0
        p.destroy()
        ok
    } catch (_: Exception) {
        false
    }
}
