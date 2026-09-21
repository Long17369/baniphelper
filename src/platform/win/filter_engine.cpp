#include "platform/win/filter_engine.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fwpmu.h>

#include <cstdint>
#include <memory>

namespace baniphelper::core {
namespace {

/// WFP 引擎句柄与普通内核句柄类型不同，这里统一在实现内部转换。
HANDLE toHandle(void* raw) {
  return reinterpret_cast<HANDLE>(raw);
}

void* toVoid(HANDLE handle) {
  return reinterpret_cast<void*>(handle);
}

/// 本工具的 provider 与 sublayer 标识。
///
/// ⚠️ **这两个 GUID 一旦发布就不能再改**。启动清理靠 provider 认出「哪些过滤器是我们的」，
/// 换掉 GUID 等于把旧版本留下的过滤器变成无人认领的垃圾，而且再也清不掉。
/// 它们与代码里的名字不同：名字只是给人看的，GUID 才是身份。
constexpr GUID kProviderKey = {
    0x7c6a2e51, 0x9d3b, 0x4f8a, {0xb1, 0xe4, 0x2a, 0x5c, 0x8d, 0x0f, 0x6b, 0x10}};

constexpr GUID kSubLayerKey = {
    0x2f1d8b74, 0x5c6e, 0x4a93, {0x8d, 0x27, 0x4e, 0x9b, 0x1c, 0x7a, 0x3f, 0x52}};

constexpr wchar_t kProviderName[] = L"BanIPHelper";
constexpr wchar_t kProviderDescription[] = L"BanIPHelper 过滤器提供者";

constexpr wchar_t kSubLayerName[] = L"BanIPHelper 常规规则";
constexpr wchar_t kSubLayerDescription[] =
    L"BanIPHelper 的常规规则层。白名单模式的默认阻断使用独立且权重更高的层，见 S4.1。";

/// 子层权重。白名单模式会用更高的权重压在它上面，因此这里留出空间。
constexpr UINT16 kSubLayerWeight = 0x7000;

/// `FWP_E_ALREADY_EXISTS`。
///
/// 用字面值而不是系统头里的名字：不同 SDK 把它定义成宏还是枚举并不一致，
/// 条件编译会把代码变得难看且容易出错。数值本身是 ABI 的一部分，不会变。
constexpr DWORD kFwpAlreadyExists = 0x80320009L;

/// `FWP_E_LAYER_NOT_FOUND`。见下面枚举时对「层正在消失」的处理。
constexpr DWORD kFwpLayerNotFound = 0x80320004L;

/// `RPC_C_AUTHN_WINNT`。为这一个常量引入 rpc.h 不值得。
constexpr UINT32 kAuthnWinnt = 10;

/// `RPC_C_AUTHN_LEVEL_PKT_PRIVACY` 的等价物在 WFP 里用默认（0）即可，
/// 本地调用不需要加密级别的协商。

/// 把 Win32 错误包装成中文明确的失败。`action` 是「刚才在做什么」，例如「创建过滤器枚举句柄」。
///
/// 原始错误码直接写进 message：这类失败几乎只能靠错误码排查，
/// 只留在结构体字段里的话，日志与界面就只有一句「失败」，等于没说什么。
Error platformError(const QString& action, DWORD code, const wchar_t* api) {
  return makeError(ErrorCode::Platform,
                   QStringLiteral("%1失败（错误码 0x%2）")
                       .arg(action, QString::number(static_cast<qulonglong>(code), 16).toUpper()),
                   static_cast<std::int32_t>(code),
                   QString::fromWCharArray(api));
}

/// 注册自有 provider 与 sublayer。调用方负责事务。
Result<void> ensureOwnIdentity(HANDLE engine) {
  FWPM_PROVIDER0 provider{};
  provider.providerKey = kProviderKey;
  provider.displayData.name = const_cast<wchar_t*>(kProviderName);
  provider.displayData.description = const_cast<wchar_t*>(kProviderDescription);

  const DWORD providerResult = ::FwpmProviderAdd0(engine, &provider, nullptr);
  if (providerResult != ERROR_SUCCESS && providerResult != kFwpAlreadyExists) {
    return Result<void>::fail(
        platformError(QStringLiteral("注册自有过滤器提供者"), providerResult, L"FwpmProviderAdd0"));
  }

  FWPM_SUBLAYER0 subLayer{};
  subLayer.subLayerKey = kSubLayerKey;
  subLayer.displayData.name = const_cast<wchar_t*>(kSubLayerName);
  subLayer.displayData.description = const_cast<wchar_t*>(kSubLayerDescription);
  subLayer.weight = kSubLayerWeight;

  const DWORD subLayerResult = ::FwpmSubLayerAdd0(engine, &subLayer, nullptr);
  if (subLayerResult != ERROR_SUCCESS && subLayerResult != kFwpAlreadyExists) {
    return Result<void>::fail(
        platformError(QStringLiteral("注册自有子层"), subLayerResult, L"FwpmSubLayerAdd0"));
  }

  return Result<void>::ok();
}

/// 问系统要全部 WFP 层的 GUID。
///
/// 为什么是「问系统」而不是写死一份层清单：写死的清单必须随功能扩展而增补，
/// 而只要有一次忘了补，那一层上的旧过滤器就永远清理不掉，并且是**静默**的 ——
/// 清理会报告成功，残留却还在生效。逐层问一遍的代价是启动时多几十毫秒，
/// 换的是「清理的完整性不依赖人记得改」。
///
/// 顺带绕开了一个现实约束：MinGW 的 fwpmu.h 不提供 FWPM_LAYER_* 这类层 GUID 常量，
/// 系统里也没有可读的层名（displayData.name 是资源串），写死 GUID 无从校验。
Result<QList<GUID>> enumAllLayerKeys(HANDLE engine) {
  FWPM_LAYER_ENUM_TEMPLATE0 layerTemplate{};
  HANDLE enumHandle = nullptr;

  const DWORD created = ::FwpmLayerCreateEnumHandle0(engine, &layerTemplate, &enumHandle);
  if (created != ERROR_SUCCESS) {
    return Result<QList<GUID>>::fail(
        platformError(QStringLiteral("创建层枚举句柄"), created, L"FwpmLayerCreateEnumHandle0"));
  }

  QList<GUID> keys;
  constexpr UINT32 kBatch = 64;

  for (;;) {
    FWPM_LAYER0** entries = nullptr;
    UINT32 returned = 0;

    const DWORD result = ::FwpmLayerEnum0(engine, enumHandle, kBatch, &entries, &returned);
    if (result != ERROR_SUCCESS) {
      ::FwpmLayerDestroyEnumHandle0(engine, enumHandle);
      return Result<QList<GUID>>::fail(
          platformError(QStringLiteral("枚举系统过滤器层"), result, L"FwpmLayerEnum0"));
    }

    if (returned == 0) {
      break;
    }

    for (UINT32 index = 0; index < returned; ++index) {
      keys.append(entries[index]->layerKey);
    }

    ::FwpmFreeMemory0(reinterpret_cast<void**>(&entries));

    if (returned < kBatch) {
      break;
    }
  }

  ::FwpmLayerDestroyEnumHandle0(engine, enumHandle);
  return Result<QList<GUID>>::ok(keys);
}

}  // namespace

WinFilterEngine::WinFilterEngine() = default;

WinFilterEngine::~WinFilterEngine() {
  // 析构里不做可能失败的事，也不吞错误：关闭失败只可能发生在句柄已经无效时，
  // 那种情况系统已经替我们回收了。真正的关闭走 close()，那里会如实报错。
  if (engine_ != nullptr) {
    ::FwpmEngineClose0(toHandle(engine_));
    engine_ = nullptr;
  }
}

bool WinFilterEngine::isOpen() const {
  return engine_ != nullptr;
}

Result<void> WinFilterEngine::open() {
  if (isOpen()) {
    // 重复打开是安全的，接口契约要求幂等。
    return Result<void>::ok();
  }

  // 非动态会话。过滤器在进程退出后仍然存活，由启动清理负责回收。
  //
  // 这里刻意**不用** FWPM_SESSION_FLAG_DYNAMIC：配置项 revoke_on_exit 的语义是
  // 「退出时是否撤销」，也就是说规则本来就可以跨越进程生命周期存活，
  // 动态会话会让这个选项失去意义。代价是必须把启动清理做对，而 S1.9 的异常路径演练
  // 正是用来守住这一点的。
  FWPM_SESSION0 session{};
  session.flags = 0;

  HANDLE engine = nullptr;
  const DWORD result = ::FwpmEngineOpen0(nullptr, kAuthnWinnt, nullptr, &session, &engine);
  if (result != ERROR_SUCCESS) {
    return Result<void>::fail(platformError(
        QStringLiteral("打开过滤器引擎（需要管理员权限）"), result, L"FwpmEngineOpen0"));
  }

  // provider 与 sublayer 必须在一个事务里建好，避免留下「有子层但没提供者」这种中间态。
  const DWORD begin = ::FwpmTransactionBegin0(engine, 0);
  if (begin != ERROR_SUCCESS) {
    ::FwpmEngineClose0(engine);
    return Result<void>::fail(
        platformError(QStringLiteral("开启过滤器事务"), begin, L"FwpmTransactionBegin0"));
  }

  const Result<void> identity = ensureOwnIdentity(engine);
  if (!identity) {
    ::FwpmTransactionAbort0(engine);
    ::FwpmEngineClose0(engine);
    return identity;
  }

  const DWORD commit = ::FwpmTransactionCommit0(engine);
  if (commit != ERROR_SUCCESS) {
    ::FwpmTransactionAbort0(engine);
    ::FwpmEngineClose0(engine);
    return Result<void>::fail(
        platformError(QStringLiteral("提交过滤器事务"), commit, L"FwpmTransactionCommit0"));
  }

  engine_ = toVoid(engine);
  return Result<void>::ok();
}

Result<void> WinFilterEngine::close() {
  if (!isOpen()) {
    // 重复关闭是安全的。
    return Result<void>::ok();
  }

  HANDLE engine = toHandle(engine_);
  engine_ = nullptr;

  const DWORD result = ::FwpmEngineClose0(engine);
  if (result != ERROR_SUCCESS) {
    return Result<void>::fail(
        platformError(QStringLiteral("关闭过滤器引擎"), result, L"FwpmEngineClose0"));
  }

  return Result<void>::ok();
}

Result<QList<unsigned long long>> WinFilterEngine::enumOwnFilterIds() const {
  if (!isOpen()) {
    return Result<QList<unsigned long long>>::fail(makeError(
        ErrorCode::InvalidArgument, QStringLiteral("过滤器引擎尚未打开，无法枚举过滤器")));
  }

  HANDLE engine = toHandle(engine_);

  const Result<QList<GUID>> layers = enumAllLayerKeys(engine);
  if (!layers) {
    return Result<QList<unsigned long long>>::fail(layers.error());
  }

  QList<unsigned long long> ids;
  constexpr UINT32 kBatch = 64;

  for (const GUID& layerKey : layers.value()) {
    // 枚举模板必须**同时**给出 providerKey 与 layerKey。
    // 只给 providerKey 而把 layerKey 留空，WFP 会以 FWP_E_LAYER_NOT_FOUND 拒绝
    // （0x80320004，实测）。
    //
    // providerKey 在结构里是 `GUID*` 而不是值，所以传地址；
    // 常量是 const 的，得先拷一份可写的出来。
    GUID ownProviderKey = kProviderKey;
    GUID layer = layerKey;

    FWPM_FILTER_ENUM_TEMPLATE0 enumTemplate{};
    enumTemplate.providerKey = &ownProviderKey;
    enumTemplate.layerKey = layer;
    enumTemplate.actionMask = 0xFFFFFFFF;

    HANDLE enumHandle = nullptr;
    const DWORD created = ::FwpmFilterCreateEnumHandle0(engine, &enumTemplate, &enumHandle);

    if (created == kFwpLayerNotFound) {
      // 刚刚从系统问出来的层，这里却说不存在，说明它在这几毫秒里被卸载了。
      // 跳过即可，不该因为一个正在消失的层让整次清理失败。
      continue;
    }

    if (created != ERROR_SUCCESS) {
      return Result<QList<unsigned long long>>::fail(platformError(
          QStringLiteral("创建过滤器枚举句柄"), created, L"FwpmFilterCreateEnumHandle0"));
    }

    for (;;) {
      FWPM_FILTER0** entries = nullptr;
      UINT32 returned = 0;

      const DWORD result = ::FwpmFilterEnum0(engine, enumHandle, kBatch, &entries, &returned);
      if (result != ERROR_SUCCESS) {
        ::FwpmFilterDestroyEnumHandle0(engine, enumHandle);
        return Result<QList<unsigned long long>>::fail(
            platformError(QStringLiteral("枚举自有过滤器"), result, L"FwpmFilterEnum0"));
      }

      if (returned == 0) {
        break;
      }

      for (UINT32 index = 0; index < returned; ++index) {
        ids.append(entries[index]->filterId);
      }

      ::FwpmFreeMemory0(reinterpret_cast<void**>(&entries));

      if (returned < kBatch) {
        break;
      }
    }

    ::FwpmFilterDestroyEnumHandle0(engine, enumHandle);
  }

  return Result<QList<unsigned long long>>::ok(ids);
}

Result<int> WinFilterEngine::removeOwnFilters() {
  const Result<QList<unsigned long long>> ids = enumOwnFilterIds();
  if (!ids) {
    return Result<int>::fail(ids.error());
  }

  HANDLE engine = toHandle(engine_);
  int removed = 0;

  for (unsigned long long id : ids.value()) {
    const DWORD result = ::FwpmFilterDeleteById0(engine, id);
    if (result != ERROR_SUCCESS) {
      // 这里不把已经删掉的条数藏起来：清理是「尽力删干净」，半途失败时
      // 说清删到哪一条、错在哪个过滤器，比一句「清理失败」有用得多。
      return Result<int>::fail(
          makeError(ErrorCode::Platform,
                    QStringLiteral("删除自有过滤器失败（已删除 %1 条，共 %2 条）")
                        .arg(removed)
                        .arg(ids.value().size()),
                    static_cast<std::int32_t>(result),
                    QStringLiteral("FwpmFilterDeleteById0")));
    }
    ++removed;
  }

  return Result<int>::ok(removed);
}

Result<CleanupReport> WinFilterEngine::cleanupOrphans() {
  const Result<int> removed = removeOwnFilters();
  if (!removed) {
    return Result<CleanupReport>::fail(removed.error());
  }

  CleanupReport report;
  report.removedOwn = removed.value();

  // 恒为 0，而且这是**结构性**的 0 而不是「恰好没遇到」：枚举按自有 provider 限定，
  // 他方过滤器根本不会被选出来。这正是「绝不按层全量清理」的落地方式。
  report.skippedForeign = 0;

  return Result<CleanupReport>::ok(report);
}

Result<void> WinFilterEngine::revokeAll() {
  const Result<int> removed = removeOwnFilters();
  if (!removed) {
    return Result<void>::fail(removed.error());
  }
  return Result<void>::ok();
}

// 下面这些属于阶段二 S2.4 的工作。在实现之前一律明确报「不支持」，
// 与能力声明（未声明 FilterIPv4 / FilterIPv6）保持一致：
// 声明说不支持，接口就必须失败，不允许悄悄成功。
namespace {

Error notImplementedYet(const QString& operation) {
  return unsupportedError(operation, QStringLiteral("过滤器引擎的下发能力尚未实现，阶段二 S2.4"));
}

}  // namespace

Result<void> WinFilterEngine::applyRule(const RuleSpec& rule) {
  Q_UNUSED(rule);
  return Result<void>::fail(notImplementedYet(QStringLiteral("下发封禁规则")));
}

Result<void> WinFilterEngine::revokeRule(const RuleId& id) {
  Q_UNUSED(id);
  return Result<void>::fail(notImplementedYet(QStringLiteral("撤销指定规则")));
}

Result<QList<AppliedRuleSummary>> WinFilterEngine::appliedRules() const {
  return Result<QList<AppliedRuleSummary>>::fail(
      notImplementedYet(QStringLiteral("查询已生效规则")));
}

Result<void> WinFilterEngine::applyDefaultBlock(std::int32_t priority) {
  Q_UNUSED(priority);
  return Result<void>::fail(notImplementedYet(QStringLiteral("下发默认阻断")));
}

Result<bool> WinFilterEngine::hasDefaultBlock() const {
  return Result<bool>::fail(notImplementedYet(QStringLiteral("查询默认阻断状态")));
}

Result<void> WinFilterEngine::revokeDefaultBlock() {
  return Result<void>::fail(notImplementedYet(QStringLiteral("撤销默认阻断")));
}

}  // namespace baniphelper::core
