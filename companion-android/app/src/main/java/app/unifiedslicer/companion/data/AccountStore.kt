package app.unifiedslicer.companion.data

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Log
import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.net.LenientJson
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.serialization.builtins.ListSerializer
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Accounts and printers, saved as JSON encrypted with an AES key that never leaves the Android
 * Keystore. Tokens and access codes are therefore unreadable outside this app on this device.
 */
class AccountStore(context: Context) {
    private val file = File(context.filesDir, "accounts.bin")
    private val serializer = ListSerializer(Account.serializer())
    private val _accounts = MutableStateFlow(load())
    val accounts: StateFlow<List<Account>> = _accounts.asStateFlow()

    @Synchronized
    fun add(account: Account) = save(_accounts.value.filterNot { it.id == account.id } + account)

    /** Replaces the saved account with the same id (e.g. after a token refresh). */
    @Synchronized
    fun update(account: Account) {
        if (_accounts.value.none { it.id == account.id }) return
        save(_accounts.value.map { if (it.id == account.id) account else it })
    }

    @Synchronized
    fun remove(id: String) = save(_accounts.value.filterNot { it.id == id })

    private fun save(list: List<Account>) {
        _accounts.value = list
        runCatching {
            val tmp = File(file.path + ".tmp")
            tmp.writeBytes(encrypt(LenientJson.encodeToString(serializer, list).toByteArray()))
            if (!tmp.renameTo(file)) { file.delete(); tmp.renameTo(file) }
        }.onFailure { Log.e(TAG, "Could not save accounts", it) }
    }

    private fun load(): List<Account> = runCatching {
        if (!file.exists()) return emptyList()
        LenientJson.decodeFromString(serializer, String(decrypt(file.readBytes())))
    }.getOrElse {
        Log.e(TAG, "Could not read saved accounts", it)
        emptyList()
    }

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getEntry(ALIAS, null) as? KeyStore.SecretKeyEntry)?.let { return it.secretKey }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        gen.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
        )
        return gen.generateKey()
    }

    private fun encrypt(plain: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        return cipher.iv + cipher.doFinal(plain)
    }

    private fun decrypt(data: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, data, 0, IV_SIZE))
        return cipher.doFinal(data, IV_SIZE, data.size - IV_SIZE)
    }

    private companion object {
        const val TAG = "AccountStore"
        const val ALIAS = "companion-accounts"
        const val IV_SIZE = 12
    }
}
