#pragma once

#include <QString>

#include <cstdint>

namespace baniphelper::core {

/// 错误分类。
///
/// 用「分类」而不是「每处失败各占一个枚举值」的原因：上层要按类统一处理，
/// 而各平台实现新增的失败细节不该逼着所有分支跟着改。需要细节时看 nativeCode 与 nativeSource。
enum class ErrorCode : std::int32_t {
  /// 未归类。出现在这里说明某处漏了归类，属于待补的缺陷。
  Unknown = -1,
  /// 本平台或本后端不具备该能力。**必须如实上报，禁止伪装成功**。
  NotSupported = 1,
  /// 权限不足，通常意味着需要提权重试。
  NotPermitted = 2,
  /// 入参不合法。
  InvalidArgument = 3,
  /// 目标不存在。
  NotFound = 4,
  /// 目标已存在，且本次操作不是幂等语义。
  AlreadyExists = 5,
  /// 资源被占用，稍后重试可能成功。
  Busy = 6,
  /// 超时。**不得用它表达「不支持」**，两者对界面是不同提示。
  Timeout = 7,
  /// 文件、数据库、管道等 I/O 失败。
  Io = 8,
  /// 不该发生的情况，属于程序缺陷。
  Internal = 9,
  /// 操作系统返回了错误，细节在 nativeCode 与 nativeSource。
  Platform = 10,
};

/// 人类可读的英文代号，供日志与调试输出使用；界面显示请用 Error::message。
[[nodiscard]] const char* errorCodeName(ErrorCode code) noexcept;

/// 失败原因。成功路径不会构造它，因此「有错必有据」。
struct Error {
  ErrorCode code = ErrorCode::Unknown;

  /// 给人看的中文说明，**不允许为空**。
  /// 界面会直接显示它，留空就等于把一次失败变成静默失败。
  QString message;

  /// 操作系统或底层库返回的原始错误码，没有就为 0。
  std::int32_t nativeCode = 0;

  /// 原始错误来自哪里，例如 WFP、IP Helper、SQLite。
  QString nativeSource;
};

/// 构造错误。调用方必须给出非空的 message。
[[nodiscard]] Error makeError(ErrorCode code,
                              QString message,
                              std::int32_t nativeCode = 0,
                              QString nativeSource = QString());

/// 构造「能力不可用」错误。
///
/// 所有 NotSupported 都必须走这里，以保证 message 里永远说清「什么做不到」与「为什么」，
/// 界面才不会出现没有任何信息的禁用状态。
[[nodiscard]] Error unsupportedError(const QString& what, const QString& reason);

}  // namespace baniphelper::core
