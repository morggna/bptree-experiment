import csv
import os
import sys
import tempfile

# Matplotlib 会写字体缓存，放到临时目录可以减少不同机器上的权限问题。
os.environ.setdefault('MPLCONFIGDIR', os.path.join(tempfile.gettempdir(), 'bptree_matplotlib'))
os.environ.setdefault('XDG_CACHE_HOME', os.path.join(tempfile.gettempdir(), 'bptree_cache'))

import numpy as np
import matplotlib
import matplotlib.font_manager as font_manager
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec

SUMMARY_CSV = 'query_summary.csv'
DETAIL_CSV = 'query_results.csv'
OUTPUT_IMAGE = 'bptree_performance.png'


def setup_chinese_font():
    # 图表标题和坐标轴含中文，优先使用系统里可用的中文字体。
    font_names = [
        'STHeiti',
        'Hiragino Sans GB',
        'Songti SC',
        'Arial Unicode MS',
        'Microsoft YaHei',
        'SimHei',
        'DejaVu Sans',
    ]
    font_paths = [
        '/System/Library/Fonts/STHeiti Medium.ttc',
        '/System/Library/Fonts/STHeiti Light.ttc',
        '/System/Library/Fonts/Hiragino Sans GB.ttc',
        '/System/Library/Fonts/Supplemental/Songti.ttc',
        '/System/Library/Fonts/Supplemental/Arial Unicode.ttf',
    ]

    for path in font_paths:
        if not os.path.exists(path):
            continue
        try:
            font_manager.fontManager.addfont(path)
            font_name = font_manager.FontProperties(fname=path).get_name()
            matplotlib.rcParams['font.sans-serif'] = [font_name] + font_names
            break
        except RuntimeError:
            continue
    else:
        matplotlib.rcParams['font.sans-serif'] = font_names

    matplotlib.rcParams['axes.unicode_minus'] = False


setup_chinese_font()


def check_input_files():
    # 可视化依赖主程序生成的两个 CSV 文件。
    missing_files = []
    for path in (SUMMARY_CSV, DETAIL_CSV):
        if not os.path.exists(path):
            missing_files.append(path)

    if missing_files:
        print('缺少可视化输入文件: ' + ', '.join(missing_files), file=sys.stderr)
        print('请先运行 make run 生成 query_results.csv 和 query_summary.csv。', file=sys.stderr)
        return False

    return True


def load_summary(path=SUMMARY_CSV):
    # 汇总文件记录每类查询的平均时间、平均 I/O 和平均命中数。
    cats, avg_t, max_t, avg_io, avg_hits = [], [], [], [], []
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            cats.append(row['category'])
            avg_t.append(float(row['avg_time_ms']))
            max_t.append(float(row['max_time_ms']))
            avg_io.append(float(row['avg_io']))
            avg_hits.append(float(row['avg_hits']))
    return cats, avg_t, max_t, avg_io, avg_hits


def load_detail(path=DETAIL_CSV):
    # 明细文件记录每一次范围查询的耗时、逻辑 I/O 和命中记录数。
    data = {0: [], 1: [], 2: [], 3: []}
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            c = int(row['category'])
            data[c].append((
                float(row['time_ms']),
                int(row['io_count']),
                int(row['result_count'])
            ))
    return data


