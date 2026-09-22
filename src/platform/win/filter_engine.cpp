#include "platform/win/filter_engine.h"
#include "platform/win/filter_plan.h"
#include "platform/win/wfp_guids.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fwpmu.h>

#include <cstdint>
#include <functional>
#include <memory>

#include <QList>
#include <QString>

#include "core/error.h"
#include "core/rule_model.h"

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
/// 系统里也没有可读的层名（displayData.name 是资源串），写死一份清单无从校验。
///
/// `wfp_guids.h` 里现在有本工具需要的那几层，但那是**下发规则时按需取用**的常量，
/// 不能拿来替换这里的全量枚举 —— 上一段说的「漏一层就静默留垃圾」依然成立。
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

// ---------------------------------------------------------------------------
// 规则标识在过滤器上的落法（S2.4）
// ---------------------------------------------------------------------------
//
// 「这条过滤器属于哪条规则」必须能从**系统侧**读回来：撤销、统计、重启后的清点
// 都要靠它。WFP 只有一个自由文本字段能承载（`displayData.name`）——
// `filterKey` 是我们自己生成的随机值，系统侧没有反查手段。
//
// 加 `rule:` 前缀而不是直接写规则标识，是为了给其它自家过滤器
// （白名单的默认阻断之类）留出互不混淆的名字空间：那些不属于任何一条规则，
// 不能被 `appliedRules()` 算进去。

constexpr char kRuleNamePrefix[] = "rule:";

QString ruleDisplayName(const RuleId& id) {
  return QString::fromLatin1(kRuleNamePrefix) + id;
}

/// 从显示名取回规则标识。不是按规则下发的自家过滤器返回空串。
QString ruleIdOfDisplayName(const wchar_t* name) {
  if (name == nullptr) {
    return QString();
  }
  const QString prefix = QString::fromLatin1(kRuleNamePrefix);
  const QString text = QString::fromWCharArray(name);
  if (!text.startsWith(prefix)) {
    return QString();
  }
  return text.mid(prefix.size());
}

/// 给新过滤器取一个标识。
///
/// 用随机值而不是从规则标识派生：一条规则展开出的条数**不是恒定的**
/// （取反求补会让条数变化），派生出来的标识会与上一版撞车。而「同一事务里
/// 删掉旧的那条、再加一条同标识的」会有什么行为，没有实测依据。
/// 归属靠显示名，`filterKey` 只需要唯一。
GUID nextFilterKey() {
  GUID key{};
  ::CoCreateGuid(&key);
  return key;
}

/// 落点 + 地址族 → 层 GUID。
///
/// 三个落点各对应两个层（IPv4 / IPv6 是两个不同的层），共 6 个取值，
/// 全部来自 `wfp_guids.h`，那张表的取值已在本机运行时逐条对上过（S1.15）。
GUID layerKeyFor(FilterStage stage, AddressFamily family) {
  const bool v6 = family == AddressFamily::V6;
  switch (stage) {
    case FilterStage::OutboundConnect:
      return v6 ? kLayerAleAuthConnectV6 : kLayerAleAuthConnectV4;
    case FilterStage::InboundAccept:
      return v6 ? kLayerAleAuthRecvAcceptV6 : kLayerAleAuthRecvAcceptV4;
    case FilterStage::InboundDatagram:
      return v6 ? kLayerDatagramDataV6 : kLayerDatagramDataV4;
  }
  return kLayerAleAuthConnectV4;
}

/// 落点的中文名，只进描述文本，供人核对。
QString stageTitle(FilterStage stage) {
  switch (stage) {
    case FilterStage::OutboundConnect:
      return QStringLiteral("出站连接");
    case FilterStage::InboundAccept:
      return QStringLiteral("入站接受");
    case FilterStage::InboundDatagram:
      return QStringLiteral("入站数据报");
  }
  return QString();
}

