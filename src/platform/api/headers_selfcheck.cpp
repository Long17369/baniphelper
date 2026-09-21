// 与 core 的同名文件同理：这个翻译单元的唯一作用是把平台接口头文件真正编译一遍，
// 并用静态断言把「纯接口、不可复制、继承同一个 Result 错误模型」这些约定钉住。
//
// 新增平台接口头文件时请同步加到这里。

#include <type_traits>

#include "platform/api/iautostart.h"
#include "platform/api/iconnmonitor.h"
#include "platform/api/ifilterengine.h"
#include "platform/api/ikiller.h"
#include "platform/api/ipaths.h"
#include "platform/api/iprivilege.h"
#include "platform/api/isessionmonitor.h"
#include "platform/api/isingleinstance.h"
#include "platform/api/itargetresolver.h"
#include "platform/api/itrafficstats.h"
#include "platform/api/platform_types.h"

namespace {

using namespace baniphelper::core;

#define BANIPHELPER_ASSERT_PURE_INTERFACE(Type)                                \
  static_assert(std::is_abstract_v<Type>, #Type " 必须保持为纯接口");          \
  static_assert(!std::is_copy_constructible_v<Type>, #Type " 不允许按值传递"); \
  static_assert(!std::is_move_constructible_v<Type>, #Type " 不允许按值传递")

BANIPHELPER_ASSERT_PURE_INTERFACE(IFilterEngine);
BANIPHELPER_ASSERT_PURE_INTERFACE(ITargetResolver);
BANIPHELPER_ASSERT_PURE_INTERFACE(IConnMonitor);
BANIPHELPER_ASSERT_PURE_INTERFACE(ITrafficStats);
BANIPHELPER_ASSERT_PURE_INTERFACE(IKiller);
BANIPHELPER_ASSERT_PURE_INTERFACE(ISessionMonitor);
BANIPHELPER_ASSERT_PURE_INTERFACE(IPrivilege);
BANIPHELPER_ASSERT_PURE_INTERFACE(ISingleInstance);
BANIPHELPER_ASSERT_PURE_INTERFACE(IAutoStart);
BANIPHELPER_ASSERT_PURE_INTERFACE(IPaths);

#undef BANIPHELPER_ASSERT_PURE_INTERFACE

// 平台接口的值类型同样要可复制。
static_assert(std::is_copy_constructible_v<AppliedRuleSummary>);
static_assert(std::is_copy_constructible_v<CleanupReport>);
static_assert(std::is_copy_constructible_v<ConnectionEvent>);
static_assert(std::is_copy_constructible_v<KillReport>);
static_assert(std::is_copy_constructible_v<SessionEvent>);
static_assert(std::is_copy_constructible_v<Paths>);

}  // namespace
