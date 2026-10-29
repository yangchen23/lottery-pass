# Lottery Pass · 彩票模拟器

一个跑在 **FoloToy AI Passport**（ESP32-C3，8 MB Flash，240×320，三键）上的本地彩票模拟器。

七款彩种、机选 1–5 注、逐位改号、摇奖动画与逐注对奖。开奖号码由设备片内硬件 RNG 生成，**完全离线**：不联网、不记录、不涉及真实购彩。

> 本项目基于 [FoloToy/ai-passport](https://gitee.com/FoloToy/ai-passport) 的 BSP 与工程骨架开发，遵循原仓库 MIT 许可。

---

## 支持的彩种

| 彩种 | 选号 | 对奖规则 |
| --- | --- | --- |
| 双色球 | 红 6/33 + 蓝 1/16 | 一至六等奖，六等奖只认蓝球 |
| 大乐透 | 前 5/35 + 后 2/12 | 一至九等奖，含 0+2、1+2 |
| 福彩 3D | 三位数字 | 直选 1040 / 组三 346 / 组六 173 |
| 排列三 | 三位数字 | 同福彩 3D |
| 排列五 | 五位数字 | 定位全中 100000 |
| 七星彩 | 七位数字 | 按位计中，2–7 位对应六至一等奖 |
| 七乐彩 | 基本 7/30 + 特别 1/30 | 一至七等奖，特别号与基本号互斥 |

浮动奖（一、二等奖等）用固定模拟值，仅用于演示。

## 操作

- **主菜单**：上下选彩种，OK 进入，长按 OK 进设置
- **列表**：一屏列出每一注，上下切换，最后一行「开奖」按 OK 开始
- **编辑**：在某注上按 OK，上下改当前位，OK 跳下一位
- **双击 OK**：全部重新机选
- **设置**：息屏时间、开奖音效、开奖速度、屏幕亮度、修改机选注数、关于

设置写入 NVS，重启保留。息屏为真实关背光，任意键唤醒。

## 构建

需要 ESP-IDF **5.5.3**（目标 `esp32c3`）。

```bash
cd lottery-pass
idf.py set-target esp32c3
idf.py build
idf.py merge-bin -o FoloToy-AI-Passport-full.bin
```

烧录（合并镜像从 `0x0` 写入）：

```bash
python -m esptool --chip esp32c3 -b 460800 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

也可以直接用官方网页刷机工具：<https://ai-passport.folotoy.cn/tools/web-flasher/>

## 目录

```
main/
  main.c        外设初始化、按键队列、随机源
  lottery.c     彩种定义、随机选号、开奖、奖级判定、开机自检
  app_ui.c      界面、状态机、音效、息屏、设置
components/bsp/ 官方板级支持包（显示/按键/音频/电量/I2C）
assets/fonts/   三档中文字体子集（12/16/20 px，由 lv_font_conv 生成）
tools/          字体字符集提取脚本
```

## 重新生成中文字体

界面文案改动后，重新提取字符并生成子集：

```bash
python tools/extract_symbols.py
npx lv_font_conv --font NotoSansCJKsc-Regular.otf \
  --range 0x20-0x7E --symbols "$(cat tools/symbols.txt)" \
  --size 16 --bpp 4 --format lvgl --no-compress \
  --lv-font-name app_font_16 --lv-include lvgl.h -o assets/fonts/app_font_16.c
```

源字体用 [Noto Sans CJK SC](https://github.com/googlefonts/noto-cjk)（SIL OFL 1.1），未随仓库附带。

## 说明

- 开奖结果由 ESP32-C3 硬件随机数生成，与任何真实彩票开奖无关。
- 奖级按公开规则实现，浮动奖为固定模拟值。
- 仅供娱乐，不构成投资、购彩或任何形式的建议。

## License

MIT，见 [LICENSE](LICENSE)。`components/bsp` 与工程骨架版权归 FoloToy 及原仓库贡献者。
