from pathlib import Path

from docx import Document
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml.ns import qn
from docx.shared import Inches, Pt, RGBColor

from generate_sw3518s_i2c_docx import add_table, body, code_line, heading, set_run


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "参考资料" / "ESP32-SW3518S_I2C读写通信协议_V1.0.docx"
FONT = "Microsoft YaHei"
GRAY = "555555"


def add_bullet(doc, text):
    p = doc.add_paragraph(style="List Bullet")
    p.paragraph_format.space_after = Pt(3)
    p.paragraph_format.line_spacing = 1.15
    set_run(p.add_run(text), 10.5)
    return p


def configure_doc(doc):
    section = doc.sections[0]
    section.top_margin = Inches(0.7)
    section.bottom_margin = Inches(0.7)
    section.left_margin = Inches(0.78)
    section.right_margin = Inches(0.78)
    section.header_distance = Inches(0.3)
    section.footer_distance = Inches(0.3)

    normal = doc.styles["Normal"]
    normal.font.name = FONT
    normal._element.rPr.rFonts.set(qn("w:eastAsia"), FONT)
    normal.font.size = Pt(10.5)
    normal.paragraph_format.space_after = Pt(5)
    normal.paragraph_format.line_spacing = 1.18
    for name, size, before, after in (("Heading 1", 15, 14, 7), ("Heading 2", 12.5, 10, 5)):
        style = doc.styles[name]
        style.font.name = FONT
        style._element.rPr.rFonts.set(qn("w:eastAsia"), FONT)
        style.font.size = Pt(size)
        style.font.bold = True
        style.font.color.rgb = RGBColor.from_string("000000")
        style.paragraph_format.space_before = Pt(before)
        style.paragraph_format.space_after = Pt(after)

    footer = section.footer.paragraphs[0]
    footer.alignment = WD_ALIGN_PARAGRAPH.RIGHT
    footer.paragraph_format.space_after = Pt(0)
    set_run(footer.add_run("LumiDock | ESP32-SW3518S I2C 读写通信协议"), 8.5, color=GRAY)


