#pragma once

#include <QList>
#include <QString>

#include "core/result.h"
#include "core/types.h"

namespace baniphelper::core {

/// 进程标识解析与存活跟随。
///
/// 契约要点：
///
/// - `resolve` 返回的 `PlatformTargetId` **对核心层不透明**。核心层只负责在
///   `resolve` 与 `IFilterEngine` 之间原样转交，**不得解析、拼接或写入规则与记录**；
/// - 存活判据必须是**身份**而不是 PID。PID 会被系统复用，用 PID 判断会把
///   后来的进程误认成目标；
/// - `listProcesses` 只用于界面选择目标，不保证与系统工具逐条一致
///   （系统进程与受保护进程可能拿不到路径），但**能列出什么必须如实反映**，
///   不允许用占位名糊过去。
class ITargetResolver {
 public:
  ITargetResolver() = default;
  virtual ~ITargetResolver() = default;

  ITargetResolver(const ITargetResolver&) = delete;
  ITargetResolver& operator=(const ITargetResolver&) = delete;
  ITargetResolver(ITargetResolver&&) = delete;
  ITargetResolver& operator=(ITargetResolver&&) = delete;

  /// 枚举当前进程，供界面选择目标。按可执行文件路径去重。
  [[nodiscard]] virtual Result<QList<ProcessRef>> listProcesses() const = 0;

  /// 把可执行文件全路径转成平台内部标识。
  ///
  /// 失败是常态而不是异常：路径不存在、卷未挂载、路径指向受保护位置都要给出明确错误，
  /// 而不是返回一个原样拼好的字符串。
  [[nodiscard]] virtual Result<PlatformTargetId> resolve(const QString& imagePath) const = 0;

  /// 目标进程是否仍在运行。
  [[nodiscard]] virtual Result<bool> isAlive(const ProcessRef& process) const = 0;

  /// 自身进程。
  ///
  /// 自我保护与放行清单都要用它，因此它**不允许失败**：拿不到自身标识意味着
  /// 白名单模式一定会把自己封掉。
  [[nodiscard]] virtual Result<ProcessRef> self() const = 0;
};

}  // namespace baniphelper::core
