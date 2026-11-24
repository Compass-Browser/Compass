// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/on_device_model.h"

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/claude/claude_quick.h"
#include "components/prefs/testing_pref_service.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

// The helper's protocol, both ways (CompassKit's OnDeviceLines).
TEST(OnDeviceModelTest, ParsesReplies) {
  EXPECT_EQ(ParseOnDeviceReply(R"({"id":1,"text":"{\"name\":\"Viaggi\"}"})"),
            R"({"name":"Viaggi"})");
  EXPECT_EQ(ParseOnDeviceReply(R"({"id":1,"error":"unavailable"})"),
            base::unexpected(OnDeviceError::kUnavailable));
  EXPECT_EQ(ParseOnDeviceReply(R"({"id":1,"error":"refused"})"),
            base::unexpected(OnDeviceError::kRefused));
  EXPECT_EQ(ParseOnDeviceReply(R"({"id":1,"error":"bad_request"})"),
            base::unexpected(OnDeviceError::kFailed));
  EXPECT_EQ(ParseOnDeviceReply("not json"),
            base::unexpected(OnDeviceError::kFailed));

  EXPECT_EQ(ParseOnDeviceStatus(R"({"id":1,"availability":"available"})"),
            OnDeviceAvailability::kAvailable);
  EXPECT_EQ(ParseOnDeviceStatus(R"({"id":1,"availability":"disabled"})"),
            OnDeviceAvailability::kDisabled);
  EXPECT_EQ(ParseOnDeviceStatus(R"({"id":1,"availability":"not_ready"})"),
            OnDeviceAvailability::kNotReady);
  EXPECT_EQ(ParseOnDeviceStatus(R"({"id":1,"availability":"unsupported"})"),
            OnDeviceAvailability::kUnsupported);
  EXPECT_EQ(ParseOnDeviceStatus("{}"), OnDeviceAvailability::kUnsupported);

  EXPECT_TRUE(
      DescribeOnDeviceAvailability(OnDeviceAvailability::kAvailable).empty());
  EXPECT_FALSE(
      DescribeOnDeviceAvailability(OnDeviceAvailability::kDisabled).empty());
}

// Fake helpers: shell scripts that answer as compass-on-device would.
class OnDeviceHelperTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }

  // A helper running |body| (sh), which reads the request with `read line`.
  base::FilePath Helper(std::string_view body) {
    const base::FilePath path =
        dir_.GetPath().AppendASCII("helper" + base::NumberToString(next_++));
    EXPECT_TRUE(base::WriteFile(path, "#!/bin/sh\n" + std::string(body)));
    EXPECT_TRUE(base::SetPosixFilePermissions(path, 0700));
    return path;
  }

  // Where a helper may save what it was sent.
  base::FilePath Saved() const {
    return dir_.GetPath().AppendASCII("request");
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
  int next_ = 0;
};

TEST_F(OnDeviceHelperTest, RunsTheHelper) {
  // The request arrives as one line, and the first line back is the reply.
  EXPECT_EQ(RunOnDeviceHelper(Helper("read line\n"
                                     "printf '%s\\n' \"$line\"\n"
                                     "echo second\n"),
                              R"({"id":1})", base::Seconds(10)),
            R"({"id":1})");
  // No line back.
  EXPECT_EQ(RunOnDeviceHelper(Helper("read line\nexit 1\n"), "{}",
                              base::Seconds(10)),
            std::nullopt);
  // Not there.
  EXPECT_EQ(RunOnDeviceHelper(dir_.GetPath().AppendASCII("missing"), "{}",
                              base::Seconds(10)),
            std::nullopt);
  // Too slow: stopped at the timeout, not when it would end.
  const base::TimeTicks start = base::TimeTicks::Now();
  EXPECT_EQ(RunOnDeviceHelper(Helper("read line\nsleep 30\necho late\n"),
                              "{}", base::Milliseconds(300)),
            std::nullopt);
  EXPECT_LT(base::TimeTicks::Now() - start, base::Seconds(10));
}

TEST_F(OnDeviceHelperTest, AsksAndChecks) {
  OnDeviceModel model(Helper(
      "read line\n"
      "printf '%s' \"$line\" > \"" + Saved().value() + "\"\n"
      "case \"$line\" in\n"
      "  *'\"status\"'*) echo '{\"id\":1,\"availability\":\"disabled\"}' ;;\n"
      "  *) echo '{\"id\":2,\"text\":\"{\\\"name\\\":\\\"Rust\\\"}\"}' ;;\n"
      "esac\n"));
  EXPECT_EQ(model.availability(), OnDeviceAvailability::kUnknown);

  base::test::TestFuture<OnDeviceAvailability> status;
  model.CheckAvailability(status.GetCallback());
  EXPECT_EQ(status.Get(), OnDeviceAvailability::kDisabled);
  EXPECT_EQ(model.availability(), OnDeviceAvailability::kDisabled);

  base::test::TestFuture<base::expected<std::string, OnDeviceError>> reply;
  model.Ask("folder_name", "Name it.", "- Tokio (tokio.rs)", base::Seconds(10),
            reply.GetCallback());
  EXPECT_EQ(reply.Get(), R"({"name":"Rust"})");
  EXPECT_EQ(model.availability(), OnDeviceAvailability::kAvailable);
  std::string sent;
  ASSERT_TRUE(base::ReadFileToString(Saved(), &sent));
  std::optional<base::DictValue> request =
      base::JSONReader::ReadDict(sent, base::JSON_PARSE_RFC);
  ASSERT_TRUE(request);
  EXPECT_EQ(*request->FindString("feature"), "folder_name");
  EXPECT_EQ(*request->FindString("system"), "Name it.");
  EXPECT_EQ(*request->FindString("user"), "- Tokio (tokio.rs)");
}

