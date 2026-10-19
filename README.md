# 乐乐牌 ESP32-S3 蓝牙遥控器

支持三设备切换的 BLE HID 遥控器，集成体感鼠标、键盘/媒体控制、OLED 中文菜单和 Wi-Fi 自定义设置。

## 当前归档

- 版本：v0.1.0，2026-10-19 固件快照。
- 框架：ESP-IDF v5.4.1，目标 ESP32-S3。
- Git 仓库中的 firmware/ 保存首次归档的实机固件；自动发行包中的 firmware/ 由该次 CI 新编译内容替换。
- 已通过主机端回归测试、编译与 COM3 写入校验。日志复核了 Windows/手机配对及双向回连，修正后一次15秒 Windows 输入采样没有出现原先的重复位移洪泛。
- 用户最终手感、长时间稳定性和所有系统组合仍需实机验证；此快照不作全面稳定版承诺。

## 自动编译与发行

推送到 `main` 后，GitHub Actions 会使用 ESP-IDF v5.4.1 编译并运行回归测试，成功后自动创建 GitHub Release。
发行编号格式为 `build-运行编号-重试次数-提交短哈希`，每次推送独立保留。构建失败时不会发布。
PR 只做构建、测试和产物打包；也可以在 Actions 页手动运行。

[构建记录](https://github.com/1571394785/lele-esp32s3-remote/actions/workflows/firmware-release.yml) · [下载最新发行版](https://github.com/1571394785/lele-esp32s3-remote/releases/latest)

每次自动发行包含：

- `*-firmware.zip`：本次新编译的应用、引导程序、分区表、烧录说明和许可。
- `*-source-and-firmware.zip`：源码与本次新编译固件的整包。
- `BUILD_INFO.json`：对应提交、构建时间和固件校验值。
- `SHA256SUMS`：下载文件的 SHA-256 校验。

只使用 GitHub 自动提供的 `GITHUB_TOKEN`，无需自行创建密钥。自动编译成功不等于实机已验证。
仓库中原有 `v0.1.0` 固件快照保持独立，下载更新应以 Releases 中对应提交的产物为准。

## 功能

- 三个独立蓝牙设备位置，选择后仅回连对应主机。
- 中文设备名称可通过网页修改，保存到 NVS，断电保留。
- 设备页显示当前名称，当前设备反色；连接未完成时闪烁、连接成功后常亮。
- 超宽标题和名称逐像素滚动；独立5毫秒按键扫描与10毫秒消抖。
- BMI220 重力参考体感鼠标，提供 X/Y 灵敏度、加速与滚轮设置。
- 支持 HID Resolution Multiplier，高分辨率滚轮由主机协商，普通主机自动使用普通精度。
- 键盘快捷键、媒体控制、手势识别、自定义 HTTP 请求、小型游戏、温度提醒和电量显示。

## 硬件连接

| 项目 | 引脚/参数 |
| --- | --- |
| MCU | ESP32-S3 |
| OLED | SSD1306，128×64，I2C 地址0x3C |
| OLED / BMI220 共用 I2C | SDA GPIO18，SCL GPIO17 |
| IMU | BMI220，通常地址0x68，代码也探测0x69 |
| 上 / 下 / 左 / 右 | GPIO5 / GPIO6 / GPIO7 / GPIO4 |
| F1 / F2 / F3 / F4 | GPIO41 / GPIO40 / GPIO42 / GPIO39，低电平有效 |
| 电池采样 | GPIO3，具体电路比例见 main/battery_adc.c |
| 固件 Flash 布局 | 8 MB；分区表见 partitions.csv |

## 编译与烧录

安装并导出 ESP-IDF v5.4.1 的环境后，在仓库根目录执行：

```text
idf.py build
idf.py -p COM3 flash monitor
```

仓库保留完整 sdkconfig，确保归档配置可复现。也可直接按 [预编译固件说明](firmware/README.md) 写入本版二进制。
设备实际使用 BMI220；bmi270.c/.h 文件名是历史命名。

## 配对与设置

1. 在设备选择页选定一个位置，再进入“设置 → 配对模式”。
2. 在电脑或手机中搜索“乐乐牌遥控器”完成配对；另外两个位置按同样方法设置。
3. 如果是在系统中删除后重新配对，应同时在遥控器进入配对模式。首次配对时可临时关闭其他已配对设备蓝牙，避免它们抢先连接。
4. 新密钥配对允许将同一主机移到当前位置；旧位置的名称仍由用户自行修改。普通回连继续使用白名单。
5. 遥控器设置中打开 Wi-Fi，连接“自定义设置”热点并访问 http://192.168.4.1 ，可修改三设备名称、快捷键和网络动作等设置。

新配对会重新接收本次主机的 HID 订阅和滚轮协商；普通回连恢复本主机已保存的状态。
拒绝异常旧绑定后暂停广播，用户可重新选择位置或进入配对模式重试。

## 回归测试

需要 Python、主机 GCC；运行实际 ESP-IDF HID 解析器测试时还需要 IDF_PATH 环境变量。

```text
python tests/run_air_mouse_frame.py
```

测试覆盖坐标变换、滚轮累计、HID 描述符、每主机状态、三设备配对、旧绑定迁移、拒绝后停止重连、名称持久化、OLED 逐像素滚动、按键队列及 BMI220 初始化等待。
测试自动清理临时编译文件；硬件体验仍需实测。

## 目录

- main/：固件源码、中文字库和 OLED 资源。
- tests/：主机端回归测试。
- firmware/：本版预编译固件和写入参数。
- train_gesture.py、gesture_model.*：训练脚本及已有模型；原始采集记录未公开。
- serial_3d_visualizer.py：串口姿态可视化，依赖见 requirements_visualizer.txt。
- SHA256SUMS：打包文件校验。

## 许可

原创代码采用 [0BSD](LICENSE)，允许商用、修改、分发，不要求署名。第三方代码和既有资源按各自许可处理，详见 [第三方说明](THIRD_PARTY_NOTICES.md)。
