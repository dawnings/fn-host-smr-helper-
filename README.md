# Host-SMR 识别（fn-host-smr-helper）

飞牛 fnOS 第三方应用：识别、映射和管理**主机管理型 SMR（Host-Managed SMR / ZAC）硬盘**。

主要解决 Host-SMR 硬盘接在 ASM 等 SATA 控制器后无法被系统自动识别的问题，并提供 dm-zoned 映射、外置盘挂载、飞牛原生存储空间初始化、开机服务、控制器 NCQ / 32 位 DMA 设置、依赖检查和卸载清理。

> ⚠️ 本项目处于测试阶段，涉及磁盘格式化和内核模块。请先在测试机验证，并做好数据备份。HC620 4Kn 盘目前还不能保证通过 ASM 芯片识别(4Kn盘和AF盘存在一些区别,某些背板4Kn盘会直接不上电)。

## 功能

- **识别**：动态扫描 PCI/libata 控制器端口，只对有明确 ZAC 证据的端口做兼容处理；支持按端口强制尝试一次识别。
- **USB**：只读 sysfs，分别显示主控、桥接设备、驱动和块设备。
- **单盘管理**：格式化为外置盘（dm-zoned + Btrfs）或飞牛存储（dm-zoned + LVM + Btrfs）。
- **区带状态和清理**：读取 dm-zoned 卷状态，发送后台 reclaim 请求。
- **手动重建登记**：根据现有映射和 UUID 补回应用配置。
- **控制器选项**：每块 SATA 控制器可单独强制关闭 NCQ / 强制 32 位 DMA(ARM如果走了ASM建议使用)，重启后生效。

详细说明和依赖列表见 [app/docs/README.md](app/docs/README.md)。

## 系统要求

- fnOS 1.2.0 及以上，x86-64；系统需提供 `apt-get`/`dpkg` 软件包管理器和可用软件源。
- Python 3.8 及以上；安装/升级时会自动安装缺少的 `lvm2`、`btrfs-progs`、`util-linux`、`kmod` 软件包。
- 兼容识别还需要 `gcc`、`make`、与当前运行内核匹配的 `linux-headers-$(uname -r)`，且内核版本 ≥ 6.12。安装/升级时会尝试自动安装这些依赖
- `dm-zoned` 内核模块、内核 BTF 与相关内核配置无法通过通用软件包可靠补齐；安装脚本会检查，缺少必须条件时中止安装并在日志中说明。

安装脚本以非交互方式调用 apt；如软件包索引过期，会尝试更新索引后重试。必须依赖安装或检查失败时，安装/升级会中止并写明原因。

## 安装

从 [Releases](https://github.com/dawnings/fn-host-smr-helper/releases) 下载 `.fpk`，在飞牛应用中心使用「手动安装」选择该文件。

## 构建

在仓库根目录运行飞牛官方打包工具：

```bash
fnpack build
```

内核模块（`app/compat/smr_sata.c`、`smr_policy.c`）在目标机上按运行内核现场编译。`app/compat/smr-parser.so` 是预编译文件，修改 `smr-parser.c` 后需在 x86-64 Linux 上重新编译：

```bash
gcc -shared -fPIC -O2 -o app/compat/smr-parser.so app/compat/smr-parser.c -ldl
```

## 目录结构

```
manifest              应用元数据（版本号在这里改）
cmd/                  飞牛安装 / 升级 / 卸载 / 启停脚本
config/               权限与资源声明
app/bin/              后端二进制、USB 网关和扫描脚本（Python）
app/compat/           内核模块、smr-parser、dmzadm 及其源码
app/www/              Web 界面
app/ui/               飞牛桌面入口配置和图标
app/docs/             应用内说明文档
```


## 许可证

本项目以 [GNU General Public License v3.0](LICENSE) 发布。

`app/compat/dmzadm` 来自 Western Digital 的 [dm-zoned-tools](https://github.com/westerndigitalcorporation/dm-zoned-tools)（GPL-3.0），源码见 `app/compat/source/dm-zoned-tools.tar.gz`。