/// 条件字段 → WFP 条件 GUID。
///
/// ⚠️ **地址用不带版本后缀的 `IP_REMOTE_ADDRESS`**，不是 `_V4` / `_V6` 那一对。
/// 这一点是实测出来的（`tmp/probe_wfp_apply.cpp` 与 `tmp/probe_wfp_fields.cpp`，
/// 结论见 docs/phases/02-filtering.md 第 3.4 节）：在 ALE 三层的字段清单里
/// **只有** `IP_REMOTE_ADDRESS`，带后缀的那两个根本不存在，用了会得到
/// `FWP_E_CONDITION_NOT_FOUND`（`0x80320002`）而整条过滤器加不进去。
/// 带后缀的那一对是给 IP 层（转发、入站 IP 包之类）用的。
///
/// 取值宽度随层而变，由 `fillAddressValue` 按地址族给出：
/// ALE V4 层要 32 位数值、V6 层要 16 字节数组。
///
/// 端口按方向取远端或本地：**出站过滤的是「连到别人的哪个端口」，
/// 入站过滤的是「别人连我的哪个端口」**。对方连过来时它的源端口是随机的，
/// 拿它做条件没有意义。
GUID conditionKeyFor(FilterField field, FilterStage stage) {
  switch (field) {
    case FilterField::AppPath:
      return kConditionAleAppId;
    case FilterField::RemoteAddress:
      return kConditionIpRemoteAddress;
    case FilterField::Port:
      return stage == FilterStage::OutboundConnect ? kConditionIpRemotePort : kConditionIpLocalPort;
    case FilterField::Protocol:
      return kConditionIpProtocol;
  }
  return kConditionIpProtocol;
}

/// 核心层优先级 → WFP 过滤器权重。
///
/// 核心层算出的 `priority` 是「动作基址 + 具体度之和 + 取反加成」：
/// `allow` 从 10000 起、`block` 到 5005 封顶，两边靠**数值区间**隔开，
/// 「放行永远赢」不依赖比较运气。所以平台侧只要**单调**映射过去，这个性质就保住。
///
/// ⚠️ WFP 的过滤器权重只收 0–15（`FWP_UINT8`），所以除以 1000 再夹取：
/// `block` 落在 0–5、`allow` 落在 10–15，两边仍分得开。代价是**同一大档内
/// 的精细次序丢失**（例如 1000 与 1900 都映射成 1）。这不影响正确性：
/// 同一档内要么都是阻断、要么都是放行，不存在「谁该赢」的问题。
FWP_VALUE0 weightFor(std::int32_t priority) {
  std::int32_t coarse = priority / 1000;
  if (coarse < 0) {
    coarse = 0;
  } else if (coarse > 15) {
    coarse = 15;
  }

  FWP_VALUE0 weight{};
  weight.type = FWP_UINT8;
  weight.uint8 = static_cast<UINT8>(coarse);
  return weight;
}

/// 本工具名下的一条过滤器，连同它承载的规则标识。
struct OwnFilterRecord {
  unsigned long long filterId = 0;

  /// 空表示这是自家的过滤器、但不属于任何一条规则。
  QString ruleId;

  RuleAction action = RuleAction::Block;
};

// ---------------------------------------------------------------------------
// 过滤器构造（S2.4）
// ---------------------------------------------------------------------------

/// 一条 WFP 过滤器的全部内容，以及它指向的那些内存。
///
/// `FWPM_FILTER0` 里是**裸指针**（显示名、provider、条件取值里的数组与区间），
/// `FwpmFilterAdd0` 只读取它们、**不接管内存**，调用返回后就不再使用。
/// 所以每一块被指到的内存都必须活到那次调用结束 —— 本结构就是那份存储。
///
/// ⚠️ **不要拷贝本结构**：拷贝之后 `filter` 里的指针仍然指向原对象的内存。
/// 它只在本文件内临时存在，不作为返回值传递，就是为了避免这个坑。
///
/// ⚠️ 构造顺序有硬要求：先把 `ranges` 与 `addresses` 填满，再往 `conditions` 里
/// 放那些指向它们的指针。这几个都是连续容器，`append` 超出容量时会重新分配，
/// 先前取到的元素地址全部作废。所以流程是「先算个数、一次 `reserve`、填满、
/// 再取地址」，中途不再增元素。
struct FilterDraft {
  FWPM_FILTER0 filter{};
  GUID providerKey{};
  GUID subLayerKey{};

