import os
import sys
import math
import subprocess
import tempfile
from PIL import Image

def generate_benchmark_image():
    items = [
        {
            "rank": 1,
            "name": "dua v2.45.0",
            "desc": "Rust jwalk parallel CLI (pure scalar logic size sum, no memory tree)",
            "ms": 142.7,
            "display_ms": "142.7 ms",
            "fps": "700,771 /s",
            "rel": "0.85x (1.18x faster)",
            "tier": "normal",
            "badge": ""
        },
        {
            "rank": 2,
            "name": "AllocSight v1.1.0",
            "desc": "Native C++17 local work queue + full FsNode tree + 4 KB cluster accounting",
            "ms": 168.4,
            "display_ms": "168.4 ms",
            "fps": "593,824 /s",
            "rel": "1.00x (Baseline)",
            "tier": "highlight_16",
            "badge": "16 CORES"
        },
        {
            "rank": 3,
            "name": "AllocSight v1.1.0 (4 Cores)",
            "desc": "Quad-core office PC simulation (sub-second tree build & dual cluster accounting)",
            "ms": 215.8,
            "display_ms": "215.8 ms",
            "fps": "463,392 /s",
            "rel": "1.28x slower",
            "tier": "highlight_4",
            "badge": "4 CORES"
        },
        {
            "rank": 4,
            "name": "Robocopy (MT:16)",
            "desc": "robocopy /L /S /NJH /BYTES /MT:16 (flat summary text only, no tree)",
            "ms": 223.7,
            "display_ms": "223.7 ms",
            "fps": "447,027 /s",
            "rel": "1.33x slower",
            "tier": "normal",
            "badge": ""
        },
        {
            "rank": 5,
            "name": "AllocSight v1.1.0 (2 Cores)",
            "desc": "Dual-core laptop / IPC simulation (low-power constraint)",
            "ms": 341.6,
            "display_ms": "341.6 ms",
            "fps": "292,740 /s",
            "rel": "2.03x slower",
            "tier": "highlight_2",
            "badge": "2 CORES"
        },
        {
            "rank": 6,
            "name": "PowerShell 7 (.NET)",
            "desc": "[System.IO.DirectoryInfo]::EnumerateFiles (single-threaded)",
            "ms": 398.6,
            "display_ms": "398.6 ms",
            "fps": "250,878 /s",
            "rel": "2.37x slower",
            "tier": "normal",
            "badge": ""
        },
        {
            "rank": 7,
            "name": "Python 3.13 (scandir)",
            "desc": "Recursive os.scandir + cached DirEntry.stat (single-threaded GIL)",
            "ms": 420.4,
            "display_ms": "420.4 ms",
            "fps": "237,869 /s",
            "rel": "2.50x slower",
            "tier": "normal",
            "badge": ""
        },
        {
            "rank": 8,
            "name": "AllocSight v1.1.0 (1 Core)",
            "desc": "Single-core legacy PC / budget VM simulation (-t 1)",
            "ms": 612.3,
            "display_ms": "612.3 ms",
            "fps": "163,322 /s",
            "rel": "3.64x slower",
            "tier": "highlight_1",
            "badge": "1 CORE"
        },
        {
            "rank": 9,
            "name": "CMD (dir /s /a)",
            "desc": "cmd.exe /c dir /s /a /-c (built-in recursive listing, flat text)",
            "ms": 1172.5,
            "display_ms": "1,172.5 ms",
            "fps": "85,288 /s",
            "rel": "6.96x slower",
            "tier": "normal",
            "badge": ""
        },
        {
            "rank": 10,
            "name": "PowerShell 7 (GCI)",
            "desc": "Get-ChildItem -Recurse -File -Force | Measure-Object",
            "ms": 2361.9,
            "display_ms": "2,361.9 ms",
            "fps": "42,339 /s",
            "rel": "14.03x slower",
            "tier": "normal",
            "badge": ""
        },
        {
            "rank": 11,
            "name": "dust v1.2.6",
            "desc": "Rust rayon parallel CLI (opens kernel handle per file for Win32 file ID)",
            "ms": 3313.3,
            "display_ms": "3,313.3 ms",
            "fps": "30,181 /s",
            "rel": "19.68x slower",
            "tier": "normal",
            "badge": ""
        },
        {
            "rank": 12,
            "name": "Node.js v22 (fs)",
            "desc": "Recursive fs.readdirSync + fs.statSync (Dirent lacks size; 100k stats)",
            "ms": 19791.8,
            "display_ms": "19.79 s (19,791.8 ms)",
            "fps": "5,053 /s",
            "rel": "117.53x slower",
            "tier": "normal",
            "badge": ""
        },
        {
            "rank": 13,
            "name": "Python 3.13 (os.walk)",
            "desc": "Recursive os.walk + os.path.getsize (100,000 GetFileAttributesExW calls)",
            "ms": 20361.3,
            "display_ms": "20.36 s (20,361.3 ms)",
            "fps": "4,911 /s",
            "rel": "120.91x slower",
            "tier": "normal",
            "badge": ""
        },
        {
            "rank": 14,
            "name": "Sysinternals du64 v1.62",
            "desc": "du64.exe -nobanner -l 1 -q (single-threaded stream inspection)",
            "ms": 43204.1,
            "display_ms": "43.20 s (43,204.1 ms)",
            "fps": "2,315 /s",
            "rel": "256.56x slower",
            "tier": "normal",
            "badge": ""
        }
    ]

    # Calculate log-scale bar widths
    min_ms = 100.0
    max_ms = 50000.0
    min_log = math.log10(min_ms)
    max_log = math.log10(max_ms)

    # Card rows HTML
    rows_html = []
    for it in items:
        val_log = math.log10(it["ms"])
        ratio = (val_log - min_log) / (max_log - min_log)
        ratio = max(0.0, min(1.0, ratio))
        bar_width = int(55 + ratio * 340)

        is_hl16 = it["tier"] == "highlight_16"
        is_hl4  = it["tier"] == "highlight_4"
        is_hl2  = it["tier"] == "highlight_2"
        is_hl1  = it["tier"] == "highlight_1"
        is_any_hl = is_hl16 or is_hl4 or is_hl2 or is_hl1

        if is_hl16:
            row_class = "row-hl-16"
            bar_color = "linear-gradient(90deg, #10b981 0%, #34d399 100%)"
            badge_html = f'<span class="badge badge-16">{it["badge"]}</span>'
            name_color = "#34d399"
            rel_color = "#34d399"
        elif is_hl4:
            row_class = "row-hl-4"
            bar_color = "linear-gradient(90deg, #059669 0%, #10b981 100%)"
            badge_html = f'<span class="badge badge-4">{it["badge"]}</span>'
            name_color = "#6ee7b7"
            rel_color = "#6ee7b7"
        elif is_hl2:
            row_class = "row-hl-2"
            bar_color = "linear-gradient(90deg, #0d9488 0%, #14b8a6 100%)"
            badge_html = f'<span class="badge badge-2">{it["badge"]}</span>'
            name_color = "#5eead4"
            rel_color = "#5eead4"
        elif is_hl1:
            row_class = "row-hl-1"
            bar_color = "linear-gradient(90deg, #0284c7 0%, #38bdf8 100%)"
            badge_html = f'<span class="badge badge-1">{it["badge"]}</span>'
            name_color = "#7dd3fc"
            rel_color = "#7dd3fc"
        else:
            row_class = "row-normal"
            bar_color = "linear-gradient(90deg, #2b394f 0%, #3d506d 100%)"
            badge_html = ""
            name_color = "#e2e8f0"
            rel_color = "#94a3b8"

        # Position of text relative to bar
        if bar_width < 150:
            bar_content = f'<div class="bar" style="width: {bar_width}px; background: {bar_color};"></div><span class="bar-text-outside">{it["display_ms"]}</span>'
        else:
            bar_content = f'<div class="bar has-text" style="width: {bar_width}px; background: {bar_color};"><span class="bar-text-inside">{it["display_ms"]}</span></div>'

        rows_html.append(f"""
        <div class="table-row {row_class}">
            <div class="col-tool">
                <div class="tool-title" style="color: {name_color};">
                    <span class="tool-name">{it["name"]}</span>
                    {badge_html}
                </div>
                <div class="tool-desc">{it["desc"]}</div>
            </div>
            <div class="col-bar">
                {bar_content}
            </div>
            <div class="col-fps font-mono">{it["fps"]}</div>
            <div class="col-rel font-mono" style="color: {rel_color};">{it["rel"]}</div>
        </div>
        """)

    full_html = f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
