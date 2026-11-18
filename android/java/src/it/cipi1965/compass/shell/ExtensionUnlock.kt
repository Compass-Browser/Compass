// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.SharedPreferences
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricManager.Authenticators.BIOMETRIC_STRONG
import android.hardware.biometrics.BiometricPrompt
import android.os.Build
import android.os.CancellationSignal
import android.os.Handler
import android.os.Looper
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.PrivateKey
import java.security.spec.MGF1ParameterSpec
import java.security.spec.X509EncodedKeySpec
import java.util.WeakHashMap
import javax.crypto.Cipher
import javax.crypto.spec.OAEPParameterSpec
import javax.crypto.spec.PSource
import org.chromium.content_public.browser.WebContents
import org.json.JSONObject

/**
 * Unlocking an extension that asks for a password (1Password) with the fingerprint or face, as
 * iOS's ExtensionUnlock does with Face ID (docs/design/android.md, "Extensions"). On the desktop,
 * 1Password unlocks with its desktop app's biometrics over native messaging, which nothing offers
 * on a phone. Instead, a script in the extension's pages (its popup, its pages in tabs) watches for
 * a password field:
 * - once the user types the password and the field goes away (unlocked), Compass offers to keep
 *   it: encrypted to a key in the Android Keystore that only a strong biometric unlocks, and that
 *   a new fingerprint or face drops;
 * - from then on, when the field shows up, the biometric prompt fills and submits it; if the field
 *   is still there 10 seconds later, the saved password is wrong (changed elsewhere): it's
 *   forgotten and the user types it once.
 *
 * The script runs in an isolated world (ExtensionUnlockBridge), which can't post to the browser,
 * so the browser asks it for news every 400 ms while an extension page is shown.
 */