  QList<FWPM_FILTER_CONDITION0> conditions;
  QList<FWP_RANGE0> ranges;
  QList<FWP_BYTE_ARRAY16> addresses;

  /// 由 `FwpmGetAppIdFromFileName0` 分配，用完要 `FwpmFreeMemory0` 还回去。
  QList<void*> ownedAppIds;

  /// 显示名与描述。`filter.displayData` 指向这两条字符串的 UTF-16 存储，
  /// 所以必须先给它们赋完值、再取指针。
  QString name;
  QString description;

  FilterDraft() = default;
  ~FilterDraft() {
    for (void* blob : ownedAppIds) {
      if (blob != nullptr) {
        void* raw = blob;
        ::FwpmFreeMemory0(&raw);
      }
    }
  }

  FilterDraft(const FilterDraft&) = delete;
  FilterDraft& operator=(const FilterDraft&) = delete;
};

/// 一个条件在取值池里要占多少位置。先算清楚，才能一次 `reserve` 到位。
struct DraftSizes {
  int ranges = 0;
  int addresses = 0;
};

DraftSizes measureConditions(const QList<FilterCondition>& conditions) {
  DraftSizes sizes;
  for (const FilterCondition& condition : conditions) {
    switch (condition.field) {
      case FilterField::RemoteAddress:
        ++sizes.ranges;
        if (condition.address.lower.family == AddressFamily::V6) {
          // 区间两端各要一个 16 字节数组，IPv4 走 32 位数值不需要额外内存。
          sizes.addresses += 2;
        }
        break;
      case FilterField::Port:
        ++sizes.ranges;
        break;
      default:
        break;
    }
  }
  return sizes;
}

/// 程序路径 → WFP 的应用标识。
///
/// 用系统给的 `FwpmGetAppIdFromFileName0` 而不是自己拼 NT 设备路径：
/// 它接受 DOS 路径并按当前系统的卷映射解析，卷序号、符号链接、短路径
/// 这些情况都由它处理。自己拼一份就等于把这套映射规则抄进我们的代码，
/// 抄错了规则会落到别的程序上，而且不报错。
Result<FWP_BYTE_BLOB*> appIdFor(FilterDraft& draft, const QString& path) {
  if (path.isEmpty()) {
    return makeError(ErrorCode::InvalidArgument, QStringLiteral("程序路径为空，转不出应用标识"));
  }

  FWP_BYTE_BLOB* blob = nullptr;
  const DWORD result =
      ::FwpmGetAppIdFromFileName0(reinterpret_cast<const wchar_t*>(path.utf16()), &blob);
  if (result != ERROR_SUCCESS) {
    return Result<FWP_BYTE_BLOB*>::fail(
        platformError(QStringLiteral("把程序路径转成应用标识（%1）").arg(path),
                      result,
                      L"FwpmGetAppIdFromFileName0"));
  }

  draft.ownedAppIds.append(blob);
  return blob;
}

/// 把一个地址写进 WFP 取值。
///
/// IPv4 给 32 位数值、IPv6 给 16 字节数组 —— 这是 `IP_REMOTE_ADDRESS` 在
/// ALE V4 层与 V6 层上的取值类型（实测，见 `conditionKeyFor` 的说明）。
/// **同一个条件字段，两种层两种宽度**：按地址族给错了宽度不会报错，
/// 系统会把那串字节解释成别的地址，规则就封到别处去了。
Result<void> fillAddressValue(FilterDraft& draft, const Address& address, FWP_VALUE0& value) {
  auto bytes = addressToBytes(address);
  if (!bytes) {
    return Result<void>::fail(bytes.error());
  }
  const QByteArray& raw = bytes.value();

  if (address.family == AddressFamily::V4) {
    if (raw.size() != 4) {
      return Result<void>::fail(makeError(
          ErrorCode::Internal, QStringLiteral("IPv4 地址的字节数不是 4，而是 %1").arg(raw.size())));
    }
    const quint32 packed = (static_cast<quint32>(static_cast<quint8>(raw.at(0))) << 24) |
                           (static_cast<quint32>(static_cast<quint8>(raw.at(1))) << 16) |
                           (static_cast<quint32>(static_cast<quint8>(raw.at(2))) << 8) |
                           static_cast<quint32>(static_cast<quint8>(raw.at(3)));
    value.type = FWP_UINT32;
    value.uint32 = static_cast<UINT32>(packed);
    return Result<void>::ok();
  }

  if (raw.size() != 16) {
    return Result<void>::fail(makeError(
        ErrorCode::Internal, QStringLiteral("IPv6 地址的字节数不是 16，而是 %1").arg(raw.size())));
  }

  FWP_BYTE_ARRAY16 array{};
  for (int i = 0; i < 16; ++i) {
    array.byteArray16[i] = static_cast<UINT8>(raw.at(i));
  }
  draft.addresses.append(array);

  value.type = FWP_BYTE_ARRAY16_TYPE;
  value.byteArray16 = &draft.addresses.last();
  return Result<void>::ok();
}