TEST_F(OnDeviceHelperTest, TimesOut) {
  OnDeviceModel model(Helper("read line\nsleep 30\n"));
  base::test::TestFuture<base::expected<std::string, OnDeviceError>> reply;
  model.Ask("folder_name", "s", "u", base::Milliseconds(300),
            reply.GetCallback());
  EXPECT_EQ(reply.Get(), base::unexpected(OnDeviceError::kTimeout));
}

// ClaudeQuick on the on-device model: no key, the on-device prompt, and the
// reply parsed as Claude's.
class ClaudeQuickOnDeviceTest : public OnDeviceHelperTest {
 protected:
  void SetUp() override {
    OnDeviceHelperTest::SetUp();
    prefs::RegisterProfilePrefs(prefs_.registry());
    prefs_.SetBoolean(prefs::kNameLinkFolders, true);
    prefs_.SetBoolean(prefs::kSidebarOnDevice, true);
  }

  void MakeQuick(std::string_view helper_body) {
    model_ = std::make_unique<OnDeviceModel>(Helper(helper_body));
    quick_ = std::make_unique<ClaudeQuick>(
        &prefs_,
        base::BindRepeating(
            []() -> scoped_refptr<network::SharedURLLoaderFactory> {
              ADD_FAILURE() << "Nothing goes to the network.";
              return nullptr;
            }),
        base::BindRepeating([](ClaudeQuick::KeyCallback callback) {
          std::move(callback).Run(std::nullopt);
        }),
        model_.get());
  }

  ClaudeQuick::Result Ask(std::string_view feature_pref = "") {
    QuickRequest request;
    request.feature_pref =
        feature_pref.empty() ? prefs::kNameLinkFolders : feature_pref;
    request.system = "Claude's prompt.";
    request.user = "- Tokio (tokio.rs)";
    request.on_device_feature = "folder_name";
    request.on_device_system = "The small model's prompt.";
    base::test::TestFuture<ClaudeQuick::Result> future;
    quick_->Ask(std::move(request), future.GetCallback());
    return future.Take();
  }

  TestingPrefServiceSimple prefs_;
  std::unique_ptr<OnDeviceModel> model_;
  std::unique_ptr<ClaudeQuick> quick_;
};

TEST_F(ClaudeQuickOnDeviceTest, AnswersWithoutAKey) {
  MakeQuick("read line\n"
            "printf '%s' \"$line\" > \"" + Saved().value() + "\"\n"
            "echo '{\"id\":1,\"text\":\"{\\\"name\\\":\\\"Rust\\\"}\"}'\n");
  EXPECT_FALSE(quick_->IsSetUp());
  EXPECT_TRUE(quick_->UsesOnDevice(prefs::kNameLinkFolders));
  EXPECT_TRUE(quick_->IsEnabled(prefs::kNameLinkFolders));
  // Ask in the command bar stays with the chat's provider.
  prefs_.SetBoolean(prefs::kCommandBarAsk, true);
  EXPECT_FALSE(quick_->UsesOnDevice(prefs::kCommandBarAsk));
  EXPECT_FALSE(quick_->IsEnabled(prefs::kCommandBarAsk));

  ClaudeQuick::Result result = Ask();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result->FindString("name"), "Rust");
  std::string sent;
  ASSERT_TRUE(base::ReadFileToString(Saved(), &sent));
  std::optional<base::DictValue> request =
      base::JSONReader::ReadDict(sent, base::JSON_PARSE_RFC);
  ASSERT_TRUE(request);
  EXPECT_EQ(*request->FindString("system"), "The small model's prompt.");
  EXPECT_EQ(*request->FindString("feature"), "folder_name");
}

TEST_F(ClaudeQuickOnDeviceTest, Errors) {
  MakeQuick("read line\necho '{\"id\":1,\"error\":\"unavailable\"}'\n");
  EXPECT_EQ(Ask(), base::unexpected(QuickError::kNotSetUp));

  MakeQuick("read line\necho '{\"id\":1,\"error\":\"refused\"}'\n");
  EXPECT_EQ(Ask(), base::unexpected(QuickError::kRefused));

  MakeQuick("read line\necho '{\"id\":1,\"text\":\"not json\"}'\n");
  EXPECT_EQ(Ask(), base::unexpected(QuickError::kBadReply));

  // The switch off: nothing asked.
  MakeQuick("read line\ntouch \"" + Saved().value() + "\"\n");
  prefs_.SetBoolean(prefs::kNameLinkFolders, false);
  EXPECT_EQ(Ask(), base::unexpected(QuickError::kOff));
  EXPECT_FALSE(base::PathExists(Saved()));

  // "Answer with" the chat's provider, which has no key.
  prefs_.SetBoolean(prefs::kNameLinkFolders, true);
  prefs_.SetBoolean(prefs::kSidebarOnDevice, false);
  EXPECT_FALSE(quick_->IsEnabled(prefs::kNameLinkFolders));
  EXPECT_EQ(Ask(), base::unexpected(QuickError::kNotSetUp));
  EXPECT_FALSE(base::PathExists(Saved()));
}

TEST_F(ClaudeQuickOnDeviceTest, UnavailableTurnsTheSwitchesOff) {
  MakeQuick("read line\necho '{\"id\":1,\"availability\":\"not_ready\"}'\n");
  base::test::TestFuture<OnDeviceAvailability> status;
  model_->CheckAvailability(status.GetCallback());
  EXPECT_EQ(status.Get(), OnDeviceAvailability::kNotReady);
  EXPECT_FALSE(quick_->IsEnabled(prefs::kNameLinkFolders));
  EXPECT_EQ(Ask(), base::unexpected(QuickError::kNotSetUp));
}

}  // namespace
}  // namespace compass::claude
