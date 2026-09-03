# -*- coding: utf-8 -*-
"""生成 LumiDock 定时器功能流程图（drawio 可编辑版 + SVG 预览版）。

覆盖定时器功能全链路：
  一、设置（语音命令 -> IoT 解析 -> timer_service -> NVS）
  二、运行（tick 轮询 -> 到期判定 -> 事件投递）
  三、响铃（TIMER_RING 打断会话 -> 检测暂停 -> 本地提示音）
  四、停止（按键 / 30s 超时 / 唤醒词 -> EVENT_STOPPED -> 恢复）
  侧支：语音查询 -> announce -> [device_call] 云端播报
"""
import html

# (id, x, y, w, h, kind, [行文本])
NODES = [
    ("title", 560, 20, 640, 44, "title",
     ["LumiDock 定时器功能流程图（定闹钟 · 正计时 · 倒计时）"]),
    # 泳道标签
    ("l1", 20, 110, 190, 70, "stage", ["一、设置", "（语音命令）"]),
    ("l2", 20, 330, 190, 100, "stage", ["二、运行", "（tick 轮询）"]),
    ("l3", 20, 590, 190, 70, "stage", ["三、响铃", "（打断会话）"]),
    ("l4", 20, 830, 190, 70, "stage", ["四、停止"]),
    # 段一：设置
    ("s1", 240, 110, 280, 70, "set", ["语音命令", "定闹钟 / 倒计时 / 开始计时"]),
    ("s2", 550, 110, 300, 70, "set", ["IoT 命令服务", "词表匹配 + 中文数字解析"]),
    ("s3", 880, 110, 280, 70, "set", ["timer_service", "set_alarm / add_countdown", "start_stopwatch"]),
    ("s4", 1190, 110, 280, 70, "set", ["NVS 持久化", "闹钟+倒计时（30tick 节流）", "正计时仅内存"]),
    # 段二：运行
    ("b1", 240, 330, 300, 70, "run", ["语音任务每秒 tick", "→ timer_service_poll()"]),
    ("b2", 570, 330, 300, 70, "run", ["到期判定", "倒计时真实时间差递减", "闹钟时刻比对"]),
    ("b3", 880, 330, 260, 70, "run", ["事件投递", "EVENT_RING", "EVENT_STOPPED"]),
    # 查询侧支（右列）
    ("q1", 1520, 330, 230, 70, "query", ["查询：还剩多久", "/ 计时多久"]),
    ("q2", 1520, 430, 230, 70, "query", ["announce 文案", "（剩余 / 已计时分秒）"]),
    ("q3", 1520, 530, 230, 70, "query", ["[device_call]", "云端 TTS 播报"]),
    # 段三：响铃
    ("c1", 240, 590, 300, 70, "ring", ["TIMER_RING 事件", "会话中：中止对话 + 关会话"]),
    ("c2", 570, 590, 300, 70, "ring", ["检测暂停 pause_detect", "VAD 抑制", "唤醒词仍放行"]),
    ("c3", 900, 590, 260, 70, "ring", ["本地提示音 chime", "2 秒心跳重播"]),
    # 段四：停止
    ("d1", 240, 830, 300, 70, "stop", ["三种停止方式", "按键短按 / 30 秒超时", "响铃时唤醒词"]),
    ("d2", 570, 830, 300, 70, "stop", ["EVENT_STOPPED", "停 chime → 恢复检测"]),
    ("d3", 900, 830, 260, 70, "stop", ["一次性闹钟删除", "排队闹钟交接响铃"]),
]

COLORS = {
    "title": ("#dae8fc", "#6c8ebf"),
    "stage": ("#f5f5f5", "#666666"),
    "set":   ("#dae8fc", "#6c8ebf"),
    "run":   ("#d5e8d4", "#82b366"),
    "ring":  ("#ffe6cc", "#d79b00"),
    "stop":  ("#e1d5e7", "#9673a6"),
    "query": ("#fff2cc", "#d6b656"),
}

