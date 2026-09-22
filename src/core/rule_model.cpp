// 规则模型的实现（阶段二 S2.1）。
//
// 这个文件只回答两个问题：**这条规则合法吗**，以及**它有多具体、有多宽**。
// 它不下发过滤器、不判定连接是否命中、不碰数据库 —— 那分别是 S2.2、S2.4、S2.9 的事。
//
// 一条贯穿全文件的原则：**宁可拒绝，也不宽松解释**。
// 规则被拒绝的代价是用户改一下再存；规则被按错语义接受的代价是断网。
// 所以遇到「看起来像但不确定」的输入一律失败，并且错误信息要说清哪里不对、怎么改。

#include "core/rule_model.h"

#include <QAbstractSocket>
#include <QDir>
#include <QHostAddress>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <utility>

#include "core/error.h"

namespace baniphelper::core {
namespace {

// ---------------------------------------------------------------------------
// (域, 方式) 定义表
// ---------------------------------------------------------------------------

/// 具体度分档按 architecture.md 第 4.6 节的顺序，档差固定 100。
///
/// 分档刻意拉开到 100：取反加成只有 1，所以「取反略高于同方式」永远
/// 抬不过一档，也就不会出现「加了取反就比更具体的方式还优先」这种反直觉结果。
constexpr ModeSpec kModeSpecs[] = {
    // 程序域：exact > set > dir > wildcard > any。
    {MatchDomain::Proc, MatchMode::Exact, ValueShape::ExecutablePath, 1000, false},
    {MatchDomain::Proc, MatchMode::Set, ValueShape::ExecutablePath, 800, false},
    {MatchDomain::Proc, MatchMode::Dir, ValueShape::DirectoryPath, 600, false},
    {MatchDomain::Proc, MatchMode::Wildcard, ValueShape::WildcardPath, 400, false},
    {MatchDomain::Proc, MatchMode::Any, ValueShape::None, 0, true},

    // 地址域：范围比网段具体，因为它把两端都限死了。
    {MatchDomain::Address, MatchMode::Exact, ValueShape::AddressLiteral, 1000, false},
    {MatchDomain::Address, MatchMode::Range, ValueShape::AddressRange, 900, false},
    {MatchDomain::Address, MatchMode::Cidr, ValueShape::Subnet, 800, false},
    {MatchDomain::Address, MatchMode::Any, ValueShape::None, 0, true},

    {MatchDomain::Port, MatchMode::Exact, ValueShape::PortNumber, 1000, false},
    {MatchDomain::Port, MatchMode::Set, ValueShape::PortNumber, 900, false},
    {MatchDomain::Port, MatchMode::Range, ValueShape::PortRange, 800, false},
    {MatchDomain::Port, MatchMode::Any, ValueShape::None, 0, true},

    // 协议域与方向域：取值就是方式本身，条件不带额外取值。
    {MatchDomain::Protocol, MatchMode::Tcp, ValueShape::None, 1000, false},
    {MatchDomain::Protocol, MatchMode::Udp, ValueShape::None, 1000, false},
    {MatchDomain::Protocol, MatchMode::Any, ValueShape::None, 0, true},

    {MatchDomain::Direction, MatchMode::Out, ValueShape::None, 1000, false},
    {MatchDomain::Direction, MatchMode::In, ValueShape::None, 1000, false},
    {MatchDomain::Direction, MatchMode::Both, ValueShape::None, 0, true},
};

/// 该方式是否表示「不限定取值」。
///
/// `any` 与 `both` 都属于这一类：前者不限取值集合，后者不限方向。
bool matchesEverything(MatchMode mode) {
  return mode == MatchMode::Any || mode == MatchMode::Both;
}

// ---------------------------------------------------------------------------
// 取值解析
// ---------------------------------------------------------------------------

/// 解析 IP 地址字面量。
///
/// 用 `QHostAddress` 而不是自己写解析器：IPv6 的写法太多（压缩零段、内嵌 IPv4、
/// 十六进制大小写），自己写容易在边界上出错，而地址解析错的后果是**静默封错范围**。
/// `QHostAddress` 是 QtNetwork 里的跨平台值类型，不涉及任何平台专有头，
/// 所以放在核心层不违反 S1.10 立下的隔离约束。
Result<QHostAddress> parseAddressLiteral(const QString& text) {
  if (text.isEmpty()) {
    return makeError(ErrorCode::InvalidArgument, QStringLiteral("地址不能为空"));
  }
  if (text.contains(QLatin1Char('%'))) {
    // 带作用域的地址（例如 fe80::1%12）只在链路本地场景有意义，过滤条件里的地址不带它。
    // 与其悄悄丢掉作用域，不如当场拒绝：用户看到拒绝会去查，看到"成功"不会。
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("地址不能带作用域标识（百分号）：%1").arg(text));
  }

  QHostAddress address;
  if (!address.setAddress(text)) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("不是合法的 IP 地址：%1").arg(text));
  }
  return address;
}

/// 地址的定长字节表示，用于比较大小。
///
/// 两种地址族分别 4 字节与 16 字节，同族长度的字节串按字典序比就是按数值比，
/// 所以范围校验不需要再分族写两套比较逻辑。
QByteArray addressBytes(const QHostAddress& address) {
  if (address.protocol() == QAbstractSocket::IPv6Protocol) {
    const Q_IPV6ADDR value = address.toIPv6Address();
    QByteArray bytes(16, Qt::Uninitialized);
    for (int i = 0; i < 16; ++i) {
      bytes[i] = static_cast<char>(value.c[i]);
    }
    return bytes;
  }

  const quint32 value = address.toIPv4Address();
  QByteArray bytes(4, Qt::Uninitialized);
  bytes[0] = static_cast<char>((value >> 24) & 0xFFU);
  bytes[1] = static_cast<char>((value >> 16) & 0xFFU);
  bytes[2] = static_cast<char>((value >> 8) & 0xFFU);
  bytes[3] = static_cast<char>(value & 0xFFU);
  return bytes;
}

