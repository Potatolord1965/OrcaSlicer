# Paho loads its network modules and logger by name.
-keep class org.eclipse.paho.client.mqttv3.** { *; }
-dontwarn org.eclipse.paho.client.mqttv3.**
# Saved accounts are (de)serialized by kotlinx.serialization.
-keep class app.unifiedslicer.companion.core.model.** { *; }
# The Elegoo sign-in page calls into this object from JavaScript.
-keepclassmembers class * { @android.webkit.JavascriptInterface <methods>; }
-dontwarn org.bouncycastle.**
-dontwarn org.conscrypt.**
-dontwarn org.openjsse.**
# Agora RTM calls back into Java from native code.
-keep class io.agora.** { *; }
-dontwarn io.agora.**
