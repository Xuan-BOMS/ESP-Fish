# ESP Fish

ESP32-C6 上的余额显示与倾斜互动桌宠，搭配香橙派动画库和网页控制台。

- **两种模式**：余额与静态图像，或随机活动、跟随倾斜移动的桌宠。
- **按需下载**：香橙派保存 106 段动画，网页预览、勾选后发送到 ESP32。
- **离线播放**：动画缓存到设备 Flash，下载完成后断网、重启仍可播放。
- **节省内存**：播放缓冲约 39.6 KB，余额模式释放桌宠缓冲。

## 使用

打开 `http://<香橙派地址>:8082/`，选择动画，点击“发送到 ESP32”，等待“已同步”。同一页面可切换模式、调整倾斜方向和校准姿态。

动画缓存上限为 1,750,000 字节，最多选择 16 段，网页会提示容量。少选动画主要节省 Flash，播放 RAM 不随动画数量增加。同步期间请保持供电和网络连接。

## 硬件

| 硬件 | 连接 |
| --- | --- |
| ESP32-C6FH4 | 4 MB Flash，无 PSRAM |
| ST7789，240×240 | MOSI 6、SCLK 7、CS 14、DC 15、RST 21、BL 22 |
| BMI160 | SDA 2、SCL 1 |
| 香橙派 | 与 ESP32 位于同一局域网，运行 Python 3 网关 |

## 构建与部署

### ESP32

安装 PlatformIO，将 [示例配置](include/secrets_example.h) 复制为 `include/secrets_local.h`，填写 2.4 GHz Wi-Fi 和香橙派地址，然后执行：

```sh
python -m platformio run
python -m platformio run -t upload
```

默认串口为 COM5，可在 [platformio.ini](platformio.ini) 修改。分区为 2 MiB 应用加动画文件系统，不支持双分区 OTA；首次烧录前请备份设备。私密配置和生成文件已加入 Git 忽略规则。

### 香橙派

在构建主机生成动画库：

```sh
python -m pip install Pillow imageio-ffmpeg
python tools/prepare_library.py
```

将以下文件复制到香橙派：

| 本地文件 | 香橙派路径 |
| --- | --- |
| `gateway/control_service.py`、`gateway/index.html` | `~/.local/share/espfish/` |
| `.cache/pet-library/` 下的全部文件 | `~/.local/share/espfish/library/` |
| `gateway/espfish-control.service` | `~/.config/systemd/user/` |

在香橙派启用网关：

```sh
loginctl enable-linger "$USER"
systemctl --user daemon-reload
systemctl --user enable --now espfish-control
```

网页端口为 **8082**，选择与设置会自动保存。网关仅用于可信局域网。

余额代理独立运行在 **8081**：使用 [balance_service.py](gateway/balance_service.py) 和 [服务模板](gateway/espfish-balance.service)，通过模板指定的 `/etc/espfish/balance.env` 提供 `DEEPSEEK_API_KEY`。密钥只保存在香橙派，不写入固件或仓库。

## 许可与来源

本项目原创代码和文档采用 **[PolyForm Noncommercial 1.0.0](LICENSE)**，允许协议规定的非商业使用、修改和分发；商业用途请通过 [GitHub Issues](https://github.com/Xuan-BOMS/ESP-Fish/issues) 联系维护者另行授权。这是源码可用、限制商业用途的许可，不属于 OSI 定义的开源许可。

第三方素材保留原 **MIT** 许可，不受本项目新增的非商用限制：

- 静态图：[MeteorNOX/DeepSeek-Balance-Whale-Widget](https://github.com/MeteorNOX/DeepSeek-Balance-Whale-Widget)。
- 动画：[PC2005-cloud/dsh-pet](https://github.com/PC2005-cloud/dsh-pet)，经 [Bai-Cute/big-blue-fish-desktop-pet](https://github.com/Bai-Cute/big-blue-fish-desktop-pet) 获取。

完整适用范围与版权声明见 [NOTICE](NOTICE)，素材许可文本位于 [assets](assets/)。