/// `addressBytes` 的逆向：把定长字节串还原成地址。
///
/// 网段的两个端点是用字节位运算算出来的，得再包回地址值类型才能交给上层。
Result<Address> addressFromBytes(const QByteArray& bytes) {
  if (bytes.size() != 4 && bytes.size() != 16) {
    return makeError(ErrorCode::Internal,
                     QStringLiteral("地址字节数只能是 4 或 16，给的是 %1").arg(bytes.size()));
  }

  Address address;
  if (bytes.size() == 4) {
    const quint32 raw = (static_cast<quint32>(static_cast<quint8>(bytes.at(0))) << 24) |
                        (static_cast<quint32>(static_cast<quint8>(bytes.at(1))) << 16) |
                        (static_cast<quint32>(static_cast<quint8>(bytes.at(2))) << 8) |
                        static_cast<quint32>(static_cast<quint8>(bytes.at(3)));
    address.family = AddressFamily::V4;
    address.text = QHostAddress(raw).toString();
    return address;
  }

  Q_IPV6ADDR raw{};
  for (int i = 0; i < 16; ++i) {
    raw.c[i] = static_cast<quint8>(bytes.at(i));
  }
  address.family = AddressFamily::V6;
  address.text = QHostAddress(raw).toString();
  return address;
}

QString addressFamilyTitle(const QHostAddress& address) {
  return address.protocol() == QAbstractSocket::IPv6Protocol ? QStringLiteral("IPv6")
                                                             : QStringLiteral("IPv4");
}

Result<QString> normalizeAddressLiteral(const QString& text) {
  auto address = parseAddressLiteral(text);
  if (!address) {
    return Result<QString>::fail(address.error());
  }
  // toString() 输出标准形式：IPv4 点分十进制，IPv6 走 RFC 5952 的压缩冒分十六进制。
  return address.value().toString();
}

Result<QString> normalizeSubnet(const QString& text) {
  const int slash = text.indexOf(QLatin1Char('/'));
  if (slash < 0) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("网段要写成「地址/前缀长度」，例如 1.2.3.0/24：%1").arg(text));
  }

  auto address = parseAddressLiteral(text.left(slash));
  if (!address) {
    return Result<QString>::fail(address.error());
  }

  const QString prefixText = text.mid(slash + 1);
  if (prefixText.isEmpty()) {
    return makeError(ErrorCode::InvalidArgument, QStringLiteral("网段缺少前缀长度：%1").arg(text));
  }
  for (const QChar ch : prefixText) {
    if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) {
      return makeError(ErrorCode::InvalidArgument,
                       QStringLiteral("前缀长度必须是十进制数字：%1").arg(text));
    }
  }
  // 先看位数再转数字：超长数字串会让 toInt 溢出并静默返回 0，
  // 那会把一个明显的错误输入变成「前缀长度 0」，也就是悄悄放大了整条规则。
  if (prefixText.size() > 3) {
    return makeError(ErrorCode::InvalidArgument, QStringLiteral("前缀长度超出范围：%1").arg(text));
  }

  const int prefix = prefixText.toInt();
  const int maxPrefix = address.value().protocol() == QAbstractSocket::IPv6Protocol ? 128 : 32;
  if (prefix > maxPrefix) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("%1 的前缀长度最多 %2，给的是 %3")
                         .arg(addressFamilyTitle(address.value()))
                         .arg(maxPrefix)
                         .arg(prefix));
  }

  // 前缀长度统一成十进制无前导零写法，免得同一个网段有两种写法。
  return QStringLiteral("%1/%2").arg(address.value().toString()).arg(prefix);
}

Result<QString> normalizeAddressRange(const QString& text) {
  const int dash = text.indexOf(QLatin1Char('-'));
  if (dash < 0) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("地址范围要写成「起-止」，例如 1.2.3.4-1.2.3.9：%1").arg(text));
  }

  auto lower = parseAddressLiteral(text.left(dash));
  if (!lower) {
    return Result<QString>::fail(lower.error());
  }
  auto upper = parseAddressLiteral(text.mid(dash + 1));
  if (!upper) {
    return Result<QString>::fail(upper.error());
  }

  if (lower.value().protocol() != upper.value().protocol()) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("地址范围的两端必须是同一地址族：%1").arg(text));
  }
  if (addressBytes(upper.value()) < addressBytes(lower.value())) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("地址范围的起点不能大于终点：%1").arg(text));
  }

  return QStringLiteral("%1-%2").arg(lower.value().toString(), upper.value().toString());
}

/// 解析端口号。端口在过滤条件里是 16 位无符号数，0 也是合法取值。
Result<int> parsePortNumber(const QString& text) {
  if (text.isEmpty()) {
    return makeError(ErrorCode::InvalidArgument, QStringLiteral("端口不能为空"));
  }
  for (const QChar ch : text) {
    if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) {
      return makeError(ErrorCode::InvalidArgument,
                       QStringLiteral("端口必须是十进制数字：%1").arg(text));
    }
  }
  if (text.size() > 5) {
    return makeError(ErrorCode::InvalidArgument, QStringLiteral("端口超出范围：%1").arg(text));
  }

  const int port = text.toInt();
  if (port > 65535) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("端口最多 65535，给的是 %1").arg(port));
  }
  return port;
}