def make_doc():
    doc = Document()
    configure_doc(doc)

    title = doc.add_paragraph()
    title.paragraph_format.space_after = Pt(2)
    set_run(title.add_run("ESP32-SW3518S I2C 读写通信协议"), 20, True)
    subtitle = doc.add_paragraph()
    subtitle.paragraph_format.space_after = Pt(12)
    set_run(subtitle.add_run(
        "适用于 LumiDock ESP32-S3 | SW3518S I2C 主机通信 | V1.0 | 2026-07-31"
    ), 9.5, color=GRAY)

    heading(doc, "1. 适用范围")
    body(doc, "本协议定义 LumiDock ESP32-S3 主控与 SW3518S 快充电源管理芯片之间的 I2C 读写通信方式。ESP32 是 I2C 主机，SW3518S 是 I2C 从机。协议包括状态读取、运行时控制、配置寄存器写入、应答判断和异常处理。")
    body(doc, "本文只定义 ESP32 与 SW3518S 之间的 I2C 通信，不包含 USB PD、QC、FCP、SCP、AFC 等协议本身的电气协商过程。上述协议由 SW3518S 内部完成，ESP32 通过寄存器读取或修改其状态和配置。")

    heading(doc, "2. 通信参数")
    add_table(doc, ("项目", "规定", "LumiDock 实现"), [
        ("主机/从机", "ESP32 为主机，SW3518S 为从机", "ESP32 发起全部读写事务"),
        ("I2C 地址", "7 位地址 0x3C", "ESP-IDF 传入 0x3C；物理写/读地址分别为 0x78/0x79"),
        ("速率", "支持 100 kHz 和 400 kHz", "首版使用 400 kHz；联调异常时降为 100 kHz"),
        ("信号", "SDA、SCK", "SDA=GPIO15，SCK/SCL=GPIO16"),
        ("芯片引脚", "Pin 20=SDA，Pin 21=SCK", "Pin 21 在原厂资料中即 I2C Clock"),
        ("校验", "I2C ACK/NACK", "总线不增加 UART 式 CRC 字段；软件检查 NACK、超时和读回值"),
    ], (1800, 3300, 4260))
    body(doc, "原厂 Datasheet DS031 v1.2 第 9 页明确给出 I2C Read Timing 和 Write Timing，并规定从机地址为 0x3C。SDA/SCK 的上拉电压必须低于 ESP32-S3 可承受电平；若模块上拉至 5 V，必须增加双向电平转换。")

    heading(doc, "3. I2C 事务定义")
    heading(doc, "3.1 下行事务：ESP32 -> SW3518S", level=2)
    body(doc, "下行表示 ESP32 向 SW3518S 发送寄存器地址、配置数据或控制命令。普通寄存器写入不包含帧头、长度和 CRC，I2C 的 ACK/NACK 即为链路级确认。")
    code_line(doc, "START -> 0x78(W) -> ACK -> 寄存器地址 -> ACK -> 数据字节... -> ACK -> STOP")
    body(doc, "解锁序列中的每一步必须是独立的寄存器写事务，不能把三个解锁值合并成一次连续写入。")

    heading(doc, "3.2 上行事务：SW3518S -> ESP32", level=2)
    body(doc, "上行表示 SW3518S 返回寄存器数据。ESP32 先发送寄存器地址，再用重复起始读取数据。")
    code_line(doc, "START -> 0x78(W) -> ACK -> 寄存器地址 -> ACK")
    code_line(doc, "RESTART -> 0x79(R) -> ACK -> 数据... -> NACK -> STOP")
    body(doc, "ESP-IDF 中应使用 7 位地址 0x3C，不要把已经左移后的 0x78 或 0x79 再传给驱动。")

    heading(doc, "4. 状态读取寄存器")
    add_table(doc, ("寄存器", "字段/分辨率", "读取内容"), [
        ("0x01", "Bit 2:0", "IC Version；RG004_2_v2.1 手册对应版本值为 0x07"),
        ("0x06", "Bit 5:4", "PD 版本：1=PD2.0，2=PD3.0"),
        ("0x06", "Bit 3:0", "协议：1 QC2.0，2 QC3.0，3 FCP，4 SCP，5 PD Fixed，6 PD PPS，7 PE1.1，8 PE2.0，9 VOOC，A SFCP，B AFC"),
        ("0x07", "Bit 2/1/0", "Buck、Type-A、Type-C 状态；1=开启"),
        ("0x08", "Bit 7:4", "外部设备状态：A+C 模式下表示两路端口连接情况"),
        ("0x30", "160 mV/bit", "输入电压"),
        ("0x31", "96 mV/bit", "输出电压"),
        ("0x33", "40 mA/bit", "Type-C 输出电流"),
        ("0x34", "40 mA/bit", "Type-A 输出电流"),
        ("0x37", "8 mV/bit", "NTC 电压数据"),
        ("0x3A", "Bit 2:0", "12 位 ADC 类型选择"),
        ("0x3B/0x3C", "12 位", "ADC 高 8 位和低 4 位"),
    ], (1500, 2100, 5760), 8.8)

    heading(doc, "5. 运行时写控制")
    body(doc, "以下寄存器用于运行时控制，建议由 LumiDock 提供白名单 API，不允许业务层直接写任意寄存器。")
    add_table(doc, ("寄存器", "写入内容", "动作"), [
        ("0x16", "bit1=1", "强制打开 Buck"),
        ("0x16", "bit0=1", "强制关闭 Buck"),
        ("0x16", "bit1/bit0=0", "不产生强制动作"),
        ("0x70", "bit7=1，bit3:0=1", "发送 PD Hard Reset；bit7 自动由硬件清零"),
        ("0x71", "按位配置", "PD Get Source、Get Status、VCONN Swap、DR Swap 开关"),
        ("0x76", "按位配置", "5V CC、BC1.2、CC1/CC2 驱动选项"),
        ("0x13", "按位配置", "ADC Vin 使能、PPS/SCP NTC 固定 45 度选项"),
        ("0x3A", "1/2/3/4/6", "选择 12 位输入电压、输出电压、电流或 NTC ADC"),
    ], (1500, 2800, 5060), 9.0)

    heading(doc, "5.1 Buck 控制解锁流程", level=2)
    code_line(doc, "写 0x15 = 0x20")
    code_line(doc, "写 0x15 = 0x40")
    code_line(doc, "写 0x15 = 0x80")
    code_line(doc, "写 0x16 = 0x02  // 强制打开 Buck")
    code_line(doc, "写 0x16 = 0x01  // 强制关闭 Buck")
    body(doc, "0x15 的解锁序列来自 Register List RG004_2_v2.1。打开和关闭不能同时设置 bit1、bit0；发生保护或看门狗故障时优先执行关闭动作。")

    heading(doc, "5.2 PD Hard Reset 写入流程", level=2)
    code_line(doc, "写 0x70 = 0x81  // bit7=发送使能，低四位=1 Hard Reset")
    body(doc, "0x70 bit7 的属性为 W/C，写入后由硬件自动清零。发送后应重新读取 0x06、0x07、0x08 和电压电流寄存器，确认协议重新协商结果。")

    heading(doc, "6. A0~BF 配置寄存器写入")
    body(doc, "寄存器手册规定，写入 0xA0~0xBF 前必须先执行 0x12 解锁序列。该区域包含功率档位、端口模式、QC/PD/FCP/SCP/AFC 开关、PD 固定 PDO 电流、PPS 配置和 VID 等内容。")
    code_line(doc, "写 0x12 = 0x20")
    code_line(doc, "写 0x12 = 0x40")
    code_line(doc, "写 0x12 = 0x80")
    code_line(doc, "写 0xA6~0xBF 中指定的配置寄存器")
    add_table(doc, ("寄存器范围", "配置内容", "策略"), [
        ("0xA6", "18W/24W/36-45W/60W 功率档位", "仅工程配置"),
        ("0xAA/0xAD", "QC3.0、Samsung 1.2V", "仅工程配置"),
        ("0xAB", "单口/双口和端口模式", "必须结合硬件端口设计"),
        ("0xB0~0xB4", "PD 5V/9V/12V/15V/20V 电流", "50 mA/bit；需厂家参数确认"),
        ("0xB5~0xB8", "PPS、电压档位、PD 功能", "需确认 PD 版本和电源能力"),
        ("0xB9~0xBD", "协议开关、最大电压、电流限制", "禁止普通用户运行时修改"),
        ("0xBE", "PPS 电压选择", "需确认产品功率和温升"),
        ("0xAF/0xBF", "USB VID", "产品配置项"),
    ], (1700, 3500, 4060), 8.8)
    body(doc, "以上寄存器在手册中标记为 R/W OTP。软件协议将其视为高风险配置寄存器：除非厂家确认当前芯片版本、写入是否只影响运行时寄存器以及是否涉及 OTP 编程，否则不在普通业务流程中写入。")

    heading(doc, "7. ESP32 软件 API 建议")
    add_table(doc, ("API", "用途", "限制"), [
        ("sw3518s_i2c_read_reg(reg, data, len)", "读取任意已确认可读寄存器", "检查参数、超时和 NACK"),
        ("sw3518s_i2c_write_reg(reg, data, len)", "底层写寄存器", "只允许上层白名单调用"),
        ("charging_get_snapshot()", "获取缓存状态", "不直接触发 I2C"),
        ("charging_force_buck(bool)", "打开/关闭 Buck", "执行 0x15 解锁和 0x16 写入"),
        ("charging_pd_hard_reset()", "发送 PD Hard Reset", "写 0x70 后重新读取状态"),
        ("charging_set_config(...)", "修改 A0~BF 配置", "只允许工程模式，写后读回"),
    ], (2600, 3600, 3060), 8.8)
    body(doc, "应用层不应直接调用底层任意寄存器写接口。所有写命令都应记录寄存器、写入值、返回码、读回值和时间戳。")

    heading(doc, "8. 轮询和异常处理")
    code_line(doc, "启动 -> I2C 初始化 -> 读取 0x01 版本 -> 读取状态寄存器")
    code_line(doc, "每 1 s 读取协议/端口/电压/电流 -> 更新 ChargingSnapshot -> 刷新屏幕")
    code_line(doc, "写控制命令 -> 等待 ACK -> 读回目标寄存器 -> 校验结果 -> 记录日志")
    add_table(doc, ("异常", "判定", "处理"), [
        ("NACK/超时", "I2C 事务返回错误", "重试 2 次，仍失败则标记数据无效"),
        ("版本不匹配", "0x01 不在已支持版本", "保留基础读取，禁止写配置"),
        ("写后读回不一致", "目标寄存器值不等于期望值", "记录错误，禁止继续写入配置"),
        ("协议状态变化", "0x06/0x07 与上次不同", "重新读取电压、电流并刷新显示"),
        ("看门狗/硬件故障", "系统保护触发", "关闭 Buck，记录故障，不发送配置写入"),
    ], (1800, 3000, 4460), 8.8)

    heading(doc, "9. 报文示例")
    body(doc, "读取协议寄存器 0x06：")
    code_line(doc, "下行：START 78 06 STOP")
    code_line(doc, "上行：START 79 [protocol] NACK STOP")
    body(doc, "强制关闭 Buck：")
    code_line(doc, "下行 1：START 78 15 20 STOP")
    code_line(doc, "下行 2：START 78 15 40 STOP")
    code_line(doc, "下行 3：START 78 15 80 STOP")
    code_line(doc, "下行 4：START 78 16 01 STOP")
    body(doc, "发送 PD Hard Reset：")
    code_line(doc, "下行：START 78 70 81 STOP")
    body(doc, "上述示例中的 0x78 和 0x79 是 I2C 物理字节地址；ESP-IDF API 参数仍使用 7 位地址 0x3C。")

    heading(doc, "10. 版本和资料依据")
    body(doc, "本协议依据厂家提供的 SW3518S Datasheet_Release_DS031_v1.2.pdf、SW3518S_Register_List_Release_RG004_2_v2.1.pdf 及 Demo Board 资料整理。固件启动后应先读取 0x01，再按照实际 IC 版本选择寄存器定义。")
    body(doc, "协议版本 V1.0 已包含读写接口，但默认仅开放状态读取、Buck 安全控制和 PD Hard Reset。A0~BF 配置寄存器必须经过工程模式和厂家参数确认后才能启用。")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    doc.save(OUT)
    print(OUT)


if __name__ == "__main__":
    make_doc()