FONT_SIZES = {"title": 20, "stage": 15, "set": 13, "run": 13,
              "ring": 13, "stop": 13, "query": 13}

# (src, dst, label, drawio_waypoints, dashed, (exitX, exitY, entryX, entryY) or None)
EDGES = [
    ("s1", "s2", "", None, False, None),
    ("s2", "s3", "", None, False, None),
    ("s3", "s4", "", None, False, None),
    ("s2", "q1", "查询类命令",
     [(850, 215), (1450, 215), (1450, 365)], True, (1, 0.5, 0, 0.5)),
    ("q1", "q2", "", None, False, (0.5, 1, 0.5, 0)),
    ("q2", "q3", "", None, False, (0.5, 1, 0.5, 0)),
    ("s4", "b1", "", [(1330, 255), (390, 255)], False, (0.5, 1, 0.5, 0)),
    ("b1", "b2", "", None, False, None),
    ("b2", "b3", "", None, False, None),
    ("b3", "c1", "", [(1010, 480), (390, 480)], False, (0.5, 1, 0.5, 0)),
    ("c1", "c2", "", None, False, None),
    ("c2", "c3", "", None, False, None),
    ("c3", "d1", "", [(1030, 700), (390, 700)], False, (0.5, 1, 0.5, 0)),
    ("d1", "d2", "", None, False, None),
    ("d2", "d3", "", None, False, None),
    ("d2", "b1", "继续轮询",
     [(1400, 900), (1400, 395), (390, 395)], True, (0.5, 1, 0.5, 1)),
]

# SVG 折线（显式全部点）+ 虚线标记 + 标签及位置
SVG_EDGES = [
    ([(520, 145), (550, 145)], False, None),
    ([(850, 145), (880, 145)], False, None),
    ([(1160, 145), (1190, 145)], False, None),
    ([(850, 145), (850, 215), (1450, 215), (1450, 365), (1520, 365)],
     True, ("查询类命令", 1150, 206)),
    ([(1635, 400), (1635, 430)], False, None),
    ([(1635, 500), (1635, 530)], False, None),
    ([(1330, 180), (1330, 255), (390, 255), (390, 330)], False, None),
    ([(540, 365), (570, 365)], False, None),
    ([(870, 365), (880, 365)], False, None),
    ([(1010, 400), (1010, 480), (390, 480), (390, 590)], False, None),
    ([(540, 625), (570, 625)], False, None),
    ([(870, 625), (900, 625)], False, None),
    ([(1030, 660), (1030, 700), (390, 700), (390, 830)], False, None),
    ([(540, 865), (570, 865)], False, None),
    ([(870, 865), (900, 865)], False, None),
    ([(720, 900), (1400, 900), (1400, 395), (390, 395), (390, 400)],
     True, ("继续轮询", 1060, 893)),
]

PAGE_W, PAGE_H = 1850, 1120


