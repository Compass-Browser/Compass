// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/peek/peek.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/file_select_helper.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tab_helpers.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/download/public/common/download_url_parameters.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/tabs/public/tab_interface.h"
#include "components/vector_icons/vector_icons.h"
#include "compass/browser/ui/views/sidebar/compass_sidebar_view.h"
#include "compass/browser/window_sync/tab_strip_bridge.h"
#include "compass/browser/window_sync/window_features.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/animation/animation_delegate_views.h"
#include "ui/views/background.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/widget/widget_delegate.h"

// TabHelpers lets only a few classes give a WebContents the tab helpers of a
// tab. Upstream's Link Preview, a Peek of its own since removed, was one of
// them, as `PreviewTab`; Peek takes its place. The page needs the helpers
// for everything a tab does (permissions, autofill, find, ...) and keeps them
// when it becomes one.
class PreviewTab {
 public:
  static void AttachTabHelpers(content::WebContents* contents) {
    TabHelpers::AttachTabHelpers(contents);
  }
};

namespace compass {

namespace {

// The window frame's card radius (docs/design/window-frame.md).
constexpr float kContentCornerRadius = 12;
constexpr float kCardCornerRadius = 10;
// The buttons beside the card's top trailing corner, and the title pill
// above its leading corner.
constexpr int kButtonSize = 28;
constexpr int kButtonIconSize = 16;
constexpr int kButtonSpacing = 8;
constexpr int kTitleHorizontalPadding = 12;
// The card's distance from the content area's top (room for the title) and
// bottom, and at least from its sides (more on wide windows, see
// CalculateCardBounds()).
constexpr int kTopInset = kButtonSize + 2 * kButtonSpacing;
constexpr int kBottomInset = 24;
constexpr int kMinHorizontalInset = 48;
// Opening: the card fades in and grows from this scale, around its center.
// Closing runs it backwards, faster.
constexpr base::TimeDelta kOpenDuration = base::Milliseconds(160);
constexpr base::TimeDelta kCloseDuration = base::Milliseconds(120);
constexpr double kOpenStartScale = 0.92;

constexpr int kKeyModifiers =
    blink::WebInputEvent::kShiftKey | blink::WebInputEvent::kControlKey |
    blink::WebInputEvent::kAltKey | blink::WebInputEvent::kMetaKey;
#if BUILDFLAG(IS_MAC)
constexpr int kCommandKey = blink::WebInputEvent::kMetaKey;
#else
constexpr int kCommandKey = blink::WebInputEvent::kControlKey;
#endif

bool IsKeyDown(const input::NativeWebKeyboardEvent& event,
               ui::KeyboardCode key,
               int modifiers) {
  return event.GetType() == blink::WebInputEvent::Type::kRawKeyDown &&
         event.windows_key_code == key &&
         (event.GetModifiers() & kKeyModifiers) == modifiers;
}

BrowserView* GetBrowserView(BrowserWindowInterface* browser) {
  return BrowserView::GetBrowserViewForBrowser(browser);
}

void DownloadLink(content::WebContents* source,
                  const content::OpenURLParams& params) {
  constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
      net::DefineNetworkTrafficAnnotation("compass_option_click_download", R"(
        semantics {
          sender: "Compass Peek"
          description:
            "Downloads a link the user Option+clicked where Compass can't "
            "peek at it (upstream downloads every Option+clicked link)."
          trigger: "Option+click on a link outside a Compass window."
          data: "Only the URL being downloaded."
          destination: WEBSITE
        }
        policy {
          cookies_allowed: YES
          cookies_store: "user"
          setting: "This feature cannot be disabled by settings."
          policy_exception_justification: "Same as a renderer download."
        })");
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(
      params.source_render_process_id, params.source_render_frame_id);
  if (!frame || content::WebContents::FromRenderFrameHost(frame) != source) {
    frame = source->GetPrimaryMainFrame();
  }
  std::unique_ptr<download::DownloadUrlParameters> download =
      frame->CreateDownloadUrlParameters(params.url, kTrafficAnnotation);
  download->set_referrer(params.referrer.url);
  download->set_referrer_policy(
      content::Referrer::ReferrerPolicyForUrlRequest(params.referrer.policy));
  download->set_has_user_gesture(params.user_gesture);
  download->set_download_source(download::DownloadSource::FROM_RENDERER);
  source->GetBrowserContext()->GetDownloadManager()->DownloadUrl(
      std::move(download));
}

}  // namespace

// The widget's contents: a dimmed content area with the card in it, the
// page's title and host above the card, and the close and open buttons
// beside it. A click on the dimmed part closes.
class PeekView : public views::View, public views::AnimationDelegateViews {
  METADATA_HEADER(PeekView, views::View)

