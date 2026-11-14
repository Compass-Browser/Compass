// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.autofill;

import android.app.PendingIntent;
import android.app.assist.AssistStructure;
import android.app.assist.AssistStructure.ViewNode;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.CancellationSignal;
import android.service.autofill.AutofillService;
import android.service.autofill.Dataset;
import android.service.autofill.FillCallback;
import android.service.autofill.FillContext;
import android.service.autofill.FillRequest;
import android.service.autofill.FillResponse;
import android.service.autofill.SaveCallback;
import android.service.autofill.SaveInfo;
import android.service.autofill.SaveRequest;
import android.text.InputType;
import android.util.Base64;
import android.view.View;
import android.view.autofill.AutofillId;
import android.view.autofill.AutofillValue;
import android.widget.RemoteViews;

import org.chromium.build.annotations.Nullable;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.List;
import java.util.Locale;

/**
 * Compass's saved passwords in other apps (docs/design/android.md, "Passwords in other apps"), as
 * the iOS AutoFill extension does: on a login form, a "Compass passwords" suggestion that opens
 * the picker (PasswordPickerActivity), which unlocks with the screen lock and fills the one the
 * user picks. Pages in Compass itself fill through Chrome's own autofill. A login form's
 * submission can be saved into Compass's passwords (onSaveRequest()).
 *
 * <p>It's in :base_java, in the app bundle's base module, as the system binds services by their
 * class (b/169196314): it names the activities it starts, which are in the "chrome" split, rather
 * than referring to their classes.
 */
public class CompassAutofillService extends AutofillService {
    static final String PICKER_ACTIVITY = "it.cipi1965.compass.autofill.PasswordPickerActivity";
    static final String SAVE_ACTIVITY = "it.cipi1965.compass.autofill.PasswordSaveActivity";
    // The picker's extras.
    static final String EXTRA_USERNAME_ID = "it.cipi1965.compass.autofill.USERNAME_ID";
    static final String EXTRA_PASSWORD_ID = "it.cipi1965.compass.autofill.PASSWORD_ID";
    static final String EXTRA_WEB_DOMAIN = "it.cipi1965.compass.autofill.WEB_DOMAIN";
    static final String EXTRA_PACKAGE = "it.cipi1965.compass.autofill.PACKAGE";
    // The save activity's.
    static final String EXTRA_SIGNON_REALM = "it.cipi1965.compass.autofill.SIGNON_REALM";
    static final String EXTRA_USERNAME = "it.cipi1965.compass.autofill.USERNAME";
    static final String EXTRA_PASSWORD = "it.cipi1965.compass.autofill.PASSWORD";

    /** The fields of a login form, and whose form it is. */
    static class LoginFields {
        @Nullable AutofillId username;
        @Nullable AutofillId password;
        @Nullable String webDomain;
        String packageName = "";
    }

    @Override
    @SuppressWarnings("deprecation") // Dataset.Builder(RemoteViews), setValue(): API 29's way.
    public void onFillRequest(
            FillRequest request, CancellationSignal cancellationSignal, FillCallback callback) {
        List<FillContext> contexts = request.getFillContexts();
        if (contexts.isEmpty()) {
            callback.onSuccess(null);
            return;
        }
        AssistStructure structure = contexts.get(contexts.size() - 1).getStructure();
        LoginFields fields = findLoginFields(structure);
        if (fields.password == null
                || fields.packageName.equals(getPackageName())) {
            callback.onSuccess(null);
            return;
        }
        Intent picker = new Intent().setClassName(this, PICKER_ACTIVITY);
        picker.putExtra(EXTRA_USERNAME_ID, fields.username);
        picker.putExtra(EXTRA_PASSWORD_ID, fields.password);
        picker.putExtra(EXTRA_WEB_DOMAIN, fields.webDomain);
        picker.putExtra(EXTRA_PACKAGE, fields.packageName);
        PendingIntent pending =
                PendingIntent.getActivity(
                        this,
                        /* requestCode= */ 0,
                        picker,
                        PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_MUTABLE);

        RemoteViews presentation = new RemoteViews(getPackageName(), android.R.layout.simple_list_item_1);
        presentation.setTextViewText(android.R.id.text1, "Compass passwords…");
        // The values come from the picker, once unlocked.
        Dataset.Builder dataset = new Dataset.Builder(presentation);
        if (fields.username != null) dataset.setValue(fields.username, null);
        dataset.setValue(fields.password, null);
        dataset.setAuthentication(pending.getIntentSender());
        FillResponse.Builder response = new FillResponse.Builder().addDataset(dataset.build());
        // Saving what the form submits, where Compass can tell whose login it is.
        if (signonRealm(fields) != null) {
            SaveInfo.Builder save =
                    new SaveInfo.Builder(
                            SaveInfo.SAVE_DATA_TYPE_PASSWORD
                                    | (fields.username != null
                                            ? SaveInfo.SAVE_DATA_TYPE_USERNAME
                                            : 0),
                            new AutofillId[] {fields.password});
            if (fields.username != null) {
                save.setOptionalIds(new AutofillId[] {fields.username});
            }
            response.setSaveInfo(save.build());
        }
        callback.onSuccess(response.build());
    }