def drawio_xml():
    cells = ['<mxCell id="0"/>', '<mxCell id="1" parent="0"/>']
    for nid, x, y, w, h, kind, lines in NODES:
        fill, stroke = COLORS[kind]
        size = FONT_SIZES[kind]
        bold = "1" if kind in ("title", "stage") else "0"
        val = html.escape("\n".join(lines))
        cells.append(
            f'<mxCell id="{nid}" value="{val}" style="rounded=1;'
            f'whiteSpace=wrap;html=1;fillColor={fill};strokeColor={stroke};'
            f'fontSize={size};fontStyle={bold};" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" '
            f'as="geometry"/></mxCell>')
    for i, (src, dst, label, pts, dashed, exits) in enumerate(EDGES):
        style = ("edgeStyle=orthogonalEdgeStyle;rounded=0;html=1;"
                 "endArrow=block;strokeColor=#808080;fontSize=12;")
        if dashed:
            style += "dashed=1;"
        if exits:
            exx, exy, enx, eny = exits
            style += f"exitX={exx};exitY={exy};entryX={enx};entryY={eny};"
        cells.append(
            f'<mxCell id="e{i}" value="{html.escape(label)}" style="{style}" '
            f'edge="1" parent="1" source="{src}" target="{dst}">'
            f'<mxGeometry relative="1" as="geometry">')
        if pts:
            cells.append('<Array as="points">')
            for px, py in pts:
                cells.append(f'<mxPoint x="{px}" y="{py}"/>')
            cells.append('</Array>')
        cells.append('</mxGeometry></mxCell>')
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<mxfile host="app.diagrams.net" '
            'agent="LumiDock timer flow generator" version="24.7.17">\n'
            f'  <diagram id="lumidock-timer" name="定时器功能流程">\n'
            f'    <mxGraphModel dx="900" dy="600" grid="1" gridSize="10" '
            f'guides="1" tooltips="1" connect="1" arrows="1" fold="1" '
            f'page="1" pageScale="1" pageWidth="{PAGE_W}" '
            f'pageHeight="{PAGE_H}" math="0" shadow="0">\n'
            f'      <root>\n        ' + '\n        '.join(cells) +
            '\n      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n')


def svg():
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{PAGE_W}" '
           f'height="{PAGE_H}" viewBox="0 0 {PAGE_W} {PAGE_H}" '
           f'font-family="Microsoft YaHei, SimHei, sans-serif">']
    out.append(f'<rect width="{PAGE_W}" height="{PAGE_H}" fill="#ffffff"/>')
    # 边
    for pts, dashed, lbl in SVG_EDGES:
        dash = ' stroke-dasharray="7,5"' if dashed else ''
        poly = " ".join(f"{x},{y}" for x, y in pts)
        out.append(f'<polyline points="{poly}" fill="none" stroke="#808080" '
                   f'stroke-width="1.6"{dash}/>')
        x1, y1 = pts[-2]
        x2, y2 = pts[-1]
        if x2 > x1:  # 右箭头
            out.append(f'<polygon points="{x2},{y2} {x2-8},{y2-4} '
                       f'{x2-8},{y2+4}" fill="#808080"/>')
        elif y2 > y1:  # 下箭头
            out.append(f'<polygon points="{x2},{y2} {x2-4},{y2-8} '
                       f'{x2+4},{y2-8}" fill="#808080"/>')
        if lbl:
            t, lx, ly = lbl
            out.append(f'<text x="{lx}" y="{ly}" font-size="12" '
                       f'fill="#555555" text-anchor="middle">{t}</text>')
    # 节点
    for _nid, x, y, w, h, kind, lines in NODES:
        fill, stroke = COLORS[kind]
        size = FONT_SIZES[kind]
        bold = ' font-weight="bold"' if kind in ("title", "stage") else ""
        cx = x + w / 2
        out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" '
                   f'fill="{fill}" stroke="{stroke}" stroke-width="1.5"/>')
        n = len(lines)
        line_h = 18 if n == 3 else 19
        y0 = y + h / 2 - (n - 1) * line_h / 2 + size * 0.35
        for i, line in enumerate(lines):
            out.append(f'<text x="{cx}" y="{y0 + i * line_h}" '
                       f'font-size="{size}" fill="#1a1a1a" '
                       f'text-anchor="middle"{bold}>{html.escape(line)}</text>')
    out.append('</svg>')
    return "\n".join(out)


if __name__ == "__main__":
    import os
    base = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(base)
    with open(os.path.join(root, "outputs", "LumiDock_定时器功能流程图.drawio"),
              "w", encoding="utf-8") as f:
        f.write(drawio_xml())
    with open(os.path.join(root, "preview", "LumiDock_定时器功能流程图.svg"),
              "w", encoding="utf-8") as f:
        f.write(svg())
    print("OK: drawio + svg 已生成")
