#include "core/error.h"

namespace baniphelper::core {

const char* errorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Unknown:
      return "Unknown";
    case ErrorCode::NotSupported:
      return "NotSupported";
    case ErrorCode::NotPermitted:
      return "NotPermitted";
    case ErrorCode::InvalidArgument:
      return "InvalidArgument";
    case ErrorCode::NotFound:
      return "NotFound";
    case ErrorCode::AlreadyExists:
      return "AlreadyExists";
    case ErrorCode::Busy:
      return "Busy";
    case ErrorCode::Timeout:
      return "Timeout";
    case ErrorCode::Io:
      return "Io";
    case ErrorCode::Internal:
      return "Internal";
    case ErrorCode::Platform:
      return "Platform";
  }
  return "Unknown";
}

Error makeError(ErrorCode code, QString message, std::int32_t nativeCode, QString nativeSource) {
  Error error;
  error.code = code;
  error.message = message.isEmpty() ? QStringLiteral("未提供失败原因") : std::move(message);
  error.nativeCode = nativeCode;
  error.nativeSource = std::move(nativeSource);
  return error;
}

Error unsupportedError(const QString& what, const QString& reason) {
  return makeError(ErrorCode::NotSupported,
                   QStringLiteral("%1：当前平台不具备该能力（%2）").arg(what, reason));
}

}  // namespace baniphelper::core
