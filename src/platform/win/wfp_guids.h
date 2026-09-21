#pragma once

#include <guiddef.h>

#include <cstddef>

namespace baniphelper::core {

/// WFP 的常量 GUID 表。
///
/// ## 为什么代码里要自己抄一份
///
/// MinGW-w64 自带的 `fwpmu.h` 只声明函数、结构体与枚举，**不含** `FWPM_LAYER_*` 与
/// `FWPM_CONDITION_*` 这些 `DEFINE_GUID` 常量；`libfwpuclnt.a` 也只导出函数，
/// 没有对应的 GUID 符号。而建过滤器（`FwpmFilterAdd0`）必须给出具体的层 GUID，
/// 所以这些值只能写进代码。S1.7 的清理路径靠运行时枚举全部层绕开了这个缺口，
/// 但下发规则绕不过去。
///
/// ## 值的出处
///
/// 两个来源逐字比对一致（比对脚本见 `tmp/extract-fwpmu.ps1`）：
///
/// 1. Windows SDK 的 `um/fwpmu.h`，取自 win32metadata 仓库（微软自有）：
///    `https://github.com/microsoft/win32metadata/blob/main/generation/`
///    `WinSDK/RecompiledIdlHeaders/um/fwpmu.h`
/// 2. Windows SDK 10.0.16299.0 的 `um/fwpmu.h`（第三方镜像）。
///
/// 层部分另有**本机运行时复核**：用 `FwpmLayerEnum0` 枚举本机现有层，
/// 与本表逐条对上（见 `docs/phases/02-filtering.md` §2.1）。
///
/// ## 抄的时候踩过的坑
///
/// SDK 头里有些字面量不补前导零（例如 `0xa62` 而不是 `0x0a62`、`0x8c7` 而不是
/// `0x08c7`）。当初抽出 `FWPM_CONDITION_ALE_PACKAGE_ID` 时因此得到过一个畸形 GUID，
/// 是按**固定宽度补零**重抽才对的。要复核本表，务必用 `tmp/extract-fwpmu.ps1`，
/// 不要手工从头上誊。
///
/// ## 为什么不用官方名字当标识符
///
/// 官方名字形如 `FWPM_LAYER_ALE_AUTH_CONNECT_V4`。哪天某个翻译单元真的包含了带这些
/// 常量的 SDK 头，重名会直接变成编译错误或被悄悄改写。带 `k` 前缀的写法没有这个问题。

/// ALE 授权连接层：出站连接在被允许之前经过这里。封禁出站连接用这两层。
inline constexpr GUID kLayerAleAuthConnectV4 = {
    0xc38d57d1, 0x05a7, 0x4c33, {0x90, 0x4f, 0x7f, 0xbc, 0xee, 0xe6, 0x0e, 0x82}};
inline constexpr GUID kLayerAleAuthConnectV6 = {
    0x4a72393b, 0x319f, 0x44bc, {0x84, 0xc3, 0xba, 0x54, 0xdc, 0xb3, 0xb6, 0xb4}};

/// ALE 授权接收层：入站连接（含监听后的首次接受）在这里判定。
inline constexpr GUID kLayerAleAuthRecvAcceptV4 = {
    0xe1cd9fe7, 0xf4b5, 0x4273, {0x96, 0xc0, 0x59, 0x2e, 0x48, 0x7b, 0x86, 0x50}};
inline constexpr GUID kLayerAleAuthRecvAcceptV6 = {
    0xa3b42c97, 0x9f04, 0x4672, {0xb8, 0x7e, 0xce, 0xe9, 0xc4, 0x83, 0x25, 0x7f}};

/// 数据报层：UDP 收发包经过这里，是统计 UDP 流量的落点（S3）。
inline constexpr GUID kLayerDatagramDataV4 = {
    0x3d08bf4e, 0x45f6, 0x4930, {0xa9, 0x22, 0x41, 0x70, 0x98, 0xe2, 0x00, 0x27}};
inline constexpr GUID kLayerDatagramDataV6 = {
    0xfa45fe2f, 0x3cba, 0x4427, {0x87, 0xfc, 0x57, 0xb9, 0xa4, 0xb1, 0x0d, 0x00}};

/// 流建立层：连接被允许之后、正式传输之前，用来记录「谁连了谁」（S3）。
inline constexpr GUID kLayerAleFlowEstablishedV4 = {
    0xaf80470a, 0x5596, 0x4c13, {0x99, 0x92, 0x53, 0x9e, 0x6f, 0xe5, 0x79, 0x67}};
inline constexpr GUID kLayerAleFlowEstablishedV6 = {
    0x7021d2b3, 0xdfa4, 0x406e, {0xaf, 0xeb, 0x6a, 0xfa, 0xf7, 0xe7, 0x0e, 0xfd}};

/// 端点关闭层：连接结束时经过这里，用来回收连接记录（S3）。
inline constexpr GUID kLayerAleEndpointClosureV4 = {
    0xb4766427, 0xe2a2, 0x467a, {0xbd, 0x7e, 0xdb, 0xcd, 0x1b, 0xd8, 0x5a, 0x09}};
inline constexpr GUID kLayerAleEndpointClosureV6 = {
    0xbb536ccd, 0x4755, 0x4ba9, {0x9f, 0xf7, 0xf9, 0xed, 0xf8, 0x69, 0x9c, 0x7b}};

/// 本表覆盖的全部层，按 IPv4 / IPv6 成对排列。
inline constexpr GUID kKnownLayerKeys[] = {kLayerAleAuthConnectV4,
                                           kLayerAleAuthConnectV6,
                                           kLayerAleAuthRecvAcceptV4,
                                           kLayerAleAuthRecvAcceptV6,
                                           kLayerDatagramDataV4,
                                           kLayerDatagramDataV6,
                                           kLayerAleFlowEstablishedV4,
                                           kLayerAleFlowEstablishedV6,
                                           kLayerAleEndpointClosureV4,
                                           kLayerAleEndpointClosureV6};

/// 层数。写成常量是为了让下面两条断言与使用处不必各自硬编码 10。
inline constexpr std::size_t kKnownLayerCount =
    sizeof(kKnownLayerKeys) / sizeof(kKnownLayerKeys[0]);

/// IP 协议号，取值如 6（TCP）、17（UDP）。
inline constexpr GUID kConditionIpProtocol = {
    0x3971ef2b, 0x623e, 0x4f9a, {0x8c, 0xb1, 0x6e, 0x79, 0xb8, 0x06, 0xb9, 0xa7}};

/// 本地与远端 IP。带版本后缀的那组只在对应版本的层上可用。
inline constexpr GUID kConditionIpLocalAddress = {
    0xd9ee00de, 0xc1ef, 0x4617, {0xbf, 0xe3, 0xff, 0xd8, 0xf5, 0xa0, 0x89, 0x57}};
inline constexpr GUID kConditionIpRemoteAddress = {
    0xb235ae9a, 0x1d64, 0x49b8, {0xa4, 0x4c, 0x5f, 0xf3, 0xd9, 0x09, 0x50, 0x45}};
inline constexpr GUID kConditionIpLocalAddressV4 = {
    0x03a629cb, 0x6e52, 0x49f8, {0x9c, 0x41, 0x57, 0x09, 0x63, 0x3c, 0x09, 0xcf}};
inline constexpr GUID kConditionIpRemoteAddressV4 = {
    0x1febb610, 0x3bcc, 0x45e1, {0xbc, 0x36, 0x2e, 0x06, 0x7e, 0x2c, 0xb1, 0x86}};
inline constexpr GUID kConditionIpLocalAddressV6 = {
    0x2381be84, 0x7524, 0x45b3, {0xa0, 0x5b, 0x1e, 0x63, 0x7d, 0x9c, 0x7a, 0x6a}};
inline constexpr GUID kConditionIpRemoteAddressV6 = {
    0x246e1d8c, 0x8bee, 0x4018, {0x9b, 0x98, 0x31, 0xd4, 0x58, 0x2f, 0x33, 0x61}};

/// 本地与远端端口。
inline constexpr GUID kConditionIpLocalPort = {
    0x0c1ba1af, 0x5765, 0x453f, {0xaf, 0x22, 0xa8, 0xf7, 0x91, 0xac, 0x77, 0x5b}};
inline constexpr GUID kConditionIpRemotePort = {
    0xc35a604d, 0xd22b, 0x4e1a, {0x91, 0xb4, 0x68, 0xf6, 0x74, 0xee, 0x67, 0x4b}};

/// 发起方身份：应用路径、用户 SID、打包应用标识。后两者用于「按进程封」与白名单。
inline constexpr GUID kConditionAleAppId = {
    0xd78e1e87, 0x8644, 0x4ea5, {0x94, 0x37, 0xd8, 0x09, 0xec, 0xef, 0xc9, 0x71}};
inline constexpr GUID kConditionAleUserId = {
    0xaf043a0a, 0xb34d, 0x4f86, {0x97, 0x9c, 0xc9, 0x03, 0x71, 0xaf, 0x6e, 0x66}};
inline constexpr GUID kConditionAlePackageId = {
    0x71bc78fa, 0xf17c, 0x4997, {0xa6, 0x02, 0x6a, 0xbb, 0x26, 0x1f, 0x35, 0x1c}};

/// 本地接口：用来把规则限定在某张网卡上。
inline constexpr GUID kConditionIpLocalInterface = {
    0x4cd62a49, 0x59c3, 0x4969, {0xb7, 0xf3, 0xbd, 0xa5, 0xd3, 0x28, 0x90, 0xa4}};

/// 通用标志位，如 `FWP_CONDITION_FLAG_IS_LOOPBACK`、`_IS_REAUTHORIZE`。
/// 回流流量必须能认出来，否则本工具会连自己的观测流量一起封（见 S4）。
inline constexpr GUID kConditionFlags = {
    0x632ce23b, 0x5167, 0x435c, {0x86, 0xd7, 0xe9, 0x03, 0x68, 0x4a, 0xa8, 0x0c}};

/// 判断两个 GUID 是否相同。
///
/// `GUID` 是 C 结构体，C++17 没有它的 `operator==`，所以这里手写一个 constexpr 版本，
/// 好让下面的断言能在编译期跑。
constexpr bool sameGuid(const GUID& left, const GUID& right) {
  return left.Data1 == right.Data1 && left.Data2 == right.Data2 && left.Data3 == right.Data3 &&
         left.Data4[0] == right.Data4[0] && left.Data4[1] == right.Data4[1] &&
         left.Data4[2] == right.Data4[2] && left.Data4[3] == right.Data4[3] &&
         left.Data4[4] == right.Data4[4] && left.Data4[5] == right.Data4[5] &&
         left.Data4[6] == right.Data4[6] && left.Data4[7] == right.Data4[7];
}

/// 判断一段 GUID 里有没有重复项。
constexpr bool allDistinct(const GUID* keys, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    for (std::size_t j = i + 1; j < count; ++j) {
      if (sameGuid(keys[i], keys[j])) {
        return false;
      }
    }
  }
  return true;
}

/// 编译期自检：层之间、条件之间都不得重复。
///
/// 这是防抄错的最后一道网。GUID 抄错一位在运行期的表现是「层找不到」或
/// 「规则静默不生效」，都不容易一眼看出来；重复才是最容易出的手误。
static_assert(kKnownLayerCount == 10, "层常量表被人改过数量，请同步复核本文件的断言与注释");
static_assert(allDistinct(kKnownLayerKeys, kKnownLayerCount), "层 GUID 表里有重复项，抄错了");
static_assert(!sameGuid(kConditionIpProtocol, kConditionIpLocalPort), "条件 GUID 抄重了");
static_assert(!sameGuid(kConditionIpRemoteAddressV4, kConditionIpRemoteAddressV6),
              "IPv4 与 IPv6 的远端地址条件不可能是同一个 GUID");
static_assert(!sameGuid(kLayerDatagramDataV4, kLayerDatagramDataV6),
              "IPv4 与 IPv6 的数据报层不可能是同一个 GUID");

}  // namespace baniphelper::core