/// 把一个已解析取反的条件翻成 WFP 条件并追加。
///
/// 只有程序域可能带着 `negate` 进来（见 `resolveNegation`）：那是唯一求不出补集的
/// 字段，单值取反只能靠平台的「不等于」表达。
Result<void> appendWfpCondition(FilterDraft& draft,
                                const FilterPlanEntry& entry,
                                const FilterCondition& condition) {
  FWPM_FILTER_CONDITION0 wfp{};

  switch (condition.field) {
    case FilterField::AppPath: {
      if (entry.stage == FilterStage::InboundDatagram) {
        // 数据报层的字段清单里**没有** ALE_APP_ID（实测，见
        // tmp/probe_wfp_fields.cpp）。拿不到程序标识，这一条就表达不出来。
        //
        // 到这里就整体报错，不「下发一半」：悄悄丢掉程序条件的后果是这条规则
        // 对**所有**程序的入站 UDP 都生效，比用户要的宽得多，而且下发是成功的。
        return Result<void>::fail(
            unsupportedError(QStringLiteral("对入站 UDP 按程序限定的规则"),
                             QStringLiteral("Windows 的 UDP 数据报层没有程序标识这个条件，"
                                            "只有接收授权层有。请把这条规则的方向改成只看出站，"
                                            "或者改用地址、端口来限定范围")));
      }

      auto blob = appIdFor(draft, condition.appPath);
      if (!blob) {
        return Result<void>::fail(blob.error());
      }
      wfp.fieldKey = conditionKeyFor(FilterField::AppPath, entry.stage);
      wfp.matchType = condition.negate ? FWP_MATCH_NOT_EQUAL : FWP_MATCH_EQUAL;
      wfp.conditionValue.type = FWP_BYTE_BLOB_TYPE;
      wfp.conditionValue.byteBlob = blob.value();
      break;
    }

    case FilterField::RemoteAddress: {
      FWP_RANGE0 range{};
      auto lower = fillAddressValue(draft, condition.address.lower, range.valueLow);
      if (!lower) {
        return Result<void>::fail(lower.error());
      }
      auto upper = fillAddressValue(draft, condition.address.upper, range.valueHigh);
      if (!upper) {
        return Result<void>::fail(upper.error());
      }
      draft.ranges.append(range);

      wfp.fieldKey = conditionKeyFor(FilterField::RemoteAddress, entry.stage);
      // 单个地址也走区间：两端相等。统一成一种形状，就少一类「单值那条忘了改」的错误。
      wfp.matchType = FWP_MATCH_RANGE;
      wfp.conditionValue.type = FWP_RANGE_TYPE;
      wfp.conditionValue.rangeValue = &draft.ranges.last();
      break;
    }

    case FilterField::Port: {
      FWP_RANGE0 range{};
      range.valueLow.type = FWP_UINT16;
      range.valueLow.uint16 = condition.portLower;
      range.valueHigh.type = FWP_UINT16;
      range.valueHigh.uint16 = condition.portUpper;
      draft.ranges.append(range);

      wfp.fieldKey = conditionKeyFor(FilterField::Port, entry.stage);
      wfp.matchType = FWP_MATCH_RANGE;
      wfp.conditionValue.type = FWP_RANGE_TYPE;
      wfp.conditionValue.rangeValue = &draft.ranges.last();
      break;
    }

    case FilterField::Protocol: {
      wfp.fieldKey = conditionKeyFor(FilterField::Protocol, entry.stage);
      wfp.matchType = FWP_MATCH_EQUAL;
      wfp.conditionValue.type = FWP_UINT8;
      wfp.conditionValue.uint8 = condition.protocol == TransportProtocol::Tcp ? 6U : 17U;
      break;
    }
  }

  draft.conditions.append(wfp);
  return Result<void>::ok();
}

