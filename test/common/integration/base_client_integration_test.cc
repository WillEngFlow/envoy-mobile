#include "test/common/integration/base_client_integration_test.h"

#include <string>

#include "test/common/http/common.h"

#include "gtest/gtest.h"
#include "library/cc/bridge_utility.h"
#include "library/common/config/internal.h"
#include "library/common/http/header_utility.h"

namespace Envoy {
namespace {

void validateStreamIntel(const envoy_final_stream_intel& final_intel, bool expect_dns,
                         bool upstream_tls, bool is_first_request) {
  if (expect_dns) {
    EXPECT_NE(-1, final_intel.dns_start_ms);
    EXPECT_NE(-1, final_intel.dns_end_ms);
  }

  if (upstream_tls) {
    EXPECT_GT(final_intel.ssl_start_ms, 0);
    EXPECT_GT(final_intel.ssl_end_ms, 0);
  } else {
    EXPECT_EQ(-1, final_intel.ssl_start_ms);
    EXPECT_EQ(-1, final_intel.ssl_end_ms);
  }

  ASSERT_NE(-1, final_intel.stream_start_ms);
  ASSERT_NE(-1, final_intel.connect_start_ms);
  ASSERT_NE(-1, final_intel.connect_end_ms);
  ASSERT_NE(-1, final_intel.sending_start_ms);
  ASSERT_NE(-1, final_intel.sending_end_ms);
  ASSERT_NE(-1, final_intel.response_start_ms);
  ASSERT_NE(-1, final_intel.stream_end_ms);

  if (is_first_request) {
    ASSERT_LE(final_intel.stream_start_ms, final_intel.connect_start_ms);
  }
  ASSERT_LE(final_intel.connect_start_ms, final_intel.connect_end_ms);
  ASSERT_LE(final_intel.connect_end_ms, final_intel.sending_start_ms);
  ASSERT_LE(final_intel.sending_start_ms, final_intel.sending_end_ms);
  ASSERT_LE(final_intel.response_start_ms, final_intel.stream_end_ms);
}

// Use the Envoy mobile default config as much as possible in this test.
// There are some config modifiers below which do result in deltas.
std::string defaultConfig() {
  Platform::EngineBuilder builder;
  std::string config_str = absl::StrCat(config_header, builder.generateConfigStr());
  return config_str;
}

} // namespace

BaseClientIntegrationTest::BaseClientIntegrationTest(Network::Address::IpVersion ip_version)
    : BaseIntegrationTest(ip_version, defaultConfig()) {
  skip_tag_extraction_rule_check_ = true;
  full_dispatcher_ = api_->allocateDispatcher("fake_envoy_mobile");
  use_lds_ = false;
  autonomous_upstream_ = true;
  defer_listener_finalization_ = true;
}

void BaseClientIntegrationTest::initialize() {
  BaseIntegrationTest::initialize();
  stream_prototype_ = engine_->streamClient()->newStreamPrototype();

  stream_prototype_->setOnHeaders(
      [this](Platform::ResponseHeadersSharedPtr headers, bool, envoy_stream_intel intel) {
        cc_.on_headers_calls++;
        cc_.status = absl::StrCat(headers->httpStatus());
        cc_.on_header_consumed_bytes_from_response = intel.consumed_bytes_from_response;
      });
  stream_prototype_->setOnData([this](envoy_data c_data, bool) {
    cc_.on_data_calls++;
    release_envoy_data(c_data);
  });
  stream_prototype_->setOnComplete(
      [this](envoy_stream_intel, envoy_final_stream_intel final_intel) {
        validateStreamIntel(final_intel, expect_dns_, upstream_tls_, cc_.on_complete_calls == 0);
        cc_.on_complete_received_byte_count = final_intel.received_byte_count;
        cc_.on_complete_calls++;
        cc_.terminal_callback->setReady();
      });
  stream_prototype_->setOnError(
      [this](Platform::EnvoyErrorSharedPtr, envoy_stream_intel, envoy_final_stream_intel) {
        cc_.on_error_calls++;
        cc_.terminal_callback->setReady();
      });
  stream_prototype_->setOnCancel([this](envoy_stream_intel, envoy_final_stream_intel final_intel) {
    EXPECT_NE(-1, final_intel.stream_start_ms);
    cc_.on_cancel_calls++;
    cc_.terminal_callback->setReady();
  });

  stream_ = (*stream_prototype_).start(explicit_flow_control_);
  std::string host(fake_upstreams_[0]->localAddress()->asStringView());
  HttpTestUtility::addDefaultHeaders(default_request_headers_);
  default_request_headers_.setHost(fake_upstreams_[0]->localAddress()->asStringView());
}

std::shared_ptr<Platform::RequestHeaders> BaseClientIntegrationTest::envoyToMobileHeaders(
    const Http::TestRequestHeaderMapImpl& request_headers) {

  Platform::RequestHeadersBuilder builder(
      Platform::RequestMethod::GET,
      std::string(default_request_headers_.Scheme()->value().getStringView()),
      std::string(default_request_headers_.Host()->value().getStringView()),
      std::string(default_request_headers_.Path()->value().getStringView()));
  if (upstreamProtocol() == Http::CodecType::HTTP2) {
    builder.addUpstreamHttpProtocol(Platform::UpstreamHttpProtocol::HTTP2);
  }

  request_headers.iterate(
      [&request_headers, &builder](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
        std::string key = std::string(header.key().getStringView());
        if (request_headers.formatter().has_value()) {
          const Envoy::Http::StatefulHeaderKeyFormatter& formatter =
              request_headers.formatter().value();
          key = formatter.format(key);
        }
        auto value = std::vector<std::string>();
        value.push_back(std::string(header.value().getStringView()));
        builder.set(key, value);
        return Http::HeaderMap::Iterate::Continue;
      });

  return std::make_shared<Platform::RequestHeaders>(builder.build());
}

void BaseClientIntegrationTest::threadRoutine(absl::Notification& engine_running) {
  setOnEngineRunning([&]() { engine_running.Notify(); });
  engine_ = build();
  full_dispatcher_->run(Event::Dispatcher::RunType::Block);
}

void BaseClientIntegrationTest::TearDown() {
  test_server_.reset();
  fake_upstreams_.clear();
  engine_->terminate();
  engine_.reset();
  full_dispatcher_->exit();
  envoy_thread_->join();
}

void BaseClientIntegrationTest::createEnvoy() {
  std::vector<uint32_t> ports;
  for (auto& upstream : fake_upstreams_) {
    if (upstream->localAddress()->ip()) {
      ports.push_back(upstream->localAddress()->ip()->port());
    }
  }

  finalizeConfigWithPorts(config_helper_, ports, use_lds_);

  if (override_builder_config_) {
    setOverrideConfigForTests(MessageUtil::getYamlStringFromMessage(config_helper_.bootstrap()));
  } else {
    ENVOY_LOG_MISC(warn, "Using builder config and ignoring config modifiers");
  }

  absl::Notification engine_running;
  envoy_thread_ = api_->threadFactory().createThread(
      [this, &engine_running]() -> void { threadRoutine(engine_running); });
  engine_running.WaitForNotification();
}

void BaseClientIntegrationTest::cleanup() {
  if (xds_connection_ != nullptr) {
    cleanUpXdsConnection();
  }
  test_server_.reset();
  fake_upstreams_.clear();
}

} // namespace Envoy