Result<QString> normalizePortNumber(const QString& text) {
  auto port = parsePortNumber(text);
  if (!port) {
    return Result<QString>::fail(port.error());
  }
  return QString::number(port.value());
}

Result<QString> normalizePortRange(const QString& text) {
  const int dash = text.indexOf(QLatin1Char('-'));
  if (dash < 0) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("端口范围要写成「起-止」，例如 8000-8100：%1").arg(text));
  }

  auto lower = parsePortNumber(text.left(dash));
  if (!lower) {
    return Result<QString>::fail(lower.error());
  }
  auto upper = parsePortNumber(text.mid(dash + 1));
  if (!upper) {
    return Result<QString>::fail(upper.error());
  }
  if (upper.value() < lower.value()) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("端口范围的起点不能大于终点：%1").arg(text));
  }

  return QStringLiteral("%1-%2").arg(lower.value()).arg(upper.value());
}

/// 路径里的非法字符检查。
///
/// ⚠️ 这里用的是 **Windows 的文件名规则**（禁止 `"<>|` 与通配字符），
/// 把平台差异放进了核心层，是全项目里少数几处例外之一。
/// 理由是这个判断要在「用户敲完就报错」的时刻给出，而那时还没有任何平台调用；
/// 真的做别的平台（S8）时，这里要按平台替换成对应的规则，或者下沉成平台接口。
///
/// `*` 与 `?` 在 Windows 上是通配字符，普通路径里出现它们一定是写错了；
/// 通配模式单独放宽这两个。
Result<void> checkPathCharacters(const QString& text, bool allowWildcards) {
  const QString forbidden = allowWildcards ? QStringLiteral("\"<>|") : QStringLiteral("\"<>|*?");
  for (const QChar ch : text) {
    if (forbidden.contains(ch)) {
      return makeError(ErrorCode::InvalidArgument,
                       QStringLiteral("路径里不能出现「%1」：%2").arg(ch).arg(text));
    }
    if (ch.unicode() < 0x20) {
      return makeError(ErrorCode::InvalidArgument, QStringLiteral("路径里不能出现控制字符：%1"));
    }
  }
  return Result<void>::ok();
}

// 路径的三个模式都**只校验、不改写**：用户写什么就存什么。
//
// 曾经这里做过归一化（先是一段手写的「去尾部分隔符」，后来换成 `QDir::cleanPath`），
// 两次都不对。手写那版把盘符判断这种 Windows 专有的东西带进了核心层；
// 换成 `cleanPath` 之后虽然把两种分隔符写法收敛了，但代价是**用户写的路径被改掉了**：
// 界面上变成正斜杠形式，`..` 被解开，数据一旦存进去就回不到用户写的样子。
//
// 正确的分界是：**存储只判断「合不合法」，比较才归一**（见 `sameProcessIdentity`）。
// 「等价」本来就依赖平台，而且只在该判断的时候才需要判断。

Result<QString> normalizeExecutablePath(const QString& text) {
  if (text.isEmpty()) {
    return makeError(ErrorCode::InvalidArgument, QStringLiteral("可执行文件路径不能为空"));
  }
  if (!QDir::isAbsolutePath(text)) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("可执行文件必须写完整路径：%1").arg(text));
  }
  auto characters = checkPathCharacters(text, false);
  if (!characters) {
    return Result<QString>::fail(characters.error());
  }
  if (text.endsWith(QLatin1Char('\\')) || text.endsWith(QLatin1Char('/'))) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("这是目录而不是可执行文件：%1").arg(text));
  }
  return text;
}

Result<QString> normalizeWildcardPath(const QString& text) {
  if (text.isEmpty()) {
    return makeError(ErrorCode::InvalidArgument, QStringLiteral("通配模式不能为空"));
  }
  auto characters = checkPathCharacters(text, true);
  if (!characters) {
    return Result<QString>::fail(characters.error());
  }
  return text;
}

Result<QString> normalizeDirectoryPath(const QString& text) {
  if (text.isEmpty()) {
    return makeError(ErrorCode::InvalidArgument, QStringLiteral("目录路径不能为空"));
  }
  if (!QDir::isAbsolutePath(text)) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("目录必须写完整路径：%1").arg(text));
  }
  auto characters = checkPathCharacters(text, false);
  if (!characters) {
    return Result<QString>::fail(characters.error());
  }
  return text;
}

Result<QString> normalizeValueForShape(ValueShape shape, const QString& value) {
  switch (shape) {
    case ValueShape::None:
      return makeError(ErrorCode::InvalidArgument, QStringLiteral("这个匹配方式不接受取值"));
    case ValueShape::ExecutablePath:
      return normalizeExecutablePath(value);
    case ValueShape::WildcardPath:
      return normalizeWildcardPath(value);
    case ValueShape::DirectoryPath:
      return normalizeDirectoryPath(value);
    case ValueShape::AddressLiteral:
      return normalizeAddressLiteral(value);
    case ValueShape::Subnet:
      return normalizeSubnet(value);
    case ValueShape::AddressRange:
      return normalizeAddressRange(value);
    case ValueShape::PortNumber:
      return normalizePortNumber(value);
    case ValueShape::PortRange:
      return normalizePortRange(value);
  }
  return makeError(ErrorCode::Internal, QStringLiteral("没有处理的取值形状"));
}

QString joinedCodes(const QList<const char*>& codes) {
  QStringList list;
  for (const char* code : codes) {
    list.append(QString::fromLatin1(code));
  }
  return list.join(QStringLiteral("、"));
}

