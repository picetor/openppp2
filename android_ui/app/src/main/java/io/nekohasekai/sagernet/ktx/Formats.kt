/******************************************************************************
 *                                                                            *
 * openppp2 fork: Formats.kt only parses openppp2 AppConfiguration JSON       *
 * documents. All sing-box share-link protocols have been removed.            *
 *                                                                            *
 ******************************************************************************/

package io.nekohasekai.sagernet.ktx

import io.nekohasekai.sagernet.fmt.AbstractBean
import io.nekohasekai.sagernet.fmt.internal.ConfigBean
import kotlin.io.encoding.Base64
import org.json.JSONArray
import org.json.JSONObject

fun String.decodeBase64(): String {
    if (this.lines().size > 1) {
        return String(Base64.Mime.withPadding(Base64.PaddingOption.PRESENT_OPTIONAL).decode(this))
    }
    if (this.contains("-") || this.contains("_")) {
        return String(Base64.UrlSafe.withPadding(Base64.PaddingOption.ABSENT_OPTIONAL).decode(this))
    }
    if (this.contains("+") || this.contains("/")) {
        return String(Base64.withPadding(Base64.PaddingOption.PRESENT_OPTIONAL).decode(this))
    }
    return String(Base64.withPadding(Base64.PaddingOption.PRESENT_OPTIONAL).decode(this))
}

fun parseShareLinks(text: String): List<AbstractBean> {
    val entities = ArrayList<AbstractBean>()
    val trimmed = text.trim().removePrefix("\uFEFF").trim()
    if (trimmed.isEmpty()) return entities

    val array = runCatching { JSONArray(trimmed) }.getOrNull()
    if (array != null) {
        for (i in 0 until array.length()) {
            array.optJSONObject(i)?.let { obj ->
                entityFromJson(obj)?.let { entities.add(it) }
            }
        }
    } else {
        runCatching { JSONObject(trimmed) }.getOrNull()?.let { obj ->
            entityFromJson(obj)?.let { entities.add(it) }
        }
    }
    return entities
}

private fun entityFromJson(obj: JSONObject): ConfigBean? {
    val hasKey = obj.has("key") || obj.has("password")
    val hasEndpoint = (obj.has("ip") || obj.has("server")) && obj.optInt("port", 0) > 0
    if (!hasKey && !hasEndpoint) return null
    val fullText = obj.toString()
    val server = runCatching {
        obj.optJSONObject("client")?.optString("server").orEmpty()
    }.getOrDefault("")
    return ConfigBean().apply {
        name = obj.optString("name").ifBlank {
            val label = server.trim().removePrefix("ppp://").removePrefix("tcp://")
                .removePrefix("ws://").removePrefix("wss://")
                .removePrefix("http://").removePrefix("https://").removePrefix("socks://")
            label.substringBefore('/').ifBlank { "openppp2" }
        }
        type = "openppp2"
        content = fullText
        serverAddresses = server
    }
}