/// 把一条条目翻成一条 WFP 过滤器并加进当前事务。
///
/// 一条条目对一条过滤器：多地址、多端口、多程序路径都放在同一字段的多个条件里，
/// 语义是「或」（实测确认，见 filter_plan.h 顶部）。所以条数就是条目数，
/// 不需要在这里再拆。
///
/// 调用方负责开事务与提交 —— 本函数只管「加一条」。
Result<unsigned long long> addFilterForEntry(
    HANDLE engine, const RuleId& ruleId, const FilterPlanEntry& entry, int index, int total) {
  const DraftSizes sizes = measureConditions(entry.conditions);

  FilterDraft draft;
  // 先把取值池按算好的个数留够，之后填进去的元素地址才稳定。
  draft.ranges.reserve(sizes.ranges);
  draft.addresses.reserve(sizes.addresses);

  for (const FilterCondition& condition : entry.conditions) {
    auto appended = appendWfpCondition(draft, entry, condition);
    if (!appended) {
      return Result<unsigned long long>::fail(appended.error());
    }
  }

  draft.name = ruleDisplayName(ruleId);
  draft.description =
      QStringLiteral("BanIPHelper 规则 %1 第 %2/%3 条：%4 %5")
          .arg(ruleId)
          .arg(index + 1)
          .arg(total)
          .arg(stageTitle(entry.stage),
               entry.family == AddressFamily::V6 ? QStringLiteral("IPv6") : QStringLiteral("IPv4"));

  draft.providerKey = kProviderKey;
  draft.subLayerKey = kSubLayerKey;

  draft.filter.filterKey = nextFilterKey();
  draft.filter.displayData.name =
      const_cast<wchar_t*>(reinterpret_cast<const wchar_t*>(draft.name.utf16()));
  draft.filter.displayData.description =
      const_cast<wchar_t*>(reinterpret_cast<const wchar_t*>(draft.description.utf16()));
  draft.filter.flags = 0;
  draft.filter.providerKey = &draft.providerKey;
  draft.filter.layerKey = layerKeyFor(entry.stage, entry.family);
  draft.filter.subLayerKey = draft.subLayerKey;
  draft.filter.weight = weightFor(entry.priority);
  draft.filter.numFilterConditions = static_cast<UINT32>(draft.conditions.size());
  draft.filter.filterCondition = draft.conditions.data();
  draft.filter.action.type =
      entry.action == RuleAction::Allow ? FWP_ACTION_PERMIT : FWP_ACTION_BLOCK;

  UINT64 filterId = 0;
  const DWORD result = ::FwpmFilterAdd0(engine, &draft.filter, nullptr, &filterId);
  if (result != ERROR_SUCCESS) {
    return Result<unsigned long long>::fail(
        platformError(QStringLiteral("下发规则「%1」的第 %2 条过滤器").arg(ruleId).arg(index + 1),
                      result,
                      L"FwpmFilterAdd0"));
  }

  return static_cast<unsigned long long>(filterId);
}