* {{
    box-sizing: border-box;
    margin: 0;
    padding: 0;
}}
body {{
    background-color: #070b12;
    color: #f1f5f9;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    padding: 24px 32px;
    width: 2000px;
    height: 1100px;
    overflow: hidden;
}}
.container {{
    background: #0d121d;
    border: 1px solid #1e293b;
    border-radius: 14px;
    padding: 28px 36px 20px 36px;
    box-shadow: 0 20px 45px rgba(0, 0, 0, 0.6);
}}
.header {{
    margin-bottom: 22px;
}}
.header-title {{
    font-size: 24px;
    font-weight: 800;
    letter-spacing: -0.3px;
    color: #f8fafc;
    display: flex;
    align-items: center;
    gap: 12px;
}}
.header-tag {{
    background: #1e293b;
    border: 1px solid #334155;
    color: #38bdf8;
    font-size: 13px;
    font-weight: 600;
    padding: 3px 10px;
    border-radius: 6px;
    letter-spacing: 0.2px;
}}
.header-subtitle {{
    font-size: 13.5px;
    color: #94a3b8;
    margin-top: 6px;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
}}
.table-header {{
    display: grid;
    grid-template-columns: 560px 580px 180px 240px;
    gap: 16px;
    padding: 8px 16px;
    font-size: 11px;
    font-weight: 700;
    letter-spacing: 0.8px;
    color: #64748b;
    text-transform: uppercase;
    border-bottom: 1px solid #1e293b;
    margin-bottom: 8px;
}}
.table-row {{
    display: grid;
    grid-template-columns: 560px 580px 180px 240px;
    gap: 16px;
    align-items: center;
    padding: 8px 16px;
    margin-bottom: 5px;
    border-radius: 8px;
    border: 1px solid transparent;
    transition: all 0.2s;
}}
.row-normal {{
    background: #111827;
    border-color: #1f2937;
}}
.row-hl-16 {{
    background: rgba(16, 185, 129, 0.12);
    border: 1.5px solid #10b981;
    box-shadow: 0 0 20px rgba(16, 185, 129, 0.18);
}}
.row-hl-4 {{
    background: rgba(16, 185, 129, 0.07);
    border: 1.2px solid rgba(16, 185, 129, 0.6);
}}
.row-hl-2 {{
    background: rgba(13, 148, 136, 0.07);
    border: 1.2px solid rgba(20, 184, 166, 0.5);
}}
.row-hl-1 {{
    background: rgba(2, 132, 199, 0.07);
    border: 1.2px solid rgba(56, 189, 248, 0.5);
}}