class ExtensionUnlock(
    private val activity: Activity,
    /** The name of the extension with this ID, for the prompts. */
    private val nameOf: (String) -> String,
    /** The shown tab's page, when there's one. */
    private val shownTab: () -> WebContents?,
) {
    private val handler = Handler(Looper.getMainLooper())
    private var popup: WebContents? = null
    private var ticking = false
    private var prompting = false
    private var destroyed = false
    /** Pages where the prompt was just shown, so one field isn't asked for twice. */
    private val recentlyFilled = WeakHashMap<WebContents, Boolean>()
    private val tick = Runnable { poll() }

    /** The extension popup's page (null once it's closed). */
    fun setPopup(contents: WebContents?) {
        popup = contents
        kick()
    }

    /** Something may now show an extension page (a tab shown or loaded). */
    fun kick() {
        if (destroyed || ticking || !isAvailable(activity)) return
        if (targets().isEmpty()) return
        ticking = true
        handler.postDelayed(tick, POLL_MS)
    }

    fun destroy() {
        destroyed = true
        handler.removeCallbacks(tick)
        popup = null
    }

    /**
     * The popup while it's open (its page may not have loaded yet: the bridge skips it until it
     * has), and the shown tab while it shows an extension page.
     */
    private fun targets(): List<WebContents> {
        val targets = ArrayList<WebContents>()
        popup?.takeIf { !it.isDestroyed }?.let { targets.add(it) }
        shownTab()?.takeIf { !it.isDestroyed && isExtensionPage(it) && it != popup }?.let { targets.add(it) }
        return targets
    }

    private fun poll() {
        ticking = false
        if (destroyed) return
        val targets = targets()
        if (targets.isEmpty()) return
        for (contents in targets) {
            ExtensionUnlockBridge.run(contents, POLL_SCRIPT) { json -> received(contents, json) }
        }
        ticking = true
        handler.postDelayed(tick, POLL_MS)
    }

    private fun received(contents: WebContents, json: String) {
        if (destroyed || contents.isDestroyed || !isExtensionPage(contents)) return
        val id = contents.lastCommittedUrl.host
        if (id.isEmpty()) return
        if (json == "\"install\"") {
            ExtensionUnlockBridge.run(contents, INSTALL_SCRIPT) {}
            return
        }
        val events =
            try {
                JSONObject(json).optJSONArray("events") ?: return
            } catch (e: Exception) {
                return
            }
        for (i in 0 until events.length()) {
            val event = events.optJSONObject(i) ?: continue
            when (event.optString("event")) {
                "field" -> fill(contents, id)
                "unlocked" -> event.optString("password").takeIf { it.isNotEmpty() }?.let { offerToSave(it, id) }
                "fillFailed" -> {
                    forget(activity, id)
                    AlertDialog.Builder(activity)
                        .setTitle("Biometric unlock turned off")
                        .setMessage(
                            "The saved password for ${nameOf(id)} didn't work. Type it once to turn " +
                                "biometric unlock back on."
                        )
                        .setPositiveButton("OK", null)
                        .show()
                }
            }
        }
    }

    private fun fill(contents: WebContents, id: String) {
        if (!isSaved(activity, id) || recentlyFilled.containsKey(contents) || prompting) return
        val cipher =
            try {
                decryptCipher(id)
            } catch (e: KeyPermanentlyInvalidatedException) {
                // A fingerprint or face was added: the key is gone for good.
                forget(activity, id)
                return
            } catch (e: Exception) {
                return
            }
        val sealed = prefs(activity).getString(PREF_PASSWORD + id, null) ?: return
        recentlyFilled[contents] = true
        prompting = true
        var finished = false
        val done = done@{ password: String? ->
            if (finished) return@done
            finished = true
            prompting = false
            if (password == null || contents.isDestroyed) {
                // Cancelled: the field stays for typing; the prompt again next time.
                recentlyFilled.remove(contents)
            } else {
                ExtensionUnlockBridge.run(
                    contents,
                    "window.__compassUnlock && window.__compassUnlock.fill(${JSONObject.quote(password)})",
                ) {}
                // A fresh field later (locked again) can be filled again.
                handler.postDelayed({ recentlyFilled.remove(contents) }, 10_000)
            }
        }
        val prompt =
            BiometricPrompt.Builder(activity)
                .setTitle("Unlock ${nameOf(id)}")
                .setAllowedAuthenticators(BIOMETRIC_STRONG)
                .setNegativeButton("Type password", activity.mainExecutor) { _, _ -> done(null) }
                .build()
        prompt.authenticate(
            BiometricPrompt.CryptoObject(cipher),
            CancellationSignal(),
            activity.mainExecutor,
            object : BiometricPrompt.AuthenticationCallback() {
                override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) {
                    val password =
                        try {
                            String(
                                result.cryptoObject!!.cipher!!.doFinal(Base64.decode(sealed, Base64.NO_WRAP)),
                                Charsets.UTF_8,
                            )
                        } catch (e: Exception) {
                            null
                        }
                    done(password)
                }

                override fun onAuthenticationError(errorCode: Int, errString: CharSequence) {
                    done(null)
                }
            },
        )
    }

    private fun offerToSave(password: String, id: String) {
        val prefs = prefs(activity)
        val declined = prefs.getStringSet(PREF_DECLINED, emptySet())!!
        if (isSaved(activity, id) || declined.contains(id)) return
        val name = nameOf(id)
        AlertDialog.Builder(activity)
            .setTitle("Unlock $name with biometrics?")
            .setMessage(
                "Compass keeps the password on this device, behind your fingerprint or face, and " +
                    "fills it in when $name asks."
            )
            .setNegativeButton("Not now") { _, _ ->
                prefs.edit().putStringSet(PREF_DECLINED, declined + id).apply()
            }
            .setPositiveButton(useBiometricsLabel()) { _, _ -> save(activity, password, id) }
            .show()
    }

    private fun useBiometricsLabel(): CharSequence {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            activity
                .getSystemService(BiometricManager::class.java)
                ?.getStrings(BIOMETRIC_STRONG)
                ?.buttonLabel
                ?.let { return it }
        }
        return "Use biometrics"
    }

    companion object {
        private const val POLL_MS = 400L
        private const val PREFS = "compass_extension_unlock"
        private const val PREF_PASSWORD = "password:"
        private const val PREF_DECLINED = "declined"
        private const val KEY_PREFIX = "compass-extension-unlock:"
        private const val KEYSTORE = "AndroidKeyStore"
        private const val TRANSFORMATION = "RSA/ECB/OAEPWithSHA-256AndMGF1Padding"
        private val OAEP =
            OAEPParameterSpec("SHA-256", "MGF1", MGF1ParameterSpec.SHA1, PSource.PSpecified.DEFAULT)

        private fun prefs(context: Context): SharedPreferences = context.getSharedPreferences(PREFS, 0)

        private fun isExtensionPage(contents: WebContents) =
            contents.lastCommittedUrl.scheme == "chrome-extension"

        /** Whether this device can use it (a strong biometric enrolled). */
        @JvmStatic
        fun isAvailable(context: Context): Boolean =
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                context.getSystemService(BiometricManager::class.java)?.canAuthenticate(BIOMETRIC_STRONG) ==
                    BiometricManager.BIOMETRIC_SUCCESS

        /** Whether a password is kept for the extension. */
        @JvmStatic
        fun isSaved(context: Context, id: String): Boolean =
            prefs(context).contains(PREF_PASSWORD + id) &&
                try {
                    keyStore().containsAlias(KEY_PREFIX + id)
                } catch (e: Exception) {
                    false
                }

        @JvmStatic
        fun forget(context: Context, id: String) {
            prefs(context).edit().remove(PREF_PASSWORD + id).apply()
            try {
                keyStore().deleteEntry(KEY_PREFIX + id)
            } catch (e: Exception) {}
        }

        /**
         * Keeps |password|, sealed with a new key pair's public half: sealing needs no biometric
         * (as iOS's Keychain write needs no Face ID), opening needs one each time.
         */
        private fun save(context: Context, password: String, id: String) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
            try {
                val generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, KEYSTORE)
                generator.initialize(
                    KeyGenParameterSpec.Builder(KEY_PREFIX + id, KeyProperties.PURPOSE_DECRYPT)
                        .setKeySize(2048)
                        .setDigests(KeyProperties.DIGEST_SHA256)
                        .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_RSA_OAEP)
                        .setUserAuthenticationRequired(true)
                        .setUserAuthenticationParameters(0, KeyProperties.AUTH_BIOMETRIC_STRONG)
                        .setInvalidatedByBiometricEnrollment(true)
                        .build()
                )
                val pair = generator.generateKeyPair()
                // A copy of the public key outside the Keystore, whose own copy carries the key's
                // biometric requirement.
                val public =
                    KeyFactory.getInstance(pair.public.algorithm)
                        .generatePublic(X509EncodedKeySpec(pair.public.encoded))
                val cipher = Cipher.getInstance(TRANSFORMATION)
                cipher.init(Cipher.ENCRYPT_MODE, public, OAEP)
                val sealed = cipher.doFinal(password.toByteArray(Charsets.UTF_8))
                val prefs = prefs(context)
                prefs
                    .edit()
                    .putString(PREF_PASSWORD + id, Base64.encodeToString(sealed, Base64.NO_WRAP))
                    .putStringSet(PREF_DECLINED, prefs.getStringSet(PREF_DECLINED, emptySet())!! - id)
                    .apply()
            } catch (e: Exception) {
                forget(context, id)
            }
        }

        private fun decryptCipher(id: String): Cipher {
            val key = keyStore().getKey(KEY_PREFIX + id, null) as PrivateKey
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.DECRYPT_MODE, key, OAEP)
            return cipher
        }

        private fun keyStore(): KeyStore = KeyStore.getInstance(KEYSTORE).apply { load(null) }

        /** Asks the page's script for news; "install" when it isn't there yet. */
        private const val POLL_SCRIPT =
            "window.__compassUnlock ? window.__compassUnlock.take() : \"install\""

        /**
         * As iOS's ExtensionUnlock.script, in an isolated world: watches for a visible password
         * field and queues "field", keeps what the user submits and queues "unlocked" once the
         * field is gone; fill() types a password the way a user would (React and friends listen to
         * input events) and submits it (Enter, else the button beside the field, else the form),
         * and queues "fillFailed" if the field is still there 10 s later.
         */
        private val INSTALL_SCRIPT =
            """
            (() => {
              if (window.__compassUnlock) return;
              const queue = [];
              const post = (m) => queue.push(m);
              const visible = (el) => el && el.isConnected && !el.disabled && el.getClientRects().length > 0;
              const find = () => Array.from(document.querySelectorAll("input[type=password]")).find(visible);
              let field = null, typed = null, filled = false;
              const remember = () => { const f = find(); if (f && f.value && !filled) typed = f.value; };
              document.addEventListener("keydown", (e) => { if (e.key === "Enter" && e.target && e.target.type === "password") remember(); }, true);
              document.addEventListener("submit", remember, true);
              document.addEventListener("pointerdown", (e) => {
                if (e.target && e.target.closest && e.target.closest("button, [type=submit], [role=button]")) remember();
              }, true);
              const check = () => {
                const f = find();
                if (f && f !== field) { field = f; post({ event: "field" }); }
                if (!f && field) {
                  field = null;
                  if (typed) post({ event: "unlocked", password: typed });
                  typed = null;
                  filled = false;
                }
              };
              new MutationObserver(check).observe(document.documentElement, { childList: true, subtree: true, attributes: true });
              check();
              const working = () => { const f = find(); return !f || f.readOnly || f.disabled; };
              const fill = (password) => {
                const f = find();
                if (!f) return false;
                filled = true;
                typed = null;
                f.focus();
                Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set.call(f, password);
                f.dispatchEvent(new Event("input", { bubbles: true }));
                f.dispatchEvent(new Event("change", { bubbles: true }));
                const still = () => visible(f) && f === find();
                const enter = () => {
                  for (const type of ["keydown", "keypress", "keyup"]) {
                    f.dispatchEvent(new KeyboardEvent(type, { key: "Enter", code: "Enter", keyCode: 13, which: 13, bubbles: true, cancelable: true }));
                  }
                };
                const label = (b) => [b.getAttribute("aria-label"), b.getAttribute("title"), b.textContent].join(" ").toLowerCase();
                const toggle = (b) => b.hasAttribute("aria-pressed") || b.hasAttribute("aria-checked")
                  || /^(switch|checkbox)$/.test(b.getAttribute("role") || "")
                  || /password|reveal|conceal|show|hide|mostra|nascondi|rivela|afficher|masquer|mostrar|ocultar|anzeigen|verbergen|einblenden|ausblenden/.test(label(b));
                const submitLike = /unlock|sblocca|déverrouiller|desbloquear|entsperren|ontgrendel|sign in|log ?in|accedi|connexion|continue|continua|submit|invia|\bgo\b/;
                const usable = (b) => b && visible(b) && !b.disabled && b.getAttribute("aria-disabled") !== "true";
                const button = () => {
                  const known = document.querySelector("[data-testid=lock-screen-submit-button]");
                  if (usable(known)) return known;
                  for (let node = f.parentElement, depth = 0; node && depth < 6; node = node.parentElement, depth++) {
                    const candidates = Array.from(node.querySelectorAll("button, [role=button], input[type=submit]"))
                      .filter((b) => usable(b)
                        && (f.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING) && !toggle(b));
                    if (!candidates.length) continue;
                    return candidates.find((b) => b.querySelector("[id*=unlock-icon], [class*=unlock-icon]") || /unlock-icon/.test(b.id + " " + b.className))
                      || candidates.find((b) => b.type === "submit")
                      || candidates.find((b) => submitLike.test(label(b)))
                      || candidates[candidates.length - 1];
                  }
                  return null;
                };
                setTimeout(() => {
                  if (!still()) return;
                  enter();
                  setTimeout(() => {
                    if (!still() || working()) return;
                    const b = button();
                    if (b) { b.click(); return; }
                    if (f.form && f.form.requestSubmit) f.form.requestSubmit();
                  }, 500);
                }, 150);
                setTimeout(() => { if (visible(f) && f === find()) post({ event: "fillFailed" }); }, 10000);
                return true;
              };
              window.__compassUnlock = { take: () => ({ events: queue.splice(0) }), fill };
            })();
            """.trimIndent()
    }
}
