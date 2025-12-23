#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os, argparse
import numpy as np
import pandas as pd
import matplotlib
import matplotlib.pyplot as plt

plt.rcParams['font.sans-serif'] = ['WenQuanYi Micro Hei']  # 或者其他支持中文的字体
plt.rcParams['axes.unicode_minus'] = False  # 解决负号显示问题

def setup_chinese_font():
    matplotlib.rcParams["axes.unicode_minus"] = False
    candidates = [
        "Noto Sans CJK SC","Source Han Sans SC","SimHei",
        "Microsoft YaHei","PingFang SC","WenQuanYi Zen Hei",
        "Arial Unicode MS"
    ]
    for f in candidates:
        try:
            matplotlib.rcParams["font.sans-serif"] = [f]
            return
        except:
            pass

def read_csv_safe(path):
    if not os.path.exists(path):
        raise FileNotFoundError(path)
    return pd.read_csv(path)

def ensure_dir(d):
    os.makedirs(d, exist_ok=True)

def time_binned_bytes(df_send, bin_sec=0.2):
    # df_send: sender_tasks.csv (bytesSent per task)
    # But we only have per-task bytesSent, no per-packet.
    # Proxy: spread bytes uniformly in a tiny interval around send time.
    if df_send.empty:
        return pd.DataFrame(columns=["t_bin","bytes"])
    t = df_send["simTime"].values
    b = df_send["bytesSent"].values
    t0 = float(np.min(t))
    t1 = float(np.max(t))
    nb = int(np.ceil((t1 - t0) / bin_sec)) + 1
    bins = t0 + np.arange(nb+1)*bin_sec
    idx = np.clip(np.digitize(t, bins)-1, 0, nb-1)
    agg = np.zeros(nb, dtype=np.float64)
    for i,bi in enumerate(idx):
        agg[bi] += b[i]
    out = pd.DataFrame({"t_bin": bins[:-1], "bytes": agg})
    return out

def compute_task_completion(df_recv):
    # receiver_tasks.csv rows are per chunk (bytesRx). We want per task completion time as lastRx when completed.
    # Columns: taskId, completed, deadlineMiss, firstRx, lastRx, totalBytes...
    if df_recv.empty:
        return pd.DataFrame()
    # keep last record per taskId (max lastRx)
    g = df_recv.groupby("taskId", as_index=False).agg({
        "g":"max","t":"max","k":"max",
        "isRedundant":"max","originTaskId":"max",
        "priority":"max","deadlineSec":"max",
        "totalBytes":"max",
        "completed":"max",
        "deadlineMiss":"max",
        "firstRx":"min",
        "lastRx":"max",
    })
    # slack
    g["slackSec"] = g["deadlineSec"] - g["lastRx"]
    return g

def compute_gop_completion(task_done_df):
    # GOP completion time = max lastRx of ORIGINAL tasks (isRedundant==0) in that g
    if task_done_df.empty:
        return pd.DataFrame(columns=["g","gop_done_time"])
    orig = task_done_df[task_done_df["isRedundant"]==0].copy()
    gop = orig.groupby("g", as_index=False)["lastRx"].max().rename(columns={"lastRx":"gop_done_time"})
    return gop

def jain_fairness(x):
    x = np.array(x, dtype=np.float64)
    if np.all(x<=0):
        return 0.0
    s = np.sum(x)
    return (s*s) / (len(x)*np.sum(x*x) + 1e-12)