/// 通配模式是否过于宽泛。
///
/// 两条判据：去掉通配符后不剩任何字面字符（`*`、`?*` 这类，等于「任意文件」），
/// 或者模式里没有目录分隔符（`*.exe` 会命中所有目录下的同名文件）。
bool hasLooseWildcard(const QStringList& patterns) {
  for (const QString& pattern : patterns) {
    QString literalPart;
    for (const QChar ch : pattern) {
      if (ch != QLatin1Char('*') && ch != QLatin1Char('?')) {
        literalPart.append(ch);
      }
    }
    if (literalPart.trimmed().isEmpty()) {
      return true;
    }
    if (!pattern.contains(QLatin1Char('/')) && !pattern.contains(QLatin1Char('\\'))) {
      return true;
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// 域与方式
// ---------------------------------------------------------------------------

const char* domainCode(MatchDomain domain) noexcept {
  switch (domain) {
    case MatchDomain::Proc:
      return "proc";
    case MatchDomain::Address:
      return "addr";
    case MatchDomain::Port:
      return "port";
    case MatchDomain::Protocol:
      return "proto";
    case MatchDomain::Direction:
      return "direction";
  }
  return "";
}

QString domainTitle(MatchDomain domain) {
  switch (domain) {
    case MatchDomain::Proc:
      return QStringLiteral("程序");
    case MatchDomain::Address:
      return QStringLiteral("地址");
    case MatchDomain::Port:
      return QStringLiteral("端口");
    case MatchDomain::Protocol:
      return QStringLiteral("协议");
    case MatchDomain::Direction:
      return QStringLiteral("方向");
  }
  return QStringLiteral("未知域");
}

Result<MatchDomain> parseDomain(const QString& code) {
  for (const MatchDomain candidate : kAllMatchDomains) {
    if (code.compare(QLatin1String(domainCode(candidate)), Qt::CaseInsensitive) == 0) {
      return candidate;
    }
  }

  QList<const char*> codes;
  for (const MatchDomain candidate : kAllMatchDomains) {
    codes.append(domainCode(candidate));
  }
  return makeError(
      ErrorCode::InvalidArgument,
      QStringLiteral("不认识的匹配域「%1」。已知的域有：%2").arg(code, joinedCodes(codes)));
}

const char* modeCode(MatchMode mode) noexcept {
  switch (mode) {
    case MatchMode::Exact:
      return "exact";
    case MatchMode::Set:
      return "set";
    case MatchMode::Any:
      return "any";
    case MatchMode::Wildcard:
      return "wildcard";
    case MatchMode::Dir:
      return "dir";
    case MatchMode::Cidr:
      return "cidr";
    case MatchMode::Range:
      return "range";
    case MatchMode::Tcp:
      return "tcp";
    case MatchMode::Udp:
      return "udp";
    case MatchMode::Out:
      return "out";
    case MatchMode::In:
      return "in";
    case MatchMode::Both:
      return "both";
  }
  return "";
}

QString modeTitle(MatchMode mode) {
  switch (mode) {
    case MatchMode::Exact:
      return QStringLiteral("精确");
    case MatchMode::Set:
      return QStringLiteral("集合");
    case MatchMode::Any:
      return QStringLiteral("全部");
    case MatchMode::Wildcard:
      return QStringLiteral("通配");
    case MatchMode::Dir:
      return QStringLiteral("目录");
    case MatchMode::Cidr:
      return QStringLiteral("网段");
    case MatchMode::Range:
      return QStringLiteral("范围");
    case MatchMode::Tcp:
      return QStringLiteral("TCP");
    case MatchMode::Udp:
      return QStringLiteral("UDP");
    case MatchMode::Out:
      return QStringLiteral("出站");
    case MatchMode::In:
      return QStringLiteral("入站");
    case MatchMode::Both:
      return QStringLiteral("出入站");
  }
  return QStringLiteral("未知方式");
}

Result<MatchMode> parseMode(const QString& code) {
  for (const MatchMode candidate : kAllMatchModes) {
    if (code.compare(QLatin1String(modeCode(candidate)), Qt::CaseInsensitive) == 0) {
      return candidate;
    }
  }

  QList<const char*> codes;
  for (const MatchMode candidate : kAllMatchModes) {
    codes.append(modeCode(candidate));
  }
  return makeError(
      ErrorCode::InvalidArgument,
      QStringLiteral("不认识的匹配方式「%1」。已知的方式有：%2").arg(code, joinedCodes(codes)));
}

Result<ModeSpec> findModeSpec(MatchDomain domain, MatchMode mode) {
  for (const ModeSpec& spec : kModeSpecs) {
    if (spec.domain == domain && spec.mode == mode) {
      return spec;
    }
  }

  QStringList supported;
  for (const MatchMode candidate : modesOf(domain)) {
    supported.append(QString::fromLatin1(modeCode(candidate)));
  }
  return makeError(ErrorCode::InvalidArgument,
                   QStringLiteral("「%1」域不支持「%2」匹配。它支持的方式有：%3")
                       .arg(domainTitle(domain),
                            QString::fromLatin1(modeCode(mode)),
                            supported.join(QStringLiteral("、"))));
}

QList<MatchMode> modesOf(MatchDomain domain) {
  QList<MatchMode> modes;
  for (const ModeSpec& spec : kModeSpecs) {
    if (spec.domain == domain) {
      modes.append(spec.mode);
    }
  }
  return modes;
}

// ---------------------------------------------------------------------------
// 取值
// ---------------------------------------------------------------------------

Result<QString> normalizeValue(MatchDomain domain, MatchMode mode, const QString& value) {
  auto spec = findModeSpec(domain, mode);
  if (!spec) {
    return Result<QString>::fail(spec.error());
  }
  return normalizeValueForShape(spec.value().shape, value);
}

Result<Address> parseAddress(const QString& literal) {
  auto parsed = parseAddressLiteral(literal);
  if (!parsed) {
    return Result<Address>::fail(parsed.error());
  }

  Address address;
  address.text = parsed.value().toString();
  address.family = parsed.value().protocol() == QAbstractSocket::IPv6Protocol ? AddressFamily::V6
                                                                              : AddressFamily::V4;
  return address;
}

Result<AddressSpan> subnetToSpan(const QString& subnet) {
  const int slash = subnet.indexOf(QLatin1Char('/'));
  if (slash < 0) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("网段要写成「地址/前缀长度」：%1").arg(subnet));
  }

  auto parsed = parseAddressLiteral(subnet.left(slash));
  if (!parsed) {
    return Result<AddressSpan>::fail(parsed.error());
  }

  const bool isV6 = parsed.value().protocol() == QAbstractSocket::IPv6Protocol;
  const int totalBits = isV6 ? 128 : 32;

  bool ok = false;
  const int prefix = subnet.mid(slash + 1).toInt(&ok);
  if (!ok || prefix < 0 || prefix > totalBits) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("网段的前缀长度不合法：%1").arg(subnet));
  }

  // 主机位全部置 0 得到起点，全部置 1 得到终点。
  // 地址是大端存储，所以第 bit 位落在第 bit/8 字节的第 7-(bit%8) 位。
  QByteArray lower = addressBytes(parsed.value());
  QByteArray upper = lower;
  for (int bit = prefix; bit < totalBits; ++bit) {
    const int byteIndex = bit / 8;
    const int bitIndex = 7 - (bit % 8);
    const char mask = static_cast<char>(1U << bitIndex);
    lower[byteIndex] = static_cast<char>(lower.at(byteIndex) & ~mask);
    upper[byteIndex] = static_cast<char>(upper.at(byteIndex) | mask);
  }

  auto lowerAddress = addressFromBytes(lower);
  if (!lowerAddress) {
    return Result<AddressSpan>::fail(lowerAddress.error());
  }
  auto upperAddress = addressFromBytes(upper);
  if (!upperAddress) {
    return Result<AddressSpan>::fail(upperAddress.error());
  }

  AddressSpan span;
  span.lower = lowerAddress.value();
  span.upper = upperAddress.value();
  return span;
}

