# ESP Fish

ESP32-C6 + ST7789 的余额显示与倾斜桌宠。香橙派提供余额代理和局域网模式切换网页。

## 使用

打开 `http://<香橙派地址>:8082/`，选择“余额与图像”或“自由桌宠”。设备通常在 1–2 秒内接收选择；余额上游超时时最多额外等待约 10 秒。网页区分已保存的选择、设备实际模式和离线状态。

- **余额模式**：保留角色图像、余额和已修复的右上角时钟；余额读取失败时保留最后结果并标记缓存。
- **桌宠模式**：四段动画（待机、奔跑、挥手、吐泡泡），自主移动、随机活动、屏幕边界限制。倾斜优先于自主移动，传感器未连接时仍可自主活动。
- **倾斜方向**：进入桌宠时自动将当前姿态设为中性方向，静置时随机活动，倾斜时跟随移动。摆放角度变化后，点击“以当前姿态校准”。网页支持交换横纵轴和分别反向，适配传感器安装方向；采用低通滤波和约 0.12g 死区。
- **掉线与重启**：断网继续当前显示；设备保存最后成功进入的模式，网关保存模式和方向设置。重新连接后按网关配置同步。

桌宠为原桌面项目的嵌入式动画适配，选取四段动作；桌面版的本地模型、OCR、MCP 等功能不包含在本项目中。

## 硬件与内存

| 硬件 | 连接 |
| --- | --- |
| ESP32-C6FH4 | 4 MB Flash，无需 PSRAM |
| ST7789 240×240 | MOSI 6、SCLK 7、CS 14、DC 15、RST 21、BL 22 |
| BMI160 | SDA 2、SCL 1，探测 0x69 / 0x68，仅启用加速度计 |

静态图像和压缩动画均存放在 Flash。直接从原始 640×360 视频先裁去透明空白，再缩小为 **168×168**，避免先降至小图再放大造成细节丢失。全部动作使用共同裁剪框，避免尺寸跳变，横纵各保留 72 像素移动范围。240 帧动画采用 128 色、6 fps 和 zlib 压缩，压缩数据共 **1,677,667 字节**，另有调色板与帧索引；移动位置约每秒更新 30 次。

进入桌宠模式时分配一张 RGB565 帧缓冲和 ROM 解码器状态，共 **67,440 字节**，切回余额时全部释放。调色板索引先解压到帧缓冲的后半部分，再安全地原地展开为 RGB565，不额外分配索引整帧、全屏或双帧缓冲。解码器状态也放在堆中，避免占满任务栈；移动时复用已解码帧。后台网络任务使用独立的 8 KB 栈，网络等待不阻塞动画。

固件使用 4 MB Flash 的 `huge_app.csv` 分区布局，为清晰动画提供 3 MiB 应用分区；当前不支持双分区 OTA。NVS 起始地址与原布局一致，保留设备设置。运行时 Wi-Fi、JSON、任务栈和绘图缓冲额外使用堆内存，网页展示设备实测的可用内存。

## 构建与烧录

1. 安装 PlatformIO，复制 `include/secrets_example.h` 为 `include/secrets_local.h`，填写 2.4 GHz Wi-Fi 与香橙派地址。私密配置已加入忽略规则。
2. `python -m platformio run` 编译，`python -m platformio run -t upload` 烧录。默认 COM5，接入不同设备时修改 `platformio.ini`。
3. 图像头文件已经提交，无需重新生成。需要重建时安装 Pillow、imageio-ffmpeg，再运行 `python tools/convert_whale.py`、`python tools/convert_pet.py`。动画来源固定到具体 Git 提交，清单记录源文件 SHA-256。

## 香橙派服务

### 余额代理（8081）

`gateway/balance_service.py` 从 `DEEPSEEK_API_KEY` 环境变量读取密钥，查询 DeepSeek 余额并缓存最后成功结果。密钥只保存在香橙派受限环境文件中，不写入设备、仓库或命令参数。

新部署时，由管理员创建 `espfish` 服务用户，将脚本放入 `/opt/espfish/balance_service.py`，将环境文件放入 `/etc/espfish/balance.env`（root:espfish，640），再安装 `gateway/espfish-balance.service` 并启用。已有余额服务可以继续使用。

### 模式网关（8082）

模式服务使用 Python 标准库，独立于余额代理，不需要 API 密钥。在香橙派以常用 SSH 用户执行：

```sh
mkdir -p ~/.local/share/espfish ~/.config/systemd/user
# 将 gateway/control_service.py 与 gateway/index.html 复制到 ~/.local/share/espfish/
# 将 gateway/espfish-control.service 复制到 ~/.config/systemd/user/
loginctl enable-linger "$USER"
systemctl --user daemon-reload
systemctl --user enable --now espfish-control
```

`enable-linger` 使服务在退出 SSH 和重启后继续运行；系统策略不同的设备可能需要管理员授权。配置默认写入 `~/.local/state/espfish/control.json`，可通过 `CONTROL_STATE_FILE` 覆盖。监听地址和端口支持 `CONTROL_HOST`、`CONTROL_PORT` 环境变量。两个服务均仅用于可信局域网。

| 接口 | 用途 |
| --- | --- |
| `GET /` | 控制网页 |
| `GET /api/state` | 模式、方向配置、设备在线和遥测 |
| `POST /api/mode` | 设置 `mode: balance/pet` 或布尔方向配置 `swap_axes`、`invert_x`、`invert_y` |
| `POST /api/device` | 设备上报当前模式与内存，获取期望配置 |
| `POST /api/calibrate` | 发送 `{}`，以当前设备姿态重新校准倾斜零点 |

## 验证与还原

运行 `python -m unittest discover -s gateway -v` 检查余额响应、模式持久化、设备确认、无效参数及跨站请求。固件须经 PlatformIO 编译，并在实物上检查时钟、模式切换和实际倾斜方向。

改动前版本已标记为 **`baseline-display`**（提交 `2ba6a06`）。推荐在单独目录检出此标签，复制本机私密配置后重新编译烧录；不要在正在工作的目录直接覆盖未提交文件。也可使用本地保留的完整 Flash 备份恢复设备，备份含私密网络配置，不应公开。

停用新增网关：`systemctl --user disable --now espfish-control`。原有 8081 余额代理可继续运行；只有确定没有其他用户服务依赖时才考虑关闭 linger。

## 素材许可

- 静态角色图：来自 [MeteorNOX/DeepSeek-Balance-Whale-Widget](https://github.com/MeteorNOX/DeepSeek-Balance-Whale-Widget)，MIT；见 `assets/LICENSE-MeteorNOX`。
- 四段桌宠动画：取自 [Bai-Cute/big-blue-fish-desktop-pet](https://github.com/Bai-Cute/big-blue-fish-desktop-pet)，原动画作者 [PC2005-cloud/dsh-pet](https://github.com/PC2005-cloud/dsh-pet)，MIT；见 `assets/LICENSE-PC2005-cloud` 与 `assets/pet-sources.json`。动画经过缩小、降帧、调色板量化和压缩。