 public:
  PeekView(base::RepeatingClosure close, base::RepeatingClosure open_as_entry)
      : views::AnimationDelegateViews(this), close_(std::move(close)) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);

    auto title = std::make_unique<views::View>();
    title->SetBackground(views::CreateRoundedRectBackground(
        ui::kColorSysSurface, kButtonSize / 2.f));
    title->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal,
        gfx::Insets::VH(0, kTitleHorizontalPadding), kButtonSpacing,
        /*collapse_margins_spacing=*/false));
    static_cast<views::BoxLayout*>(title->GetLayoutManager())
        ->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    title_label_ = title->AddChildView(std::make_unique<views::Label>());
    title_label_->SetEnabledColor(ui::kColorSysOnSurface);
    title_label_->SetElideBehavior(gfx::ELIDE_TAIL);
    host_label_ = title->AddChildView(std::make_unique<views::Label>());
    host_label_->SetEnabledColor(ui::kColorSysOnSurfaceSubtle);
    host_label_->SetElideBehavior(gfx::ELIDE_HEAD);
    static_cast<views::BoxLayout*>(title->GetLayoutManager())
        ->SetFlexForView(title_label_, 1);
    title_ = AddChildView(std::move(title));

    auto card = std::make_unique<views::View>();
    card->SetBackground(views::CreateRoundedRectBackground(
        ui::kColorSysSurface, kCardCornerRadius));
    web_view_ = card->AddChildView(std::make_unique<views::WebView>(nullptr));
    web_view_->holder()->SetNativeViewCornerRadii(
        gfx::RoundedCornersF(kCardCornerRadius));
    card_ = AddChildView(std::move(card));

    close_button_ =
        AddButton(close_, vector_icons::kCloseIcon, u"Close (Esc)");
    open_button_ = AddButton(std::move(open_as_entry), kOpenInFullIcon,
#if BUILDFLAG(IS_MAC)
                             u"Open as Tab (⌘O)"
#else
                             u"Open as Tab (Ctrl+O)"
#endif
    );
  }
  PeekView(const PeekView&) = delete;
  PeekView& operator=(const PeekView&) = delete;
  ~PeekView() override = default;

  views::WebView* web_view() { return web_view_; }
  views::View* card() { return card_; }
  std::u16string title_for_testing() const {
    return std::u16string(title_label_->GetText());
  }
  std::u16string host_for_testing() const {
    return std::u16string(host_label_->GetText());
  }

  // Shows |contents|'s title and host above the card.
  void UpdateTitle(content::WebContents* contents) {
    const GURL& url = contents->GetVisibleURL();
    std::u16string host = base::UTF8ToUTF16(url.host());
    if (base::StartsWith(host, u"www.")) {
      host = host.substr(4);
    }
    std::u16string title = contents->GetTitle();
    // Before the page names itself, the title is its URL.
    if (title == base::UTF8ToUTF16(url.spec()) ||
        title == base::UTF8ToUTF16(url.GetContent())) {
      title.clear();
    }
    title_label_->SetText(title.empty() ? host : title);
    host_label_->SetText(title.empty() ? std::u16string() : host);
    host_label_->SetVisible(!title.empty() && !host.empty());
    title_->SetTooltipText(base::UTF8ToUTF16(url.spec()));
    InvalidateLayout();
  }

  // Fades in and grows to its size, unless animations are off. The page is
  // a native view on macOS, out of reach of layer opacity and transforms, so
  // the whole widget fades and the card's bounds grow (the page lays out
  // again as it does).
  void StartOpening() {
    if (!gfx::Animation::ShouldRenderRichAnimation()) {
      return;
    }
    animation_.SetSlideDuration(kOpenDuration);
    animation_.SetTweenType(gfx::Tween::EASE_OUT);
    GetWidget()->SetOpacity(0);
    animation_.Show();
  }

  // Fades out and shrinks back, then runs |done|; at once if animations are
  // off. Takes no more input meanwhile.
  void StartClosing(base::OnceClosure done) {
    SetCanProcessEventsWithinSubtree(false);
    if (!gfx::Animation::ShouldRenderRichAnimation()) {
      std::move(done).Run();
      return;
    }
    closed_ = std::move(done);
    animation_.SetSlideDuration(kCloseDuration);
    // From wherever opening got to.
    if (!animation_.is_animating()) {
      animation_.Reset(1);
    }
    animation_.Hide();
  }

  // views::AnimationDelegateViews:
  void AnimationProgressed(const gfx::Animation* animation) override {
    GetWidget()->SetOpacity(static_cast<float>(animation_.GetCurrentValue()));
    InvalidateLayout();
  }

  void AnimationEnded(const gfx::Animation* animation) override {
    if (closed_) {
      GetWidget()->SetOpacity(0);
      std::move(closed_).Run();
      return;
    }
    GetWidget()->SetOpacity(1);
    InvalidateLayout();
  }

  // views::View:
  void Layout(PassKey) override {
    gfx::Rect card = CalculateCardBounds(GetLocalBounds());
    if (animation_.is_animating()) {
      const double scale = gfx::Tween::DoubleValueBetween(
          animation_.GetCurrentValue(), kOpenStartScale, 1.0);
      const gfx::Point center = card.CenterPoint();
      card.set_size(gfx::ScaleToRoundedSize(card.size(), scale));
      card.set_origin(center - gfx::Vector2d(card.width() / 2,
                                             card.height() / 2));
    }
    card_->SetBoundsRect(card);
    web_view_->SetBoundsRect(card_->GetLocalBounds());
    // The title above the card's leading corner, no wider than the card.
    const int title_width =
        std::min(title_->GetPreferredSize().width(), card.width());
    title_->SetBounds(card.x(), card.y() - kButtonSpacing - kButtonSize,
                      title_width, kButtonSize);
    int y = card.y();
    for (views::View* button : {close_button_.get(), open_button_.get()}) {
      button->SetBounds(card.right() + kButtonSpacing, y, kButtonSize,
                        kButtonSize);
      y += kButtonSize + kButtonSpacing;
    }
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(SkColorSetA(SK_ColorBLACK, 0x66));
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), kContentCornerRadius,
                          flags);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    // Beside the card and the buttons (they take their own presses).
    return !card_->bounds().Contains(event.location());
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (!card_->bounds().Contains(event.location())) {
      close_.Run();
    }
  }

  static gfx::Rect CalculateCardBounds(const gfx::Rect& area) {
    // Room for the buttons beside the card.
    const int horizontal = std::max(
        {kMinHorizontalInset, area.width() / 12,
         kButtonSize + 2 * kButtonSpacing});
    gfx::Rect card = area;
    card.Inset(gfx::Insets::TLBR(kTopInset, horizontal, kBottomInset,
                                 horizontal));
    return card;
  }

 private:
  views::ImageButton* AddButton(base::RepeatingClosure callback,
                                const gfx::VectorIcon& icon,
                                std::u16string tooltip) {
    auto button = views::CreateVectorImageButton(
        base::BindRepeating([](base::RepeatingClosure callback,
                               const ui::Event&) { callback.Run(); },
                            std::move(callback)));
    views::SetImageFromVectorIconWithColor(
        button.get(), icon, kButtonIconSize,
        views::IconColors(ui::kColorSysOnSurface, ui::kColorSysOnSurface));
    button->SetBackground(views::CreateRoundedRectBackground(
        ui::kColorSysSurface, kButtonSize / 2.f));
    button->SetTooltipText(tooltip);
    button->GetViewAccessibility().SetName(tooltip);
    return AddChildView(std::move(button));
  }

  base::RepeatingClosure close_;
  // Run when closing ends.
  base::OnceClosure closed_;
  gfx::SlideAnimation animation_{this};
  raw_ptr<views::View> title_ = nullptr;
  raw_ptr<views::Label> title_label_ = nullptr;
  raw_ptr<views::Label> host_label_ = nullptr;
  raw_ptr<views::View> card_ = nullptr;
  raw_ptr<views::WebView> web_view_ = nullptr;
  raw_ptr<views::ImageButton> close_button_ = nullptr;
  raw_ptr<views::ImageButton> open_button_ = nullptr;
};