Result<AddressSpan> addressRangeToSpan(const QString& range) {
  const int dash = range.indexOf(QLatin1Char('-'));
  if (dash < 0) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("地址范围要写成「起-止」：%1").arg(range));
  }

  auto lower = parseAddress(range.left(dash));
  if (!lower) {
    return Result<AddressSpan>::fail(lower.error());
  }
  auto upper = parseAddress(range.mid(dash + 1));
  if (!upper) {
    return Result<AddressSpan>::fail(upper.error());
  }
  if (lower.value().family != upper.value().family) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("地址范围的两端必须是同一地址族：%1").arg(range));
  }

  AddressSpan span;
  span.lower = lower.value();
  span.upper = upper.value();
  return span;
}

Result<PortSpan> portToSpan(const QString& value) {
  const int dash = value.indexOf(QLatin1Char('-'));
  if (dash < 0) {
    auto port = parsePortNumber(value);
    if (!port) {
      return Result<PortSpan>::fail(port.error());
    }
    PortSpan span;
    span.lower = static_cast<std::uint16_t>(port.value());
    span.upper = span.lower;
    return span;
  }

  auto lower = parsePortNumber(value.left(dash));
  if (!lower) {
    return Result<PortSpan>::fail(lower.error());
  }
  auto upper = parsePortNumber(value.mid(dash + 1));
  if (!upper) {
    return Result<PortSpan>::fail(upper.error());
  }

  PortSpan span;
  span.lower = static_cast<std::uint16_t>(lower.value());
  span.upper = static_cast<std::uint16_t>(upper.value());
  return span;
}

// ---------------------------------------------------------------------------
// 地址与端口的取值运算（S2.4 的取反求补要用）
// ---------------------------------------------------------------------------
//
// 「求补」是取反条件唯一正确的翻法：多值取反若翻成「≠A 或 ≠B」就恒为真，
// 等于把条件整个丢掉，规则会比预期**封得宽**（见 filter_plan.h）。
// 这段运算放在核心层而不是平台层，是因为它纯粹是取值集合的事，
// 与用哪套过滤 API 无关；放在核心层还能直接单元测试。

