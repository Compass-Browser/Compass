// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.autofill

import android.app.KeyguardManager
import android.content.Intent
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.Build
import android.os.Bundle
import android.os.CancellationSignal
import android.service.autofill.Dataset
import android.view.autofill.AutofillId
import android.view.autofill.AutofillManager
import android.view.autofill.AutofillValue
import android.widget.RemoteViews
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import it.cipi1965.compass.shell.CompassTheme
import org.chromium.chrome.browser.init.ChromeBrowserInitializer
import org.chromium.chrome.browser.profiles.ProfileManager
import org.json.JSONArray

/**
 * The autofill picker (CompassAutofillService): unlocks with the screen lock, lists the saved
 * passwords (the app's or site's first) and fills the one picked.
 */
class PasswordPickerActivity : ComponentActivity() {
    companion object {
        const val EXTRA_USERNAME_ID = "it.cipi1965.compass.autofill.USERNAME_ID"
        const val EXTRA_PASSWORD_ID = "it.cipi1965.compass.autofill.PASSWORD_ID"
        const val EXTRA_WEB_DOMAIN = "it.cipi1965.compass.autofill.WEB_DOMAIN"
        const val EXTRA_PACKAGE = "it.cipi1965.compass.autofill.PACKAGE"

        /** Words of a package name that say nothing of its site. */
        private val PACKAGE_NOISE =
            setOf("com", "org", "net", "io", "app", "apps", "android", "mobile", "client", "www")

        /** How well a saved password's site fits the form: higher first, 0 not at all. */
        fun relevance(site: String, webDomain: String?, packageName: String): Int {
            val host = site.substringAfter("://").substringBefore('/').removePrefix("www.").lowercase()
            if (host.isEmpty()) return 0
            if (!webDomain.isNullOrEmpty()) {
                val domain = webDomain.removePrefix("www.").lowercase()
                if (host == domain) return 3
                if (host.endsWith(".$domain") || domain.endsWith(".$host")) return 2
            }
            // An app: its package's distinctive words in the site's name (com.example.app ->
            // example).
            val words = packageName.lowercase().split('.').filter { it.length > 2 && it !in PACKAGE_NOISE }
            return if (words.any { host.split('.').contains(it) }) 1 else 0
        }
    }

    private class Entry(val index: Int, val site: String, val username: String, val relevance: Int)

