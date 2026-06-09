#!/usr/bin/env python3
"""
分析 MLIR 文件中的 UB (Unified Buffer) 地址转换指令
提取 hivm.hir.pointer_cast 指令，按地址排序，并生成网页报告
"""

import re
import sys
from dataclasses import dataclass
from typing import List, Optional
import html


@dataclass
class PointerCastEntry:
    """表示一个 pointer_cast 指令的条目"""
    line_number: int
    address: int
    line_content: str
    context_before: List[str]
    context_after: List[str]
    memref_type: str


def extract_address_from_line(line: str) -> Optional[tuple]:
    """
    从 pointer_cast 行中提取地址和 memref 类型
    示例: %75 = hivm.hir.pointer_cast(%c16448_i64) : memref<2x129x16x1xf16, #hivm.address_space<ub>>
    """
    # 匹配 pointer_cast 模式，包括 UB 地址空间
    # 注意: memref 类型中可能有嵌套的 < >，所以需要更复杂的匹配
    pattern = r'hivm\.hir\.pointer_cast\(%c(\d+)_i64\)\s*:\s*(memref<[^,]+,\s*#hivm\.address_space<ub>)'
    match = re.search(pattern, line)

    if match:
        address = int(match.group(1))
        memref_type = match.group(2)
        # 补全 memref 类型的右尖括号
        if memref_type.count('<') > memref_type.count('>'):
            memref_type += '>>'
        return address, memref_type

    return None


def find_pointer_cast_instructions(content: str, context_lines: int = 5) -> List[PointerCastEntry]:
    """
    查找所有 pointer_cast 指令并提取上下文
    """
    lines = content.split('\n')
    entries = []

    for i, line in enumerate(lines, start=1):
        if 'hivm.hir.pointer_cast' in line and 'ub>' in line:
            result = extract_address_from_line(line)
            if result:
                address, memref_type = result

                # 获取上下文
                start_idx = max(0, i - context_lines - 1)
                end_idx = min(len(lines), i + context_lines)

                context_before = lines[start_idx:i-1]
                context_after = lines[i:end_idx]

                entry = PointerCastEntry(
                    line_number=i,
                    address=address,
                    line_content=line,
                    context_before=context_before,
                    context_after=context_after,
                    memref_type=memref_type
                )
                entries.append(entry)

    # 按地址排序
    entries.sort(key=lambda x: x.address)
    return entries


