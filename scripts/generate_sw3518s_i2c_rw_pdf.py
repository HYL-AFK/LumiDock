import importlib.util
import os
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import Flowable, Paragraph, PageBreak, SimpleDocTemplate, Spacer


ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get(
    "SW3518S_I2C_PDF_OUT",
    str(ROOT / "参考资料" / "ESP32-SW3518S_I2C读写通信协议_V1.0.pdf"),
))
TEMPLATE = Path(r"D:\Project\VoltPilot\scripts\generate_stc_protocol_pdf.py")


def load_template():
    spec = importlib.util.spec_from_file_location("stc_pdf_template", TEMPLATE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class VariableFrameDiagram(Flowable):
    """Template-style frame diagram with explicit byte labels for variable fields."""

    def __init__(self, fields, width=170 * mm, height=20 * mm):
        super().__init__()
        self.fields, self.width, self.height = fields, width, height

    def wrap(self, avail_width, avail_height):
        return min(self.width, avail_width), self.height

    def draw(self):
        total = sum(size for _, size, _ in self.fields)
        x = 0
        for label, size, byte_text in self.fields:
            w = self.width * size / total
            self.canv.setStrokeColor(colors.HexColor("#111827"))
            self.canv.setLineWidth(0.9)
            self.canv.rect(x, 0, w, self.height, fill=0, stroke=1)
            lines = label.split("\n")
            self.canv.setFont("CN", 8.2 if len(lines) == 1 else 7.5)
            if len(lines) == 1:
                self.canv.drawCentredString(x + w / 2, self.height / 2 + 3, lines[0])
            else:
                self.canv.drawCentredString(x + w / 2, self.height / 2 + 8, lines[0])
                self.canv.drawCentredString(x + w / 2, self.height / 2 - 1, lines[1])
            self.canv.setFont("CN", 7.5)
            self.canv.drawCentredString(x + w / 2, self.height / 2 - 11, byte_text)
            x += w


def main():
    template = load_template()
    font = template.setup_font()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    styles = getSampleStyleSheet()
    title = ParagraphStyle(
        "title", parent=styles["Title"], fontName=font, fontSize=20, leading=28,
        alignment=TA_CENTER, textColor=colors.HexColor("#111827"), spaceAfter=8 * mm,
    )
    h1 = ParagraphStyle(
        "h1", parent=styles["Heading1"], fontName=font, fontSize=13, leading=18,
        textColor=colors.HexColor("#111827"), spaceBefore=4 * mm, spaceAfter=3 * mm,
    )
    h2 = ParagraphStyle(
        "h2", parent=styles["Heading2"], fontName=font, fontSize=10, leading=14,
        textColor=colors.HexColor("#1F2937"), spaceBefore=3 * mm, spaceAfter=2 * mm,
    )
    body = ParagraphStyle(
        "body", parent=styles["BodyText"], fontName=font, fontSize=8.5, leading=13,
        textColor=colors.HexColor("#1F2937"),
    )
    small = ParagraphStyle("small", parent=body, fontSize=7.2, leading=10)
    doc = SimpleDocTemplate(
        str(OUT), pagesize=A4, leftMargin=18 * mm, rightMargin=18 * mm,
        topMargin=17 * mm, bottomMargin=18 * mm,
        title="ESP32-SW3518S I2C 读写通信协议",
    )

    P = lambda text, style: Paragraph(text, style)
    FrameDiagram = template.FrameDiagram
    ExampleDiagram = template.ExampleDiagram
    build_table = template.build_table
    story = [P("ESP32-SW3518S I2C 读写通信协议", title)]
    story += [
        P("适用接口：ESP32-S3 与 SW3518S 的 I2C 主从通信", body),
        P("通信参数：I2C 主机；7 位从机地址 0x3C；支持 100 kHz / 400 kHz；SDA/SCK；不使用 UART 式 CRC。I2C 的 ACK/NACK 用于链路确认。", body),
        P("物理收发方向：ESP32 GPIO15 连接 SW3518S Pin 20 SDA；ESP32 GPIO16 连接 SW3518S Pin 21 SCK/SCL；两端共地。ESP32 为主机，SW3518S 为从机。", body),
        P("报文方向：下行表示 ESP32 向 SW3518S 写入寄存器地址、配置数据或控制命令；上行表示 SW3518S 根据寄存器地址返回状态数据。所有事务均由 ESP32 发起，SW3518S 不主动发送无请求数据。", body),
        Spacer(1, 5 * mm),
        P("1. 适用范围", h1),
        P("本规范定义 ESP32 与单个 SW3518S 之间的 I2C 读写时序、寄存器访问方式、运行时控制、配置寄存器解锁和读回校验方法。", body),
        P("2. 通用约定", h1),
        P("SW3518S 原厂资料中的 SCK 即 I2C Clock，等同标准 I2C 的 SCL。ESP-IDF 使用 7 位地址 0x3C；物理字节层写地址为 0x78，读地址为 0x79。I2C 事务不增加帧头、LEN、CRC 或帧尾。", body),
        P("普通读取采用“写寄存器地址 + 重复起始读取”；普通写入采用“写寄存器地址 + 数据”。每个解锁步骤必须使用独立的 I2C 写事务。", body),
        PageBreak(),
    ]

    story += [P("3. ESP32 -> SW3518S 下行写事务", h1), P("普通写入格式：寄存器地址后跟 1 个或多个数据字节", h2)]
    story += [
        VariableFrameDiagram([
            ("从机地址(W)", 1, "1 字节"), ("寄存器地址", 1, "1 字节"),
            ("数据", 1, "N 字节"), ("STOP", 1, "-"),
        ], height=20 * mm),
        Spacer(1, 3 * mm),
    ]
    write_rows = [
        ["偏移", "字段", "字节", "类型/字节序", "对应值", "十六进制含义与说明"],
        ["0", "从机地址(W)", "1", "I2C 地址字节", "0x78", "7 位地址 0x3C 左移后的写地址；ESP-IDF 不直接传 0x78"],
        ["1", "寄存器地址", "1", "u8", "0x00-0xFF", "SW3518S 目标寄存器"],
        ["2...", "数据", "N", "u8[N]", "0x00-0xFF", "按目标寄存器定义写入 1 个或多个字节"],
        ["结束", "STOP", "-", "I2C", "-", "完成本次写事务；解锁序列的每一步必须单独 STOP"],
    ]
    story += [build_table(write_rows, [14 * mm, 31 * mm, 12 * mm, 31 * mm, 38 * mm, 44 * mm], font)]
    story += [P("3.1 运行时控制寄存器", h2)]
    control_rows = [
        ["寄存器", "字段", "字节", "类型/字节序", "对应值", "十六进制含义与说明"],
        ["0x16", "PWR Control", "1", "u8", "0x02 / 0x01", "bit1=1（数据 0x02）强制打开 Buck；bit0=1（数据 0x01）强制关闭 Buck；0x03 不允许使用"],
        ["0x70", "PD Common Request", "1", "u8", "0x81", "bit7=1 发送命令，bit3:0=1 为 Hard Reset，所以写入 0x81；bit7 发送后由硬件自动清零"],
        ["0x71", "PD Common Enable", "1", "u8", "bit4..bit1", "bit4=Get Source，bit3=Get Status，bit2=VCONN Swap，bit1=DR Swap；对应位写 1 才使能"],
        ["0x76", "Connect Options", "1", "u8", "bit2..bit0", "bit2=5V CC，bit1=BC1.2，bit0=取消 CC1/CC2 驱动；写 1 表示使能该选项"],
        ["0x13", "ADC Vin Enable", "1", "u8", "bit6 / bit1", "bit1=1 开启 Vin ADC；bit6=1 时 PPS/SCP 使用固定 45 度 NTC 值；保留位必须保持 0"],
        ["0x3A", "ADC Config", "1", "u8", "0x01/02/03/04/06", "bit2:0 选择 12 位 ADC：1=Vin，2=Vout，3=Iout1，4=Iout2，6=NTC；随后读 0x3B/0x3C"],
    ]
    story += [build_table(control_rows, [14 * mm, 31 * mm, 12 * mm, 31 * mm, 38 * mm, 44 * mm], font)]
    story += [P("3.2 Buck 控制解锁与写入示例", h2)]
    story += [
        ExampleDiagram([
            ("写0x15", "20", 1), ("写0x15", "40", 1), ("写0x15", "80", 1),
            ("写0x16", "02", 1),
        ], height=25 * mm),
        P("完整事务序列：START 78 15 20 STOP；START 78 15 40 STOP；START 78 15 80 STOP；START 78 16 02 STOP。将最后一个数据改为 01 即为强制关闭 Buck。", small),
        P("3.3 PD Hard Reset 示例", h2),
        ExampleDiagram([("写地址", "78", 1), ("寄存器", "70", 1), ("数据", "81", 1), ("STOP", "-", 1)], height=25 * mm),
        P("完整事务：START 78 70 81 STOP。发送后转到第 4 节读取 0x06、0x07、0x08 以及电压电流寄存器，确认协商结果。", small),
        P("3.4 配置寄存器下行写入", h2),
        P("本小节仍属于下行：ESP32 通过第 3 节的写事务把工程配置写入 SW3518S。写入 0xA0~0xBF 前必须分别完成 0x12 解锁序列；这些寄存器部分标记为 R/W OTP，只能在确认 IC 版本、功率级和厂家参数后使用。", body),
        ExampleDiagram([("写0x12", "20", 1), ("写0x12", "40", 1), ("写0x12", "80", 1), ("写配置", "A6~BF", 2)], height=25 * mm),
        P("完整解锁序列：START 78 12 20 STOP；START 78 12 40 STOP；START 78 12 80 STOP。随后才能执行例如 START 78 A6 01 STOP（选择 24W）的配置写入。写完必须用第 4 节上行事务回读校验。", small),
    ]

    config_rows = [
        ["寄存器", "字段", "字节", "类型/字节序", "对应值", "十六进制含义与说明"],
        ["0xA6", "Power Config", "1", "u8", "bit1:0", "只看低 2 位：0=18W，1=24W，2=36/45W，3=60W。例如写 data=0x01 选择 24W；PD/SCP 或双口开启时该值可能无效"],
        ["0xAA", "Fast Charge CONF0", "1", "u8", "bit6", "bit6=1 开启 QC3.0，bit6=0 关闭；其余位为 OTP 保留位，不应改写"],
        ["0xAB", "Port CONF", "1", "u8", "bit3:2", "取 (data>>2)&3：0=单口 A，1=双口 A，2=单口 C，3=A+C；例如 data=0x0C 表示 A+C"],
        ["0xB0~0xB4", "PD Config0~4", "5", "u8", "bit7 / bit6:0", "分别配置 PD 固定 5V/9V/12V/15V/20V；bit7=0 才使用低 7 位，电流=低 7 位*50mA。例如 0x1E=30*50mA=1.50A"],
        ["0xB5~0xB6", "PPS Config", "2", "u8", "bit7 / bit6:0", "PPS0/PPS1 电流；bit7=0 使用低 7 位，电流=低 7 位*50mA。bit7=1 时由芯片按 20V PDO 自动计算"],
        ["0xB7", "PD Config7", "1", "u8", "bit7:0", "bit7/6=PPS1/PPS0，bit5..2=20V/15V/12V/9V 固定 PDO，bit1=读电子标记，bit0=PD3.0；每一位写 1 表示使能"],
        ["0xB8", "PD Config8", "1", "u8", "bit6:0", "bit6=DR Swap，bit5=PD 5V/2A，bit4=65W 电子标记检测，bit3=Hard Reset 后禁用 PPS，bit2=Discovery Identity，bit1:0=PDO 峰值电流信息"],
        ["0xB9", "Fast Charge Config1", "1", "u8", "bit7..0", "bit7=C 快充，bit6=A 快充，bit5=PD，bit4=QC，bit3=FCP，bit2=SCP，bit0=PE；对应位写 1 开启"],
        ["0xBA", "Fast Charge Config2", "1", "u8", "bit6 / bit3:2", "bit6=1 开启 AFC；bit3:2=0/1/2(或3) 时非 PD 最大电压为 9V/12V/20V"],
        ["0xBC", "Fast Charge Config3", "1", "u8", "bit7/6/3", "bit7=1 关闭快充空载检测；bit6=1 关闭峰值电流；bit3=1 关闭 C 口空载检测。这里 0 表示功能开启，1 表示关闭"],
        ["0xBD", "Current Limit Config", "1", "u8", "bit6 / bit5:4", "bit6=1 开启双口 DPDM；bit5:4 选择双口时单口限流：0=2.6A，1=2.2A，2=1.7A，3=3.2A"],
        ["0xBE", "PD Config9", "1", "u8", "bit7..0", "bit7/3=PPS1/PPS0 电压配置使能；bit6/2=功率限制；bit5:4 与 bit1:0 的编码 0/1/2/3 对应 5.9V/11V/16V/21V"],
        ["0xAF/0xBF", "VID Config0/1", "2", "u8", "8+8 位", "0xAF 是 VID[15:8]，0xBF 是 VID[7:0]。例如 VID=0x1234 时写 0xAF=0x12、0xBF=0x34"],
    ]
    story += [build_table(config_rows, [14 * mm, 31 * mm, 12 * mm, 31 * mm, 38 * mm, 44 * mm], font),
              P("3.5 下行写后校验与限制", h2)]
    write_limit_rows = [
        ["项目", "判定", "处理"],
        ["版本检查", "第 4 节读 0x01 后 data&0x07 不是已支持版本", "允许基础状态读取，但禁止执行 0x12/0x15 解锁和配置写入"],
        ["运行时安全", "过流、过温、看门狗或系统故障", "先执行 0x15=20、40、80 解锁，再写 0x16=0x01 关闭 Buck；转到第 4 节读 0x07 确认 bit2=0"],
        ["写后校验", "下行写入后上行读回值与期望不一致", "记录寄存器地址、写值、读值；停止后续配置，不连续覆盖写入"],
        ["配置风险", "0xA0~0xBF 标记 R/W OTP", "只在工程配置模式使用，必须确认 IC 版本、功率级和厂家参数；普通用户流程禁止写"],
    ]
    story += [build_table(write_limit_rows, [36 * mm, 60 * mm, 64 * mm], font), PageBreak()]

    story += [P("4. SW3518S -> ESP32 上行读回事务", h1),
              P("本节就是上行通信：ESP32 先发起读取请求，SW3518S 不主动推送数据。下面的状态寄存器、协议类型、电压和电流，全部通过本节的读事务返回给 ESP32。", body),
              P("普通读取格式：先写寄存器地址，再重复起始读取数据", h2)]
    story += [
        VariableFrameDiagram([
            ("从机地址(W)", 1, "1 字节"), ("寄存器地址", 1, "1 字节"),
            ("RESTART", 1, "-"), ("从机地址(R)", 1, "1 字节"),
            ("数据", 1, "N 字节"), ("NACK/STOP", 1, "-"),
        ], height=20 * mm),
        Spacer(1, 3 * mm),
    ]
    read_rows = [
        ["偏移", "字段", "字节", "类型/字节序", "对应值", "十六进制含义与说明"],
        ["0", "从机地址(W)", "1", "I2C 地址字节", "0x78", "发送寄存器地址阶段"],
        ["1", "寄存器地址", "1", "u8", "0x00-0xFF", "指定读取起始寄存器"],
        ["2", "从机地址(R)", "1", "I2C 地址字节", "0x79", "重复起始后的读地址；ESP-IDF 仍传 0x3C"],
        ["3...", "返回数据", "N", "u8[N]", "0x00-0xFF", "按寄存器定义解释；最后一个字节由主机 NACK"],
        ["结束", "STOP", "-", "I2C", "-", "结束本次读取事务"],
    ]
    story += [build_table(read_rows, [14 * mm, 31 * mm, 12 * mm, 31 * mm, 38 * mm, 44 * mm], font)]
    story += [P("读取协议寄存器 0x06 示例", h2)]
    story += [
        ExampleDiagram([("写地址", "78", 1), ("寄存器", "06", 1), ("读地址", "79", 1), ("数据", "P", 1), ("NACK", "-", 1)], height=25 * mm),
        P("完整事务：START 78 06 RESTART 79 [data] NACK STOP。这里的 78/79 是物理地址字节；ESP-IDF 的设备地址参数仍填 0x3C。假设返回 data=0x26，则 bit5:4=2 表示 PD3.0，bit3:0=6 表示 PD PPS。", small),
    ]
    story += [P("4.1 上行状态寄存器及十六进制解释", h2),
              P("本节的每一行都属于上行数据。ESP32 读到一个字节后，不能把整个字节直接当作十进制数使用，而要按 bit 位拆分；带有“倍率”的 ADC 数据还要先乘以倍率，才能得到伏特、安培或欧姆。", body),
              P("十六进制约定：0x 表示十六进制；bit7 是最高位，bit0 是最低位；bit5:4 表示连续两位。比如 0x26 = 0010 0110，bit5:4=2，bit3:0=6。", small)]
    status_rows = [
        ["寄存器", "字段", "字节", "类型/字节序", "对应值", "十六进制含义与说明"],
        ["0x01", "IC Version", "1", "u8", "bit2:0", "读取 data & 0x07；当前 RG004_2_v2.1 的正常值为 0x07，表示 IC version 7。bit7:3 保留，不参与业务判断"],
        ["0x06", "Protocol Indication", "1", "u8", "bit7/5:4/3:0", "bit7=快充 LED；bit5:4=1 PD2.0、2 PD3.0；bit3:0 协议码：1 QC2.0、2 QC3.0、3 FCP、4 SCP、5 PD FIX、6 PD PPS、7 PE1.1、8 PE2.0、9 VOOC、A SFCP、B AFC"],
        ["0x07", "System Status", "1", "u8", "bit2/1/0", "bit2=1 Buck 开启；bit1=1 Type-A 开启；bit0=1 Type-C 开启。例如 data=0x05（二进制 101）表示 Buck 和 Type-C 开启"],
        ["0x08", "External Device", "1", "u8", "bit7:4", "取 data >> 4。AA 模式：1=无设备、2=仅 A1、3=仅 A2、4=A1+A2；AC 模式：5=无设备、6=仅 C、7=仅 A、8=A+C"],
        ["0x30", "ADC Vin Data", "1", "u8", "160 mV/bit", "输入电压 Vin = data * 0.160 V。例如 data=0x32（50）时 Vin=8.00 V"],
        ["0x31", "ADC Vout Data", "1", "u8", "96 mV/bit", "输出电压 Vout = data * 0.096 V。例如 data=0x5A（90）时 Vout=8.64 V"],
        ["0x33", "ADC Iout1 Data", "1", "u8", "40 mA/bit", "Type-C 输出电流 Iout1 = data * 0.040 A。例如 data=0x19（25）时为 1.00 A"],
        ["0x34", "ADC Iout2 Data", "1", "u8", "40 mA/bit", "Type-A 输出电流 Iout2 = data * 0.040 A；该寄存器对应 Port A"],
        ["0x37", "ADC NTC Data", "1", "u8", "8 mV/bit", "NTC 电压 Vntc=data*0.008 V；电阻 Rntc=Vntc/0.0001-2000 欧姆。该值用于温度换算"],
        ["0x3A", "ADC Config", "1", "u8", "1/2/3/4/6", "先写入选择值，再读 0x3B/0x3C：1=Vin(10mV/bit)，2=Vout(6mV/bit)，3=Iout1(2.5mA/bit)，4=Iout2(2.5mA/bit)，6=NTC(0.5mV/bit)"],
        ["0x3B/0x3C", "ADC Data", "2", "u8[2]", "12 位", "adc=(0x3B<<4)|(0x3C&0x0F)。例如 0x3B=0x01、0x3C=0x02，则 adc=0x012=18；再按 0x3A 的倍率换算"],
    ]
    story += [build_table(status_rows, [14 * mm, 31 * mm, 12 * mm, 31 * mm, 38 * mm, 44 * mm], font),
              P("4.2 上行数据有效性与业务应用", h2),
              P("上行读回后，ESP32 应按以下顺序处理：1）确认 I2C 地址阶段 ACK、返回长度和最后一个字节 NACK；2）读 0x01，确认 data&0x07=0x07；3）读 0x06，按 bit5:4 和 bit3:0 解码协议名称；4）读 0x30/31/33/34，按倍率换算 Vin、Vout、Type-C 电流和 Type-A 电流；5）把结果显示到 OLED。若读回长度不符、数据未完成或 I2C NACK/超时，则丢弃本次数据，不刷新 OLED 上的有效值，并记录通信无效。", body),
              P("4.3 一次完整业务交互示例", h2),
              P("ESP32 周期性上行读取 0x01、0x06、0x07、0x30、0x31、0x33、0x34；需要改变 Buck 或发送 Hard Reset 时，再下行写入 0x15/0x16 或 0x70；每次下行写入后，回到本节上行读取 0x06、0x07 和相关 ADC 寄存器确认结果。", body)]

    def footer(canvas, document):
        canvas.saveState()
        canvas.setStrokeColor(colors.HexColor("#9CA3AF"))
        canvas.line(document.leftMargin, 12 * mm, A4[0] - document.rightMargin, 12 * mm)
        canvas.setFont("CN", 7)
        canvas.setFillColor(colors.HexColor("#4B5563"))
        canvas.drawString(document.leftMargin, 7.5 * mm, "ESP32-SW3518S I2C 读写通信协议 V1.0")
        canvas.drawRightString(A4[0] - document.rightMargin, 7.5 * mm, f"第 {document.page} 页")
        canvas.restoreState()

    doc.build(story, onFirstPage=footer, onLaterPages=footer)
    print(OUT)


if __name__ == "__main__":
    main()
