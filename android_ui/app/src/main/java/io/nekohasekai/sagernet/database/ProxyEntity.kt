/******************************************************************************
 *                                                                            *
 * Copyright (C) 2021 by nekohasekai <contact-sagernet@sekai.icu>             *
 *                                                                            *
 * This program is free software: you can redistribute it and/or modify       *
 * it under the terms of the GNU General Public License as published by       *
 * the Free Software Foundation, either version 3 of the License, or          *
 *  (at your option) any later version.                                       *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.       *
 *                                                                            *
 ******************************************************************************/

package io.nekohasekai.sagernet.database

import android.content.Context
import android.content.Intent
import androidx.room.*
import com.esotericsoftware.kryo.io.ByteBufferInput
import com.esotericsoftware.kryo.io.ByteBufferOutput
import io.nekohasekai.sagernet.R
import io.nekohasekai.sagernet.aidl.TrafficStats
import io.nekohasekai.sagernet.fmt.AbstractBean
import io.nekohasekai.sagernet.fmt.KryoConverters
import io.nekohasekai.sagernet.fmt.Serializable
import io.nekohasekai.sagernet.fmt.internal.ConfigBean
import io.nekohasekai.sagernet.ktx.app
import io.nekohasekai.sagernet.ktx.applyDefaultValues
import io.nekohasekai.sagernet.ui.profile.ConfigSettingsActivity
import io.nekohasekai.sagernet.ui.profile.ProfileSettingsActivity