CSS_STYLES = """
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
        }

        .container {
            max-width: 1400px;
            margin: 0 auto;
        }

        .header {
            background: white;
            border-radius: 15px;
            padding: 30px;
            margin-bottom: 20px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
        }

        .header h1 {
            color: #333;
            font-size: 28px;
            margin-bottom: 10px;
        }

        .header .subtitle {
            color: #666;
            font-size: 16px;
        }

        .stats {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 15px;
            margin-top: 20px;
        }

        .stat-card {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 20px;
            border-radius: 10px;
            text-align: center;
        }

        .stat-card .number {
            font-size: 32px;
            font-weight: bold;
        }

        .stat-card .label {
            font-size: 14px;
            opacity: 0.9;
        }

        .content {
            background: white;
            border-radius: 15px;
            padding: 30px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
        }

        .entry {
            margin-bottom: 25px;
            border: 1px solid #e0e0e0;
            border-radius: 10px;
            overflow: hidden;
            transition: transform 0.2s, box-shadow 0.2s;
        }

        .entry:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(0,0,0,0.1);
        }

        .entry-header {
            background: linear-gradient(135deg, #f5f7fa 0%, #c3cfe2 100%);
            padding: 15px 20px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            flex-wrap: wrap;
            gap: 10px;
        }

        .entry-header .address {
            background: #667eea;
            color: white;
            padding: 5px 15px;
            border-radius: 20px;
            font-weight: bold;
            font-family: 'Courier New', monospace;
        }

        .entry-header .line-number {
            color: #666;
            font-size: 14px;
        }

        .entry-header .memref-type {
            color: #333;
            font-size: 13px;
            font-family: 'Courier New', monospace;
            background: rgba(255,255,255,0.7);
            padding: 3px 10px;
            border-radius: 5px;
        }

        .code-block {
            background: #1e1e1e;
            color: #d4d4d4;
            padding: 20px;
            overflow-x: auto;
            font-family: 'Courier New', monospace;
            font-size: 13px;
            line-height: 1.6;
        }

        .code-line {
            display: flex;
            padding: 2px 0;
        }

        .code-line .line-num {
            color: #858585;
            min-width: 50px;
            text-align: right;
            padding-right: 15px;
            user-select: none;
        }

        .code-line .line-content {
            white-space: pre;
        }

        .code-line.highlight {
            background: rgba(255, 255, 0, 0.15);
        }

        .code-line.highlight .line-num {
            color: #ffd700;
            font-weight: bold;
        }

        /* 语法高亮 */
        .keyword { color: #569cd6; }
        .string { color: #ce9178; }
        .number { color: #b5cea8; }
        .comment { color: #6a9955; }
        .type { color: #4ec9b0; }
        .variable { color: #9cdcfe; }
        .function { color: #dcdcaa; }

        .chart-container {
            background: white;
            border-radius: 15px;
            padding: 30px;
            margin-bottom: 20px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
        }

        .address-chart {
            width: 100%;
            height: 400px;
            position: relative;
            overflow-x: auto;
            overflow-y: hidden;
            background: #f8f9fa;
            border-radius: 10px;
            border: 1px solid #e0e0e0;
        }

        .chart-svg {
            min-width: 100%;
            height: 100%;
        }

        .axis-line {
            stroke: #666;
            stroke-width: 2;
        }

        .grid-line {
            stroke: #e0e0e0;
            stroke-width: 1;
            stroke-dasharray: 4;
        }

        .address-point {
            cursor: pointer;
            transition: r 0.2s;
        }

        .address-point:hover {
            r: 8;
        }

        .address-label {
            font-size: 11px;
            font-family: 'Courier New', monospace;
            fill: #333;
            pointer-events: none;
        }

        .tooltip {
            position: fixed;
            background: rgba(0, 0, 0, 0.9);
            color: white;
            padding: 10px 15px;
            border-radius: 8px;
            font-size: 13px;
            pointer-events: none;
            z-index: 1000;
            display: none;
            max-width: 300px;
            box-shadow: 0 4px 15px rgba(0,0,0,0.3);
        }

        .tooltip-row {
            margin: 3px 0;
        }

        .tooltip-label {
            color: #aaa;
            font-size: 11px;
        }

        .tooltip-value {
            color: #fff;
            font-weight: bold;
        }

        .chart-legend {
            margin-top: 15px;
            padding: 10px;
            background: #f5f5f5;
            border-radius: 8px;
            font-size: 13px;
            color: #666;
        }

        .zoom-controls {
            position: absolute;
            top: 10px;
            right: 10px;
            display: flex;
            gap: 5px;
        }

        .zoom-btn {
            background: white;
            border: 1px solid #ccc;
            padding: 5px 12px;
            cursor: pointer;
            border-radius: 5px;
            font-size: 14px;
            transition: all 0.2s;
        }

        .zoom-btn:hover {
            background: #f0f0f0;
        }

        .footer {
            text-align: center;
            color: white;
            margin-top: 20px;
            opacity: 0.8;
        }

        @media (max-width: 768px) {
            .header h1 {
                font-size: 20px;
            }

            .entry-header {
                flex-direction: column;
                align-items: flex-start;
            }
        }
"""


