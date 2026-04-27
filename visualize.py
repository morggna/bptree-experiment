#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
visualize.py  ——  B+树范围查询性能可视化
运行方法: python visualize.py
依赖: matplotlib, numpy (pip install matplotlib numpy)
"""

import csv
import numpy as np
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.gridspec import GridSpec

# 解决中文乱码
matplotlib.rcParams['font.sans-serif'] = ['Microsoft YaHei', 'SimHei', 'DejaVu Sans']
matplotlib.rcParams['axes.unicode_minus'] = False

# ============================================================
#  读取数据
# ============================================================
def load_summary(path='query_summary.csv'):
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


def load_detail(path='query_results.csv'):
    data = {0: [], 1: [], 2: [], 3: []}  # category -> list of (time_ms, io_count, hits)
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


# ============================================================
#  绘图
# ============================================================
def main():
    cats, avg_t, max_t, avg_io, avg_hits = load_summary()
    detail = load_detail()

    cat_labels = ['1/1000', '1/10000', '1/100000', '1/1000000']
    x = np.arange(len(cat_labels))
    colors = ['#4C72B0', '#DD8452', '#55A868', '#C44E52']

    fig = plt.figure(figsize=(16, 12))
    fig.suptitle('B+树范围查询性能分析\n(IO块 = 8 KB，GEOLIFE数据集)',
                 fontsize=15, fontweight='bold', y=0.98)
    gs = GridSpec(2, 3, figure=fig, hspace=0.45, wspace=0.38)

    # --------------------------------------------------
    #  图1：平均查询时间（柱状图）
    # --------------------------------------------------
    ax1 = fig.add_subplot(gs[0, 0])
    bars = ax1.bar(x, avg_t, color=colors, edgecolor='white', linewidth=0.8)
    ax1.set_xticks(x); ax1.set_xticklabels(cat_labels, fontsize=9)
    ax1.set_xlabel('覆盖范围类别', fontsize=10)
    ax1.set_ylabel('平均响应时间 (ms)', fontsize=10)
    ax1.set_title('① 各类别平均查询时间', fontsize=11, fontweight='bold')
    for bar, val in zip(bars, avg_t):
        ax1.text(bar.get_x() + bar.get_width()/2, bar.get_height() + max(avg_t)*0.01,
                 f'{val:.4f}', ha='center', va='bottom', fontsize=8)
    ax1.grid(axis='y', alpha=0.3)

    # --------------------------------------------------
    #  图2：平均IO次数（柱状图）
    # --------------------------------------------------
    ax2 = fig.add_subplot(gs[0, 1])
    bars2 = ax2.bar(x, avg_io, color=colors, edgecolor='white', linewidth=0.8)
    ax2.set_xticks(x); ax2.set_xticklabels(cat_labels, fontsize=9)
    ax2.set_xlabel('覆盖范围类别', fontsize=10)
    ax2.set_ylabel('平均IO块次数', fontsize=10)
    ax2.set_title('② 各类别平均IO次数', fontsize=11, fontweight='bold')
    for bar, val in zip(bars2, avg_io):
        ax2.text(bar.get_x() + bar.get_width()/2, bar.get_height() + max(avg_io)*0.01,
                 f'{val:.1f}', ha='center', va='bottom', fontsize=8)
    ax2.grid(axis='y', alpha=0.3)

    # --------------------------------------------------
    #  图3：平均命中记录数（柱状图）
    # --------------------------------------------------
    ax3 = fig.add_subplot(gs[0, 2])
    bars3 = ax3.bar(x, avg_hits, color=colors, edgecolor='white', linewidth=0.8)
    ax3.set_xticks(x); ax3.set_xticklabels(cat_labels, fontsize=9)
    ax3.set_xlabel('覆盖范围类别', fontsize=10)
    ax3.set_ylabel('平均命中记录数', fontsize=10)
    ax3.set_title('③ 各类别平均命中记录数', fontsize=11, fontweight='bold')
    for bar, val in zip(bars3, avg_hits):
        ax3.text(bar.get_x() + bar.get_width()/2, bar.get_height() + max(avg_hits)*0.01,
                 f'{val:.1f}', ha='center', va='bottom', fontsize=8)
    ax3.grid(axis='y', alpha=0.3)

    # --------------------------------------------------
    #  图4：查询时间箱线图
    # --------------------------------------------------
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

    # --------------------------------------------------
    #  图5：IO次数 vs 命中记录数（散点图）
    # --------------------------------------------------
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

    plt.savefig('bptree_performance.png', dpi=150, bbox_inches='tight')
    print("图表已保存: bptree_performance.png")
    plt.show()


if __name__ == '__main__':
    main()
