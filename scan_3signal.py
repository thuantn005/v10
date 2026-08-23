#!/usr/bin/env python3
"""
scan_3signal.py — Quet seed voi thuat toan "3 dau hieu lich su":
  Dau 1: tan suat trong cua so gan (200 ky gan nhat)
  Dau 2: tan suat toan lich su
  Dau 3: so ky vang mat (cold bonus)

Voi moi seed: dung seed lam RNG state, bo 5 so chinh + 1 so DB theo
trong so (khong hoan lai), CHO TUNG KY rieng biet (weights tinh lai
moi ky dua tren lich su TRUOC ky do -- khong nhin tuong lai).

Cham hon nhieu so voi SplitMix64 vi khong the dung LUT (trong so doi
theo tung ky). Chi nen dung voi so luong seed nho (hang trieu, khong
phai hang ty).

Da kiem chung: logic tuong tu code da dung trong repo v5.
"""
import csv, json, os, sys, time
from collections import Counter
from pathlib import Path
import random as pyrandom

CSV_PATH = os.environ.get("CSV_PATH", "data/all.csv")
OUT_PATH = os.environ.get("OUT_PATH", "results/chunk.json")
START    = int(os.environ.get("SCAN_START", "1"))
END      = int(os.environ.get("SCAN_END",   "1000000"))
MIN_LOG  = int(os.environ.get("MIN_LOG",    "2"))
WARMUP   = int(os.environ.get("WARMUP",     "200"))  # can it nhat 200 ky lich su
WINDOW   = int(os.environ.get("WINDOW",     "200"))  # cua so gan de tinh "hot/cold"
CHECKPOINT_SEC = 30


def calc_weights(history):
    """history: list of draws (da sort theo draw_id tang dan)."""
    freq_all = Counter(); last_seen = {}; sp_all = Counter()
    for i, d in enumerate(history):
        for x in d["numbers"]:
            freq_all[x] += 1
            last_seen[x] = i
        sp_all[d["special"]] += 1

    last_idx = len(history) - 1
    overdue = {x: (last_idx - last_seen.get(x, -1)) for x in range(1, 36)}
    max_all = max(freq_all.values()) if freq_all else 1
    max_od = max(overdue.values()) if overdue else 1
    max_sp = max(sp_all.values()) if sp_all else 1

    recent = history[-WINDOW:]
    freq_recent = Counter(); sp_recent = Counter()
    for d in recent:
        for x in d["numbers"]:
            freq_recent[x] += 1
        sp_recent[d["special"]] += 1
    max_recent = max(freq_recent.values()) if freq_recent else 1
    max_sp_recent = max(sp_recent.values()) if sp_recent else 1

    w = {x: 0.1 + 0.4*(freq_recent.get(x, 0)/max_recent)
             + 0.3*(freq_all.get(x, 0)/max_all)
             + 0.3*(overdue[x]/max_od)
         for x in range(1, 36)}
    sw = {x: 0.1 + 0.5*(sp_recent.get(x, 0)/max_sp_recent)
              + 0.5*(sp_all.get(x, 0)/max_sp)
          for x in range(1, 13)}
    return w, sw


def sample_weighted(weights, k, rng):
    pool = list(weights.items())
    picked = []
    for _ in range(k):
        total = sum(v for _, v in pool)
        r = rng.random() * total
        for i, (num, v) in enumerate(pool):
            r -= v
            if r <= 0:
                picked.append(num)
                pool.pop(i)
                break
        else:
            picked.append(pool[-1][0])
            pool.pop()
    return picked