def syntax_highlight(line: str) -> str:
    """简单的语法高亮"""
    # HTML 转义
    line = html.escape(line)

    # 高亮关键字
    keywords = ['func.func', 'arith.constant', 'memref', 'scf.for', 'vector',
               'hivm.hir.pointer_cast', 'memref.subview', 'affine.apply',
               'hivm.address_space', 'return', 'index', 'f32', 'f16', 'i32', 'i64']
    for kw in keywords:
        line = re.sub(r'\b' + re.escape(kw) + r'\b', r'<span class="keyword">' + kw + r'</span>', line)

    # 高亮数字
    line = re.sub(r'\b(\d+)\b', r'<span class="number">\1</span>', line)

    # 高亮注释
    line = re.sub(r'(//.*)$', r'<span class="comment">\1</span>', line)

    # 高亮变量
    line = re.sub(r'(%[\w_]+)', r'<span class="variable">\1</span>', line)

    return line


def generate_html_report(entries: List[PointerCastEntry], filename: str) -> str:
    """生成 HTML 报告"""

    # 生成条目 HTML
    entries_html = ""
    addresses = []

    for idx, entry in enumerate(entries):
        addresses.append(entry.address)

        # 构建代码块
        code_lines = []

        # 添加上下文（前）
        for i, ctx_line in enumerate(entry.context_before, start=entry.line_number - len(entry.context_before)):
            code_lines.append(f'<div class="code-line"><span class="line-num">{i}</span><span class="line-content">{syntax_highlight(ctx_line)}</span></div>')

        # 高亮当前行
        code_lines.append(f'<div class="code-line highlight" id="entry-{idx}"><span class="line-num">{entry.line_number}</span><span class="line-content">{syntax_highlight(entry.line_content)}</span></div>')

        # 添加上下文（后）
        for i, ctx_line in enumerate(entry.context_after, start=entry.line_number + 1):
            code_lines.append(f'<div class="code-line"><span class="line-num">{i}</span><span class="line-content">{syntax_highlight(ctx_line)}</span></div>')

        entry_html = f"""<div class="entry" id="entry-{idx}">
            <div class="entry-header">
                <span class="address">0x{entry.address:04X} ({entry.address})</span>
                <span class="memref-type">{html.escape(entry.memref_type)}</span>
                <span class="line-number">第 {entry.line_number} 行</span>
            </div>
            <div class="code-block">
                {''.join(code_lines)}
            </div>
        </div>"""
        entries_html += entry_html

    # 计算统计信息
    total_count = len(entries)
    min_addr = min(addresses) if addresses else 0
    max_addr = max(addresses) if addresses else 0
    addr_range = max_addr - min_addr

    # 准备详细数据用于JavaScript
    entries_data = []
    for idx, entry in enumerate(entries):
        entries_data.append({
            'index': idx,
            'address': entry.address,
            'line_number': entry.line_number,
            'memref_type': entry.memref_type,
            'line_content': entry.line_content.strip()
        })
    import json
    entries_json = json.dumps(entries_data, ensure_ascii=False)

    html_content = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>UB Pointer Cast 分析报告 - {html.escape(filename)}</title>
    <style>
{CSS_STYLES}
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>UB Pointer Cast 分析报告</h1>
            <p class="subtitle">文件: {html.escape(filename)}</p>
            <div class="stats">
                <div class="stat-card">
                    <div class="number">{total_count}</div>
                    <div class="label">总指令数</div>
                </div>
                <div class="stat-card">
                    <div class="number">{min_addr}</div>
                    <div class="label">最小地址</div>
                </div>
                <div class="stat-card">
                    <div class="number">{max_addr}</div>
                    <div class="label">最大地址</div>
                </div>
                <div class="stat-card">
                    <div class="number">{addr_range}</div>
                    <div class="label">地址范围</div>
                </div>
            </div>
        </div>

        <div class="chart-container">
            <h2 style="margin-bottom: 20px; color: #333;">地址趋势图 (等间距 / 按序号)</h2>
            <div class="address-chart" id="addressChart">
                <div class="zoom-controls">
                    <button class="zoom-btn" onclick="zoomIn()">+</button>
                    <button class="zoom-btn" onclick="zoomOut()">-</button>
                    <button class="zoom-btn" onclick="resetZoom()">⟲</button>
                </div>
                <svg class="chart-svg" id="chartSvg" preserveAspectRatio="none"></svg>
            </div>
            <div class="chart-legend">
                提示：鼠标悬停查看详细信息，点击圆点跳转到对应代码。使用右上角按钮缩放图表。
            </div>
        </div>

        <div class="content">
            <h2 style="margin-bottom: 20px; color: #333;">详细代码片段</h2>
            {entries_html}
        </div>

        <div class="footer">
            <p>Generated by MLIR UB Pointer Analyzer</p>
        </div>
    </div>

    <div class="tooltip" id="tooltip">
        <div class="tooltip-row"><span class="tooltip-label">地址:</span> <span class="tooltip-value" id="tt-address"></span></div>
        <div class="tooltip-row"><span class="tooltip-label">行号:</span> <span class="tooltip-value" id="tt-line"></span></div>
        <div class="tooltip-row"><span class="tooltip-label">类型:</span> <span class="tooltip-value" id="tt-type"></span></div>
    </div>

    <script>
        const entries = {entries_json};
        let currentZoom = 1;
        const baseWidth = 1200;
        const height = 380;
        const padding = {{ top: 40, right: 50, bottom: 60, left: 80 }};

        function drawChart() {{
            const svg = document.getElementById('chartSvg');
            const chartWidth = baseWidth * currentZoom;
            const plotWidth = chartWidth - padding.left - padding.right;
            const plotHeight = height - padding.top - padding.bottom;

            svg.setAttribute('width', chartWidth);
            svg.setAttribute('height', height);
            svg.innerHTML = '';

            const addresses = entries.map(e => e.address);
            const minAddr = Math.min(...addresses);
            const maxAddr = Math.max(...addresses);
            const addrRange = maxAddr - minAddr || 1;
            const count = entries.length;

            // 定义颜色映射（根据内存大小）
            function getMemrefSize(type) {{
                const match = type.match(/memref<([^>]+)>/);
                if (!match) return 0;
                const dims = match[1].split('x').map(Number).filter(n => !isNaN(n));
                return dims.reduce((a, b) => a * b, 1);
            }}

            const sizes = entries.map(e => getMemrefSize(e.memref_type));
            const maxSize = Math.max(...sizes, 1);

            // 计算等间距的坐标点
            const points = entries.map((entry, idx) => {{
                const x = count === 1 ? padding.left + plotWidth / 2 : padding.left + (idx / (count - 1)) * plotWidth;
                const y = padding.top + plotHeight - ((entry.address - minAddr) / addrRange) * plotHeight;
                return {{ x, y, idx, ...entry }};
            }});

            // 绘制网格线（垂直，对应序号）
            const xGridCount = Math.min(count, 10);
            for (let i = 0; i <= xGridCount; i++) {{
                const x = padding.left + (plotWidth / xGridCount) * i;
                const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
                line.setAttribute('x1', x);
                line.setAttribute('y1', padding.top);
                line.setAttribute('x2', x);
                line.setAttribute('y2', padding.top + plotHeight);
                line.setAttribute('class', 'grid-line');
                svg.appendChild(line);

                // X轴标签（序号）
                const idxLabel = Math.round((count - 1) * (i / xGridCount)) + 1;
                const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
                text.setAttribute('x', x);
                text.setAttribute('y', padding.top + plotHeight + 25);
                text.setAttribute('text-anchor', 'middle');
                text.setAttribute('class', 'address-label');
                text.textContent = idxLabel;
                svg.appendChild(text);
            }}

            // 绘制网格线（水平，对应地址）
            for (let i = 0; i <= 5; i++) {{
                const y = padding.top + (plotHeight / 5) * i;
                const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
                line.setAttribute('x1', padding.left);
                line.setAttribute('y1', y);
                line.setAttribute('x2', padding.left + plotWidth);
                line.setAttribute('y2', y);
                line.setAttribute('class', 'grid-line');
                svg.appendChild(line);

                // Y轴标签（地址）
                const addr = Math.round(maxAddr - (addrRange / 5) * i);
                const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
                text.setAttribute('x', padding.left - 15);
                text.setAttribute('y', y + 4);
                text.setAttribute('text-anchor', 'end');
                text.setAttribute('class', 'address-label');
                text.textContent = '0x' + addr.toString(16).toUpperCase();
                svg.appendChild(text);
            }}

            // 坐标轴
            const xAxis = document.createElementNS('http://www.w3.org/2000/svg', 'line');
            xAxis.setAttribute('x1', padding.left);
            xAxis.setAttribute('y1', padding.top + plotHeight);
            xAxis.setAttribute('x2', padding.left + plotWidth);
            xAxis.setAttribute('y2', padding.top + plotHeight);
            xAxis.setAttribute('class', 'axis-line');
            svg.appendChild(xAxis);

            const yAxis = document.createElementNS('http://www.w3.org/2000/svg', 'line');
            yAxis.setAttribute('x1', padding.left);
            yAxis.setAttribute('y1', padding.top);
            yAxis.setAttribute('x2', padding.left);
            yAxis.setAttribute('y2', padding.top + plotHeight);
            yAxis.setAttribute('class', 'axis-line');
            svg.appendChild(yAxis);

            // 轴标题
            const xTitle = document.createElementNS('http://www.w3.org/2000/svg', 'text');
            xTitle.setAttribute('x', padding.left + plotWidth / 2);
            xTitle.setAttribute('y', height - 10);
            xTitle.setAttribute('text-anchor', 'middle');
            xTitle.setAttribute('fill', '#333');
            xTitle.setAttribute('font-size', '14');
            xTitle.setAttribute('font-weight', 'bold');
            xTitle.textContent = '序号';
            svg.appendChild(xTitle);

            const yTitle = document.createElementNS('http://www.w3.org/2000/svg', 'text');
            yTitle.setAttribute('x', 20);
            yTitle.setAttribute('y', padding.top + plotHeight / 2);
            yTitle.setAttribute('text-anchor', 'middle');
            yTitle.setAttribute('fill', '#333');
            yTitle.setAttribute('font-size', '14');
            yTitle.setAttribute('font-weight', 'bold');
            yTitle.setAttribute('transform', `rotate(-90, 20, ${{padding.top + plotHeight / 2}})`);
            yTitle.textContent = 'UB 地址 (十六进制)';
            svg.appendChild(yTitle);

            // 绘制连接线（趋势图）
            if (points.length > 1) {{
                const polyline = document.createElementNS('http://www.w3.org/2000/svg', 'polyline');
                const pointsAttr = points.map(p => `${{p.x}},${{p.y}}`).join(' ');
                polyline.setAttribute('points', pointsAttr);
                polyline.setAttribute('fill', 'none');
                polyline.setAttribute('stroke', '#667eea');
                polyline.setAttribute('stroke-width', '2');
                polyline.setAttribute('stroke-linejoin', 'round');
                polyline.setAttribute('stroke-linecap', 'round');
                polyline.setAttribute('opacity', '0.6');
                svg.appendChild(polyline);
            }}

            // 绘制数据点
            points.forEach((item) => {{
                const size = getMemrefSize(item.memref_type);
                const radius = 4 + (size / maxSize) * 6;
                const intensity = 0.4 + (size / maxSize) * 0.6;

                const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
                circle.setAttribute('cx', item.x);
                circle.setAttribute('cy', item.y);
                circle.setAttribute('r', radius);
                circle.setAttribute('fill', `rgba(102, 126, 234, ${{intensity}})`);
                circle.setAttribute('stroke', '#667eea');
                circle.setAttribute('stroke-width', '2');
                circle.setAttribute('class', 'address-point');
                circle.setAttribute('data-index', item.idx);

                // 事件处理
                circle.addEventListener('mouseenter', (e) => showTooltip(e, item));
                circle.addEventListener('mouseleave', hideTooltip);
                circle.addEventListener('click', () => scrollToEntry(item.idx));

                svg.appendChild(circle);

                // 为部分点添加地址标签，避免拥挤
                if (item.idx % Math.max(1, Math.floor(count / 8)) === 0) {{
                    const label = document.createElementNS('http://www.w3.org/2000/svg', 'text');
                    label.setAttribute('x', item.x);
                    label.setAttribute('y', item.y - radius - 5);
                    label.setAttribute('text-anchor', 'middle');
                    label.setAttribute('class', 'address-label');
                    label.setAttribute('font-size', '9');
                    label.textContent = '0x' + item.address.toString(16).toUpperCase();
                    svg.appendChild(label);
                }}
            }});
        }}

        function showTooltip(e, item) {{
            const tooltip = document.getElementById('tooltip');
            document.getElementById('tt-address').textContent = '0x' + item.address.toString(16).toUpperCase() + ' (' + item.address + ')';
            document.getElementById('tt-line').textContent = item.line_number;
            document.getElementById('tt-type').textContent = item.memref_type;
            tooltip.style.display = 'block';
            tooltip.style.left = (e.clientX + 15) + 'px';
            tooltip.style.top = (e.clientY - 10) + 'px';
        }}

        function hideTooltip() {{
            document.getElementById('tooltip').style.display = 'none';
        }}

        function scrollToEntry(index) {{
            const entry = document.getElementById('entry-' + index);
            if (entry) {{
                entry.scrollIntoView({{ behavior: 'smooth', block: 'center' }});
                entry.style.border = '3px solid #667eea';
                setTimeout(() => {{
                    entry.style.border = '1px solid #e0e0e0';
                }}, 3000);
            }}
        }}

        function zoomIn() {{
            currentZoom *= 1.5;
            drawChart();
        }}

        function zoomOut() {{
            currentZoom = Math.max(0.5, currentZoom / 1.5);
            drawChart();
        }}

        function resetZoom() {{
            currentZoom = 1;
            drawChart();
        }}

        // 初始化
        drawChart();
    </script>
