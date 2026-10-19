# ESP Fish

ESP32-C6 + ST7789 的余额显示与倾斜桌宠。香橙派保存完整动画库，网页选择后，设备只下载需要的动画。

## 使用

打开 `http://<香橙派地址>:8082/`：

1. 在“动画水族馆”搜索、预览和勾选动画。底部显示所选容量，超出上限时不能发送。
2. 点击“发送到 ESP32”，等待“已同步”。下载期间桌宠暂停，完成后自动恢复。切换余额模式仍可正常使用。
3. 选择“自由桌宠”或“余额与图像”。桌宠随机活动，倾斜时跟随移动；传感器不可用时仍可自主活动。
4. 改变摆放角度后，保持设备静止，点击“校准当前姿态”。横纵轴交换、反向设置适配不同安装方向。

香橙派目前保存 **106 段原动画（约 54 MB）** 和设备动画包（约 41 MB）。ESP32 缓存上限 **1,750,000 字节**，按每段大小通常可选约 4 段，短小动作可多选；数量最多 16 段，至少选择一段。默认待机、奔跑、挥手、吐泡泡合计 **1,679,747 字节**。也可只选择其他动作，缺少待机或奔跑时使用已选动画代替。

断网后，已经下载的动画继续播放，重启也可直接读取本地缓存。网页保存模式、方向和动画选择，设备重新连上网关后自动同步。余额读取失败时保留上次结果并标记缓存；时钟末位越界已经修复。模式通常 1–2 秒内同步，余额上游超时可能额外延迟约 10 秒。

桌面项目的本地模型、OCR、MCP 等功能不包含在此嵌入式适配中。

## 存储与内存

| 项目 | 实现 |
| --- | --- |
| 完整动画库 | 香橙派磁盘，预览时才加载视频 |
| 所选动画 | ESP32 LittleFS Flash 缓存，下载后离线播放 |
| 播放 RAM | 28,224 字节索引帧 + 10,992 字节解码器 + 336 字节 RGB565 行缓冲，共 **39,552 字节** |
| 余额模式 | 释放全部桌宠绘图缓冲 |
| 固件应用 | 约 **1.32 MB**，动画不再编译进程序 |
| Flash 分区 | 4 MB `no_ota.csv`，2 MiB 应用 + 1,966,080 字节文件系统，无双分区 OTA |

相较上一版 67,440 字节绘图缓冲，减少 **27,888 字节（约 41%）**。另外有固定调色板、文件句柄、Wi-Fi、JSON 和任务栈开销；后台网络任务使用 12 KB 栈。网页展示设备实际可用 RAM。少选动画主要减少 Flash 占用，播放缓冲大小不会随动画数量增加。

从 640×360 原视频裁掉透明空白后直接缩小到 **168×168**，128 色、6 fps、逐帧 zlib 压缩，横纵各保留 72 像素移动范围。裁剪以统一参考框为下限，较大动作扩框以保留完整画面，避免从小图放大造成模糊。Flash 分块解压，移动时复用索引帧，逐行转换 RGB565 绘制，不分配全屏或双帧缓冲。

更新时复用仍然选中的文件，删除不再选择的文件，下载到临时文件，经过大小、SHA-256、包头和索引校验后提交清单。下载失败会显示错误并自动重试。由于容量有限，替换过程中会先回收旧文件；中断后保留仍可校验的缓存子集，联网后补齐。不要在同步过程中断电；下载完成后可离线使用。

## 硬件与构建

| 硬件 | 连接 |
| --- | --- |
| ESP32-C6FH4 | 4 MB Flash，无 PSRAM |
| ST7789 240×240 | MOSI 6、SCLK 7、CS 14、DC 15、RST 21、BL 22 |
| BMI160 | SDA 2、SCL 1，探测 0x69 / 0x68，仅启用加速度计 |

1. 安装 PlatformIO，复制 `include/secrets_example.h` 为 `include/secrets_local.h`，填写 2.4 GHz Wi-Fi 与香橙派地址。私密配置已忽略。
2. `python -m platformio run` 编译，`python -m platformio run -t upload` 烧录。默认 COM5，不同设备请修改 `platformio.ini`。
3. 首次使用新分区会格式化动画缓存，保留地址相同的 NVS。请先保留完整 Flash 备份。静态图已提交，动画由网关下载。

重建香橙派动画库（主机需要 Python、Pillow、imageio-ffmpeg）：

