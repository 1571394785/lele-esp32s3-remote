# 预编译固件

此固件对应 2026-10-19 最后一版 COM3 烧录内容，目标 ESP32-S3，ESP-IDF v5.4.1，8 MB 分区布局。
不包含设备 NVS、蓝牙密钥、Wi-Fi 凭据或用户配置。

安装 Python 与 esptool 4.8.1 后，在本目录运行（COM3 按实际串口修改）：

```powershell
python -m pip install esptool==4.8.1
python -m esptool --chip esp32s3 --port COM3 --baud 460800 --before default_reset --after hard_reset write_flash '@flash_args'
```

写入包含引导程序、分区表和应用，不执行整片擦除。已有相容分区布局下的 NVS 配置会保留。
请确认硬件和引脚与主 README 一致。首次使用后选择设备位置，再进入设置中的配对模式。