BEGIN_METADATA(PeekView)
END_METADATA

DEFINE_USER_DATA(Peek);

Peek::Peek(BrowserWindowInterface* browser)
    : browser_(browser),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {}

Peek::~Peek() {
  content_area_observation_.Reset();
  browser_widget_observation_.Reset();
  widget_observation_.Reset();
  active_tab_watcher_.reset();
  if (web_view_) {
    web_view_->SetWebContents(nullptr);
  }
  web_view_ = nullptr;
  // The window is going: nothing is handling an event of the page now.
  widget_.reset();
  widget_delegate_.reset();
  contents_.reset();
}

// static
Peek* Peek::From(BrowserWindowInterface* browser) {
  return Get(browser->GetUnownedUserDataHost());
}

void Peek::Open(content::WebContents* source,
                const content::OpenURLParams& params) {
  content::NavigationController::LoadURLParams load(params);
  load.transition_type = ui::PAGE_TRANSITION_LINK;
  // The page isn't the source's: it opens in its own WebContents.
  load.frame_tree_node_id = content::FrameTreeNodeId();
  load.should_replace_current_entry = false;

  if (closing_) {
    FinishClose();
  }
  // The second click of an Option+double-click.
  if (contents_ && contents_->GetVisibleURL() == params.url) {
    return;
  }
  if (!contents_) {
    content::WebContents::CreateParams create(browser_->GetProfile());
    contents_ = content::WebContents::Create(create);
    PreviewTab::AttachTabHelpers(contents_.get());
    contents_->SetDelegate(this);
  }
  contents_->GetController().LoadURLWithParams(load);

  source_tab_ = tabs::TabInterface::MaybeGetFromContents(source);
  if (!widget_) {
    Show();
  } else if (peek_view_) {
    peek_view_->UpdateTitle(contents_.get());
  }
}