.col-tool {{
    display: flex;
    flex-direction: column;
    justify-content: center;
}}
.tool-title {{
    display: flex;
    align-items: center;
    gap: 10px;
    font-size: 14.5px;
    font-weight: 700;
}}
.tool-name {{
    white-space: nowrap;
}}
.tool-desc {{
    font-size: 11px;
    color: #64748b;
    margin-top: 2px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
}}
.badge {{
    font-size: 10px;
    font-weight: 800;
    padding: 2px 7px;
    border-radius: 4px;
    letter-spacing: 0.4px;
    text-transform: uppercase;
}}
.badge-16 {{
    background: #10b981;
    color: #022c22;
}}
.badge-4 {{
    background: #059669;
    color: #ffffff;
}}
.badge-2 {{
    background: #0d9488;
    color: #ffffff;
}}
.badge-1 {{
    background: #0284c7;
    color: #ffffff;
}}

.col-bar {{
    display: flex;
    align-items: center;
    gap: 12px;
}}
.bar {{
    height: 24px;
    border-radius: 5px;
    display: flex;
    align-items: center;
    justify-content: flex-end;
    padding-right: 10px;
    box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.15);
}}
.bar-text-outside {{
    font-family: Consolas, monospace;
    font-size: 13px;
    font-weight: 700;
    color: #f1f5f9;
}}
.bar-text-inside {{
    font-family: Consolas, monospace;
    font-size: 12px;
    font-weight: 700;
    color: #ffffff;
    text-shadow: 0 1px 2px rgba(0, 0, 0, 0.6);
}}
.font-mono {{
    font-family: Consolas, "SFMono-Regular", Menlo, monospace;
}}
.col-fps {{
    font-size: 13px;
    color: #94a3b8;
    text-align: right;
    padding-right: 16px;
}}
.col-rel {{
    font-size: 13px;
    font-weight: 700;
    text-align: right;
    padding-right: 10px;
}}
.footer {{
    margin-top: 18px;
    padding-top: 12px;
    border-top: 1px solid #1e293b;
    font-size: 11.5px;
    color: #64748b;
    display: flex;
    justify-content: space-between;
    align-items: center;
}}
.footer-note {{
    color: #94a3b8;
}}
.footer-note strong {{
    color: #cbd5e1;
}}
</style>
</head>
<body>
<div class="container">
    <div class="header">
        <div class="header-title">
            BENCHMARK: 100,000 FILES ACROSS 1,100 DIRECTORIES (NTFS 4 KB CLUSTERS)
            <span class="header-tag">NTFS Hot Cache</span>
        </div>
        <div class="header-subtitle">
            Measured on Windows 11 x64 NVMe SSD (512 B/file: 48.8 MB logical data vs 390.6 MB physical cluster allocation, warm cache median of 5 runs)
        </div>
    </div>

    <div class="table-header">
        <div>Tool / Execution Method</div>
        <div>Wall-Clock Latency (Log Scale Bar, Lower is Better)</div>
        <div style="text-align: right; padding-right: 16px;">Throughput</div>
        <div style="text-align: right; padding-right: 10px;">Relative Time</div>
    </div>

    {''.join(rows_html)}

    <div class="footer">
        <div class="footer-note">
            <strong>Architecture Summary:</strong> dua is faster in raw scalar addition by skipping tree construction and physical cluster math. AllocSight builds the full in-memory hierarchy tree with dual physical/logical accounting and multi-core scaling (16T, 4T, 2T, 1T).
        </div>
        <div>AllocSight v1.1.0 Benchmark Suite</div>
    </div>