namespace {

/// 大端字节串加一。已经是全 0xFF 时返回 false，且**不改动入参**。
///
/// 先整体扫一遍再进位，就是为了「失败时入参不变」这一条：
/// 失败后调用方手里的游标必须还是原来那个值，否则它会拿着一个被清零的游标继续算。
bool incrementBytes(QByteArray& bytes) {
  bool allOnes = true;
  for (const char byte : bytes) {
    if (static_cast<std::uint8_t>(byte) != 0xFFU) {
      allOnes = false;
      break;
    }
  }
  if (allOnes) {
    return false;
  }

  for (int i = bytes.size() - 1; i >= 0; --i) {
    const auto value = static_cast<std::uint8_t>(bytes.at(i));
    if (value != 0xFFU) {
      bytes[i] = static_cast<char>(value + 1);
      return true;
    }
    bytes[i] = '\0';
  }
  return false;
}

/// 大端字节串减一。已经是全 0x00 时返回 false，且**不改动入参**。理由同上。
bool decrementBytes(QByteArray& bytes) {
  bool allZero = true;
  for (const char byte : bytes) {
    if (byte != '\0') {
      allZero = false;
      break;
    }
  }
  if (allZero) {
    return false;
  }

  for (int i = bytes.size() - 1; i >= 0; --i) {
    const auto value = static_cast<std::uint8_t>(bytes.at(i));
    if (value != 0U) {
      bytes[i] = static_cast<char>(value - 1);
      return true;
    }
    bytes[i] = static_cast<char>(0xFFU);
  }
  return false;
}

/// 把一对大端字节串包回地址区间并追加。字节长度必须是 4 或 16，由 `addressFromBytes` 兜底。
Result<void> appendAddressSpan(QList<AddressSpan>& spans,
                               const QByteArray& lower,
                               const QByteArray& upper) {
  auto low = addressFromBytes(lower);
  if (!low) {
    return Result<void>::fail(low.error());
  }
  auto high = addressFromBytes(upper);
  if (!high) {
    return Result<void>::fail(high.error());
  }

  AddressSpan span;
  span.lower = low.value();
  span.upper = high.value();
  spans.append(span);
  return Result<void>::ok();
}

}  // namespace

Result<QByteArray> addressToBytes(const Address& address) {
  auto parsed = parseAddressLiteral(address.text);
  if (!parsed) {
    return Result<QByteArray>::fail(parsed.error());
  }

  // 值与声明的地址族必须对得上。对不上的后果是「按 IPv4 构造的条件里放了 IPv6 的数值」，
  // 平台层会把它当成某个毫不相干的 IPv4 地址，而且是静默的。
  const bool isV6 = parsed.value().protocol() == QAbstractSocket::IPv6Protocol;
  if (isV6 != (address.family == AddressFamily::V6)) {
    return makeError(ErrorCode::InvalidArgument,
                     QStringLiteral("地址 %1 与它声明的地址族对不上").arg(address.text));
  }

  return addressBytes(parsed.value());
}

Result<QList<AddressSpan>> complementAddressSpans(const QList<AddressSpan>& spans) {
  QList<AddressSpan> result;
  if (spans.isEmpty()) {
    // 「不限定」的补集是空集。空集与全集是相反的语义，不能替调用方猜。
    return result;
  }

  const AddressFamily family = spans.first().lower.family;
  const int byteCount = family == AddressFamily::V6 ? 16 : 4;

  // 先全部转成字节串并逐条校验，再排序合并。「先校验后计算」是为了让
  // 失败路径不留下一个算了一半的结果 —— 半份补集会静默封错范围。
  QList<QPair<QByteArray, QByteArray>> ranges;
  for (const AddressSpan& span : spans) {
    if (span.lower.family != span.upper.family || span.lower.family != family) {
      return makeError(ErrorCode::InvalidArgument,
                       QStringLiteral("求补的地址区间必须同族，且两端一致：%1-%2")
                           .arg(span.lower.text, span.upper.text));
    }

    auto lower = addressToBytes(span.lower);
    if (!lower) {
      return Result<QList<AddressSpan>>::fail(lower.error());
    }
    auto upper = addressToBytes(span.upper);
    if (!upper) {
      return Result<QList<AddressSpan>>::fail(upper.error());
    }
    if (upper.value() < lower.value()) {
      return makeError(ErrorCode::InvalidArgument,
                       QStringLiteral("地址区间的起点不能大于终点：%1-%2")
                           .arg(span.lower.text, span.upper.text));
    }

    ranges.append({lower.value(), upper.value()});
  }

  std::sort(ranges.begin(),
            ranges.end(),
            [](const QPair<QByteArray, QByteArray>& lhs, const QPair<QByteArray, QByteArray>& rhs) {
              return lhs.first < rhs.first;
            });

  QList<QPair<QByteArray, QByteArray>> merged;
  for (const QPair<QByteArray, QByteArray>& range : ranges) {
    bool absorbed = false;
    if (!merged.isEmpty()) {
      QByteArray nextAfterLast = merged.last().second;
      if (!incrementBytes(nextAfterLast)) {
        // 上一段的终点已是全域最大值，后面的区间一定被它包含。
        absorbed = true;
      } else {
        absorbed = !(nextAfterLast < range.first);
      }
      if (absorbed && merged.last().second < range.second) {
        merged.last().second = range.second;
      }
    }
    if (!absorbed) {
      merged.append(range);
    }
  }

  const QByteArray zero(byteCount, '\0');
  QByteArray cursor = zero;
  bool haveCursor = true;

  for (const QPair<QByteArray, QByteArray>& range : merged) {
    if (!haveCursor) {
      break;
    }
    if (cursor < range.first) {
      QByteArray gapEnd = range.first;
      decrementBytes(gapEnd);
      auto appended = appendAddressSpan(result, cursor, gapEnd);
      if (!appended) {
        return Result<QList<AddressSpan>>::fail(appended.error());
      }
    }
    cursor = range.second;
    haveCursor = incrementBytes(cursor);
  }

  if (haveCursor) {
    const QByteArray allOnes(byteCount, static_cast<char>(0xFFU));
    auto appended = appendAddressSpan(result, cursor, allOnes);
    if (!appended) {
      return Result<QList<AddressSpan>>::fail(appended.error());
    }
  }

  return result;
}

