# songplay — ESP32-P4 无线 CarPlay 接收端

把 [xcertplay](https://github.com/shilapi/xcertplay)（Android/Kotlin 的 CarPlay 接收端）的
**无线 CarPlay** 功能移植到 **ESP32-P4-WIFI6-Touch-LCD-4.3** 开发板，图形界面使用 LVGL。
（有线 CarPlay、音频与触摸回传暂不在范围内。）

## 目录结构

| 路径 | 说明 |
|---|---|
| `esp32p4/` | 主工程（ESP-IDF v6.0.3，target `esp32p4`）：显示/LVGL/触摸、加密库、SRP-6a、iAP2 引导、AirPlay（配对/控制通道/`/auth-setup`/`/stream`/视频解码显示）|
| `esp32-bt-bridge/` | 初代 ESP32 固件（target `esp32`）：经典蓝牙 + HID（让 iPhone 可见）+ SPP 客户端（连接 iPhone 的 iAP2 RFCOMM 通道）+ UART 转发 |
| `mfi-diag/` | 独立的 MFi 协处理器诊断工程（寄存器/时序实验，用于摸清这颗芯片的访问方式）|

## 构建与烧录

激活 ESP-IDF（EIM 安装的 v6.0.3）：

```powershell
. "C:\Espressif\tools\Microsoft.v6.0.3.PowerShell_profile.ps1"
```

主工程（P4，串口按实际修改）：

```powershell
cd esp32p4
idf.py set-target esp32p4
idf.py -p COM27 -b 460800 flash monitor
```

蓝牙桥固件（初代 ESP32）：

```powershell
cd esp32-bt-bridge
idf.py set-target esp32
idf.py -p COM28 -b 460800 flash monitor
```

> `managed_components/`、`build/`、`sdkconfig` 均由 IDF/组件管理器自动生成，未纳入仓库；
> `dependencies.lock` 已提交以固定依赖版本。

## 硬件接线

| P4 开发板 | 初代 ESP32 | 说明 |
|---|---|---|
| 5V（40PIN 脚 1/3）| VIN / 5V | 供电 |
| GND（脚 5/10）| GND | 共地 |
| GPIO4（脚 13，TX）| GPIO16 (RX2) | P4 → ESP32 |
| GPIO5（脚 11，RX）| GPIO17 (TX2) | ESP32 → P4 |

MFi 模块（MFI343 类，I2C 0x10）挂在 P4 的 **SDA=GPIO3 / SCL=GPIO2**。

## 项目状态与协议说明

**请先阅读 [`esp32p4/STATUS.md`](esp32p4/STATUS.md)**：里面记录了已验证的功能、当前卡点、
以及移植过程中摸清的全部协议细节（MFi 时序、iAP2 链路层序号/确认规则、AirPlay 配对与控制
通道加密、CarPlay 的 SETUP 协商方向等）。

## 许可

本项目派生自 [xcertplay](https://github.com/shilapi/xcertplay)（GPLv3），因此同样以
**GPLv3** 发布。仓库中不包含第三方参考项目源码与自动下载的依赖。
