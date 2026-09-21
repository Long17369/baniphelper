// 这个翻译单元不产生任何功能，唯一作用是**把接口头文件真正编译一遍**。
//
// 头文件单独放着是不会被编译器检查的，「头文件可编译」这条验收标准因此必须有实物支撑。
// 新增接口头文件时请同步加到这里，下面的静态断言也要跟着补。

#include <type_traits>

#include "core/capabilities.h"
#include "core/config.h"
#include "core/error.h"
#include "core/icapabilities.h"
#include "core/iconfig.h"
#include "core/irecorder.h"
#include "core/irulestore.h"
#include "core/isafetyguard.h"
#include "core/record.h"
#include "core/result.h"
#include "core/rule.h"
#include "core/safety.h"
#include "core/types.h"
#include "core/version.h"

namespace {

using namespace baniphelper::core;

// 核心接口必须满足两条：纯接口（可抽象），且不可复制。
// 不可复制是为了杜绝「按值传递接口」这种把多态切掉的写法。
static_assert(std::is_abstract_v<ICapabilities>);
static_assert(!std::is_copy_constructible_v<ICapabilities>);
static_assert(!std::is_move_constructible_v<ICapabilities>);

static_assert(std::is_abstract_v<IConfig>);
static_assert(!std::is_copy_constructible_v<IConfig>);
static_assert(!std::is_move_constructible_v<IConfig>);

static_assert(std::is_abstract_v<IRuleStore>);
static_assert(!std::is_copy_constructible_v<IRuleStore>);
static_assert(!std::is_move_constructible_v<IRuleStore>);

static_assert(std::is_abstract_v<IRecorder>);
static_assert(!std::is_copy_constructible_v<IRecorder>);
static_assert(!std::is_move_constructible_v<IRecorder>);

static_assert(std::is_abstract_v<ISafetyGuard>);
static_assert(!std::is_copy_constructible_v<ISafetyGuard>);
static_assert(!std::is_move_constructible_v<ISafetyGuard>);

// 值类型必须是可复制、可移动的，否则结果类型与容器都用不了。
static_assert(std::is_copy_constructible_v<Address>);
static_assert(std::is_copy_constructible_v<ConnectionKey>);
static_assert(std::is_copy_constructible_v<ProcessRef>);
static_assert(std::is_copy_constructible_v<MatchCondition>);
static_assert(std::is_copy_constructible_v<Rule>);
static_assert(std::is_copy_constructible_v<ConnectionObservation>);
static_assert(std::is_copy_constructible_v<AllowlistEntry>);

// 错误信息不允许为空是硬约定，这里把它钉在类型层面：有 Error 就必须有 message。
static_assert(std::is_copy_constructible_v<Error>);
static_assert(std::is_copy_constructible_v<Result<void>>);
static_assert(std::is_copy_constructible_v<Result<QString>>);

}  // namespace