QList<PortSpan> complementPortSpans(const QList<PortSpan>& spans) {
  QList<PortSpan> result;
  if (spans.isEmpty()) {
    return result;
  }

  QList<QPair<std::uint32_t, std::uint32_t>> ranges;
  for (const PortSpan& span : spans) {
    std::uint32_t lower = span.lower;
    std::uint32_t upper = span.upper;
    if (upper < lower) {
      std::swap(lower, upper);
    }
    ranges.append({lower, upper});
  }
  std::sort(ranges.begin(),
            ranges.end(),
            [](const QPair<std::uint32_t, std::uint32_t>& lhs,
               const QPair<std::uint32_t, std::uint32_t>& rhs) { return lhs.first < rhs.first; });

  QList<QPair<std::uint32_t, std::uint32_t>> merged;
  for (const QPair<std::uint32_t, std::uint32_t>& range : ranges) {
    if (!merged.isEmpty() && range.first <= merged.last().second + 1U) {
      if (range.second > merged.last().second) {
        merged.last().second = range.second;
      }
      continue;
    }
    merged.append(range);
  }

  constexpr std::uint32_t kMaxPort = 65535U;
  std::uint32_t cursor = 0;
  bool haveCursor = true;

  for (const QPair<std::uint32_t, std::uint32_t>& range : merged) {
    if (!haveCursor) {
      break;
    }
    if (cursor < range.first) {
      PortSpan gap;
      gap.lower = static_cast<std::uint16_t>(cursor);
      gap.upper = static_cast<std::uint16_t>(range.first - 1U);
      result.append(gap);
    }
    if (range.second >= kMaxPort) {
      haveCursor = false;
    } else {
      cursor = range.second + 1U;
    }
  }

  if (haveCursor) {
    PortSpan tail;
    tail.lower = static_cast<std::uint16_t>(cursor);
    tail.upper = static_cast<std::uint16_t>(kMaxPort);
    result.append(tail);
  }

  return result;
}

// ---------------------------------------------------------------------------
// 校验与规范化
// ---------------------------------------------------------------------------

Result<void> normalizeCondition(MatchCondition& condition) {
  auto domain = parseDomain(condition.domain);
  if (!domain) {
    return Result<void>::fail(domain.error());
  }
  auto mode = parseMode(condition.mode);
  if (!mode) {
    return Result<void>::fail(mode.error());
  }
  auto spec = findModeSpec(domain.value(), mode.value());
  if (!spec) {
    return Result<void>::fail(spec.error());
  }

  // 全程改副本，最后一次性写回：半改的状态比不改更糟，调用方无从知道该回滚哪些字段。
  MatchCondition candidate = condition;
  candidate.domain = QString::fromLatin1(domainCode(domain.value()));
  candidate.mode = QString::fromLatin1(modeCode(mode.value()));

  // 是否需要取值由取值形状决定，**不是**由「是否全匹配」决定：
  // `proto` 的 `tcp` / `direction` 的 `out` 同样不接受取值，因为它们的
  // 取值就是方式本身；而 `any` / `both` 虽然也不接受取值，额外还有
  // 「不许取反」的约束。把两件事混成一件会让 `proto/tcp` 被要求给取值。
  if (spec.value().shape == ValueShape::None) {
    if (!candidate.values.isEmpty()) {
      return Result<void>::fail(makeError(
          ErrorCode::InvalidArgument,
          QStringLiteral("「%1」的「%2」匹配不接受取值，它的含义由方式本身决定，取值要留空："
                         "给的是 %3")
              .arg(domainTitle(domain.value()),
                   modeTitle(mode.value()),
                   candidate.values.join(QStringLiteral("、")))));
    }
    if (matchesEverything(mode.value()) && candidate.negate) {
      // 取反之后没有任何取值能命中，整条规则永不生效。这种规则看上去封了一批东西，
      // 实际一条都不封，属于最危险的一类「看起来生效了」，必须当场拒绝。
      return Result<void>::fail(
          makeError(ErrorCode::InvalidArgument,
                    QStringLiteral("「%1」是不限定取值的条件，不能取反：取反后没有任何取值能命中，"
                                   "整条规则会永不生效")
                        .arg(domainTitle(domain.value()))));
    }
    candidate.values.clear();
  } else {
    if (candidate.values.isEmpty()) {
      return Result<void>::fail(
          makeError(ErrorCode::InvalidArgument,
                    QStringLiteral("「%1」的「%2」匹配至少要给一个取值")
                        .arg(domainTitle(domain.value()), modeTitle(mode.value()))));
    }

    QStringList normalized;
    for (const QString& value : candidate.values) {
      auto one = normalizeValueForShape(spec.value().shape, value);
      if (!one) {
        return Result<void>::fail(
            makeError(one.error().code,
                      QStringLiteral("「%1」的取值不合法：%2")
                          .arg(domainTitle(domain.value()), one.error().message)));
      }

      // 去重按不区分大小写：地址与端口在规范化后已经没有大小写差异，
      // 而路径的比较本来就不区分大小写（同 types.h 的 sameProcessIdentity）。
      bool duplicate = false;
      for (const QString& existing : normalized) {
        if (QString::compare(existing, one.value(), Qt::CaseInsensitive) == 0) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        normalized.append(one.value());
      }
    }

    // 精确匹配只接受一个取值，但判在去重之后：同一个值写了两遍属于重复，
    // 不该被报成「取值太多」。
    if (spec.value().mode == MatchMode::Exact && normalized.size() != 1) {
      return Result<void>::fail(makeError(ErrorCode::InvalidArgument,
                                          QStringLiteral("「%1」的精确匹配只能给一个取值，"
                                                         "要给多个请改用集合匹配")
                                              .arg(domainTitle(domain.value()))));
    }

    candidate.values = normalized;
  }

  condition = candidate;
  return Result<void>::ok();
}