/// 枚举本工具名下的全部过滤器。
///
/// 必须**逐层**枚举：枚举模板要同时给出 providerKey 与 layerKey，只给 provider
/// 会被 WFP 以 `FWP_E_LAYER_NOT_FOUND` 拒绝（S1.7 实测）。层清单由
/// `enumAllLayerKeys` 每次问系统要，不写死 —— 漏一层就等于那一层上的过滤器
/// 永远清不掉，而且清理还会报成功。
Result<QList<OwnFilterRecord>> enumOwnFilters(HANDLE engine) {
  const Result<QList<GUID>> layers = enumAllLayerKeys(engine);
  if (!layers) {
    return Result<QList<OwnFilterRecord>>::fail(layers.error());
  }

  QList<OwnFilterRecord> records;
  constexpr UINT32 kBatch = 64;

  for (const GUID& layerKey : layers.value()) {
    GUID ownProviderKey = kProviderKey;
    GUID layer = layerKey;

    FWPM_FILTER_ENUM_TEMPLATE0 enumTemplate{};
    enumTemplate.providerKey = &ownProviderKey;
    enumTemplate.layerKey = layer;
    enumTemplate.actionMask = 0xFFFFFFFF;

    HANDLE enumHandle = nullptr;
    const DWORD created = ::FwpmFilterCreateEnumHandle0(engine, &enumTemplate, &enumHandle);

    if (created == kFwpLayerNotFound) {
      // 刚问出来的层，这里却说不存在，说明它在这几毫秒里被卸载了。
      // 跳过即可，不该因为一个正在消失的层让整次枚举失败。
      continue;
    }
    if (created != ERROR_SUCCESS) {
      return Result<QList<OwnFilterRecord>>::fail(platformError(
          QStringLiteral("创建过滤器枚举句柄"), created, L"FwpmFilterCreateEnumHandle0"));
    }

    for (;;) {
      FWPM_FILTER0** entries = nullptr;
      UINT32 returned = 0;

      const DWORD result = ::FwpmFilterEnum0(engine, enumHandle, kBatch, &entries, &returned);
      if (result != ERROR_SUCCESS) {
        ::FwpmFilterDestroyEnumHandle0(engine, enumHandle);
        return Result<QList<OwnFilterRecord>>::fail(
            platformError(QStringLiteral("枚举自有过滤器"), result, L"FwpmFilterEnum0"));
      }

      if (returned == 0) {
        break;
      }

      for (UINT32 index = 0; index < returned; ++index) {
        const FWPM_FILTER0* filter = entries[index];
        OwnFilterRecord record;
        record.filterId = filter->filterId;
        record.ruleId = ruleIdOfDisplayName(filter->displayData.name);
        record.action = (static_cast<UINT32>(filter->action.type) & 0x00000FFFU) == 0x00000002U
                            ? RuleAction::Allow
                            : RuleAction::Block;
        records.append(record);
      }

      ::FwpmFreeMemory0(reinterpret_cast<void**>(&entries));

      if (returned < kBatch) {
        break;
      }
    }

    ::FwpmFilterDestroyEnumHandle0(engine, enumHandle);
  }

  return Result<QList<OwnFilterRecord>>::ok(records);
}

/// 删除本工具名下满足条件的过滤器，返回删掉的条数。
///
/// `ruleId` 为空表示全删。
///
/// ⚠️ 本函数**不开事务**，由调用方负责。WFP 不支持嵌套事务，
/// 而下发规则那一步本身就要一个事务把「删旧的」与「加新的」包在一起。
Result<int> removeOwnFilters(HANDLE engine, const QString& ruleId) {
  const Result<QList<OwnFilterRecord>> records = enumOwnFilters(engine);
  if (!records) {
    return Result<int>::fail(records.error());
  }

  int removed = 0;
  for (const OwnFilterRecord& record : records.value()) {
    if (!ruleId.isEmpty() && record.ruleId != ruleId) {
      continue;
    }

    const DWORD result = ::FwpmFilterDeleteById0(engine, record.filterId);
    if (result == ERROR_SUCCESS) {
      ++removed;
      continue;
    }

    // 失败时要说清删到哪一条、还剩多少：只剩一句「删除失败」的话，
    // 使用者无从判断现在的系统里是「一条规则生效」还是「半条」。
    return Result<int>::fail(
        makeError(ErrorCode::Platform,
                  QStringLiteral("删除自有过滤器失败（已删除 %1 条，共 %2 条）")
                      .arg(removed)
                      .arg(records.value().size()),
                  static_cast<std::int32_t>(result),
                  QStringLiteral("FwpmFilterDeleteById0")));
  }

  return Result<int>::ok(removed);
}

