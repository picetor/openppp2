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

package io.nekohasekai.sagernet.fmt;

import androidx.room.TypeConverter;

import com.esotericsoftware.kryo.Kryo;
import com.esotericsoftware.kryo.io.ByteBufferInput;
import com.esotericsoftware.kryo.io.ByteBufferOutput;

import io.nekohasekai.sagernet.database.SubscriptionBean;
import io.nekohasekai.sagernet.fmt.internal.ConfigBean;

public class KryoConverters {

    @TypeConverter
    public static byte[] serialize(Serializable bean) {
        Kryo kryo = newKryo();
        ByteBufferOutput output = new ByteBufferOutput(64, -1);
        kryo.writeClassAndObject(output, bean);
        return output.toBytes();
    }

    @SuppressWarnings("unchecked")
    public static <T extends Serializable> T deserialize(T bean, byte[] bytes) {
        Kryo kryo = newKryo();
        ByteBufferInput input = new ByteBufferInput(bytes);
        return (T) kryo.readClassAndObject(input);
    }

    /**
     * Kryo 5.6+ refuses to serialize unregistered classes unless
     * registrationRequired is disabled. Register all known bean types here so
     * ProxyGroup.subscription (SubscriptionBean) and ConfigBean payloads can be
     * stored in Room without hitting
     * "Class is not registered: ..." crashes.
     */
    private static Kryo newKryo() {
        Kryo kryo = new Kryo();
        kryo.setRegistrationRequired(false);
        // Register every Serializable subclass that can reach KryoConverters.
        kryo.register(SubscriptionBean.class);
        kryo.register(ConfigBean.class);
        kryo.register(io.nekohasekai.sagernet.fmt.internal.InternalBean.class);
        kryo.register(io.nekohasekai.sagernet.fmt.internal.BalancerBean.class);
        kryo.register(io.nekohasekai.sagernet.fmt.internal.ChainBean.class);
        return kryo;
    }

    @TypeConverter
    public static ConfigBean configDeserialize(byte[] bytes) {
        if (bytes == null) return null;
        return deserialize(new ConfigBean(), bytes);
    }

    @TypeConverter
    public static SubscriptionBean subscriptionDeserialize(byte[] bytes) {
        if (bytes == null) return null;
        return deserialize(new SubscriptionBean(), bytes);
    }
}