std::u16string Peek::GetTitleForTesting() const {
  return peek_view_ ? peek_view_->title_for_testing() : std::u16string();
}

std::u16string Peek::GetHostForTesting() const {
  return peek_view_ ? peek_view_->host_for_testing() : std::u16string();
}

void Peek::Adopt(content::WebContents* source,
                 std::unique_ptr<content::WebContents> contents) {
  if (closing_) {
    FinishClose();
  }
  if (contents_) {
    if (web_view_) {
      web_view_->SetWebContents(nullptr);
    }
    contents_->SetDelegate(nullptr);
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(contents_));
  }
  contents_ = std::move(contents);
  // Safe on a WebContents that has them already.
  PreviewTab::AttachTabHelpers(contents_.get());
  contents_->SetDelegate(this);

  source_tab_ = tabs::TabInterface::MaybeGetFromContents(source);
  if (!widget_) {
    Show();
    return;
  }
  web_view_->SetWebContents(contents_.get());
  peek_view_->UpdateTitle(contents_.get());
  web_view_->RequestFocus();
}

void Peek::Show() {
  BrowserView* browser_view = GetBrowserView(browser_);
  if (!browser_view || !browser_view->GetWidget()) {
    return;
  }

  auto view = std::make_unique<PeekView>(
      base::BindRepeating(&Peek::PostAction, weak_factory_.GetWeakPtr(),
                          &Peek::Close),
      base::BindRepeating(&Peek::PostAction, weak_factory_.GetWeakPtr(),
                          &Peek::OpenAsEntry));
  widget_delegate_ = std::make_unique<views::WidgetDelegate>();
  PeekView* peek_view = widget_delegate_->SetContentsView(std::move(view));

  views::Widget::InitParams params(
      views::Widget::InitParams::CLIENT_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_WINDOW_FRAMELESS);
  params.delegate = widget_delegate_.get();
  params.parent = browser_view->GetWidget()->GetNativeView();
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
  params.shadow_type = views::Widget::InitParams::ShadowType::kNone;
  params.activatable = views::Widget::InitParams::Activatable::kYes;
  params.name = "CompassPeek";
  params.bounds = browser_view->contents_container()->GetBoundsInScreen();
  widget_ = std::make_unique<views::Widget>(std::move(params));
  widget_observation_.Observe(widget_.get());

  peek_view_ = peek_view;
  web_view_ = peek_view->web_view();
  web_view_->SetWebContents(contents_.get());
  peek_view->UpdateTitle(contents_.get());

  content_area_observation_.Observe(browser_view->contents_container());
  browser_widget_observation_.Observe(browser_view->GetWidget());
  active_tab_watcher_ = std::make_unique<ActiveTabWatcher>(
      browser_, base::BindRepeating(&Peek::OnActiveTabChanged,
                                    base::Unretained(this)));

  // Transparent from the first frame.
  peek_view->StartOpening();
  widget_->Show();
  web_view_->RequestFocus();
}

