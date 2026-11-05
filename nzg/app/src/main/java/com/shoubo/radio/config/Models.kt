package com.shoubo.radio.config

import org.json.JSONArray
import org.json.JSONObject

/**
 * 一个频道 = 主源 + 同频道备用源 + 内核偏好 + 自定义请求头 + 分组（省份）+ 地区。
 * category 用于一级分组（省份）；region 用于二级分组（地市/区县），UI 分级导航用。
 */
data class Channel(
    val id: Long,
    val name: String,
    val primaryUrl: String,
    val backupUrl: String,
    val player: String,
    val headers: Map<String, String>,
    val category: String = "",
    val region: String = ""
)

data class Resilience(
    val bufferStallSec: Int,
    val frameStallSec: Int,
    val softRetryMax: Int,
    val hardRetryMax: Int,
    val retryIntervalsSec: List<Int>,
    val probeIntervalSec: Int
)

data class Lock(val channelLock: Boolean)

/**
 * 音量策略。广播场景下很多县级源的录制电平本就偏低，系统音量顶满仍然偏小，
 * 所以在软件端再加一层数字增益。
 *
 * @param forceMax 是否把系统媒体音量顶到最大并持续纠偏（遥控器/其他 App 调低后自动顶回）
 * @param boostMb  软件端增益，单位毫巴（millibel）。0 = 关闭；1000 ≈ +10 dB ≈ 3.16 倍
 * @param maxBoostMb 增益上限保护，防止把高电平源推到削波
 */
data class Audio(
    val forceMax: Boolean = true,
    val boostMb: Int = 1200,
    val maxBoostMb: Int = 1500
)

data class Config(
    val channels: List<Channel>,
    val currentChannelId: Long,
    val resilience: Resilience,
    val lock: Lock,
    val favorites: Set<Long> = emptySet(),
    val audio: Audio = Audio()
) {
    fun currentChannel(): Channel? =
        channels.firstOrNull { it.id == currentChannelId } ?: channels.firstOrNull()
}

object Models {

    fun parse(text: String): Config = parse(JSONObject(text))

    fun parse(json: JSONObject): Config {
        val channels = mutableListOf<Channel>()
        val arr = json.optJSONArray("channels") ?: JSONArray()
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val headers = mutableMapOf<String, String>()
            val h = o.optJSONObject("headers")
            if (h != null) for (k in h.keys()) headers[k] = h.optString(k)
            channels += Channel(
                id = o.optLong("id", (i + 1).toLong()),
                name = o.optString("name", "频道${i + 1}"),
                primaryUrl = o.optString("primary_url").trim(),
                backupUrl = o.optString("backup_url").trim(),
                player = o.optString("player", "auto").trim(),
                headers = headers,
                category = o.optString("category", "").trim(),
                region = o.optString("region", "").trim()
            )
        }

        val r = json.optJSONObject("resilience") ?: JSONObject()
        val intervals = mutableListOf<Int>()
        val ia = r.optJSONArray("retry_intervals_sec")
        if (ia != null) for (i in 0 until ia.length()) intervals += ia.optInt(i)
        val resilience = Resilience(
            bufferStallSec = r.optInt("buffer_stall_sec", 10),
            frameStallSec = r.optInt("frame_stall_sec", 15),
            softRetryMax = r.optInt("soft_retry_max", 3),
            hardRetryMax = r.optInt("hard_retry_max", 3),
            retryIntervalsSec = if (intervals.isEmpty()) listOf(0, 5, 15, 30) else intervals,
            probeIntervalSec = r.optInt("network_probe_sec", 30)
        )

        val lock = Lock(json.optJSONObject("lock")?.optBoolean("channel_lock") ?: false)

        val au = json.optJSONObject("audio") ?: JSONObject()
        val audio = Audio(
            forceMax = au.optBoolean("force_max", true),
            boostMb = au.optInt("boost_mb", 1200).coerceIn(0, MAX_BOOST_MB),
            maxBoostMb = au.optInt("max_boost_mb", 1500).coerceIn(0, MAX_BOOST_MB)
        )

        val favorites = mutableSetOf<Long>()
        val fa = json.optJSONArray("favorites")
        if (fa != null) for (i in 0 until fa.length()) favorites.add(fa.optLong(i))

        return Config(channels, json.optLong("current_channel_id", 1L), resilience, lock, favorites, audio)
    }

    /** 硬上限：再高的增益几乎必然削波，且部分老设备不支持 */
    const val MAX_BOOST_MB = 2000
}