@Entity(
    tableName = "proxy_entities", indices = [Index("groupId", name = "groupId")]
)
data class ProxyEntity(
    @PrimaryKey(autoGenerate = true) var id: Long = 0L,
    var groupId: Long = 0L,
    var type: Int = 0,
    var userOrder: Long = 0L,
    var tx: Long = 0L,
    var rx: Long = 0L,
    var status: Int = 0,
    var ping: Int = 0,
    var uuid: String = "",
    var error: String? = null,
    var configBean: ConfigBean? = null
) : Serializable() {

    companion object {
        const val TYPE_CONFIG = 13

        val configName by lazy { app.getString(R.string.custom_config) }

        @JvmField
        val CREATOR = object : CREATOR<ProxyEntity>() {

            override fun newInstance(): ProxyEntity {
                return ProxyEntity()
            }

            override fun newArray(size: Int): Array<ProxyEntity?> {
                return arrayOfNulls(size)
            }
        }
    }

    @Ignore
    @Transient
    var dirty: Boolean = false

    @Ignore
    @Transient
    var stats: TrafficStats? = null

    override fun initializeDefaultValues() {
    }

    override fun serializeToBuffer(output: ByteBufferOutput) {
        output.writeInt(0)

        output.writeLong(id)
        output.writeLong(groupId)
        output.writeInt(type)
        output.writeLong(userOrder)
        output.writeLong(tx)
        output.writeLong(rx)
        output.writeInt(status)
        output.writeInt(ping)
        output.writeString(uuid)
        output.writeString(error)

        val data = KryoConverters.serialize(requireBean())
        output.writeVarInt(data.size, true)
        output.writeBytes(data)

        output.writeBoolean(dirty)
    }

    override fun deserializeFromBuffer(input: ByteBufferInput) {
        val version = input.readInt()

        id = input.readLong()
        groupId = input.readLong()
        type = input.readInt()
        userOrder = input.readLong()
        tx = input.readLong()
        rx = input.readLong()
        status = input.readInt()
        ping = input.readInt()
        uuid = input.readString()
        error = input.readString()
        putByteArray(input.readBytes(input.readVarInt(true)))

        dirty = input.readBoolean()
    }


    fun putByteArray(byteArray: ByteArray) {
        when (type) {
            TYPE_CONFIG -> configBean = KryoConverters.configDeserialize(byteArray)
        }
    }

    fun displayType() = when (type) {
        TYPE_CONFIG -> configName
        else -> "Invalid"
    }

    fun displayName() = requireBean().displayName()
    fun displayAddress() = requireBean().displayAddress()

    fun requireBean(): AbstractBean {
        return when (type) {
            TYPE_CONFIG -> configBean
            else -> null
        } ?: ConfigBean().applyDefaultValues()
    }

    fun canExportBackup(): Boolean {
        return true
    }

    fun hasShareLink(): Boolean {
        return false
    }

    fun toLink(): String? = null

    fun exportConfig(): Pair<String, String> {
        val name = "${displayName()}.json"
        return with(requireBean()) {
            if (this is ConfigBean) content else "{}"
        } to name
    }

    fun needExternal(): Boolean {
        return false
    }

    fun putBean(bean: AbstractBean): ProxyEntity {
        configBean = null

        when (bean) {
            is ConfigBean -> {
                type = TYPE_CONFIG
                configBean = bean
            }
            else -> error("Undefined type $type")
        }
        return this
    }

    fun settingIntent(ctx: Context, isSubscription: Boolean): Intent? {
        val cls = when (type) {
            TYPE_CONFIG -> ConfigSettingsActivity::class.java
            else -> return null
        }
        return Intent(
            ctx, cls
        ).apply {
            putExtra(ProfileSettingsActivity.EXTRA_PROFILE_ID, id)
            putExtra(ProfileSettingsActivity.EXTRA_IS_SUBSCRIPTION, isSubscription)
        }
    }

    @androidx.room.Dao
    interface Dao {

        @Query("select * from proxy_entities")
        fun getAll(): List<ProxyEntity>

        @Query("SELECT id FROM proxy_entities WHERE groupId = :groupId ORDER BY userOrder")
        fun getIdsByGroup(groupId: Long): List<Long>

        @Query("SELECT * FROM proxy_entities WHERE groupId = :groupId ORDER BY userOrder")
        fun getByGroup(groupId: Long): List<ProxyEntity>

        @Query("SELECT * FROM proxy_entities WHERE id in (:proxyIds)")
        fun getEntities(proxyIds: List<Long>): List<ProxyEntity>

        @Query("SELECT COUNT(*) FROM proxy_entities WHERE groupId = :groupId")
        fun countByGroup(groupId: Long): Long

        @Query("SELECT  MAX(userOrder) + 1 FROM proxy_entities WHERE groupId = :groupId")
        fun nextOrder(groupId: Long): Long?

        @Query("SELECT * FROM proxy_entities WHERE id = :proxyId")
        fun getById(proxyId: Long): ProxyEntity?

        @Query("SELECT COUNT(*) FROM proxy_entities WHERE groupId = :groupId AND id = :proxyId LIMIT 1")
        fun isIdInGroup(proxyId: Long, groupId: Long): Long

        @Query("DELETE FROM proxy_entities WHERE id IN (:proxyId)")
        fun deleteById(proxyId: Long): Int

        @Query("DELETE FROM proxy_entities WHERE groupId = :groupId")
        fun deleteByGroup(groupId: Long)

        @Query("SELECT COUNT(*) FROM proxy_entities WHERE type = :type")
        fun countByType(type: Int): Long

        @Query("DELETE FROM proxy_entities WHERE id IN (:ids)")
        fun deleteByIds(ids: List<Long>)

        @Query("SELECT * FROM proxy_entities WHERE id IN (:ids)")
        fun getByIds(ids: List<Long>): List<ProxyEntity>

        @Insert
        fun create(entity: ProxyEntity): Long

        @Insert
        fun addProxy(entity: ProxyEntity): Long

        @Update
        fun update(entity: ProxyEntity)

        @Update
        fun updateProxy(entity: ProxyEntity)

        @Update
        fun updateAll(entities: List<ProxyEntity>)

        @Update
        fun updateProxy(entities: List<ProxyEntity>)

        @Query("DELETE FROM proxy_entities WHERE groupId = :groupId")
        fun deleteAll(groupId: Long)

        @Query("DELETE FROM proxy_entities")
        fun reset()

        @Insert
        fun insert(entities: List<ProxyEntity>)

        @Query("UPDATE proxy_entities SET tx = :tx, rx = :rx WHERE id = :id")
        fun updateTraffic(id: Long, tx: Long, rx: Long)

        @Query("UPDATE proxy_entities SET status = :status, ping = :ping, error = :error WHERE id = :id")
        fun updateStatus(id: Long, status: Int, ping: Int, error: String?)

        @Query("DELETE FROM proxy_entities")
        fun clear()
    }
}