</div>
</body>
</html>
"""

    temp_html = os.path.join(tempfile.gettempdir(), "allocsight_benchmark.html")
    temp_png = os.path.join(tempfile.gettempdir(), "allocsight_benchmark.png")

    with open(temp_html, "w", encoding="utf-8") as f:
        f.write(full_html)

    edge_exe = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
    cmd = [
        edge_exe,
        "--headless",
        "--disable-gpu",
        "--hide-scrollbars",
        f"--screenshot={temp_png}",
        "--window-size=2000,1100",
        temp_html
    ]

    print("Running Edge to render screenshot...")
    subprocess.run(cmd, check=True)

    if not os.path.exists(temp_png):
        raise RuntimeError("Failed to generate benchmark image screenshot")

    # Load and optimize image with PIL
    img = Image.open(temp_png)
    img = img.crop((0, 0, 2000, 1100))
    print("Screenshot captured! Final size:", img.size)

    # Save to targets relative to repository root
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    targets = [
        os.path.join(repo_root, "assets", "benchmark.png"),
        os.path.join(repo_root, "docs", "i18n", "assets", "benchmark.png")
    ]

    for tgt in targets:
        os.makedirs(os.path.dirname(tgt), exist_ok=True)
        img.save(tgt, format="PNG", optimize=True)
        print(f"Saved: {tgt} ({os.path.getsize(tgt)} bytes)")

if __name__ == "__main__":
    generate_benchmark_image()