    private var bridge: PasswordsBridge? = null
    private var entries by mutableStateOf<List<Entry>?>(null)
    private var error by mutableStateOf("")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { CompassTheme(dark = isSystemInDarkTheme()) { Picker() } }
        unlock()
    }

    override fun onDestroy() {
        bridge?.destroy()
        bridge = null
        super.onDestroy()
    }

    /** The screen lock first, as Chrome asks for it before showing passwords. */
    private fun unlock() {
        val keyguard = getSystemService(KeyguardManager::class.java)
        if (keyguard == null || !keyguard.isDeviceSecure) {
            load()
            return
        }
        val builder = BiometricPrompt.Builder(this).setTitle("Fill a Compass password")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            builder.setAllowedAuthenticators(
                BiometricManager.Authenticators.BIOMETRIC_WEAK or
                    BiometricManager.Authenticators.DEVICE_CREDENTIAL
            )
        } else {
            @Suppress("DEPRECATION") builder.setDeviceCredentialAllowed(true)
        }
        builder
            .build()
            .authenticate(
                CancellationSignal(),
                mainExecutor,
                object : BiometricPrompt.AuthenticationCallback() {
                    override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) {
                        load()
                    }

                    override fun onAuthenticationError(code: Int, message: CharSequence) {
                        cancel()
                    }
                },
            )
    }

    private fun load() {
        try {
            // Compass's browser process, if this is all that runs of it.
            ChromeBrowserInitializer.getInstance().handleSynchronousStartup()
        } catch (e: RuntimeException) {
            error = "Compass couldn't start."
            return
        }
        val webDomain = intent.getStringExtra(EXTRA_WEB_DOMAIN)
        val packageName = intent.getStringExtra(EXTRA_PACKAGE) ?: ""
        val passwords =
            PasswordsBridge(ProfileManager.getLastUsedRegularProfile()) { json ->
                val array = JSONArray(json)
                entries =
                    (0 until array.length())
                        .map { i ->
                            val item = array.getJSONObject(i)
                            val site = item.optString("site")
                            Entry(i, site, item.optString("username"), relevance(site, webDomain, packageName))
                        }
                        .sortedWith(compareByDescending<Entry> { it.relevance }.thenBy { it.site })
            }
        bridge = passwords
        passwords.load()
    }

    @Suppress("DEPRECATION")
    private fun fill(entry: Entry) {
        val password = bridge?.passwordAt(entry.index) ?: return
        val usernameId: AutofillId? = intent.getParcelableExtra(EXTRA_USERNAME_ID)
        val passwordId: AutofillId = intent.getParcelableExtra(EXTRA_PASSWORD_ID) ?: return cancel()
        val presentation = RemoteViews(packageName, android.R.layout.simple_list_item_1)
        presentation.setTextViewText(android.R.id.text1, entry.username.ifEmpty { entry.site })
        val dataset = Dataset.Builder(presentation)
        if (usernameId != null) dataset.setValue(usernameId, AutofillValue.forText(entry.username))
        dataset.setValue(passwordId, AutofillValue.forText(password))
        setResult(RESULT_OK, Intent().putExtra(AutofillManager.EXTRA_AUTHENTICATION_RESULT, dataset.build()))
        finish()
    }

    private fun cancel() {
        setResult(RESULT_CANCELED)
        finish()
    }

    @androidx.compose.runtime.Composable
    private fun Picker() {
        var query by remember { mutableStateOf("") }
        Box(
            Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.32f)).clickable { cancel() }
        ) {
            Surface(
                color = MaterialTheme.colorScheme.surfaceContainerLow,
                shape = RoundedCornerShape(topStart = 28.dp, topEnd = 28.dp),
                modifier =
                    Modifier.align(Alignment.BottomCenter)
                        .fillMaxWidth()
                        .fillMaxHeight(0.7f)
                        // Taps on the sheet don't reach the scrim.
                        .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
            ) {
                Column(
                    Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.navigationBars).padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text("Compass passwords", style = MaterialTheme.typography.titleLarge)
                    val list = entries
                    when {
                        error.isNotEmpty() -> Text(error, color = MaterialTheme.colorScheme.error)
                        list == null -> Text("Unlocking…", color = MaterialTheme.colorScheme.onSurfaceVariant)
                        list.isEmpty() ->
                            Text(
                                "No saved passwords. Compass saves them when you sign in to sites in it.",
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        else -> {
                            OutlinedTextField(
                                value = query,
                                onValueChange = { query = it },
                                placeholder = { Text("Search") },
                                singleLine = true,
                                modifier = Modifier.fillMaxWidth(),
                            )
                            val shown =
                                list.filter {
                                    query.isBlank() ||
                                        it.site.contains(query, ignoreCase = true) ||
                                        it.username.contains(query, ignoreCase = true)
                                }
                            LazyColumn(Modifier.weight(1f)) {
                                itemsIndexed(shown) { i, entry ->
                                    if (i > 0 && entry.relevance == 0 && shown[i - 1].relevance > 0) {
                                        Text(
                                            "Other passwords",
                                            style = MaterialTheme.typography.labelLarge,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            modifier = Modifier.padding(top = 12.dp, bottom = 4.dp),
                                        )
                                    }
                                    Column(
                                        Modifier.fillMaxWidth()
                                            .clip(RoundedCornerShape(12.dp))
                                            .clickable { fill(entry) }
                                            .padding(horizontal = 8.dp, vertical = 10.dp)
                                    ) {
                                        Text(
                                            entry.site.substringAfter("://").substringBefore('/'),
                                            style = MaterialTheme.typography.bodyLarge,
                                            maxLines = 1,
                                            overflow = TextOverflow.Ellipsis,
                                        )
                                        Text(
                                            entry.username.ifEmpty { "(no username)" },
                                            style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            maxLines = 1,
                                            overflow = TextOverflow.Ellipsis,
                                        )
                                    }
                                }
                            }
                        }
                    }
                    TextButton(onClick = { cancel() }, modifier = Modifier.align(Alignment.End)) { Text("Cancel") }
                }
            }
        }
    }
}