Result<void> validateCondition(const MatchCondition& condition) {
  MatchCondition copy = condition;
  return normalizeCondition(copy);
}

Result<void> normalizeRule(Rule& rule) {
  if (rule.schema != kCurrentRuleSchema) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument,
                  QStringLiteral("规则的格式版本是 %1，当前只接受 %2。旧格式要先迁移再校验，"
                                 "更高版本则说明程序太旧")
                      .arg(rule.schema)
                      .arg(kCurrentRuleSchema)));
  }
  if (rule.conditions.isEmpty()) {
    return Result<void>::fail(makeError(
        ErrorCode::InvalidArgument,
        QStringLiteral("规则至少要有一个匹配条件。要表达「全部」请显式加一个「全部」条件，"
                       "而不是把条件留空")));
  }

  QList<MatchCondition> normalized;
  normalized.reserve(rule.conditions.size());
  QList<MatchDomain> seenDomains;

  for (int index = 0; index < rule.conditions.size(); ++index) {
    MatchCondition condition = rule.conditions.at(index);
    auto one = normalizeCondition(condition);
    if (!one) {
      return Result<void>::fail(makeError(
          one.error().code,
          QStringLiteral("第 %1 个条件不合法：%2").arg(index + 1).arg(one.error().message)));
    }

    // 同一个域出现两次是「交集」语义：port ∈ {80} 且 port ∈ {443} 恒为假。
    // 用户几乎一定是想表达并集，所以直接拒绝并指出去处，而不是照交集执行。
    const auto domain = parseDomain(condition.domain);
    if (!domain) {
      return Result<void>::fail(domain.error());
    }
    if (seenDomains.contains(domain.value())) {
      return Result<void>::fail(
          makeError(ErrorCode::InvalidArgument,
                    QStringLiteral("「%1」域出现了两次。同一个域只能有一个条件，多个取值请放进"
                                   "同一个条件的取值列表里")
                        .arg(domainTitle(domain.value()))));
    }
    seenDomains.append(domain.value());

    normalized.append(condition);
  }

  rule.conditions = normalized;
  return Result<void>::ok();
}

Result<void> validateRule(const Rule& rule) {
  Rule copy = rule;
  return normalizeRule(copy);
}

// ---------------------------------------------------------------------------
// 具体度与优先级
// ---------------------------------------------------------------------------

Result<std::int32_t> conditionSpecificity(const MatchCondition& condition) {
  auto domain = parseDomain(condition.domain);
  if (!domain) {
    return Result<std::int32_t>::fail(domain.error());
  }
  auto mode = parseMode(condition.mode);
  if (!mode) {
    return Result<std::int32_t>::fail(mode.error());
  }
  auto spec = findModeSpec(domain.value(), mode.value());
  if (!spec) {
    return Result<std::int32_t>::fail(spec.error());
  }
  return spec.value().specificity;
}

Result<std::int32_t> computePriority(const Rule& rule) {
  auto valid = validateRule(rule);
  if (!valid) {
    return Result<std::int32_t>::fail(valid.error());
  }

  std::int32_t priority =
      rule.action == RuleAction::Allow ? kAllowPriorityBase : kBlockPriorityBase;
  for (const MatchCondition& condition : rule.conditions) {
    auto specificity = conditionSpecificity(condition);
    if (!specificity) {
      return Result<std::int32_t>::fail(specificity.error());
    }
    priority += specificity.value();
    if (condition.negate) {
      priority += kNegationBonus;
    }
  }
  return priority;
}

// ---------------------------------------------------------------------------
// 宽条件与风险分级
// ---------------------------------------------------------------------------

RiskLevel riskLevelOf(int wideConditionCount) noexcept {
  if (wideConditionCount >= 3) {
    return RiskLevel::Extreme;
  }
  if (wideConditionCount >= 2) {
    return RiskLevel::High;
  }
  return RiskLevel::Normal;
}

Result<WidthReport> assessWidth(const Rule& rule) {
  auto valid = validateRule(rule);
  if (!valid) {
    return Result<WidthReport>::fail(valid.error());
  }

  WidthReport report;
  for (int index = 0; index < rule.conditions.size(); ++index) {
    const MatchCondition& condition = rule.conditions.at(index);
    // 刚校验过，这两步不会失败。
    const MatchDomain domain = parseDomain(condition.domain).value();
    const MatchMode mode = parseMode(condition.mode).value();

    QString description;
    if (matchesEverything(mode)) {
      description = mode == MatchMode::Both
                        ? QStringLiteral("方向不限定，出入站都匹配")
                        : QStringLiteral("「%1」不限定取值，等于全部").arg(domainTitle(domain));
    } else if (condition.negate) {
      description = QStringLiteral("「%1」条件被取反，命中的是「不在给定取值里」的那些")
                        .arg(domainTitle(domain));
    } else if (mode == MatchMode::Wildcard && hasLooseWildcard(condition.values)) {
      description = QStringLiteral("通配模式过于宽泛，会命中意料之外的程序：%1")
                        .arg(condition.values.join(QStringLiteral("、")));
    }

    if (!description.isEmpty()) {
      WidthIssue issue;
      issue.conditionIndex = index;
      issue.description = description;
      report.issues.append(issue);
    }
  }

  report.level = riskLevelOf(report.count());
  return report;
}

}  // namespace baniphelper::core