def main():
    if not check_input_files():
        return 1

    cat_labels, avg_t, max_t, avg_io, avg_hits = load_summary()
    detail = load_detail()

    x = np.arange(len(cat_labels))
    colors = ['#4C72B0', '#DD8452', '#55A868', '#C44E52']

    fig = plt.figure(figsize=(16, 12))
    fig.suptitle('B+树范围查询性能分析\n(IO块 = 8 KB，GEOLIFE数据集)',
                 fontsize=15, fontweight='bold', y=0.98)
    gs = GridSpec(2, 3, figure=fig, hspace=0.45, wspace=0.38)

    # 上排三张柱状图展示平均时间、平均 I/O 和平均命中数。
    ax1 = fig.add_subplot(gs[0, 0])
    bars = ax1.bar(x, avg_t, color=colors, edgecolor='white', linewidth=0.8)
    ax1.set_xticks(x)
    ax1.set_xticklabels(cat_labels, fontsize=9)
    ax1.set_xlabel('覆盖范围类别', fontsize=10)
    ax1.set_ylabel('平均响应时间 (ms)', fontsize=10)
    ax1.set_title('① 各类别平均查询时间', fontsize=11, fontweight='bold')
    for bar, val in zip(bars, avg_t):
        ax1.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + max(avg_t) * 0.01,
                 f'{val:.4f}', ha='center', va='bottom', fontsize=8)
    ax1.grid(axis='y', alpha=0.3)
    ax2 = fig.add_subplot(gs[0, 1])
    bars2 = ax2.bar(x, avg_io, color=colors, edgecolor='white', linewidth=0.8)
    ax2.set_xticks(x)
    ax2.set_xticklabels(cat_labels, fontsize=9)
    ax2.set_xlabel('覆盖范围类别', fontsize=10)
    ax2.set_ylabel('平均IO块次数', fontsize=10)
    ax2.set_title('② 各类别平均IO次数', fontsize=11, fontweight='bold')
    for bar, val in zip(bars2, avg_io):
        ax2.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + max(avg_io) * 0.01,
                 f'{val:.1f}', ha='center', va='bottom', fontsize=8)
    ax2.grid(axis='y', alpha=0.3)
    ax3 = fig.add_subplot(gs[0, 2])
    bars3 = ax3.bar(x, avg_hits, color=colors, edgecolor='white', linewidth=0.8)
    ax3.set_xticks(x)
    ax3.set_xticklabels(cat_labels, fontsize=9)
    ax3.set_xlabel('覆盖范围类别', fontsize=10)
    ax3.set_ylabel('平均命中记录数', fontsize=10)
    ax3.set_title('③ 各类别平均命中记录数', fontsize=11, fontweight='bold')
    for bar, val in zip(bars3, avg_hits):
        ax3.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + max(avg_hits) * 0.01,
                 f'{val:.1f}', ha='center', va='bottom', fontsize=8)
    ax3.grid(axis='y', alpha=0.3)

    # 下排用箱线图看耗时波动，用散点图看 I/O 与命中数的关系。
    ax4 = fig.add_subplot(gs[1, 0:2])
    time_data = [[d[0] for d in detail[c]] for c in range(4)]
    bp = ax4.boxplot(time_data, tick_labels=cat_labels, patch_artist=True,
                     medianprops=dict(color='black', linewidth=2),
                     whiskerprops=dict(linewidth=1.5),
                     capprops=dict(linewidth=1.5),
                     flierprops=dict(marker='o', markersize=3, alpha=0.5))
    for patch, color in zip(bp['boxes'], colors):
        patch.set_facecolor(color)
        patch.set_alpha(0.7)
    ax4.set_xlabel('覆盖范围类别', fontsize=10)
    ax4.set_ylabel('查询响应时间 (ms)', fontsize=10)
    ax4.set_title('④ 查询时间分布箱线图', fontsize=11, fontweight='bold')
    ax4.grid(axis='y', alpha=0.3)
    ax5 = fig.add_subplot(gs[1, 2])
    for c in range(4):
        ios  = [d[1] for d in detail[c]]
        hits = [d[2] for d in detail[c]]
        ax5.scatter(ios, hits, alpha=0.5, s=20, color=colors[c], label=cat_labels[c])
    ax5.set_xlabel('IO块次数', fontsize=10)
    ax5.set_ylabel('命中记录数', fontsize=10)
    ax5.set_title('⑤ IO次数 vs 命中记录数', fontsize=11, fontweight='bold')
    ax5.legend(title='覆盖范围', fontsize=8, title_fontsize=8)
    ax5.grid(alpha=0.3)

    plt.savefig(OUTPUT_IMAGE, dpi=150, bbox_inches='tight')
    print(f"图表已保存: {OUTPUT_IMAGE}")
    show_plot = os.environ.get('BPTREE_SHOW_PLOT', '1').lower() not in ('0', 'false', 'no')
    if show_plot and 'agg' not in matplotlib.get_backend().lower():
        # 默认保留弹窗，本地运行脚本后可以直接看到图表。
        plt.show()

    return 0

if __name__ == '__main__':
    sys.exit(main())