void Peek::Close() {
  if (closing_ || (!widget_ && !contents_)) {
    return;
  }
  if (!widget_ || widget_->IsClosed() || !peek_view_) {
    FinishClose();
    return;
  }
  // The page stays in the card while it fades out; nothing else closes or
  // moves it meanwhile.
  closing_ = true;
  StopFollowing();
  FocusActiveTab();
  peek_view_->StartClosing(
      base::BindOnce(&Peek::FinishClose, weak_factory_.GetWeakPtr()));
}

void Peek::FinishClose() {
  closing_ = false;
  if (!widget_ && !contents_) {
    return;
  }
  std::unique_ptr<content::WebContents> contents = std::move(contents_);
  Teardown();
  if (contents) {
    contents->SetDelegate(nullptr);
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(contents));
  }
}

void Peek::OpenAsEntry() {
  if (!contents_ || closing_) {
    return;
  }
  std::unique_ptr<content::WebContents> contents = std::move(contents_);
  Teardown();
  contents->SetDelegate(nullptr);
  // The strip's bridge makes it a Today entry, as for any new tab.
  browser_->GetTabStripModel()->AddWebContents(
      std::move(contents), TabStripModel::kNoTab, ui::PAGE_TRANSITION_LINK,
      AddTabTypes::ADD_ACTIVE);
}

void Peek::PostAction(void (Peek::*action)()) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(action, weak_factory_.GetWeakPtr()));
}

void Peek::UpdateBounds() {
  BrowserView* browser_view = GetBrowserView(browser_);
  if (!widget_ || !browser_view) {
    return;
  }
  widget_->SetBounds(browser_view->contents_container()->GetBoundsInScreen());
}

void Peek::StopFollowing() {
  content_area_observation_.Reset();
  browser_widget_observation_.Reset();
  active_tab_watcher_.reset();
  source_tab_ = nullptr;
}