def qoe_proxy(task_done_df, gop_dur=1.0, fps=30.0, init_buffer_sec=0.5):
    """
    不解码的 QoE 代理：
    - 假设每 GOP 对应 gop_dur 秒播放时长
    - 当 GOP 的全部 ORIGINAL tasks 在播放时间点前完成 => 该 GOP 可无卡顿播放
    - 否则产生 stall，stall 时长 = (gop_done_time - play_time), if positive
    """
    if task_done_df.empty:
        return pd.DataFrame(), {}

    gop = compute_gop_completion(task_done_df)
    if gop.empty:
        return pd.DataFrame(), {}

    gop = gop.sort_values("g").reset_index(drop=True)
    # playback timeline: start after init_buffer_sec
    gop["play_time"] = init_buffer_sec + gop["g"] * gop_dur
    gop["stall"] = np.maximum(0.0, gop["gop_done_time"] - gop["play_time"])
    gop["stall_event"] = (gop["stall"] > 1e-6).astype(int)
    gop["cum_stall"] = gop["stall"].cumsum()

    summary = {
        "卡顿次数": int(gop["stall_event"].sum()),
        "卡顿总时长(秒)": float(gop["stall"].sum()),
        "平均每GOP卡顿(秒)": float(gop["stall"].mean()),
        "卡顿比例": float(gop["stall"].sum() / (len(gop)*gop_dur + 1e-9)),
        "首段可播时间(秒)": float(np.min(gop["gop_done_time"])) if len(gop)>0 else np.nan
    }
    return gop, summary

def plot_save(figpath):
    plt.tight_layout()
    plt.savefig(figpath, dpi=180)
    plt.close()

