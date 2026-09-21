#pragma once

namespace baniphelper::core {

/// 产品版本字符串，形如 0.1.0，由构建系统注入。
/// 界面与对外的能力查询都应读取这里，不要各自硬编码版本号。
[[nodiscard]] const char* versionString() noexcept;

}  // namespace baniphelper::core