void Peek::FocusActiveTab() {
  if (BrowserView* browser_view = GetBrowserView(browser_)) {
    if (content::WebContents* active = browser_view->GetActiveWebContents()) {
      active->Focus();
    }
  }
}

void Peek::Teardown() {
  StopFollowing();
  widget_observation_.Reset();
  peek_view_ = nullptr;
  if (web_view_) {
    web_view_->SetWebContents(nullptr);
    web_view_ = nullptr;
  }
  if (widget_) {
    if (!widget_->IsClosed()) {
      widget_->Hide();
    }
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(widget_));
    // After the widget (tasks run in order).
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(widget_delegate_));
  }
  // Back to the page underneath.
  FocusActiveTab();
}

void Peek::OnActiveTabChanged(const tabs::TabInterface* tab) {
  // Also called with the current tab when the watcher starts.
  if (tab != source_tab_) {
    Close();
  }
}

content::WebContents* Peek::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)>
        navigation_handle_callback) {
  // Plain clicks, and Option+clicks, stay in the card.
  if (params.disposition == WindowOpenDisposition::CURRENT_TAB ||
      params.disposition == WindowOpenDisposition::SAVE_TO_DISK) {
    content::NavigationController::LoadURLParams load(params);
    if (params.disposition == WindowOpenDisposition::SAVE_TO_DISK) {
      load.frame_tree_node_id = content::FrameTreeNodeId();
    }
    base::WeakPtr<content::NavigationHandle> handle =
        source->GetController().LoadURLWithParams(load);
    if (handle && navigation_handle_callback) {
      std::move(navigation_handle_callback).Run(*handle);
    }
    return source;
  }
  // New tabs and windows open in the window, as from the tab underneath.
  NavigateParams navigate(browser_, params.url, params.transition);
  navigate.FillNavigateParamsFromOpenURLParams(params);
  navigate.source_contents = nullptr;
  base::WeakPtr<content::NavigationHandle> handle = Navigate(&navigate);
  if (handle && navigation_handle_callback) {
    std::move(navigation_handle_callback).Run(*handle);
  }
  return navigate.navigated_or_inserted_contents;
}

content::WebContents* Peek::AddNewContents(
    content::WebContents* source,
    std::unique_ptr<content::WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& window_features,
    bool user_gesture,
    bool* was_blocked) {
  // Pop-ups and window.open() open in the window.
  return chrome::AddWebContents(browser_, nullptr, std::move(new_contents),
                                target_url, disposition, window_features);
}

void Peek::CloseContents(content::WebContents* source) {
  PostAction(&Peek::Close);
}

content::KeyboardEventProcessingResult Peek::PreHandleKeyboardEvent(
    content::WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  if (IsKeyDown(event, ui::VKEY_W, kCommandKey)) {
    PostAction(&Peek::Close);
    return content::KeyboardEventProcessingResult::HANDLED;
  }
  if (IsKeyDown(event, ui::VKEY_O, kCommandKey)) {
    PostAction(&Peek::OpenAsEntry);
    return content::KeyboardEventProcessingResult::HANDLED;
  }
  // The window's own shortcuts (Cmd+T, Cmd+L, ...) as from the tab.
  BrowserView* browser_view = GetBrowserView(browser_);
  return browser_view ? browser_view->PreHandleKeyboardEvent(event)
                      : content::KeyboardEventProcessingResult::NOT_HANDLED;
}

bool Peek::HandleKeyboardEvent(content::WebContents* source,
                               const input::NativeWebKeyboardEvent& event) {
  // After the page, which may use Esc itself.
  if (IsKeyDown(event, ui::VKEY_ESCAPE, 0)) {
    PostAction(&Peek::Close);
    return true;
  }
  BrowserView* browser_view = GetBrowserView(browser_);
  return browser_view && browser_view->HandleKeyboardEvent(event);
}

void Peek::RunFileChooser(content::RenderFrameHost* render_frame_host,
                          scoped_refptr<content::FileSelectListener> listener,
                          const blink::mojom::FileChooserParams& params) {
  FileSelectHelper::RunFileChooser(render_frame_host, std::move(listener),
                                   params);
}

