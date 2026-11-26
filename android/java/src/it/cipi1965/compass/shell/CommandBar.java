// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.R;
import it.cipi1965.compass.claude.ClaudeSidebarBridge;

import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.omnibox.suggestions.AutocompleteController;
import org.chromium.chrome.browser.recent_tabs.ForeignSessionHelper;
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
    // Tabs on other devices (sync's SESSIONS), while open and not Incognito.
    private @Nullable ForeignSessionHelper mSessions;
    // Ask in the command bar (docs/design/ai-sidebar.md): the window's Claude, whether Ask is on,
    // the page "About this page" names, and the message Claude got (for Continue in Chat).
    private @Nullable ClaudeSidebarBridge mClaude;
    private boolean mAskEnabled;
    private String mPageTitle = "";
    private String mPageHost = "";
    private String mSent = "";

    public CommandBar(ChromeTabbedActivity activity, ViewGroup parent) {
        mActivity = activity;
        mView = CommandBarUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** Ask in the command bar's Claude (CompassBar sets it). */
    public void setClaudeSidebar(ClaudeSidebarBridge claude) {
        mClaude = claude;
    }

    /** Ask's answer so far (ClaudeSidebarBridge.Listener). */
    public void onAnswer(String text) {
        if (mState.getAsked().isEmpty()) return;
        mState.setAnswer(text);
    }

    /** Ask's answer is done: all of it, or why not. */
    public void onAnswerDone(String text, String error) {
        if (mState.getAsked().isEmpty()) return;
        if (!text.isEmpty()) mState.setAnswer(text);
        mState.setAnswerError(error);
        mState.setAnswerDone(true);
    }

    // Back to the results: the answer on its way is dropped.
    private void clearAnswer() {
        if (mClaude != null) mClaude.cancelAsk();
        mState.setAsked("");
        mState.setAnswer("");
        mState.setAnswerDone(false);
        mState.setAnswerError("");
        mSent = "";
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
        mState.setTheme(bar != null ? bar.spaceTheme() : SpaceTheme.GREY);
        mState.setTypedIsAddress(pageUrl);
        // Ask: never in incognito; "About this page" for a web page (its title and host only).
        clearAnswer();
        mAskEnabled = false;
        if (!incognito && mClaude != null) {
            try {
                mAskEnabled = new org.json.JSONObject(mClaude.snapshot()).optBoolean("ask");
            } catch (org.json.JSONException e) {
                // Not offered.
            }
        }
        GURL page = tab != null ? tab.getUrl() : GURL.emptyGURL();
        boolean web =
                tab != null
                        && !incognito
                        && page.isValid()
                        && (page.getScheme().equals("http") || page.getScheme().equals("https"));
        mPageTitle = web ? tab.getTitle() : "";
        mPageHost = web ? page.getHost() : "";
        mState.setAboutPageTitle(mPageTitle);
        mState.setAboutPage(true);
        mState.setAskOffered(false);
        updateEntries("");
        if (!incognito) {
            mSessions = new ForeignSessionHelper(profile);
            mSessions.setOnForeignSessionCallback(() -> updateDeviceTabs(mState.getText().trim()));
            mSessions.triggerSessionSync();
        }
        updateDeviceTabs("");
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
        // Typing goes back to the results.
        if (!mState.getAsked().isEmpty()) clearAnswer();
        mState.setText(text);
        mState.setAskOffered(
                mAskEnabled && mClaude != null && mClaude.looksLikeQuestion(text.trim()));
        mState.setTypedIsAddress(looksLikeAddress(text.trim()));
        updateEntries(text.trim());
        updateDeviceTabs(text.trim());
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

    /** Tabs on other devices for the typed text (CommandBarState.matchDeviceTabs()). */
    private void updateDeviceTabs(String text) {
        if (mSessions == null || !mSessions.isTabSyncEnabled() || text.isEmpty()) {
            mState.setDeviceTabs(new ArrayList<>());
            return;
        }
        List<CommandBarState.Device> devices = new ArrayList<>();
        for (ForeignSessionHelper.ForeignSession session : mSessions.getForeignSessions()) {
            List<CommandBarState.DeviceTab> tabs = new ArrayList<>();
            for (ForeignSessionHelper.ForeignSessionWindow window : session.windows) {
                for (ForeignSessionHelper.ForeignSessionTab tab : window.tabs) {
                    if (!tab.url.isValid()) continue;
                    tabs.add(
                            new CommandBarState.DeviceTab(
                                    tab.title, tab.url.getSpec(), session.name));
                }
            }
            devices.add(new CommandBarState.Device(session.name, session.modifiedTime, tabs));
        }
        mState.setDeviceTabs(CommandBarState.matchDeviceTabs(devices, text));
    }

    @Override
    public void onDeviceTabClicked(String url) {
        GURL gurl = new GURL(url);
        close();
        if (!gurl.isValid()) return;
        mActivity
                .getTabCreator(false)
                .createNewTab(
                        new LoadUrlParams(gurl.getSpec(), PageTransition.AUTO_BOOKMARK),
                        TabLaunchType.FROM_CHROME_UI,
                        null);
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
        // With Claude's answer there, Return continues in the chat; with Ask offered, it asks.
        if (!mState.getAsked().isEmpty()) {
            if (mState.getAnswerDone() && !mState.getAnswer().isEmpty()) onContinueInChat();
            return;
        }
        if (mState.getAskOffered()) {
            onAsk();
            return;
        }
        AutocompleteMatch match = mController != null ? mController.classify(trimmed) : null;
        if (match != null) {
            navigate(match.getUrl());
        } else if (!mMatches.isEmpty()) {
            navigate(mMatches.get(0).getUrl());
        }
    }

    @Override
    public void onAsk() {
        String question = mState.getText().trim();
        if (mClaude == null || question.isEmpty()) return;
        boolean about = mState.getAboutPage() && !mPageTitle.isEmpty();
        mState.setAsked(question);
        mState.setAnswer("");
        mState.setAnswerDone(false);
        mState.setAnswerError("");
        mSent = mClaude.ask(question, about ? mPageTitle : "", about ? mPageHost : "");
        if (mSent.isEmpty()) {
            mState.setAnswerError(mActivity.getString(R.string.compass_command_claude_not_set_up));
            mState.setAnswerDone(true);
        }
    }

    @Override
    public void onAboutPage(boolean on) {
        mState.setAboutPage(on);
    }

    @Override
    public void onContinueInChat() {
        String question = mState.getAsked();
        String answer = mState.getAnswer();
        String sent = mSent;
        if (question.isEmpty() || answer.isEmpty()) return;
        close();
        CompassBar bar = CompassBar.forActivity(mActivity);
        if (bar != null) bar.continueClaude(question, sent, answer);
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
        clearAnswer();
        if (mController != null) {
            mController.removeOnSuggestionsReceivedListener(this);
            mController.resetSession();
            mController = null;
        }
        mMatches.clear();
        if (mSessions != null) {
            mSessions.destroy();
            mSessions = null;
        }
        mState.setDeviceTabs(new ArrayList<>());
        mState.setOpen(false);
        mView.setVisibility(View.GONE);
    }
}