```sh
python -m pip install Pillow imageio-ffmpeg
python tools/prepare_library.py
```

固定上游提交见 `assets/library-sources.json`。生成的视频、缩略图、动画包和目录位于 `.cache/pet-library/`，不提交 Git。将该目录的内容复制到香橙派 `~/.local/share/espfish/library/`。每个 `.pak` 使用 `FISHPAK2` 包头、128 色 RGB565 调色板、帧偏移表和独立 zlib 帧。全部 106 包、6,329 帧已经过大小、哈希、偏移、解压和索引范围核验。

## 香橙派服务

### 余额代理（8081）

现有服务保持独立。`gateway/balance_service.py` 从 `DEEPSEEK_API_KEY` 环境变量读取密钥，查询并缓存余额。密钥只保存在香橙派受限环境文件中，不写入设备或仓库。

新部署时创建 `espfish` 用户，将脚本放入 `/opt/espfish/balance_service.py`，将环境文件放入 `/etc/espfish/balance.env`（root:espfish，640），再安装 `gateway/espfish-balance.service` 并启用。

### 动画与模式网关（8082）

Python 标准库，无 API 密钥。在香橙派以常用 SSH 用户执行：

```sh
mkdir -p ~/.local/share/espfish/library ~/.config/systemd/user
# 复制 gateway/control_service.py 和 gateway/index.html 到 ~/.local/share/espfish/
# 复制生成的动画库内容到 ~/.local/share/espfish/library/
# 复制 gateway/espfish-control.service 到 ~/.config/systemd/user/
loginctl enable-linger "$USER"
systemctl --user daemon-reload
systemctl --user enable --now espfish-control
```

`enable-linger` 支持退出 SSH 与重启后运行；部分系统需管理员授权。配置默认存放在 `~/.local/state/espfish/control.json`。可用环境变量 `CONTROL_STATE_FILE`、`CONTROL_LIBRARY`、`CONTROL_HOST`、`CONTROL_PORT` 覆盖路径或监听地址。两个服务仅用于可信局域网。

| 接口 | 用途 |
| --- | --- |
| `GET /` | 动画选择与设备控制网页 |
| `GET /api/library` | 动画目录、已选清单、容量上限 |
| `GET /api/state` | 模式、播放清单、同步状态和设备遥测 |
| `POST /api/playlist` | `{"animations":["动画 ID"]}`，校验容量后持久保存 |
| `GET /assets/<id>.pak` | 所选设备动画包，按目录白名单提供 |
| `GET /media/<id>.webm` | 原动画预览 |
| `GET /thumbs/<id>.jpg` | 设备尺寸缩略图 |
| `POST /api/mode` | `mode: balance/pet` 或布尔方向设置 |
| `POST /api/device` | 上报状态，返回配置及紧凑下载清单 |
| `POST /api/calibrate` | `{}`，重新校准当前姿态 |

## 验证与还原

`python -m unittest discover -s gateway -v` 验证余额、配置持久化、容量拒绝、清单生成、文件白名单、跨站限制等。实机已检查默认四段下载、清理不再选择的动画、选择新动作、失败自动重试、单动画播放、十次模式切换及断开网关后重启缓存播放。

动画库改动前的清晰版为提交 **`aa1447c`**；原始余额显示为标签 **`baseline-display`**（`2ba6a06`）。在独立目录检出对应版本，复制本机私密配置，再编译烧录即可回退。分区随对应版本恢复；本地完整 Flash 备份也能恢复当时文件和设置，备份包含私密网络配置，请勿公开。

停用网关：`systemctl --user disable --now espfish-control`。只有确定其他用户服务不依赖时才考虑关闭 linger。

## 素材许可

- 静态图来自 [MeteorNOX/DeepSeek-Balance-Whale-Widget](https://github.com/MeteorNOX/DeepSeek-Balance-Whale-Widget)，MIT，见 `assets/LICENSE-MeteorNOX`。
- 动画取自 [Bai-Cute/big-blue-fish-desktop-pet](https://github.com/Bai-Cute/big-blue-fish-desktop-pet)，原作者 [PC2005-cloud/dsh-pet](https://github.com/PC2005-cloud/dsh-pet)，MIT，见 `assets/LICENSE-PC2005-cloud`。完整来源、源文件及转换后 SHA-256 见 `assets/library-sources.json`。仅适配动画素材，未复制桌面项目代码。
