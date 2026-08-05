-repackageclasses ''
-allowaccessmodification

-keep class io.nekohasekai.sagernet.** { *;}
-keep class com.github.exclavenetwork.exclave.core.app.observatory.** { *; }

# JNI bridge class for libopenppp2.so (package supersocksr.ppp.android.c).
# Native code reflects on these static methods (protect / isProtectReady via
# OpenPPP2VpnProtectBridge, telemetryHttpPost via OpenPPP2TelemetryBridge,
# post_exec / runtime_snapshot etc.). Nothing in Java/Kotlin references them,
# so R8 strips them as dead code, which breaks socket protect() and the tunnel.
-keep class supersocksr.ppp.android.c.** { *; }

# SnakeYaml
-keep class org.yaml.snakeyaml.** { *; }

-dontobfuscate
-keepattributes SourceFile

-dontwarn java.beans.BeanInfo
-dontwarn java.beans.FeatureDescriptor
-dontwarn java.beans.IntrospectionException
-dontwarn java.beans.Introspector
-dontwarn java.beans.PropertyDescriptor
-dontwarn java.beans.Transient
-dontwarn java.beans.VetoableChangeListener
-dontwarn java.beans.VetoableChangeSupport