/// 开一个事务并把 `body` 放进去跑；`body` 失败或提交失败都整体回滚。
///
/// 事务是「失败不留半成品」的唯一手段：一条规则会展开成好几条过滤器，
/// 逐条提交的话中途失败会留下「封了一半」，而使用者看到的是失败俩字 ——
/// 那比全都没封更危险，因为网络已经被封了一部分却没人知道。
Result<void> inTransaction(HANDLE engine,
                           const QString& what,
                           const std::function<Result<void>()>& body) {
  const DWORD begin = ::FwpmTransactionBegin0(engine, 0);
  if (begin != ERROR_SUCCESS) {
    return Result<void>::fail(platformError(
        QStringLiteral("开启过滤器事务（%1）").arg(what), begin, L"FwpmTransactionBegin0"));
  }

  const Result<void> outcome = body();
  if (!outcome) {
    ::FwpmTransactionAbort0(engine);
    return outcome;
  }

  const DWORD commit = ::FwpmTransactionCommit0(engine);
  if (commit != ERROR_SUCCESS) {
    ::FwpmTransactionAbort0(engine);
    return Result<void>::fail(platformError(
        QStringLiteral("提交过滤器事务（%1）").arg(what), commit, L"FwpmTransactionCommit0"));
  }

  return Result<void>::ok();
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

Result<CleanupReport> WinFilterEngine::cleanupOrphans() {
  if (!isOpen()) {
    return Result<CleanupReport>::fail(makeError(
        ErrorCode::InvalidArgument, QStringLiteral("过滤器引擎尚未打开，无法清理残留过滤器")));
  }

  HANDLE engine = toHandle(engine_);
  int removed = 0;

  const Result<void> cleaned = inTransaction(engine, QStringLiteral("清理残留过滤器"), [&]() {
    auto count = removeOwnFilters(engine, QString());
    if (!count) {
      return Result<void>::fail(count.error());
    }
    removed = count.value();
    return Result<void>::ok();
  });
  if (!cleaned) {
    return Result<CleanupReport>::fail(cleaned.error());
  }

  CleanupReport report;
  report.removedOwn = removed;

  // 恒为 0，而且这是**结构性**的 0 而不是「恰好没遇到」：枚举按自有 provider 限定，
  // 他方过滤器根本不会被选出来。这正是「绝不按层全量清理」的落地方式。
  report.skippedForeign = 0;

  return Result<CleanupReport>::ok(report);
}

Result<void> WinFilterEngine::revokeAll() {
  if (!isOpen()) {
    return Result<void>::fail(makeError(ErrorCode::InvalidArgument,
                                        QStringLiteral("过滤器引擎尚未打开，无法撤销过滤器")));
  }

  HANDLE engine = toHandle(engine_);
  return inTransaction(engine, QStringLiteral("撤销全部过滤器"), [&]() {
    auto removed = removeOwnFilters(engine, QString());
    if (!removed) {
      return Result<void>::fail(removed.error());
    }
    return Result<void>::ok();
  });
}

Result<void> WinFilterEngine::applyRule(const RuleSpec& rule) {
  if (!isOpen()) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("过滤器引擎尚未打开，无法下发规则")));
  }
  if (rule.id.isEmpty()) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("规则标识不能为空")));
  }

  auto plan = expandRule(rule);
  if (!plan) {
    return Result<void>::fail(plan.error());
  }

  // 翻译与求补都在**动系统之前**做完。翻译失败与「已经删了旧的却没加上新的」
  // 是两回事：前者不该惊动系统，一次都不该。
  QList<FilterPlanEntry> resolved;
  resolved.reserve(plan.value().entries.size());
  for (const FilterPlanEntry& entry : plan.value().entries) {
    auto resolvedEntry = resolveNegation(entry);
    if (!resolvedEntry) {
      return Result<void>::fail(resolvedEntry.error());
    }
    resolved.append(resolvedEntry.value());
  }

  HANDLE engine = toHandle(engine_);
  const int total = static_cast<int>(resolved.size());

  return inTransaction(engine, QStringLiteral("下发规则「%1」").arg(rule.id), [&]() {
    // 先删掉这条规则上一次下发的过滤器，再下发新的，顺序不能反。
    //
    // 幂等就是靠这一步：重复下发同一条规则时，旧的那份必须**先消失**。
    // 换成「先加后删」，中间那一瞬间新旧两份会同时生效，而用户改规则的目的
    // 多半是收窄封禁范围 —— 那一瞬间会封得比预期宽，而且只在重下发时出现，
    // 极难复现。
    auto removed = removeOwnFilters(engine, rule.id);
    if (!removed) {
      return Result<void>::fail(removed.error());
    }

    for (int index = 0; index < total; ++index) {
      auto added = addFilterForEntry(engine, rule.id, resolved.at(index), index, total);
      if (!added) {
        return Result<void>::fail(added.error());
      }
    }

    return Result<void>::ok();
  });
}