</body>
</html>"""

    return html_content


def main():
    if len(sys.argv) < 2:
        print("用法: python analyze_ub_pointers.py <mlir_file>")
        print("示例: python analyze_ub_pointers.py attn_fwd.mlir")
        sys.exit(1)

    filepath = sys.argv[1]

    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
    except FileNotFoundError:
        print(f"错误: 文件 '{filepath}' 不存在")
        sys.exit(1)
    except Exception as e:
        print(f"错误: 读取文件失败 - {e}")
        sys.exit(1)

    print(f"正在分析文件: {filepath}")
    print("-" * 50)

    # 查找 pointer_cast 指令
    entries = find_pointer_cast_instructions(content, context_lines=5)

    if not entries:
        print("未找到 UB 地址空间的 pointer_cast 指令")
        sys.exit(0)

    print(f"找到 {len(entries)} 个 UB pointer_cast 指令")

    # 在控制台输出简要信息
    print("\n按地址排序的 pointer_cast 指令:")
    print("-" * 80)
    print(f"{'序号':<6} {'地址':<12} {'行号':<8} {'内存类型'}")
    print("-" * 80)

    for i, entry in enumerate(entries, 1):
        memref_short = entry.memref_type[:50] + "..." if len(entry.memref_type) > 50 else entry.memref_type
        print(f"{i:<6} 0x{entry.address:04X} ({entry.address:<5}) {entry.line_number:<8} {memref_short}")

    # 生成 HTML 报告
    output_filename = filepath.rsplit('.', 1)[0] + '_ub_analysis.html'
    html_content = generate_html_report(entries, filepath)

    with open(output_filename, 'w', encoding='utf-8') as f:
        f.write(html_content)

    print("-" * 80)
    print(f"\nHTML 报告已生成: {output_filename}")
    print(f"总计: {len(entries)} 个指令")
    print(f"地址范围: 0x{min(e.address for e in entries):04X} - 0x{max(e.address for e in entries):04X}")


if __name__ == "__main__":
    main()
