// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.autofill;

import android.app.PendingIntent;
import android.app.assist.AssistStructure;
import android.app.assist.AssistStructure.ViewNode;
import android.content.Intent;
import android.os.CancellationSignal;
import android.service.autofill.AutofillService;
import android.service.autofill.Dataset;
import android.service.autofill.FillCallback;
import android.service.autofill.FillContext;
import android.service.autofill.FillRequest;
import android.service.autofill.FillResponse;
import android.service.autofill.SaveCallback;
import android.service.autofill.SaveRequest;
import android.text.InputType;
import android.view.View;
import android.view.autofill.AutofillId;
import android.widget.RemoteViews;

import org.chromium.build.annotations.Nullable;

import java.util.List;
import java.util.Locale;

/**
 * Compass's saved passwords in other apps (docs/design/android.md, "Passwords in other apps"), as
 * the iOS AutoFill extension does: on a login form, a "Compass passwords" suggestion that opens
 * the picker (PasswordPickerActivity), which unlocks with the screen lock and fills the one the
 * user picks. Pages in Compass itself fill through Chrome's own autofill.
 */
public class CompassAutofillService extends AutofillService {
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
        Intent picker = new Intent(this, PasswordPickerActivity.class);
        picker.putExtra(PasswordPickerActivity.EXTRA_USERNAME_ID, fields.username);
        picker.putExtra(PasswordPickerActivity.EXTRA_PASSWORD_ID, fields.password);
        picker.putExtra(PasswordPickerActivity.EXTRA_WEB_DOMAIN, fields.webDomain);
        picker.putExtra(PasswordPickerActivity.EXTRA_PACKAGE, fields.packageName);
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
        callback.onSuccess(new FillResponse.Builder().addDataset(dataset.build()).build());
    }

    @Override
    public void onSaveRequest(SaveRequest request, SaveCallback callback) {
        // Saving from other apps isn't offered (no SaveInfo).
        callback.onSuccess();
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
