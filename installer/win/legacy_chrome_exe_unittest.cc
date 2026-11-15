// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/installer/win/legacy_chrome_exe.h"

#include <windows.h>

#include <memory>
#include <string>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/strcat.h"
#include "base/test/test_reg_util_win.h"
#include "base/win/registry.h"
#include "base/win/scoped_com_initializer.h"
#include "base/win/shortcut.h"
#include "chrome/install_static/install_util.h"
#include "chrome/install_static/test/scoped_install_details.h"
#include "chrome/installer/util/shell_util.h"
#include "chrome/installer/util/util_constants.h"
#include "chrome/installer/util/work_item_list.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::installer {
namespace {

std::string Read(const base::FilePath& path) {
  std::string contents;
  base::ReadFileToString(path, &contents);
  return contents;
}

TEST(LegacyChromeExeTest, BrowserIsCompassExe) {
  EXPECT_STREQ(L"compass.exe", ::installer::kChromeExe);
  EXPECT_STREQ(L"new_compass.exe", ::installer::kChromeNewExe);
  EXPECT_STREQ(L"old_compass.exe", ::installer::kChromeOldExe);
}

class LegacyChromeExeWorkItemsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    target_ = dir_.GetPath().Append(L"Application");
    version_dir_ = target_.Append(L"1.2.3.4");
    temp_ = dir_.GetPath().Append(L"Temp");
    ASSERT_TRUE(base::CreateDirectory(version_dir_));
    ASSERT_TRUE(base::CreateDirectory(temp_));
    ASSERT_TRUE(base::WriteFile(version_dir_.Append(kLegacyChromeStub), "stub"));
    ASSERT_TRUE(base::WriteFile(target_.Append(L"compass.exe"), "browser"));
  }

  std::unique_ptr<WorkItemList> Items() {
    std::unique_ptr<WorkItemList> list(WorkItem::CreateWorkItemList());
    AddLegacyChromeExeWorkItems(target_, version_dir_, temp_, list.get());
    return list;
  }

  base::ScopedTempDir dir_;
  base::FilePath target_;
  base::FilePath version_dir_;
  base::FilePath temp_;
};

TEST_F(LegacyChromeExeWorkItemsTest, ReplacesChromeExeWithTheStub) {
  ASSERT_TRUE(base::WriteFile(target_.Append(L"chrome.exe"), "old browser"));
  ASSERT_TRUE(base::WriteFile(target_.Append(L"new_chrome.exe"), "pending"));
  ASSERT_TRUE(base::WriteFile(target_.Append(L"old_chrome.exe"), "older"));
  ASSERT_TRUE(base::WriteFile(
      target_.Append(L"chrome.VisualElementsManifest.xml"), "<xml/>"));

  std::unique_ptr<WorkItemList> list = Items();
  ASSERT_TRUE(list->Do());

  EXPECT_EQ("stub", Read(target_.Append(L"chrome.exe")));
  EXPECT_EQ("browser", Read(target_.Append(L"compass.exe")));
  EXPECT_FALSE(base::PathExists(target_.Append(L"new_chrome.exe")));
  EXPECT_FALSE(base::PathExists(target_.Append(L"old_chrome.exe")));
  EXPECT_FALSE(
      base::PathExists(target_.Append(L"chrome.VisualElementsManifest.xml")));

  // A failed install puts the old one back.
  list->Rollback();
  EXPECT_EQ("old browser", Read(target_.Append(L"chrome.exe")));
  EXPECT_EQ("pending", Read(target_.Append(L"new_chrome.exe")));
}