Result<void> WinFilterEngine::revokeRule(const RuleId& id) {
  if (!isOpen()) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("过滤器引擎尚未打开，无法撤销规则")));
  }
  if (id.isEmpty()) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("规则标识不能为空")));
  }

  HANDLE engine = toHandle(engine_);

  // 没下发过的标识也走同一条路：枚举之后一条都匹配不上，删掉 0 条即成功。
  // 接口要求这个操作幂等，所以不为「找不到」单独开一条分支 ——
  // 那会让「已删除」与「从未存在」两条路径产生不同的结果。
  return inTransaction(engine, QStringLiteral("撤销规则「%1」").arg(id), [&]() {
    auto removed = removeOwnFilters(engine, id);
    if (!removed) {
      return Result<void>::fail(removed.error());
    }
    return Result<void>::ok();
  });
}

Result<QList<AppliedRuleSummary>> WinFilterEngine::appliedRules() const {
  if (!isOpen()) {
    return Result<QList<AppliedRuleSummary>>::fail(makeError(
        ErrorCode::InvalidArgument, QStringLiteral("过滤器引擎尚未打开，无法查询已生效规则")));
  }

  auto records = enumOwnFilters(toHandle(engine_));
  if (!records) {
    return Result<QList<AppliedRuleSummary>>::fail(records.error());
  }

  // 按规则标识归并，保持首次出现的顺序。用有序列而不是哈希表：
  // 同一次查询返回的顺序应当稳定，界面与测试都靠它。
  QList<AppliedRuleSummary> summaries;
  for (const OwnFilterRecord& record : records.value()) {
    if (record.ruleId.isEmpty()) {
      // 自家的、但不属于任何一条规则的过滤器（例如白名单的默认阻断），
      // 不能被算成「某条规则生效了」。
      continue;
    }

    bool merged = false;
    for (AppliedRuleSummary& summary : summaries) {
      if (summary.id == record.ruleId) {
        ++summary.filterCount;
        merged = true;
        break;
      }
    }
    if (merged) {
      continue;
    }

    AppliedRuleSummary summary;
    summary.id = record.ruleId;
    summary.action = record.action;
    summary.filterCount = 1;
    summary.active = true;
    summaries.append(summary);
  }

  return Result<QList<AppliedRuleSummary>>::ok(summaries);
}

// 下面这三个属于白名单模式的默认阻断（阶段四 S4.1），不是 S2.4 的范围。
// 在实现之前一律明确报「不支持」，与能力声明保持一致：
// 声明说不支持，接口就必须失败，不允许悄悄成功。
namespace {

Error defaultBlockNotImplementedYet(const QString& operation) {
  return unsupportedError(operation, QStringLiteral("白名单模式的默认阻断尚未实现，阶段四 S4.1"));
}

}  // namespace

Result<void> WinFilterEngine::applyDefaultBlock(std::int32_t priority) {
  Q_UNUSED(priority);
  return Result<void>::fail(defaultBlockNotImplementedYet(QStringLiteral("下发默认阻断")));
}

Result<bool> WinFilterEngine::hasDefaultBlock() const {
  return Result<bool>::fail(defaultBlockNotImplementedYet(QStringLiteral("查询默认阻断状态")));
}

Result<void> WinFilterEngine::revokeDefaultBlock() {
  return Result<void>::fail(defaultBlockNotImplementedYet(QStringLiteral("撤销默认阻断")));
}

}  // namespace baniphelper::core
