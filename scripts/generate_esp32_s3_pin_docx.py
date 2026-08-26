from pathlib import Path

from docx import Document
from docx.enum.section import WD_SECTION_START
from docx.enum.table import WD_ALIGN_VERTICAL, WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Inches, Pt, RGBColor


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "ESP32-S3-Super-Mini-引脚分配.docx"

FONT = "Microsoft YaHei"
MONO_FONT = "Consolas"
BLACK = "000000"
GRAY = "555555"
TABLE_WIDTH_DXA = 9360


def set_run_font(run, name=FONT, size=10.5, bold=False, color=BLACK):
    run.font.name = name
    run._element.rPr.rFonts.set(qn("w:ascii"), name)
    run._element.rPr.rFonts.set(qn("w:hAnsi"), name)
    run._element.rPr.rFonts.set(qn("w:eastAsia"), name)
    run.font.size = Pt(size)
    run.bold = bold
    run.font.color.rgb = RGBColor.from_string(color)


def set_cell_margins(cell, top=80, start=120, bottom=80, end=120):
    tc = cell._tc
    tcPr = tc.get_or_add_tcPr()
    tcMar = tcPr.first_child_found_in("w:tcMar")
    if tcMar is None:
        tcMar = OxmlElement("w:tcMar")
        tcPr.append(tcMar)
    for side, value in (("top", top), ("start", start), ("bottom", bottom), ("end", end)):
        node = tcMar.find(qn(f"w:{side}"))
        if node is None:
            node = OxmlElement(f"w:{side}")
            tcMar.append(node)
        node.set(qn("w:w"), str(value))
        node.set(qn("w:type"), "dxa")


def set_cell_shading(cell, fill):
    tcPr = cell._tc.get_or_add_tcPr()
    shd = tcPr.find(qn("w:shd"))
    if shd is None:
        shd = OxmlElement("w:shd")
        tcPr.append(shd)
    shd.set(qn("w:fill"), fill)


def set_table_geometry(table, widths):
    table.autofit = False
    table.alignment = WD_TABLE_ALIGNMENT.LEFT
    tbl = table._tbl
    tblPr = tbl.tblPr
    tblW = tblPr.first_child_found_in("w:tblW")
    if tblW is None:
        tblW = OxmlElement("w:tblW")
        tblPr.append(tblW)
    tblW.set(qn("w:w"), str(sum(widths)))
    tblW.set(qn("w:type"), "dxa")
    tblInd = tblPr.first_child_found_in("w:tblInd")
    if tblInd is None:
        tblInd = OxmlElement("w:tblInd")
        tblPr.append(tblInd)
    tblInd.set(qn("w:w"), "120")
    tblInd.set(qn("w:type"), "dxa")
    tblLayout = tblPr.first_child_found_in("w:tblLayout")
    if tblLayout is None:
        tblLayout = OxmlElement("w:tblLayout")
        tblPr.append(tblLayout)
    tblLayout.set(qn("w:type"), "fixed")

    grid = tbl.tblGrid
    for grid_col, width in zip(grid.gridCol_lst, widths):
        grid_col.set(qn("w:w"), str(width))
    for row in table.rows:
        for cell, width in zip(row.cells, widths):
            tcPr = cell._tc.get_or_add_tcPr()
            tcW = tcPr.find(qn("w:tcW"))
            if tcW is None:
                tcW = OxmlElement("w:tcW")
                tcPr.append(tcW)
            tcW.set(qn("w:w"), str(width))
            tcW.set(qn("w:type"), "dxa")
            set_cell_margins(cell)
            cell.vertical_alignment = WD_ALIGN_VERTICAL.CENTER


def set_repeat_table_header(row):
    trPr = row._tr.get_or_add_trPr()
    tblHeader = OxmlElement("w:tblHeader")
    tblHeader.set(qn("w:val"), "true")
    trPr.append(tblHeader)