void Peek::EnumerateDirectory(
    content::WebContents* web_contents,
    scoped_refptr<content::FileSelectListener> listener,
    const base::FilePath& path) {
  FileSelectHelper::EnumerateDirectory(web_contents, std::move(listener),
                                       path);
}

void Peek::RequestMediaAccessPermission(
    content::WebContents* web_contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback) {
  MediaCaptureDevicesDispatcher::GetInstance()->ProcessMediaAccessRequest(
      web_contents, request, std::move(callback), nullptr);
}

bool Peek::CheckMediaAccessPermission(
    content::RenderFrameHost* render_frame_host,
    const url::Origin& security_origin,
    blink::mojom::MediaStreamType type) {
  return MediaCaptureDevicesDispatcher::GetInstance()
      ->CheckMediaAccessPermission(render_frame_host, security_origin, type);
}

void Peek::OnWidgetActivationChanged(views::Widget* widget, bool active) {
  // A click on the rest of the window (the sidebar, the frame) closes Peek.
  // Other apps and Peek's own bubbles don't activate the window.
  if (active && widget != widget_.get()) {
    PostAction(&Peek::Close);
  }
}

void Peek::OnWidgetDestroying(views::Widget* widget) {
  // Peek's widget closed by the system, or the window's going away: no time
  // for an animation.
  FinishClose();
}

void Peek::OnViewBoundsChanged(views::View* observed_view) {
  UpdateBounds();
}

void Peek::OnViewIsDeleting(views::View* observed_view) {
  content_area_observation_.Reset();
  FinishClose();
}

void Peek::NavigationStateChanged(content::WebContents* source,
                                  content::InvalidateTypes changed_flags) {
  if (peek_view_ && source == contents_.get() &&
      (changed_flags &
       (content::INVALIDATE_TYPE_URL | content::INVALIDATE_TYPE_TITLE))) {
    peek_view_->UpdateTitle(source);
  }
}

void PeekOrDownload(BrowserWindowInterface* browser,
                    content::WebContents* source,
                    const content::OpenURLParams& params) {
  Peek* peek = browser->GetType() == BrowserWindowInterface::TYPE_NORMAL
                   ? Peek::From(browser)
                   : nullptr;
  if (peek && params.url.SchemeIsHTTPOrHTTPS() && !params.post_data) {
    peek->Open(source, params);
    return;
  }
  DownloadLink(source, params);
}

content::WebContents* MaybePeekNewContents(
    BrowserWindowInterface* browser,
    content::WebContents* source,
    std::unique_ptr<content::WebContents>& new_contents,
    WindowOpenDisposition disposition,
    bool user_gesture) {
  if (!source || !user_gesture ||
      disposition != WindowOpenDisposition::NEW_FOREGROUND_TAB ||
      browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
    return nullptr;
  }
  Peek* peek = Peek::From(browser);
  WindowFeatures* features = WindowFeatures::From(browser);
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(source);
  if (!peek || !features || !tab || features->is_parking_window() ||
      features->is_switching()) {
    return nullptr;
  }
  TabStripBridge& bridge = features->tab_strip_bridge();
  const SidebarNode* node = bridge.GetNode(tab);
  if (!node || !bridge.IsPinned(node)) {
    return nullptr;
  }
  content::WebContents* contents = new_contents.get();
  peek->Adopt(source, std::move(new_contents));
  return contents;
}

bool HandlePeekCommand(BrowserWindowInterface* browser, int id) {
  Peek* peek = Peek::From(browser);
  if (!peek || !peek->IsShowing()) {
    return false;
  }
  switch (id) {
    case IDC_CLOSE_TAB:
      peek->Close();
      return true;
    case IDC_OPEN_FILE:
      peek->OpenAsEntry();
      return true;
    default:
      return false;
  }
}

}  // namespace compass
