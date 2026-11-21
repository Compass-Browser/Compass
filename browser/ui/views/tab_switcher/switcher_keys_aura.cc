// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/tab_switcher/switcher_keys.h"

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "ui/aura/env.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace compass {

namespace {

// aura::Env's handlers see every window's events first, the page's too.
// Notes whether Ctrl is held.
class KeyTracker : public ui::EventHandler {
 public:
  static KeyTracker& Get() {
    static base::NoDestructor<KeyTracker> tracker;
    return *tracker;
  }

  void Start() {
    if (!started_ && aura::Env::HasInstance()) {
      aura::Env::GetInstance()->AddPreTargetHandler(this);
      started_ = true;
    }
  }

  bool control_down() const { return control_down_; }

  // ui::EventHandler:
  void OnKeyEvent(ui::KeyEvent* event) override {
    control_down_ = event->IsControlDown();
    if (event->key_code() == ui::VKEY_CONTROL) {
      control_down_ = event->type() == ui::EventType::kKeyPressed;
    }
  }

 private:
  bool started_ = false;
  bool control_down_ = false;
};

class SwitcherKeysAura : public SwitcherKeys, public ui::EventHandler {
 public:
  explicit SwitcherKeysAura(Delegate* delegate) : delegate_(delegate) {
    aura::Env::GetInstance()->AddPreTargetHandler(this);
  }

  ~SwitcherKeysAura() override {
    aura::Env::GetInstance()->RemovePreTargetHandler(this);
  }

  // ui::EventHandler:
  void OnKeyEvent(ui::KeyEvent* event) override {
    if (event->type() == ui::EventType::kKeyReleased) {
      if (event->key_code() == ui::VKEY_CONTROL || !event->IsControlDown()) {
        delegate_->OnControlReleased();
      }
      return;
    }
    switch (event->key_code()) {
      case ui::VKEY_ESCAPE:
        event->StopPropagation();
        delegate_->OnCancel();
        return;
      case ui::VKEY_RETURN:
      case ui::VKEY_SPACE:
        event->StopPropagation();
        delegate_->OnCommit();
        return;
      case ui::VKEY_LEFT:
        event->StopPropagation();
        delegate_->OnMove(-1);
        return;
      case ui::VKEY_RIGHT:
        event->StopPropagation();
        delegate_->OnMove(1);
        return;
      default:
        // Ctrl+Tab again goes on to its command, which moves the switcher.
        return;
    }
  }

 private:
  const raw_ptr<Delegate> delegate_;
};

}  // namespace

// static
std::unique_ptr<SwitcherKeys> SwitcherKeys::Watch(Delegate* delegate) {
  return std::make_unique<SwitcherKeysAura>(delegate);
}

// static
void SwitcherKeys::Track() {
  KeyTracker::Get().Start();
}

// static
bool SwitcherKeys::IsControlDown() {
  return KeyTracker::Get().control_down();
}

}  // namespace compass