def add_table(doc, headers, rows, widths, font_size=9.3):
    table = doc.add_table(rows=1, cols=len(headers))
    table.style = "Table Grid"
    set_table_geometry(table, widths)
    header_cells = table.rows[0].cells
    for cell, text in zip(header_cells, headers):
        set_cell_shading(cell, "E6E6E6")
        p = cell.paragraphs[0]
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        p.paragraph_format.space_after = Pt(0)
        r = p.add_run(text)
        set_run_font(r, size=font_size, bold=True)
    set_repeat_table_header(table.rows[0])
    for row_values in rows:
        cells = table.add_row().cells
        for index, (cell, text) in enumerate(zip(cells, row_values)):
            p = cell.paragraphs[0]
            p.alignment = WD_ALIGN_PARAGRAPH.CENTER if index == 1 else WD_ALIGN_PARAGRAPH.LEFT
            p.paragraph_format.space_after = Pt(0)
            p.paragraph_format.line_spacing = 1.05
            r = p.add_run(text)
            set_run_font(r, size=font_size)
    return table


def add_heading(doc, text, level=1):
    p = doc.add_paragraph()
    p.style = f"Heading {level}"
    p.paragraph_format.keep_with_next = True
    r = p.add_run(text)
    set_run_font(r, size={1: 15, 2: 12.5}.get(level, 11.5), bold=True)
    return p


def add_body(doc, text, bold_prefix=None):
    p = doc.add_paragraph()
    p.paragraph_format.space_after = Pt(5)
    p.paragraph_format.line_spacing = 1.18
    if bold_prefix and text.startswith(bold_prefix):
        r = p.add_run(bold_prefix)
        set_run_font(r, size=10.5, bold=True)
        r = p.add_run(text[len(bold_prefix):])
        set_run_font(r, size=10.5)
    else:
        r = p.add_run(text)
        set_run_font(r, size=10.5)
    return p


def add_note(doc, label, text):
    p = doc.add_paragraph()
    p.paragraph_format.left_indent = Inches(0.12)
    p.paragraph_format.first_line_indent = Inches(-0.12)
    p.paragraph_format.space_after = Pt(3)
    p.paragraph_format.line_spacing = 1.15
    r = p.add_run(label)
    set_run_font(r, size=10, bold=True)
    r = p.add_run(text)
    set_run_font(r, size=10)
    return p