def main():
    res = []
    with open(CSV_PATH, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                rj = json.loads(row["result_json"])
                res.append({
                    "draw_id": int(row["draw_id"]),
                    "draw_date": row.get("draw_date", ""),
                    "numbers": sorted(rj["numbers"]),
                    "special": rj["special_numbers"][0],
                })
            except Exception:
                continue
    res.sort(key=lambda x: x["draw_id"])
    n = len(res)
    print(f"Loaded {n} ky. Quet seed {START:,} -> {END:,}, MIN_LOG={MIN_LOG}", flush=True)

    if n < WARMUP + 1:
        print(f"Khong du lich su (can >= {WARMUP+1} ky)", flush=True)
        sys.exit(1)

    # Precompute weights cho TUNG ky (chi tinh 1 lan, dung chung cho moi seed)
    t_pre = time.time()
    weights_per_draw = []  # [(w, sw, actual_numbers_mask, actual_special)]
    for i in range(WARMUP, n):
        history = res[:i]
        w, sw = calc_weights(history)
        actual = res[i]
        weights_per_draw.append((w, sw, set(actual["numbers"]), actual["special"]))
    print(f"Precompute weights {len(weights_per_draw)} ky: {time.time()-t_pre:.1f}s", flush=True)

    Path(OUT_PATH).parent.mkdir(parents=True, exist_ok=True)

    if START > END:
        payload = {"status": "empty", "scan_start": START, "scan_end": END,
                   "scanned": 0, "completed": True, "min_log": MIN_LOG,
                   "found_j1_2plus": 0, "found_j1_3plus": 0, "found_j1_4plus": 0,
                   "results_by_level": {"2": [], "3": [], "4": []}}
        with open(OUT_PATH, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=1)
        print("Range rong.")
        return

    def save_checkpoint(scanned_count, found_by_level, done=False):
        tmp = OUT_PATH + ".tmp"
        payload = {
            "status": "completed" if done else "partial",
            "scan_start": START, "scan_end": END,
            "scanned": scanned_count, "completed": done, "min_log": MIN_LOG,
            "found_j1_2plus": len(found_by_level[2]),
            "found_j1_3plus": len(found_by_level[3]),
            "found_j1_4plus": len(found_by_level[4]),
            "results_by_level": {
                "2": sorted(found_by_level[2], key=lambda x: -x["j1_count"])[:200],
                "3": sorted(found_by_level[3], key=lambda x: -x["j1_count"])[:200],
                "4": sorted(found_by_level[4], key=lambda x: -x["j1_count"])[:200],
            },
        }
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=1)
        os.replace(tmp, OUT_PATH)

    found_by_level = {2: [], 3: [], 4: []}
    scanned_count = 0
    t0 = time.time(); last_checkpoint = t0; last_log = t0

    for seed in range(START, END + 1):
        cnt = 0
        hits = []
        for idx, (w, sw, actual_set, actual_sp) in enumerate(weights_per_draw):
            rng = pyrandom.Random(seed * 1000003 + idx)  # tron seed voi idx ky de moi ky khac nhau
            main = set(sample_weighted(w, 5, rng))
            sp = sample_weighted(sw, 1, rng)[0]
            if main == actual_set and sp == actual_sp:
                cnt += 1
                draw = res[WARMUP + idx]
                hits.append({"draw_id": draw["draw_id"], "draw_date": draw["draw_date"],
                             "numbers": draw["numbers"], "special": draw["special"]})

        if cnt >= MIN_LOG:
            entry = {"seed": seed, "j1_count": cnt, "jackpot1_hits": hits}
            for level in (2, 3, 4):
                if cnt >= level:
                    found_by_level[level].append(entry)
            print(f"HIT seed={seed} J1={cnt}x", flush=True)

        scanned_count = seed - START + 1
        now = time.time()
        if now - last_checkpoint >= CHECKPOINT_SEC:
            save_checkpoint(scanned_count, found_by_level, done=False)
            last_checkpoint = now
        if now - last_log >= 15:
            rate = scanned_count / (now - t0)
            eta = (END - seed) / rate if rate > 0 else 0
            print(f"  scanned={scanned_count:,}/{END-START+1:,} ({rate:.1f}/s) ETA {eta/3600:.2f}h "
                  f"f2={len(found_by_level[2])} f3={len(found_by_level[3])} f4={len(found_by_level[4])}", flush=True)
            last_log = now

    save_checkpoint(scanned_count, found_by_level, done=True)
    elapsed = time.time() - t0
    print(f"\nXong: {scanned_count:,} seed / {elapsed:.0f}s ({scanned_count/elapsed:.1f}/s). "
          f"J1>=2:{len(found_by_level[2])} J1>=3:{len(found_by_level[3])} J1>=4:{len(found_by_level[4])}")


if __name__ == "__main__":
    main()
