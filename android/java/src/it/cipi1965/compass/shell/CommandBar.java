// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.omnibox.suggestions.AutocompleteController;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.components.omnibox.AutocompleteInput;
import org.chromium.components.omnibox.AutocompleteMatch;
import org.chromium.components.omnibox.AutocompleteResult;
import org.chromium.components.omnibox.OmniboxFocusReason;
import org.chromium.components.omnibox.OmniboxSuggestionType;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.ui.base.PageTransition;
import org.chromium.url.GURL;

import java.util.ArrayList;
import java.util.List;

/**
 * Compass's command bar (CommandBarUi.kt): URL and search input over the page, with Chrome's
 * autocomplete (history, bookmarks, open tabs, search suggestions) behind it.
 */
public class CommandBar
        implements CommandBarUi.Listener, AutocompleteController.OnSuggestionsReceivedListener {
    private static final int MAX_SUGGESTIONS = 8;

    private final ChromeTabbedActivity mActivity;
    private final CommandBarState mState = new CommandBarState();
    private final View mView;
    private final AutocompleteInput mInput = new AutocompleteInput(OmniboxFocusReason.OMNIBOX_TAP);
    private final List<AutocompleteMatch> mMatches = new ArrayList<>();
    private @Nullable AutocompleteController mController;
    // Opens results in a new tab instead of the current one.
    private boolean mInNewTab;

    public CommandBar(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mView = CommandBarUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** Opens the command bar, for the current tab or for a new one. */
    public void open(boolean inNewTab) {
        Tab tab = mActivity.getActivityTabProvider().get();
        mInNewTab = inNewTab || tab == null;
        boolean incognito = tab != null ? tab.isIncognito() : false;
        var profile =
                tab != null
                        ? tab.getProfile()
                        : mActivity.getTabModelSelector().getCurrentModel().getProfile();
        if (profile == null) return;
        mController = AutocompleteController.getForProfile(profile);
        if (mController == null) return;
        mController.addOnSuggestionsReceivedListener(this);

        GURL url = tab != null && !mInNewTab ? tab.getUrl() : GURL.emptyGURL();
        boolean pageUrl = url.isValid() && !url.getScheme().startsWith("chrome");
        mState.setText(pageUrl ? url.getSpec() : "");
        mState.setSuggestions(new ArrayList<>());
        mState.setIncognito(incognito);
        mState.setEditing(!mInNewTab);
        CompassBar bar = CompassBar.forActivity(mActivity);
        mState.setSpaceName(bar != null ? bar.spaceName() : "");
        mState.setTint(bar != null ? bar.spaceTint() : "");
        mState.setTypedIsAddress(pageUrl);
        updateEntries("");
        mInput.setPageUrl(url).setUserText(mState.getText());
        mController.startZeroSuggest(tab != null ? tab.getWebContents() : null, mInput);

        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
    }

    public boolean isOpen() {
        return mState.getOpen();
    }

    // CommandBarUi.Listener:

    @Override
    public void onTextChanged(String text) {
        mState.setText(text);
        mState.setTypedIsAddress(looksLikeAddress(text.trim()));
        updateEntries(text.trim());
        if (mController == null) return;
        Tab tab = mActivity.getActivityTabProvider().get();
        mInput.setUserText(text);
        mController.start(
                tab != null ? tab.getWebContents() : null,
                mInput,
                text.length(),
                /* preventInlineAutocomplete= */ true);
    }

    @Override
    public void onSuggestionClicked(int index) {
        if (index < 0 || index >= mMatches.size()) return;
        navigate(mMatches.get(index).getUrl());
    }

    @Override
    public void onEntryClicked(String id, String url) {
        close();
        CompassBar bar = CompassBar.forActivity(mActivity);
        if (bar != null) bar.openNode(id, url);
    }

    /** The sidebar's entries for the typed text: Today's, or those it matches, at most 12. */
    private void updateEntries(String text) {
        CompassBar bar = CompassBar.forActivity(mActivity);
        if (bar == null || mState.getIncognito()) {
            mState.setEntries(new ArrayList<>());
            return;
        }
        String needle = text.toLowerCase(java.util.Locale.ROOT);
        List<CommandBarState.Entry> entries = new ArrayList<>();
        for (CommandBarState.Entry entry : bar.sidebarEntries()) {
            if (entries.size() >= 12) break;
            boolean match =
                    needle.isEmpty()
                            ? entry.where.equals("Today")
                            : entry.title.toLowerCase(java.util.Locale.ROOT).contains(needle)
                                    || entry.url.toLowerCase(java.util.Locale.ROOT).contains(needle);
            if (match) entries.add(entry);
        }
        mState.setEntries(entries);
    }

    /** "example.com", "https://…", "localhost:8000": an address, not a search. */
    private static boolean looksLikeAddress(String text) {
        if (text.isEmpty() || text.contains(" ")) return false;
        if (text.contains("://")) return true;
        int dot = text.indexOf('.');
        return (dot > 0 && dot < text.length() - 1) || text.startsWith("localhost");
    }

    @Override
    public void onSubmit(String text) {
        String trimmed = text.trim();
        if (trimmed.isEmpty()) return;
        AutocompleteMatch match = mController != null ? mController.classify(trimmed) : null;
        if (match != null) {
            navigate(match.getUrl());
        } else if (!mMatches.isEmpty()) {
            navigate(mMatches.get(0).getUrl());
        }
    }

    @Override
    public void onDismiss() {
        close();
    }

    // AutocompleteController.OnSuggestionsReceivedListener:

    @Override
    public void onSuggestionsReceived(AutocompleteResult result, boolean isFinal) {
        mMatches.clear();
        List<CommandBarState.Suggestion> rows = new ArrayList<>();
        for (AutocompleteMatch match : result.getSuggestionsList()) {
            if (rows.size() >= MAX_SUGGESTIONS) break;
            if (!match.getUrl().isValid()) continue;
            boolean search = isSearch(match.getType());
            String title = match.getDisplayText();
            String subtitle = search ? "" : match.getDescription();
            // Pages show their title first, with the URL below.
            if (!search && !subtitle.isEmpty()) {
                String url = title;
                title = subtitle;
                subtitle = url;
            }
            mMatches.add(match);
            rows.add(
                    new CommandBarState.Suggestion(
                            title, subtitle, search, match.getUrl().getSpec()));
        }
        mState.setSuggestions(rows);
    }

    private static boolean isSearch(@OmniboxSuggestionType int type) {
        return type == OmniboxSuggestionType.SEARCH_WHAT_YOU_TYPED
                || type == OmniboxSuggestionType.SEARCH_HISTORY
                || type == OmniboxSuggestionType.SEARCH_SUGGEST
                || type == OmniboxSuggestionType.SEARCH_SUGGEST_ENTITY
                || type == OmniboxSuggestionType.SEARCH_SUGGEST_TAIL
                || type == OmniboxSuggestionType.SEARCH_SUGGEST_PERSONALIZED
                || type == OmniboxSuggestionType.SEARCH_SUGGEST_PROFILE
                || type == OmniboxSuggestionType.SEARCH_OTHER_ENGINE;
    }

    private void navigate(GURL url) {
        if (!url.isValid()) return;
        Tab tab = mActivity.getActivityTabProvider().get();
        LoadUrlParams params = new LoadUrlParams(url.getSpec(), PageTransition.TYPED);
        if (mInNewTab || tab == null) {
            mActivity
                    .getTabCreator(tab != null && tab.isIncognito())
                    .createNewTab(params, TabLaunchType.FROM_OMNIBOX, tab);
        } else {
            tab.loadUrl(params);
        }
        close();
    }

    private void close() {
        if (mController != null) {
            mController.removeOnSuggestionsReceivedListener(this);
            mController.resetSession();
            mController = null;
        }
        mMatches.clear();
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