def make_document():
    doc = Document()
    section = doc.sections[0]
    section.top_margin = Inches(0.72)
    section.bottom_margin = Inches(0.72)
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
    for style_name, size, before, after in (("Heading 1", 15, 14, 7), ("Heading 2", 12.5, 10, 5)):
        style = doc.styles[style_name]
        style.font.name = FONT
        style._element.rPr.rFonts.set(qn("w:eastAsia"), FONT)
        style.font.size = Pt(size)
        style.font.color.rgb = RGBColor.from_string(BLACK)
        style.font.bold = True
        style.paragraph_format.space_before = Pt(before)
        style.paragraph_format.space_after = Pt(after)
        style.paragraph_format.keep_with_next = True

    footer = section.footer
    fp = footer.paragraphs[0]
    fp.alignment = WD_ALIGN_PARAGRAPH.RIGHT
    fp.paragraph_format.space_after = Pt(0)
    fr = fp.add_run("ESP32-S3 Super Mini | 引脚分配参考")
    set_run_font(fr, size=8.5, color=GRAY)

    title = doc.add_paragraph()
    title.alignment = WD_ALIGN_PARAGRAPH.LEFT
    title.paragraph_format.space_after = Pt(2)
    run = title.add_run("ESP32-S3 Super Mini 引脚分配")
    set_run_font(run, size=20, bold=True)
    subtitle = doc.add_paragraph()
    subtitle.paragraph_format.space_after = Pt(12)
    r = subtitle.add_run("LumiDock 硬件接口方案 | 基于 35 GPIO 焊盘版本 | 2026-07-30")
    set_run_font(r, size=9.5, color=GRAY)

    add_heading(doc, "1. 结论与使用范围")
    add_body(doc, "本方案对应你提供图片中的 ESP32-S3 Super Mini 开发板：板上实际引出 35 个 GPIO 焊盘。该板可满足当前屏幕、双路外接 WS2812、I2S 麦克风与功放、I2C、两路 MOS、单按键、单总线、RS485 和雷达数字输出的需求。")
    add_note(doc, "重要：", "此前出现的 68 引脚通用 QFP 图不是这块开发板的实际外部焊盘图，不能用于接线或画板。ESP32-S3 芯片封装有 56 个管脚、45 个可编程 GPIO；本开发板只引出其中 35 个 GPIO。")
    add_note(doc, "ASR：", "在线语音识别可以通过 Wi-Fi 调用云端服务，GPIO 资源不受影响；离线 ESP-SR 需进一步核实模组的 Flash/PSRAM，建议至少 8 MB Flash + 8 MB PSRAM。")

    add_heading(doc, "2. 已分配 GPIO")
    allocation_rows = [
        ("WS2812 #1 数据", "GPIO1", "外接；5 V 灯带建议加电平转换与串联电阻"),
        ("WS2812 #2 数据", "GPIO2", "外接；若两路灯珠允许串接，可释放该脚"),
        ("I2S BCLK", "GPIO4", "I2S 麦克风和功放共用时钟"),
        ("I2S LRCLK / WS", "GPIO5", "I2S 麦克风和功放共用帧时钟"),
        ("I2S MIC_DATA", "GPIO6", "数字 I2S 麦克风数据输出"),
        ("I2S AMP_DIN", "GPIO7", "I2S 功放数据输入，例如 MAX98357A"),
        ("ST7789 RES", "GPIO8", "屏幕复位"),
        ("ST7789 DC", "GPIO9", "屏幕数据/命令选择"),
        ("ST7789 CS", "GPIO10", "屏幕片选"),
        ("ST7789 SDA / MOSI", "GPIO11", "SPI 数据输出"),
        ("ST7789 SCL / SCLK", "GPIO12", "SPI 时钟"),
        ("ST7789 BLK", "GPIO13", "背光；可 PWM 调光"),
        ("单按键", "GPIO14", "按键另一端接 GND，软件开启内部上拉"),
        ("I2C SDA", "GPIO15", "背面内侧焊盘"),
        ("I2C SCL", "GPIO16", "背面内侧焊盘"),
        ("MOS1", "GPIO17", "背面内侧焊盘；驱动 MOS 时需合适栅极电阻/下拉"),
        ("MOS2", "GPIO18", "背面内侧焊盘；驱动 MOS 时需合适栅极电阻/下拉"),
        ("一线通数据", "GPIO21", "开漏方式；仅真 3.3 V 单总线时加 4.7 kOhm 上拉至 3.3 V"),
        ("RS485 TX", "GPIO37", "自动收发方向的 RS485 模块"),
        ("RS485 RX", "GPIO38", "自动收发方向的 RS485 模块"),
        ("雷达 UART_RX", "GPIO39", "ESP32 接收雷达 TX 数据；背面右侧内侧焊盘"),
        ("雷达 UART_TX", "GPIO40", "ESP32 发送到雷达 RX 数据；背面右侧内侧焊盘"),
        ("雷达 OUT", "GPIO41", "雷达数字输出；背面右侧内侧焊盘，5 V 输出必须先降到 3.3 V"),
    ]
    add_table(doc, ("功能", "GPIO", "接线/说明"), allocation_rows, (3000, 1400, 4960), font_size=8.8)

    add_heading(doc, "3. ST7789 屏幕接线")
    add_body(doc, "你提供的是 ST7789 SPI TFT 屏，不是 I2C OLED。供电必须使用 3.3 V 逻辑电平。")
    display_rows = [
        ("GND", "GND", "公共地"),
        ("VCC", "3V3", "不要直接接 5 V，除非模块明确带稳压与电平转换"),
        ("SCL", "GPIO12", "SPI SCLK"),
        ("SDA", "GPIO11", "SPI MOSI"),
        ("RES", "GPIO8", "复位"),
        ("DC", "GPIO9", "数据/命令"),
        ("CS", "GPIO10", "片选"),
        ("BLK", "GPIO13", "背光；可接 PWM"),
    ]
    add_table(doc, ("屏幕引脚", "开发板连接", "说明"), display_rows, (2200, 2500, 4660), font_size=9.0)

    add_heading(doc, "4. 开发板实际焊盘布局")
    add_body(doc, "以下是该 Super Mini 板的物理焊盘参考。标有“内侧焊盘”的引脚没有排针插槽，需要直接焊接细线、排针或使用转接小板。")
    pad = doc.add_paragraph()
    pad.paragraph_format.space_before = Pt(3)
    pad.paragraph_format.space_after = Pt(6)
    pad.paragraph_format.line_spacing = 1.05
    pad_text = (
        "正面上排:  5V | GND | 3V3 | GPIO13 | GPIO12 | GPIO11 | GPIO10 | GPIO9 | GPIO8\n"
        "正面下排:  TX(GPIO43) | RX(GPIO44) | GPIO1 | GPIO2 | GPIO3 | GPIO4 | GPIO5 | GPIO6 | GPIO7\n"
        "背面左内侧: GPIO38 | GPIO37 | GPIO34 | GND | GPIO33 | GPIO21 | GPIO18 | GPIO17 | GND | GPIO16 | GPIO15 | GPIO14\n"
        "背面右内侧: GPIO36 | GPIO35 | GPIO48 | GPIO47 | GPIO46 | GPIO45 | GPIO42 | GPIO41 | GPIO40 | GPIO39"
    )
    r = pad.add_run(pad_text)
    set_run_font(r, name=MONO_FONT, size=8.6)

    add_heading(doc, "5. 保留脚与可扩展脚")
    reserve_rows = [
        ("GPIO0", "保留", "下载/启动相关，不作普通外设"),
        ("GPIO3、GPIO45、GPIO46", "保留", "启动绑带相关，避免外部电路在复位时强拉高或拉低"),
        ("GPIO19、GPIO20", "保留", "原生 USB 信号，当前板子未作为外部焊盘引出"),
        ("GPIO43 / TX", "保留", "保留给下载日志和调试串口"),
        ("GPIO48", "保留", "板载 RGB LED 相关"),
        ("GPIO33、GPIO34、GPIO35、GPIO36、GPIO39、GPIO40、GPIO41、GPIO42、GPIO47", "可扩展", "目前未分配；优先用于未来普通数字输入、输出或第二路串口"),
    ]
    add_table(doc, ("GPIO", "状态", "原因/建议"), reserve_rows, (3000, 1500, 4860), font_size=9.0)

    add_heading(doc, "6. 电气与供电要求")
    add_note(doc, "WS2812：", "当灯珠工作在 5 V 时，ESP32 的 3.3 V 数据电平建议经 74AHCT1G125 或 74HCT125 转换，并在数据线串联约 330 Ohm 电阻。")
    add_note(doc, "功放：", "扬声器不能由 GPIO 直接驱动，必须接 I2S 功放模块。")
    add_note(doc, "RS485：", "本方案只适用于自动收发方向的 RS485 模块，因此不分配 DE/RE 管脚；若改用手动方向模块，需要新增一个 GPIO 控制 DE 与 /RE。")
    add_note(doc, "供电：", "Wi-Fi 峰值、电容屏、双 WS2812 和功放同时工作时，开发板自身升压路径不够。WS2812 与功放建议独立使用不低于 5 V / 2 A 的供电路径，并与 ESP32 共地。")
    add_note(doc, "雷达：", "若雷达 OUT 是 5 V 电平，必须先降压到 3.3 V 再进入 GPIO44。")

    doc.save(OUT)
    print(OUT)


if __name__ == "__main__":
    make_document()
