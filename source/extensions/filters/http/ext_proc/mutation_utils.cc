#include "source/extensions/filters/http/ext_proc/mutation_utils.h"

#include "envoy/http/header_map.h"

#include "source/common/http/header_utility.h"
#include "source/common/http/headers.h"
#include "source/common/protobuf/utility.h"
#include "source/common/runtime/runtime_features.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

using Filters::Common::MutationRules::Checker;
using Filters::Common::MutationRules::CheckOperation;
using Filters::Common::MutationRules::CheckResult;
using Http::Headers;
using Http::LowerCaseString;
using Stats::Counter;

using envoy::service::ext_proc::v3::BodyMutation;
using envoy::service::ext_proc::v3::HeaderMutation;

bool MutationUtils::headerInAllowList(
    absl::string_view key, const std::vector<Matchers::StringMatcherPtr>& header_matchers) {
  return std::any_of(header_matchers.begin(), header_matchers.end(),
                     [&key](auto& matcher) { return matcher->match(key); });
}

void MutationUtils::headersToProto(const Http::HeaderMap& headers_in,
                                   const std::vector<Matchers::StringMatcherPtr>& header_matchers,
                                   envoy::config::core::v3::HeaderMap& proto_out) {
  headers_in.iterate([&proto_out,
                      &header_matchers](const Http::HeaderEntry& e) -> Http::HeaderMap::Iterate {
    if (header_matchers.empty() || headerInAllowList(e.key().getStringView(), header_matchers)) {
      auto* new_header = proto_out.add_headers();
      new_header->set_key(std::string(e.key().getStringView()));
      // Setting up value or value_bytes field based on the runtime flag.
      if (Runtime::runtimeFeatureEnabled("envoy.reloadable_features.send_header_value_in_bytes")) {
        new_header->set_value_bytes(std::string(e.value().getStringView()));
      } else {
        new_header->set_value(MessageUtil::sanitizeUtf8String(e.value().getStringView()));
      }
    }
    return Http::HeaderMap::Iterate::Continue;
  });
}

absl::Status MutationUtils::applyHeaderMutations(const HeaderMutation& mutation,
                                                 Http::HeaderMap& headers, bool replacing_message,
                                                 const Checker& checker,
                                                 Counter& rejected_mutations) {
  for (const auto& hdr : mutation.remove_headers()) {
    if (!Http::HeaderUtility::headerNameIsValid(hdr)) {
      ENVOY_LOG(debug, "remove_headers contain invalid character, may not be removed.");
      rejected_mutations.inc();
      return absl::InvalidArgumentError("Invalid character in remove_headers mutation.");
    }
    const LowerCaseString remove_header(hdr);
    switch (checker.check(CheckOperation::REMOVE, remove_header, "")) {
    case CheckResult::OK:
      ENVOY_LOG(trace, "Removing header {}", remove_header);
      headers.remove(remove_header);
      break;
    case CheckResult::IGNORE:
      ENVOY_LOG(debug, "Header {} may not be removed per rules", remove_header);
      rejected_mutations.inc();
      break;
    case CheckResult::FAIL:
      ENVOY_LOG(debug, "Header {} may not be removed. Returning error", remove_header);
      rejected_mutations.inc();
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid attempt to remove ", remove_header.get()));
    }
  }

  for (const auto& sh : mutation.set_headers()) {
    if (!sh.has_header()) {
      continue;
    }

    // Only one of value or value_bytes in the HeaderValue message should be set.
    if (!sh.header().value().empty() && !sh.header().value_bytes().empty()) {
      ENVOY_LOG(debug, "Only one of value or value_bytes in the HeaderValue message should be set, "
                       "may not be append.");
      rejected_mutations.inc();
      return absl::InvalidArgumentError(
          "Only one of value or value_bytes in the HeaderValue message should be set.");
    }

    absl::string_view header_value;
    if (Runtime::runtimeFeatureEnabled("envoy.reloadable_features.send_header_value_in_bytes")) {
      header_value = sh.header().value_bytes();
    } else {
      header_value = sh.header().value();
    }
    if (!Http::HeaderUtility::headerNameIsValid(sh.header().key()) ||
        !Http::HeaderUtility::headerValueIsValid(header_value)) {
      ENVOY_LOG(debug,
                "set_headers contain invalid character in key or value, may not be appended.");
      rejected_mutations.inc();
      return absl::InvalidArgumentError("Invalid character in set_headers mutation.");
    }
    const LowerCaseString header_name(sh.header().key());
    const bool append = PROTOBUF_GET_WRAPPED_OR_DEFAULT(sh, append, false);
    const auto check_op = (append && !headers.get(header_name).empty()) ? CheckOperation::APPEND
                                                                        : CheckOperation::SET;
    auto check_result = checker.check(check_op, header_name, header_value);
    if (replacing_message && header_name == Http::Headers::get().Method) {
      // Special handling to allow changing ":method" when the
      // CONTINUE_AND_REPLACE option is selected, to stay compatible.
      check_result = CheckResult::OK;
    }
    switch (check_result) {
    case CheckResult::OK:
      ENVOY_LOG(trace, "Setting header {} append = {}", sh.header().key(), append);
      if (append) {
        headers.addCopy(header_name, header_value);
      } else {
        headers.setCopy(header_name, header_value);
      }
      break;
    case CheckResult::IGNORE:
      ENVOY_LOG(debug, "Header {} may not be modified per rules", header_name);
      rejected_mutations.inc();
      break;
    case CheckResult::FAIL:
      ENVOY_LOG(debug, "Header {} may not be modified. Returning error", header_name);
      rejected_mutations.inc();
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid attempt to modify ", static_cast<absl::string_view>(header_name)));
    }
  }
  return absl::OkStatus();
}

void MutationUtils::applyBodyMutations(const BodyMutation& mutation, Buffer::Instance& buffer) {
  switch (mutation.mutation_case()) {
  case BodyMutation::MutationCase::kClearBody:
    if (mutation.clear_body()) {
      ENVOY_LOG(trace, "Clearing HTTP body");
      buffer.drain(buffer.length());
    }
    break;
  case BodyMutation::MutationCase::kBody:
    ENVOY_LOG(trace, "Replacing body of {} bytes with new body of {} bytes", buffer.length(),
              mutation.body().size());
    buffer.drain(buffer.length());
    buffer.add(mutation.body());
    break;
  default:
    // Nothing to do on default
    break;
  }
}

bool MutationUtils::isValidHttpStatus(int code) { return (code >= 200); }

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