TEST_F(LegacyChromeExeWorkItemsTest, ReplacesARunningChromeExe) {
  // A running program can be renamed, not written: as a mapped image is.
  const base::FilePath chrome_exe = target_.Append(L"chrome.exe");
  ASSERT_TRUE(base::WriteFile(chrome_exe, "old browser"));
  HANDLE running = ::CreateFileW(chrome_exe.value().c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(INVALID_HANDLE_VALUE, running);

  EXPECT_TRUE(Items()->Do());
  EXPECT_EQ("stub", Read(chrome_exe));
  ::CloseHandle(running);
}

TEST_F(LegacyChromeExeWorkItemsTest, NewInstallsGetNoChromeExe) {
  ASSERT_TRUE(Items()->Do());
  EXPECT_FALSE(base::PathExists(target_.Append(L"chrome.exe")));
}

class LegacyShortcutsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    app_ = dir_.GetPath().Append(L"Application");
    ASSERT_TRUE(base::CreateDirectory(app_));
    legacy_ = app_.Append(L"chrome.exe");
    compass_ = app_.Append(L"compass.exe");
    ASSERT_TRUE(base::WriteFile(legacy_, "stub"));
    ASSERT_TRUE(base::WriteFile(compass_, "browser"));
    shortcuts_ = dir_.GetPath().Append(L"Shortcuts");
    ASSERT_TRUE(base::CreateDirectory(shortcuts_.Append(L"Sub")));
  }

  void Create(const base::FilePath& path,
              const base::FilePath& target,
              const std::wstring& arguments,
              const base::FilePath& icon) {
    base::win::ShortcutProperties properties;
    properties.set_target(target);
    properties.set_arguments(arguments);
    properties.set_icon(icon, 0);
    properties.set_app_id(L"Compass.Test");
    ASSERT_TRUE(base::win::CreateOrUpdateShortcutLink(
        path, properties, base::win::ShortcutOperation::kCreateAlways));
  }

  base::win::ShortcutProperties Resolve(const base::FilePath& path) {
    base::win::ShortcutProperties properties;
    EXPECT_TRUE(base::win::ResolveShortcutProperties(
        path, base::win::ShortcutProperties::PROPERTIES_ALL, &properties));
    return properties;
  }

  base::win::ScopedCOMInitializer com_;
  base::ScopedTempDir dir_;
  base::FilePath app_;
  base::FilePath legacy_;
  base::FilePath compass_;
  base::FilePath shortcuts_;
};

TEST_F(LegacyShortcutsTest, RetargetsShortcutsToChromeExe) {
  const base::FilePath pin = shortcuts_.Append(L"Compass.lnk");
  const base::FilePath profile = shortcuts_.Append(L"Work - Compass.lnk");
  const base::FilePath other = shortcuts_.Append(L"Other.lnk");
  const base::FilePath nested = shortcuts_.Append(L"Sub").Append(L"C.lnk");
  const base::FilePath other_exe = dir_.GetPath().Append(L"other.exe");
  ASSERT_TRUE(base::WriteFile(other_exe, "other"));
  Create(pin, legacy_, L"", legacy_);
  Create(profile, legacy_, L"--profile-directory=\"Profile 1\"",
         app_.Append(L"1.2.3.4").Append(L"profile.ico"));
  Create(other, other_exe, L"", other_exe);
  Create(nested, legacy_, L"", legacy_);

  EXPECT_EQ(2, RetargetShortcutsIn(shortcuts_, /*recursive=*/false, legacy_,
                                   compass_));

  base::win::ShortcutProperties pinned = Resolve(pin);
  EXPECT_TRUE(base::FilePath::CompareEqualIgnoreCase(compass_.value(),
                                                     pinned.target.value()));
  EXPECT_TRUE(base::FilePath::CompareEqualIgnoreCase(compass_.value(),
                                                     pinned.icon.value()));
  // What makes it the taskbar's pin stays.
  EXPECT_EQ(L"Compass.Test", pinned.app_id);

  base::win::ShortcutProperties profile_shortcut = Resolve(profile);
  EXPECT_TRUE(base::FilePath::CompareEqualIgnoreCase(
      compass_.value(), profile_shortcut.target.value()));
  EXPECT_EQ(L"--profile-directory=\"Profile 1\"", profile_shortcut.arguments);
  EXPECT_EQ(L"profile.ico", profile_shortcut.icon.BaseName().value());

  EXPECT_TRUE(base::FilePath::CompareEqualIgnoreCase(
      other_exe.value(), Resolve(other).target.value()));
  EXPECT_TRUE(base::FilePath::CompareEqualIgnoreCase(
      legacy_.value(), Resolve(nested).target.value()));

  // Subfolders with |recursive|, and nothing twice.
  EXPECT_EQ(1, RetargetShortcutsIn(shortcuts_, /*recursive=*/true, legacy_,
                                   compass_));
  EXPECT_TRUE(base::FilePath::CompareEqualIgnoreCase(
      compass_.value(), Resolve(nested).target.value()));
}

class LegacyRegistrationTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(
        registry_override_.OverrideRegistry(HKEY_CURRENT_USER));
    ASSERT_NO_FATAL_FAILURE(
        registry_override_.OverrideRegistry(HKEY_LOCAL_MACHINE));
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    app_ = dir_.GetPath().Append(L"Application");
    ASSERT_TRUE(base::CreateDirectory(app_));
    legacy_ = app_.Append(L"chrome.exe");
    compass_ = app_.Append(L"compass.exe");
    ASSERT_TRUE(base::WriteFile(legacy_, "stub"));
    ASSERT_TRUE(base::WriteFile(compass_, "browser"));
  }

  static std::wstring OpenCommandKey(const std::wstring& suffix) {
    return base::StrCat({ShellUtil::kRegStartMenuInternet, L"\\",
                         install_static::GetBaseAppName(), suffix,
                         ShellUtil::kRegShellOpen});
  }

  static std::wstring ProgIdCommandKey(const std::wstring& suffix) {
    return base::StrCat({ShellUtil::kRegClasses, L"\\",
                         install_static::GetBrowserProgIdPrefix(), suffix,
                         ShellUtil::kRegShellOpen});
  }

  static void Write(const std::wstring& key, const std::wstring& value) {
    ASSERT_EQ(ERROR_SUCCESS,
              base::win::RegKey(HKEY_CURRENT_USER, key.c_str(), KEY_SET_VALUE)
                  .WriteValue(L"", value.c_str()));
  }

  static std::wstring ReadValue(const std::wstring& key) {
    std::wstring value;
    base::win::RegKey(HKEY_CURRENT_USER, key.c_str(), KEY_QUERY_VALUE)
        .ReadValue(L"", &value);
    return value;
  }

  // Compass registered as before the rename, with |suffix|.
  void RegisterLegacy(const std::wstring& suffix) {
    const std::wstring command =
        base::CommandLine(legacy_).GetCommandLineString();
    Write(OpenCommandKey(suffix), command);
    Write(ProgIdCommandKey(suffix), command + L" --single-argument %1");
  }

  install_static::ScopedInstallDetails install_details_;
  registry_util::RegistryOverrideManager registry_override_;
  base::ScopedTempDir dir_;
  base::FilePath app_;
  base::FilePath legacy_;
  base::FilePath compass_;
};

TEST_F(LegacyRegistrationTest, FindsTheSuffix) {
  std::wstring suffix = L"unset";
  EXPECT_FALSE(FindLegacyRegistrationSuffix(legacy_, &suffix));

  std::wstring user_suffix;
  ASSERT_TRUE(ShellUtil::GetUserSpecificRegistrySuffix(&user_suffix));
  RegisterLegacy(user_suffix);
  EXPECT_TRUE(FindLegacyRegistrationSuffix(legacy_, &suffix));
  EXPECT_EQ(user_suffix, suffix);
}

TEST_F(LegacyRegistrationTest, KeepsAnUnsuffixedRegistration) {
  RegisterLegacy(std::wstring());
  EXPECT_TRUE(MigrateLegacyRegistrations(app_));

  // Same names, now opening compass.exe.
  const std::wstring compass_command =
      base::CommandLine(compass_).GetCommandLineString();
  EXPECT_EQ(compass_command, ReadValue(OpenCommandKey(std::wstring())));
  EXPECT_NE(std::wstring::npos,
            ReadValue(ProgIdCommandKey(std::wstring())).find(compass_command));
  EXPECT_EQ(std::wstring(), ShellUtil::GetCurrentInstallationSuffix(compass_));

  // Nothing left to move.
  std::wstring suffix;
  EXPECT_FALSE(FindLegacyRegistrationSuffix(legacy_, &suffix));
  EXPECT_FALSE(MigrateLegacyRegistrations(app_));
}

TEST_F(LegacyRegistrationTest, KeepsAUserSuffix) {
  std::wstring user_suffix;
  ASSERT_TRUE(ShellUtil::GetUserSpecificRegistrySuffix(&user_suffix));
  RegisterLegacy(user_suffix);
  EXPECT_TRUE(MigrateLegacyRegistrations(app_));
  EXPECT_NE(std::wstring::npos,
            ReadValue(ProgIdCommandKey(user_suffix))
                .find(base::CommandLine(compass_).GetCommandLineString()));
  EXPECT_EQ(user_suffix, ShellUtil::GetCurrentInstallationSuffix(compass_));
}

TEST_F(LegacyRegistrationTest, DeletesOnlyItsOwnAppPath) {
  const std::wstring app_path =
      base::StrCat({ShellUtil::kAppPathsRegistryKey, L"\\chrome.exe"});
  Write(app_path, legacy_.value());
  MigrateLegacyRegistrations(app_);
  EXPECT_FALSE(base::win::RegKey(HKEY_CURRENT_USER, app_path.c_str(),
                                 KEY_QUERY_VALUE)
                   .Valid());

  // Chrome's.
  Write(app_path, L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");
  MigrateLegacyRegistrations(app_);
  EXPECT_FALSE(ReadValue(app_path).empty());
}

}  // namespace
}  // namespace compass::installer