def main():
    # setup_chinese_font()
    ap = argparse.ArgumentParser()
    ap.add_argument("--log_base", default="logs", help="logs directory containing single/rr/ours subdirs")
    ap.add_argument("--out_dir", default="logs/figs_multi")
    ap.add_argument("--gop_dur", type=float, default=1.0)
    ap.add_argument("--time_bin", type=float, default=0.2)
    ap.add_argument("--high_prio_q", type=float, default=0.9, help="top quantile for high priority on-time rate")
    args = ap.parse_args()
    ensure_dir(args.out_dir)

    modes = ["single","rr","ours"]
    data = {}

    for m in modes:
        d = os.path.join(args.log_base, m)
        s = read_csv_safe(os.path.join(d, "sender_tasks.csv"))
        r = read_csv_safe(os.path.join(d, "receiver_tasks.csv"))
        p = read_csv_safe(os.path.join(d, "path_stats.csv")) if os.path.exists(os.path.join(d,"path_stats.csv")) else pd.DataFrame()
        data[m] = {"send":s, "recv":r, "path":p}

    # ---- Task completion tables ----
    done = {}
    gop_done = {}
    for m in modes:
        done[m] = compute_task_completion(data[m]["recv"])
        gop_done[m] = compute_gop_completion(done[m])

    # ---- Export Chinese summary CSV ----
    rows = []
    for m in modes:
        df = done[m]
        if df.empty:
            continue
        orig = df[df["isRedundant"]==0]
        miss_rate = float(orig["deadlineMiss"].mean()) if len(orig)>0 else np.nan

        # high priority on-time
        q = float(orig["priority"].quantile(args.high_prio_q)) if len(orig)>0 else 1.0
        hp = orig[orig["priority"]>=q]
        hp_ontime = float((hp["deadlineMiss"]==0).mean()) if len(hp)>0 else np.nan

        gop = gop_done[m]
        gop_mean = float(gop["gop_done_time"].mean()) if len(gop)>0 else np.nan
        gop_p95  = float(np.percentile(gop["gop_done_time"],95)) if len(gop)>0 else np.nan

        # fairness based on total bytes sent per subflowId from path_stats (approx): use availableWindow/cWnd not bytes, so use sender tasks for shares:
        send = data[m]["send"]
        # we don't have per-path bytes in sender_tasks.csv in this quic version; so use path_stats as proxy:
        fair = np.nan
        if not data[m]["path"].empty:
            # approximate utilization by average bytesInFlight per subflow
            util = data[m]["path"].groupby("subflowId")["bytesInFlight"].mean().values
            if len(util)>0:
                fair = jain_fairness(util)

        # QoE proxy
        gop_df, qoe_sum = qoe_proxy(df, gop_dur=args.gop_dur)
        rows.append({
            "方法": m,
            "原始任务数": int((df["isRedundant"]==0).sum()),
            "冗余任务数": int((df["isRedundant"]==1).sum()),
            "原始任务超时率": miss_rate,
            f"高优先级(top{args.high_prio_q:.2f})准时率": hp_ontime,
            "GOP完成时间均值(秒)": gop_mean,
            "GOP完成时间P95(秒)": gop_p95,
            "路径负载公平性(Jain,proxy)": fair,
            "卡顿次数(proxy)": qoe_sum.get("卡顿次数", np.nan),
            "卡顿总时长(秒,proxy)": qoe_sum.get("卡顿总时长(秒)", np.nan),
            "卡顿比例(proxy)": qoe_sum.get("卡顿比例", np.nan),
        })

    sum_df = pd.DataFrame(rows)
    sum_df.to_csv(os.path.join(args.out_dir, "汇总指标.csv"), index=False, encoding="utf-8-sig")

    # =========================
    # 图1：时间维度的路径流量分布（5条线）
    # =========================
    plt.figure()
    # single: one line
    tb = time_binned_bytes(data["single"]["send"], bin_sec=args.time_bin)
    plt.plot(tb["t_bin"], tb["bytes"]/args.time_bin, label="single-路径0(吞吐proxy B/s)")
    # rr and ours: if sender_tasks had pathId we could split; QUIC版这里用 path_stats 的 bytesInFlight 近似为“流量波动”
    # rr subflows
    if not data["rr"]["path"].empty:
        for sid,dfp in data["rr"]["path"].groupby("subflowId"):
            plt.plot(dfp["simTime"], dfp["bytesInFlight"], label=f"rr-路径{sid}(inflight proxy)")
    if not data["ours"]["path"].empty:
        for sid,dfp in data["ours"]["path"].groupby("subflowId"):
            plt.plot(dfp["simTime"], dfp["bytesInFlight"], label=f"ours-路径{sid}(inflight proxy)")
    plt.xlabel("时间(秒)")
    plt.ylabel("速率/在途字节(代理指标)")
    plt.title("多模式路径负载随时间变化（代理）")
    plt.legend()
    plot_save(os.path.join(args.out_dir, "图1_路径负载时间序列.png"))

    # =========================
    # 图2：不同方法的原始任务超时率（更合理的表达方式）
    # =========================
    plt.figure()
    miss = []
    for m in modes:
        df = done[m]
        orig = df[df["isRedundant"]==0]
        miss.append(float(orig["deadlineMiss"].mean()) if len(orig)>0 else 0.0)
    plt.bar(modes, miss)
    plt.xlabel("方法")
    plt.ylabel("原始任务超时率")
    plt.title("不同调度策略的原始任务超时率对比")
    plot_save(os.path.join(args.out_dir, "图2_超时率对比.png"))

    # =========================
    # 图3：GOP完成时间CDF（你说这个合理，保留）
    # =========================
    plt.figure()
    for m in modes:
        g = gop_done[m]
        if g.empty: continue
        x = np.sort(g["gop_done_time"].values)
        y = np.arange(1,len(x)+1)/len(x)
        plt.plot(x,y,label=m)
    plt.xlabel("GOP完成时间(秒)")
    plt.ylabel("CDF")
    plt.title("GOP完成时间CDF对比")
    plt.legend()
    plot_save(os.path.join(args.out_dir, "图3_GOP完成时间CDF.png"))

    # =========================
    # 图4：QoE代理-累计卡顿时长随GOP（新增）
    # =========================
    plt.figure()
    for m in modes:
        gop_df, _ = qoe_proxy(done[m], gop_dur=args.gop_dur)
        if gop_df.empty: continue
        plt.plot(gop_df["g"], gop_df["cum_stall"], label=m)
    plt.xlabel("GOP索引")
    plt.ylabel("累计卡顿时长(秒,proxy)")
    plt.title("累计卡顿时长随播放进度变化（QoE代理）")
    plt.legend()
    plot_save(os.path.join(args.out_dir, "图4_累计卡顿时长.png"))

    # =========================
    # 图5：QoE代理-每GOP卡顿时长分布箱线图（新增）
    # =========================
    plt.figure()
    box_data = []
    labels = []
    for m in modes:
        gop_df, _ = qoe_proxy(done[m], gop_dur=args.gop_dur)
        if gop_df.empty: continue
        box_data.append(gop_df["stall"].values)
        labels.append(m)
    if box_data:
        plt.boxplot(box_data, labels=labels, showfliers=False)
    plt.xlabel("方法")
    plt.ylabel("每GOP卡顿(秒,proxy)")
    plt.title("每GOP卡顿时长分布（箱线图）")
    plot_save(os.path.join(args.out_dir, "图5_每GOP卡顿箱线图.png"))

    # =========================
    # 图6：高优先级准时率（Top quantile）（新增）
    # =========================
    plt.figure()
    vals = []
    for m in modes:
        df = done[m]
        orig = df[df["isRedundant"]==0]
        if len(orig)==0:
            vals.append(0.0); continue
        th = float(orig["priority"].quantile(args.high_prio_q))
        hp = orig[orig["priority"]>=th]
        vals.append(float((hp["deadlineMiss"]==0).mean()) if len(hp)>0 else 0.0)
    plt.bar(modes, vals)
    plt.xlabel("方法")
    plt.ylabel(f"高优先级(top{args.high_prio_q:.2f})准时率")
    plt.title("高优先级任务准时到达率对比")
    plot_save(os.path.join(args.out_dir, "图6_高优先级准时率.png"))

    # =========================
    # 图7：RTT分布（来自子流tcb）（新增）
    # =========================
    plt.figure()
    for m in ["rr","ours"]:
        p = data[m]["path"]
        if p.empty: continue
        # take lastRttMs of all samples
        plt.hist(p["lastRttMs"].values, bins=50, alpha=0.6, label=m, density=True)
    plt.xlabel("RTT(ms)")
    plt.ylabel("概率密度")
    plt.title("子流RTT分布对比（rr vs ours）")
    plt.legend()
    plot_save(os.path.join(args.out_dir, "图7_RTT分布.png"))

    # =========================
    # 图8：优先级分桶的超时率（新增，更严谨口径：三方法都用同一批任务的priority分桶）
    # =========================
    plt.figure()
    bins = [0,0.2,0.4,0.6,0.8,1.0]
    centers = [(bins[i]+bins[i+1])/2 for i in range(len(bins)-1)]
    for m in modes:
        df = done[m]
        orig = df[df["isRedundant"]==0].copy()
        if orig.empty: continue
        orig["bin"] = pd.cut(orig["priority"], bins=bins, include_lowest=True)
        grp = orig.groupby("bin")["deadlineMiss"].mean().reindex(pd.IntervalIndex.from_breaks(bins), fill_value=np.nan)
        y = grp.values
        plt.plot(centers, y, marker="o", label=m)
    plt.xlabel("优先级分桶中心")
    plt.ylabel("桶内超时率")
    plt.title("优先级分桶的超时率对比（同任务集，不同调度）")
    plt.legend()
    plot_save(os.path.join(args.out_dir, "图8_优先级分桶超时率.png"))

    # =========================
    # 图9：路径公平性(Jain)随时间（新增，proxy=inflight）
    # =========================
    plt.figure()
    for m in ["rr","ours"]:
        p = data[m]["path"]
        if p.empty: continue
        # compute fairness at each time by pivot
        piv = p.pivot_table(index="simTime", columns="subflowId", values="bytesInFlight", aggfunc="mean").fillna(0.0)
        jf = []
        tt = piv.index.values
        for i in range(len(tt)):
            jf.append(jain_fairness(piv.iloc[i].values))
        plt.plot(tt, jf, label=m)
    plt.xlabel("时间(秒)")
    plt.ylabel("Jain公平性(0~1)")
    plt.title("路径负载公平性随时间变化（代理）")
    plt.legend()
    plot_save(os.path.join(args.out_dir, "图9_路径公平性时间序列.png"))

    print("[OK] 输出完成：")
    print(" - 汇总指标.csv")
    print(" - 图1~图9（含新增≥5张图）")
    print(" 输出目录：", args.out_dir)

if __name__ == "__main__":
    main()
