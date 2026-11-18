// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewGroup;

import androidx.coordinatorlayout.widget.CoordinatorLayout;

import it.cipi1965.compass.R;

import org.chromium.base.Callback;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tab.TabUtils;
import org.chromium.components.embedder_support.contextmenu.ContextMenuNativeDelegate;
import org.chromium.components.embedder_support.contextmenu.ContextMenuParams;
import org.chromium.components.embedder_support.contextmenu.ContextMenuUi;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.base.PageTransition;
import org.chromium.ui.base.WindowAndroid;
import org.chromium.ui.listmenu.ListMenuItemProperties;
import org.chromium.ui.listmenu.ListMenuSubmenuItemProperties;
import org.chromium.ui.modelutil.MVCListAdapter.ListItem;
import org.chromium.ui.modelutil.MVCListAdapter.ModelList;
import org.chromium.ui.modelutil.PropertyModel;
import org.chromium.url.GURL;

import java.util.ArrayList;
import java.util.List;

/**
 * Compass's long-press menu for a link, an image or the page (LinkMenuUi.kt; docs/design/
 * android.md, "The link menu"), in place of Chrome's dialog: Chrome's populator still builds the
 * items and runs them (the ids go back through |onItemClicked|), and Compass adds iOS's own: Open
 * in New Tab (shown), Open in Little Window and Pin. Chrome's "Open in new tab" opens in the
 * background, so it's "Open in Background" here; tab groups and the reading list aren't
 * Compass's, so their items go.
 */
public class LinkMenu implements ContextMenuUi, LinkMenuUi.Listener {
    /** Compass's names for some of Chrome's items, in its title case. */
    private static final java.util.Map<Integer, String> TITLES = new java.util.HashMap<>();

    static {
        TITLES.put(org.chromium.chrome.R.id.contextmenu_open_in_new_window, "Open in New Window");
        TITLES.put(org.chromium.chrome.R.id.contextmenu_copy_link_address, "Copy Link");
        TITLES.put(org.chromium.chrome.R.id.contextmenu_copy_link_text, "Copy Link Text");
        TITLES.put(org.chromium.chrome.R.id.contextmenu_save_link_as, "Download Link");
        TITLES.put(org.chromium.chrome.R.id.contextmenu_share_link, "Share Link…");
        TITLES.put(org.chromium.chrome.R.id.contextmenu_save_image, "Download Image");
        TITLES.put(org.chromium.chrome.R.id.contextmenu_copy_image, "Copy Image");
        TITLES.put(org.chromium.chrome.R.id.contextmenu_share_image, "Share Image…");
        TITLES.put(org.chromium.chrome.R.id.contextmenu_open_image_in_new_tab, "Open Image in New Tab");
        TITLES.put(org.chromium.chrome.R.id.contextmenu_open_image_in_ephemeral_tab, "Peek Image");
        TITLES.put(org.chromium.chrome.R.id.contextmenu_open_in_ephemeral_tab, "Peek");
    }

    private final ChromeTabbedActivity mActivity;
    private final Spaces mSpaces;
    private final Peek mPeek;
    private final LinkMenuState mState = new LinkMenuState();
    private final View mView;
    /** What each item's key does, while the menu shows. */
    private final List<Runnable> mActions = new ArrayList<>();
    /** The top level, to come back to from a submenu. */
    private List<List<LinkMenuState.Item>> mTopGroups = new ArrayList<>();

    private @Nullable ContextMenuNativeDelegate mNativeDelegate;
    private @Nullable Callback<Integer> mOnItemClicked;
    private @Nullable Runnable mOnMenuClosed;

