#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os, glob, csv, math, random, argparse

def tile_prob_from_viewport(tile_k, K, center_k, sigma=2.0):
    d = min((tile_k-center_k) % K, (center_k-tile_k) % K)
    return math.exp(-(d*d)/(2*sigma*sigma))

def normalize_probs(probs):
    s = sum(probs)
    if s <= 0:
        return [1.0/len(probs)]*len(probs)
    return [p/s for p in probs]

def stretch_priority(P_list, gamma=0.7, eps=1e-9):
    # 单调变换：先 min-max 到 [0,1]，再做幂变换，gamma<1 拉伸高值
    mn = min(P_list)
    mx = max(P_list)
    span = max(mx - mn, eps)
    out = []
    for p in P_list:
        x = (p - mn) / span
        x = max(0.0, min(1.0, x))
        x = x ** gamma
        out.append(x)
    return out

def even_slices(total_bytes, n_slices, min_slice=256):
    """
    把 total_bytes 均匀分成 n_slices 份，返回每份长度数组
    目标：尽量避免长尾 0；但如果文件确实太小，也保证不会负数。
    """
    if n_slices <= 0:
        return []
    if total_bytes <= 0:
        return [0]*n_slices

    base = total_bytes // n_slices
    rem  = total_bytes % n_slices
    sizes = [base]*n_slices
    for i in range(rem):
        sizes[i] += 1

    # 如果 base 太小，会导致很多 slice 过小，这里只做温和处理：
    # 允许小 slice，但确保非 0（只要 total_bytes >= n_slices）
    # 若 total_bytes < n_slices，则后面的 slice 必然为 0（物理限制）
    for i in range(n_slices):
        if sizes[i] > 0 and sizes[i] < min_slice:
            # 不强行抬高到 min_slice，否则会超总量；这里只是允许小值存在
            pass
    return sizes

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seg_dir", default="data/segments")
    ap.add_argument("--out_csv", default="data/tasks/tasks.csv")
    ap.add_argument("--K", type=int, default=8)
    ap.add_argument("--frames_per_gop", type=int, default=30)
    ap.add_argument("--gop_dur", type=float, default=1.0)
    ap.add_argument("--deadline_base", type=float, default=0.25)

    ap.add_argument("--viewport_trace", default="")

    # HQ 选择：top-M
    ap.add_argument("--top_m", type=int, default=2, help="top-M tiles per frame are HQ")

    # 冗余策略：阈值触发 + 兜底比例
    ap.add_argument("--red_threshold", type=float, default=0.75, help="create redundancy if P >= threshold")
    ap.add_argument("--gamma_red", type=float, default=0.4, help="priority multiplier for redundant tasks")
    ap.add_argument("--min_red_ratio", type=float, default=0.05, help="fallback ensure at least this ratio of originals have redundancy")

    # priority 拉伸
    ap.add_argument("--prio_gamma", type=float, default=0.7, help="priority stretching gamma (<1 expand high values)")

    # viewport sigma
    ap.add_argument("--sigma", type=float, default=2.0)

    args = ap.parse_args()

    hq = sorted(glob.glob(os.path.join(args.seg_dir, "hq", "seg_*.mp4")))
    lq = sorted(glob.glob(os.path.join(args.seg_dir, "lq", "seg_*.mp4")))
    assert len(hq) == len(lq) and len(hq) > 0, "segments mismatch or empty"

    os.makedirs(os.path.dirname(args.out_csv), exist_ok=True)

    # load viewport centers if any (each line: center_k)
    viewport_centers = []
    if args.viewport_trace and os.path.exists(args.viewport_trace):
        with open(args.viewport_trace, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    viewport_centers.append(int(line) % args.K)

    # Step A: generate all ORIGINAL tasks first (with preliminary P)
    task_id = 1
    orig_rows = []
    orig_P = []

    for g in range(len(hq)):
        center = viewport_centers[g] if g < len(viewport_centers) else random.randrange(args.K)
        probs = normalize_probs([tile_prob_from_viewport(k, args.K, center, sigma=args.sigma) for k in range(args.K)])

        # per segment sizes
        seg_hq = hq[g]
        seg_lq = lq[g]
        size_hq = os.path.getsize(seg_hq)
        size_lq = os.path.getsize(seg_lq)

        # decide HQ tiles per frame: top-M by pview
        # Precompute ranking once per GOP (你也可以换成每帧不同，这里先保持简单)
        rank = sorted(range(args.K), key=lambda k: probs[k], reverse=True)
        top_m = max(1, min(args.top_m, args.K))
        hq_set = set(rank[:top_m])

        # count tasks for HQ/LQ
        n_total = args.frames_per_gop * args.K
        n_hq = args.frames_per_gop * top_m
        n_lq = n_total - n_hq

        hq_sizes = even_slices(size_hq, n_hq)
        lq_sizes = even_slices(size_lq, n_lq)

        # assign offsets sequentially within each file but sizes are pre-decided => no long-tail zeros
        off_hq = 0
        off_lq = 0
        idx_hq = 0
        idx_lq = 0

        for t in range(args.frames_per_gop):
            # GOP/frame importance (simple but consistent): I frame boost at t==0
            wgop = 1.0 if t == 0 else 0.6

            for k in range(args.K):
                pview = probs[k]
                # preliminary fusion (same idea as before)
                P = 0.6 * pview + 0.3 * wgop + 0.1 * 1.0
                P = max(0.0, min(1.0, P))

                ddl = 0.1 + g * args.gop_dur + (t / args.frames_per_gop) * args.gop_dur + args.deadline_base

                use_hq = (k in hq_set)

                if use_hq:
                    size_bytes = hq_sizes[idx_hq] if idx_hq < len(hq_sizes) else 0
                    payload = seg_hq
                    payload_off = off_hq
                    off_hq += size_bytes
                    idx_hq += 1
                    if payload_off + size_bytes > size_hq:
                        size_bytes = max(0, size_hq - payload_off)
                else:
                    size_bytes = lq_sizes[idx_lq] if idx_lq < len(lq_sizes) else 0
                    payload = seg_lq
                    payload_off = off_lq
                    off_lq += size_bytes
                    idx_lq += 1
                    if payload_off + size_bytes > size_lq:
                        size_bytes = max(0, size_lq - payload_off)

                if size_bytes <= 0:
                    continue

                orig_rows.append([
                    task_id, g, t, k,
                    size_bytes, ddl, P,
                    0, 0,
                    payload, payload_off, size_bytes
                ])
                orig_P.append(P)
                task_id += 1

    # Step B: stretch priorities to expand range but keep relative order
    stretched = stretch_priority(orig_P, gamma=args.prio_gamma)
    for i in range(len(orig_rows)):
        orig_rows[i][6] = stretched[i]

    # Step C: redundancy selection
    cand_for_red = [(row[0], row[6]) for row in orig_rows]
    red_set = set([tid for tid, P in cand_for_red if P >= args.red_threshold])

    min_need = int(len(orig_rows) * args.min_red_ratio)
    if len(red_set) < min_need:
        cand_for_red.sort(key=lambda x: x[1], reverse=True)
        for tid, P in cand_for_red:
            red_set.add(tid)
            if len(red_set) >= min_need:
                break

    # Step D: output rows (original + redundant virtual tasks)
    rows = []
    red_count = 0
    for r in orig_rows:
        rows.append(r)
        if r[0] in red_set:
            rows.append([
                task_id, r[1], r[2], r[3],
                r[4], r[5], max(0.0, min(1.0, r[6] * args.gamma_red)),
                1, r[0],
                r[9], r[10], r[11]
            ])
            task_id += 1
            red_count += 1

    with open(args.out_csv, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([
            "taskId","g","t","k",
            "sizeBytes","deadlineSec","priority",
            "isRedundant","originTaskId",
            "payloadPath","payloadOffset","payloadLen"
        ])
        w.writerows(rows)

    print(f"[OK] tasks={len(rows)} orig={len(orig_rows)} red={red_count} "
          f"topM={args.top_m} prio_gamma={args.prio_gamma} threshold={args.red_threshold} min_red_ratio={args.min_red_ratio}")

if __name__ == "__main__":
    main()