    /**
     * Another app's form was submitted and the user picked Save: Compass's passwords keep it
     * (PasswordSaveActivity, which starts the browser if it isn't running).
     */
    @Override
    public void onSaveRequest(SaveRequest request, SaveCallback callback) {
        List<FillContext> contexts = request.getFillContexts();
        if (contexts.isEmpty()) {
            callback.onSuccess();
            return;
        }
        AssistStructure structure = contexts.get(contexts.size() - 1).getStructure();
        LoginFields fields = findLoginFields(structure);
        String realm = signonRealm(fields);
        String password = fields.password != null ? textOf(structure, fields.password) : null;
        if (realm == null || password == null || password.isEmpty()) {
            callback.onSuccess();
            return;
        }
        String username = fields.username != null ? textOf(structure, fields.username) : null;
        Intent intent = new Intent().setClassName(this, SAVE_ACTIVITY);
        intent.putExtra(EXTRA_SIGNON_REALM, realm);
        intent.putExtra(EXTRA_USERNAME, username != null ? username : "");
        intent.putExtra(EXTRA_PASSWORD, password);
        PendingIntent pending =
                PendingIntent.getActivity(
                        this,
                        /* requestCode= */ 1,
                        intent,
                        PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        callback.onSuccess(pending.getIntentSender());
    }

    /**
     * Whose login a form is, as Chrome keeps it: a web view's "https://<domain>/", or an app's
     * "android://<certificate hash>@<package>/" (affiliation_utils.h); null if neither is known.
     */
    private @Nullable String signonRealm(LoginFields fields) {
        if (fields.webDomain != null && !fields.webDomain.isEmpty()) {
            return "https://" + fields.webDomain.toLowerCase(Locale.ROOT) + "/";
        }
        String hash = certificateHash(fields.packageName);
        return hash != null ? "android://" + hash + "@" + fields.packageName + "/" : null;
    }

    /** The SHA-512 of an app's signing certificate, in URL-safe base64 with padding. */
    private @Nullable String certificateHash(String packageName) {
        if (packageName.isEmpty()) return null;
        try {
            PackageInfo info =
                    getPackageManager()
                            .getPackageInfo(packageName, PackageManager.GET_SIGNING_CERTIFICATES);
            if (info.signingInfo == null) return null;
            Signature[] signers = info.signingInfo.getApkContentsSigners();
            if (signers == null || signers.length == 0) return null;
            byte[] digest = MessageDigest.getInstance("SHA-512").digest(signers[0].toByteArray());
            return Base64.encodeToString(digest, Base64.URL_SAFE | Base64.NO_WRAP);
        } catch (PackageManager.NameNotFoundException | NoSuchAlgorithmException e) {
            return null;
        }
    }

    /** The text a field of |structure| holds, or null. */
    private static @Nullable String textOf(AssistStructure structure, AutofillId id) {
        for (int i = 0; i < structure.getWindowNodeCount(); i++) {
            String text = textOf(structure.getWindowNodeAt(i).getRootViewNode(), id);
            if (text != null) return text;
        }
        return null;
    }

    private static @Nullable String textOf(ViewNode node, AutofillId id) {
        if (id.equals(node.getAutofillId())) {
            AutofillValue value = node.getAutofillValue();
            return value != null && value.isText() ? value.getTextValue().toString() : null;
        }
        for (int i = 0; i < node.getChildCount(); i++) {
            String text = textOf(node.getChildAt(i), id);
            if (text != null) return text;
        }
        return null;
    }

    static LoginFields findLoginFields(AssistStructure structure) {
        LoginFields fields = new LoginFields();
        fields.packageName = structure.getActivityComponent().getPackageName();
        for (int i = 0; i < structure.getWindowNodeCount(); i++) {
            visit(structure.getWindowNodeAt(i).getRootViewNode(), fields, new AutofillId[1]);
        }
        return fields;
    }

    /** |lastText| is the last text field before this one: a username, if a password follows. */
    private static void visit(ViewNode node, LoginFields fields, AutofillId[] lastText) {
        if (fields.webDomain == null && node.getWebDomain() != null) {
            fields.webDomain = node.getWebDomain();
        }
        AutofillId id = node.getAutofillId();
        if (id != null && node.getAutofillType() == View.AUTOFILL_TYPE_TEXT) {
            if (isPassword(node)) {
                if (fields.password == null) {
                    fields.password = id;
                    if (fields.username == null) fields.username = lastText[0];
                }
            } else if (isUsername(node)) {
                if (fields.username == null) fields.username = id;
            } else if (node.getVisibility() == View.VISIBLE) {
                lastText[0] = id;
            }
        }
        for (int i = 0; i < node.getChildCount(); i++) {
            visit(node.getChildAt(i), fields, lastText);
        }
    }

    private static boolean isPassword(ViewNode node) {
        if (hasHint(node, View.AUTOFILL_HINT_PASSWORD, "password", "current-password")) {
            return true;
        }
        int variation = node.getInputType() & InputType.TYPE_MASK_VARIATION;
        if ((node.getInputType() & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_TEXT
                && (variation == InputType.TYPE_TEXT_VARIATION_PASSWORD
                        || variation == InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD
                        || variation == InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD)) {
            return true;
        }
        ViewStructureHtml html = ViewStructureHtml.of(node);
        return html != null && "password".equals(html.type);
    }

    private static boolean isUsername(ViewNode node) {
        if (hasHint(
                node,
                View.AUTOFILL_HINT_USERNAME,
                View.AUTOFILL_HINT_EMAIL_ADDRESS,
                "email",
                "username")) {
            return true;
        }
        int variation = node.getInputType() & InputType.TYPE_MASK_VARIATION;
        if ((node.getInputType() & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_TEXT
                && (variation == InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS
                        || variation == InputType.TYPE_TEXT_VARIATION_WEB_EMAIL_ADDRESS)) {
            return true;
        }
        ViewStructureHtml html = ViewStructureHtml.of(node);
        return html != null && ("email".equals(html.type) || html.autocompleteUsername);
    }

    private static boolean hasHint(ViewNode node, String... wanted) {
        String[] hints = node.getAutofillHints();
        if (hints == null) return false;
        for (String hint : hints) {
            String lower = hint.toLowerCase(Locale.ROOT);
            for (String w : wanted) {
                if (lower.equals(w.toLowerCase(Locale.ROOT))) return true;
            }
        }
        return false;
    }

    /** A web view's input element: its type and autocomplete attributes. */
    private static class ViewStructureHtml {
        @Nullable String type;
        boolean autocompleteUsername;

        static @Nullable ViewStructureHtml of(ViewNode node) {
            android.view.ViewStructure.HtmlInfo info = node.getHtmlInfo();
            if (info == null || !"input".equalsIgnoreCase(info.getTag())) return null;
            ViewStructureHtml html = new ViewStructureHtml();
            List<android.util.Pair<String, String>> attributes = info.getAttributes();
            if (attributes == null) return html;
            for (android.util.Pair<String, String> attribute : attributes) {
                if ("type".equalsIgnoreCase(attribute.first) && attribute.second != null) {
                    html.type = attribute.second.toLowerCase(Locale.ROOT);
                } else if ("autocomplete".equalsIgnoreCase(attribute.first)
                        && attribute.second != null) {
                    String value = attribute.second.toLowerCase(Locale.ROOT);
                    html.autocompleteUsername =
                            value.contains("username") || value.contains("email");
                }
            }
            return html;
        }
    }
}