    public LinkMenu(ChromeTabbedActivity activity, ViewGroup parent, Spaces spaces, Peek peek) {
        mActivity = activity;
        mSpaces = spaces;
        mPeek = peek;
        mView = LinkMenuUi.createView(activity, activity, activity, activity, mState, this);
        mView.setVisibility(View.GONE);
        parent.addView(
                mView,
                new CoordinatorLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** The menu for the next long press (ContextMenuHelper asks for it, then shows it). */
    public ContextMenuUi prepare(ContextMenuNativeDelegate nativeDelegate) {
        finish();
        mNativeDelegate = nativeDelegate;
        return this;
    }

    public void destroy() {
        finish();
        ((ViewGroup) mView.getParent()).removeView(mView);
    }

    // ContextMenuUi:

    @Override
    public void displayMenu(
            WindowAndroid window,
            WebContents webContents,
            ContextMenuParams params,
            List<ModelList> items,
            Callback<Integer> onItemClicked,
            Runnable onMenuShown,
            Runnable onMenuClosed) {
        mOnItemClicked = onItemClicked;
        mOnMenuClosed = onMenuClosed;
        mActions.clear();

        Tab tab = TabUtils.fromWebContents(webContents);
        boolean incognito = tab != null && tab.isIncognito();
        GURL link = params.getLinkUrl();
        boolean isLink = params.isAnchor() && !link.isEmpty();

        // The header: the link, else the image, else the page.
        if (isLink) {
            String text = params.getLinkText().trim();
            mState.setTitle(text.isEmpty() ? params.getTitleText() : text);
            mState.setUrl(link.getSpec());
        } else if (params.isImage() || params.isVideo()) {
            mState.setTitle(params.getTitleText());
            mState.setUrl(params.getSrcUrl().getSpec());
        } else {
            mState.setTitle("");
            mState.setUrl("");
        }
        mState.setImage(null);
        if (params.isImage() && mNativeDelegate != null) {
            int size = Math.round(56 * mActivity.getResources().getDisplayMetrics().density) * 2;
            mNativeDelegate.retrieveImageForContextMenu(
                    size, size, bitmap -> {
                        if (mOnMenuClosed != null) mState.setImage(bitmap);
                    });
        }

        boolean web = link.getScheme().equals("http") || link.getScheme().equals("https");
        // Compass's Open in Little Window and Pin, after Open in Incognito (else after Open in
        // Background), as on iOS; not in incognito.
        Runnable[] addOwn = new Runnable[1];
        List<List<LinkMenuState.Item>> groups = new ArrayList<>();
        for (ModelList section : items) {
            List<LinkMenuState.Item> group = new ArrayList<>();
            for (ListItem item : section) {
                int id = menuId(item.model);
                if (id == org.chromium.chrome.R.id.contextmenu_open_in_new_tab_in_group
                        || id == org.chromium.chrome.R.id.contextmenu_read_later) {
                    continue;
                }
                // Copying, saving and sharing the link: a card of their own.
                if (id == org.chromium.chrome.R.id.contextmenu_copy_link_address
                        && !group.isEmpty()) {
                    if (addOwn[0] != null) addOwn[0].run();
                    groups.add(group);
                    group = new ArrayList<>();
                }
                if (id == org.chromium.chrome.R.id.contextmenu_open_in_new_tab
                        && tab != null
                        && isLink) {
                    // Ours first: the link in a new tab, shown.
                    group.add(
                            own(
                                    "Open in New Tab",
                                    R.drawable.compass_sym_open_in_new,
                                    () -> openInNewTab(tab, params, link)));
                    group.add(chrome(item, "Open in Background", R.drawable.compass_sym_tab));
                    if (!incognito && web) {
                        List<LinkMenuState.Item> into = group;
                        addOwn[0] =
                                () -> {
                                    addOwn[0] = null;
                                    into.add(
                                            own(
                                                    "Open in Little Window",
                                                    R.drawable.compass_sym_picture_in_picture,
                                                    () ->
                                                            mPeek.showLittleWindow(
                                                                    tab.getProfile(), link)));
                                    into.add(
                                            own(
                                                    "Pin",
                                                    R.drawable.compass_sym_keep,
                                                    () -> mSpaces.pinLink(link)));
                                };
                    }
                    continue;
                }
                if (id == org.chromium.chrome.R.id.contextmenu_open_in_new_tab) {
                    // A page without a tab (Peek's card): it opens in the background.
                    group.add(chrome(item, "Open in Background", R.drawable.compass_sym_tab));
                    continue;
                }
                if (id == org.chromium.chrome.R.id.contextmenu_open_in_incognito_tab) {
                    group.add(chrome(item, "Open in Incognito", R.drawable.compass_sym_domino_mask));
                    if (addOwn[0] != null) addOwn[0].run();
                    continue;
                }
                group.add(chrome(item, TITLES.get(id), iconFor(id)));
            }
            if (addOwn[0] != null) addOwn[0].run();
            if (!group.isEmpty()) groups.add(group);
        }
        mTopGroups = groups;
        mState.setSubmenuTitle("");
        mState.setGroups(groups);
        mView.setVisibility(View.VISIBLE);
        mView.bringToFront();
        mState.setOpen(true);
        onMenuShown.run();
    }

    @Override
    public void dismiss() {
        finish();
    }

    // LinkMenuUi.Listener:

    @Override
    public void onItem(int key) {
        if (key < 0 || key >= mActions.size()) return;
        mActions.get(key).run();
    }

    @Override
    public void onSubmenuBack() {
        mState.setSubmenuTitle("");
        mState.setGroups(mTopGroups);
    }

    @Override
    public void onDismiss() {
        finish();
    }

    // Items:

    /** A Chrome item: runs it through Chrome's populator, or opens its submenu. */
    private LinkMenuState.Item chrome(ListItem item, @Nullable String title, int icon) {
        PropertyModel model = item.model;
        String label = title != null ? title : String.valueOf(get(model, ListMenuItemProperties.TITLE));
        boolean enabled =
                !model.getAllSetProperties().contains(ListMenuItemProperties.ENABLED)
                        || model.get(ListMenuItemProperties.ENABLED);
        Bitmap bitmap = iconBitmap(model);
        if (model.getAllSetProperties().contains(ListMenuSubmenuItemProperties.SUBMENU_PROVIDER)) {
            List<ListItem> children = model.get(ListMenuSubmenuItemProperties.SUBMENU_PROVIDER).get();
            return add(label, icon, enabled, bitmap, true, () -> showSubmenu(label, children));
        }
        int id = menuId(model);
        View.OnClickListener click =
                model.getAllSetProperties().contains(ListMenuItemProperties.CLICK_LISTENER)
                        ? model.get(ListMenuItemProperties.CLICK_LISTENER)
                        : null;
        return add(
                label,
                icon,
                enabled,
                bitmap,
                false,
                () -> {
                    Callback<Integer> onItemClicked = mOnItemClicked;
                    // Run before the menu closes: closing destroys the params it reads.
                    if (click != null) {
                        click.onClick(mView);
                    } else if (onItemClicked != null) {
                        onItemClicked.onResult(id);
                    }
                    finish();
                });
    }

    /** One of Compass's items. */
    private LinkMenuState.Item own(String title, int icon, Runnable action) {
        return add(
                title,
                icon,
                true,
                null,
                false,
                () -> {
                    action.run();
                    finish();
                });
    }

    private LinkMenuState.Item add(
            String title,
            int icon,
            boolean enabled,
            @Nullable Bitmap bitmap,
            boolean submenu,
            Runnable action) {
        mActions.add(action);
        return new LinkMenuState.Item(mActions.size() - 1, title, icon, enabled, bitmap, submenu);
    }

    private void showSubmenu(String title, List<ListItem> children) {
        List<LinkMenuState.Item> group = new ArrayList<>();
        for (ListItem child : children) group.add(chrome(child, null, iconFor(menuId(child.model))));
        List<List<LinkMenuState.Item>> groups = new ArrayList<>();
        groups.add(group);
        mState.setSubmenuTitle(title);
        mState.setGroups(groups);
    }

    // Ours: the link in a new tab after |tab|, shown (Chrome's own opens it in the background).
    private void openInNewTab(Tab tab, ContextMenuParams params, GURL link) {
        LoadUrlParams load = new LoadUrlParams(link.getSpec(), PageTransition.LINK);
        load.setReferrer(params.getReferrer());
        mActivity
                .getTabCreator(tab.isIncognito())
                .createNewTab(load, TabLaunchType.FROM_LONGPRESS_FOREGROUND, tab);
    }

    /** Closes the menu once: Chrome then destroys the params and the native delegate. */
    private void finish() {
        Runnable onClosed = mOnMenuClosed;
        mOnMenuClosed = null;
        mOnItemClicked = null;
        mNativeDelegate = null;
        mActions.clear();
        if (mState.getOpen()) {
            mState.setOpen(false);
            // After the sheet has slid away.
            mView.postDelayed(
                    () -> {
                        if (!mState.getOpen()) mView.setVisibility(View.GONE);
                    },
                    400);
        }
        if (onClosed != null) onClosed.run();
    }

    private static int menuId(PropertyModel model) {
        return model.getAllSetProperties().contains(ListMenuItemProperties.MENU_ITEM_ID)
                ? model.get(ListMenuItemProperties.MENU_ITEM_ID)
                : 0;
    }

    private static @Nullable Object get(PropertyModel model, PropertyModel.ReadableObjectPropertyKey<?> key) {
        return model.getAllSetProperties().contains(key) ? model.get(key) : "";
    }

    /** An extension's item's icon. */
    private @Nullable Bitmap iconBitmap(PropertyModel model) {
        if (model.getAllSetProperties().contains(ListMenuItemProperties.START_ICON_BITMAP)) {
            return model.get(ListMenuItemProperties.START_ICON_BITMAP);
        }
        if (model.getAllSetProperties().contains(ListMenuItemProperties.START_ICON_DRAWABLE)) {
            Drawable drawable = model.get(ListMenuItemProperties.START_ICON_DRAWABLE);
            if (drawable instanceof BitmapDrawable bd) return bd.getBitmap();
            if (drawable != null && drawable.getIntrinsicWidth() > 0) {
                Bitmap bitmap =
                        Bitmap.createBitmap(
                                drawable.getIntrinsicWidth(),
                                drawable.getIntrinsicHeight(),
                                Bitmap.Config.ARGB_8888);
                Canvas canvas = new Canvas(bitmap);
                drawable.setBounds(0, 0, canvas.getWidth(), canvas.getHeight());
                drawable.draw(canvas);
                return bitmap;
            }
        }
        return null;
    }

    /** Compass's icon for one of Chrome's items (0: none). */
    private static int iconFor(int id) {
        if (id == org.chromium.chrome.R.id.contextmenu_open_in_ephemeral_tab
                || id == org.chromium.chrome.R.id.contextmenu_open_image_in_ephemeral_tab) {
            return R.drawable.compass_sym_visibility;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_open_in_new_window
                || id == org.chromium.chrome.R.id.contextmenu_open_in_other_window) {
            return R.drawable.compass_sym_select_window;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_open_in_incognito_window) {
            return R.drawable.compass_sym_domino_mask;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_copy_link_address
                || id == org.chromium.chrome.R.id.contextmenu_copy_image
                || id == org.chromium.chrome.R.id.contextmenu_copy
                || id == org.chromium.chrome.R.id.contextmenu_copy_video_frame) {
            return R.drawable.compass_sym_content_copy;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_copy_link_text) {
            return R.drawable.compass_sym_text_fields;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_save_link_as
                || id == org.chromium.chrome.R.id.contextmenu_save_image
                || id == org.chromium.chrome.R.id.contextmenu_save_video
                || id == org.chromium.chrome.R.id.contextmenu_save_page
                || id == org.chromium.chrome.R.id.contextmenu_download_video_frame) {
            return R.drawable.compass_sym_download;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_share_link
                || id == org.chromium.chrome.R.id.contextmenu_share_image
                || id == org.chromium.chrome.R.id.contextmenu_share_page
                || id == org.chromium.chrome.R.id.contextmenu_share_highlight) {
            return R.drawable.compass_sym_share;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_open_image
                || id == org.chromium.chrome.R.id.contextmenu_open_image_in_new_tab
                || id == org.chromium.chrome.R.id.contextmenu_load_original_image) {
            return R.drawable.compass_sym_image;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_search_by_image
                || id == org.chromium.chrome.R.id.contextmenu_search_image_with_google_lens
                || id == org.chromium.chrome.R.id.contextmenu_shop_image_with_google_lens
                || id == org.chromium.chrome.R.id.contextmenu_search_tab_with_google_lens) {
            return R.drawable.compass_sym_image_search;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_call) return R.drawable.compass_sym_call;
        if (id == org.chromium.chrome.R.id.contextmenu_send_message) return R.drawable.compass_sym_sms;
        if (id == org.chromium.chrome.R.id.contextmenu_add_to_contacts) {
            return R.drawable.compass_sym_person_add;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_picture_in_picture) {
            return R.drawable.compass_sym_picture_in_picture;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_print_page) return R.drawable.compass_sym_print;
        if (id == org.chromium.chrome.R.id.contextmenu_translate) return R.drawable.compass_sym_translate;
        if (id == org.chromium.chrome.R.id.contextmenu_create_qr_code) return R.drawable.compass_sym_qr_code;
        if (id == org.chromium.chrome.R.id.contextmenu_view_page_source
                || id == org.chromium.chrome.R.id.contextmenu_inspect_element) {
            return R.drawable.compass_sym_code;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_send_tab_to_self) return R.drawable.compass_sym_devices;
        if (id == org.chromium.chrome.R.id.contextmenu_open_in_reading_mode) {
            return R.drawable.compass_sym_menu_book;
        }
        if (id == org.chromium.chrome.R.id.contextmenu_back) return R.drawable.compass_sym_arrow_back;
        if (id == org.chromium.chrome.R.id.contextmenu_forward) return R.drawable.compass_sym_arrow_forward;
        if (id == org.chromium.chrome.R.id.contextmenu_reload) return R.drawable.compass_sym_refresh;
        if (id == org.chromium.chrome.R.id.contextmenu_learn_more) return R.drawable.compass_sym_info;
        if (id == org.chromium.chrome.R.id.contextmenu_remove_highlight) {
            return R.drawable.compass_sym_format_quote;
        }
        return 0;
    }
}
