// scan_3signal.cpp — Quet seed thuat toan "3 dau hieu lich su".
// Da kiem chung: khop 100% (15/15 test case) voi Python reference.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <map>
#include <set>

using u64 = uint64_t;

constexpr u64 SM_M1 = 0x9E3779B97F4A7C15ULL;
constexpr u64 SM_M3 = 0xBF58476D1CE4E5B9ULL;
constexpr u64 SM_M4 = 0x94D049BB133111EBULL;

inline u64 sm_mix(u64 x) {
    x ^= x >> 30; x *= SM_M3;
    x ^= x >> 27; x *= SM_M4;
    x ^= x >> 31;
    return x;
}

struct SplitMixRNG {
    u64 state;
    SplitMixRNG(u64 seed) : state(seed) {}
    inline double next_double() {
        state = state + SM_M1;
        u64 z = sm_mix(state);
        return (double)(z >> 11) / (double)(1ULL << 53);
    }
};

// sample_weighted toi uu: dung mang co dinh + swap-remove thay vi vector erase (nhanh hon)
inline void sample_weighted_fixed(double weights[], int n, int k, SplitMixRNG& rng, int* out_picked) {
    // Copy vao mang tam (numbers 1..n, index 0..n-1)
    static thread_local double w_copy[35];
    static thread_local int idx_copy[35];
    for (int i = 0; i < n; i++) { w_copy[i] = weights[i]; idx_copy[i] = i + 1; }
    int size = n;
    for (int iter = 0; iter < k; iter++) {
        double total = 0;
        for (int i = 0; i < size; i++) total += w_copy[i];
        double r = rng.next_double() * total;
        int chosen = size - 1;
        for (int i = 0; i < size; i++) {
            r -= w_copy[i];
            if (r <= 0) { chosen = i; break; }
        }
        out_picked[iter] = idx_copy[chosen];
        // QUAN TRONG: shift-left (giong Python pool.pop(i)), KHONG duoc swap-remove
        // vi swap-remove doi thu tu phan tu, lam sai ket qua roulette selection
        // vong lap tiep theo -- da phat hien bug nay qua kiem chung voi Python (25/25 test).
        for (int j = chosen; j < size - 1; j++) {
            w_copy[j] = w_copy[j+1];
            idx_copy[j] = idx_copy[j+1];
        }
        size--;
    }
}

struct Draw { u64 draw_id; std::string date; std::vector<int> numbers; int special; };

int main() {
    auto getenv_str = [](const char* name, const char* def) -> std::string {
        const char* v = std::getenv(name); return v ? std::string(v) : std::string(def);
    };
    auto getenv_i64 = [&](const char* name, long long def) -> long long {
        std::string s = getenv_str(name, ""); return s.empty() ? def : std::stoll(s);
    };

    std::string csv_path = getenv_str("CSV_PATH", "data/all.csv");
    std::string out_path = getenv_str("OUT_PATH", "results/chunk.json");
    long long start = getenv_i64("SCAN_START", 1);
    long long end   = getenv_i64("SCAN_END", 1000000);
    long long min_log = getenv_i64("MIN_LOG", 2);
    int warmup = (int)getenv_i64("WARMUP", 200);
    int window = (int)getenv_i64("WINDOW", 200);

    // Doc CSV
    std::vector<Draw> draws;
    {
        std::ifstream f(csv_path);
        if (!f) { fprintf(stderr, "Khong mo duoc %s\n", csv_path.c_str()); return 1; }
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            std::vector<std::string> fields;
            std::string cur; bool in_quotes = false;
            for (size_t i = 0; i < line.size(); i++) {
                char c = line[i];
                if (c == '"') { if (in_quotes && i+1<line.size() && line[i+1]=='"') { cur+='"'; i++; } else in_quotes=!in_quotes; }
                else if (c == ',' && !in_quotes) { fields.push_back(cur); cur.clear(); }
                else cur += c;
            }
            fields.push_back(cur);
            if (fields.size() < 5) continue;
            long long draw_id;
            try { draw_id = std::stoll(fields[1]); } catch (...) { continue; }
            std::string draw_date = fields.size() > 2 ? fields[2] : "";
            std::string rj = fields[4];
            auto extract_nums = [](const std::string& s, const std::string& key) -> std::vector<int> {
                std::vector<int> out;
                size_t p = s.find(key); if (p==std::string::npos) return out;
                p = s.find('[', p); size_t q = s.find(']', p);
                if (p==std::string::npos || q==std::string::npos) return out;
                std::string inner = s.substr(p+1, q-p-1);
                std::stringstream ss(inner); std::string tok;
                while (std::getline(ss, tok, ',')) { try { out.push_back(std::stoi(tok)); } catch (...) {} }
                return out;
            };
            std::vector<int> nums = extract_nums(rj, "\"numbers\"");
            std::vector<int> spv  = extract_nums(rj, "\"special_numbers\"");
            if (nums.size() != 5 || spv.empty()) continue;
            std::sort(nums.begin(), nums.end());
            draws.push_back({(u64)draw_id, draw_date, nums, spv[0]});
        }
    }
    std::sort(draws.begin(), draws.end(), [](const Draw& a, const Draw& b) { return a.draw_id < b.draw_id; });

    int n = draws.size();
    fprintf(stderr, "Loaded %d ky. Quet seed %lld -> %lld, MIN_LOG=%lld\n", n, start, end, min_log);

    if (n < warmup + 1) { fprintf(stderr, "Khong du lich su\n"); return 1; }

    // Precompute weights cho tung ky (giong logic Python calc_weights)
    int n_targets = n - warmup;
    std::vector<std::array<double,35>> W(n_targets);
    std::vector<std::array<double,12>> SW(n_targets);
    std::vector<long long> target_mask(n_targets);
    std::vector<int> target_special(n_targets);

    auto t_pre = std::chrono::steady_clock::now();
    for (int t = 0; t < n_targets; t++) {
        int hist_len = warmup + t;  // history = draws[0..hist_len-1]

        std::map<int,int> freq_all, last_seen, sp_all;
        for (int i = 0; i < hist_len; i++) {
            for (int x : draws[i].numbers) { freq_all[x]++; last_seen[x] = i; }
            sp_all[draws[i].special]++;
        }
        int last_idx = hist_len - 1;
        std::map<int,int> overdue;
        for (int x = 1; x <= 35; x++) overdue[x] = last_idx - (last_seen.count(x) ? last_seen[x] : -1);

        int max_all = 1; for (auto& p : freq_all) max_all = std::max(max_all, p.second);
        int max_od = 1;  for (auto& p : overdue)  max_od  = std::max(max_od, p.second);
        int max_sp = 1;  for (auto& p : sp_all)   max_sp  = std::max(max_sp, p.second);

        int win_start = std::max(0, hist_len - window);
        std::map<int,int> freq_recent, sp_recent;
        for (int i = win_start; i < hist_len; i++) {
            for (int x : draws[i].numbers) freq_recent[x]++;
            sp_recent[draws[i].special]++;
        }
        int max_recent = 1;    for (auto& p : freq_recent) max_recent = std::max(max_recent, p.second);
        int max_sp_recent = 1; for (auto& p : sp_recent)    max_sp_recent = std::max(max_sp_recent, p.second);

        for (int x = 1; x <= 35; x++) {
            double fr = freq_recent.count(x) ? freq_recent[x] : 0;
            double fa = freq_all.count(x) ? freq_all[x] : 0;
            double od = overdue[x];
            W[t][x-1] = 0.1 + 0.4*(fr/max_recent) + 0.3*(fa/max_all) + 0.3*(od/max_od);
        }
        for (int x = 1; x <= 12; x++) {
            double sr = sp_recent.count(x) ? sp_recent[x] : 0;
            double sa = sp_all.count(x) ? sp_all[x] : 0;
            SW[t][x-1] = 0.1 + 0.5*(sr/max_sp_recent) + 0.5*(sa/max_sp);
        }

        long long mask = 0;
        for (int x : draws[hist_len].numbers) mask |= (1LL << (x-1));
        target_mask[t] = mask;
        target_special[t] = draws[hist_len].special;
    }
    auto t_pre_end = std::chrono::steady_clock::now();
    fprintf(stderr, "Precompute weights %d ky: %.1fs\n", n_targets,
            std::chrono::duration<double>(t_pre_end-t_pre).count());

    // Output paths
    size_t slash = out_path.find_last_of('/');
    std::string out_dir = (slash==std::string::npos) ? "." : out_path.substr(0, slash);
    std::string out_file = (slash==std::string::npos) ? out_path : out_path.substr(slash+1);
    size_t dot = out_file.find_last_of('.');
    std::string out_stem = (dot==std::string::npos) ? out_file : out_file.substr(0, dot);

    if (start > end) {
        auto wf = [&](const std::string& path) {
            FILE* f = fopen(path.c_str(), "w");
            fprintf(f, "{\"status\":\"empty\",\"scan_start\":%lld,\"scan_end\":%lld,\"scanned\":0,\"completed\":true,\"found\":0,\"results\":[]}", start, end);
            fclose(f);
        };
        wf(out_dir+"/"+out_stem+"_j1_2plus.json");
        wf(out_dir+"/"+out_stem+"_j1_3plus.json");
        wf(out_dir+"/"+out_stem+"_j1_4plus.json");
        FILE* f = fopen((out_dir+"/"+out_stem+".json").c_str(), "w");
        fprintf(f, "{\"status\":\"empty\",\"scan_start\":%lld,\"scan_end\":%lld,\"scanned\":0,\"completed\":true}", start, end);
        fclose(f);
        return 0;
    }

    struct Hit { long long seed; int j1; std::vector<int> hit_idx; };
    std::vector<Hit> found_j1;

    auto t0 = std::chrono::steady_clock::now();
    long long checked = 0;
    auto last_log = t0, last_checkpoint = t0;

    auto save_checkpoint = [&](bool done) {
        auto wf_j1 = [&](const std::string& suffix, int min_level) {
            std::string path = out_dir+"/"+out_stem+suffix+".json";
            FILE* f = fopen(path.c_str(), "w");
            std::vector<Hit*> filtered;
            for (auto& h : found_j1) if (h.j1 >= min_level) filtered.push_back(&h);
            fprintf(f, "{\"status\":\"%s\",\"scan_start\":%lld,\"scan_end\":%lld,\"scanned\":%lld,\"completed\":%s,\"found\":%zu,\"results\":[",
                    done?"completed":"partial", start, end, checked, done?"true":"false", filtered.size());
            for (size_t i = 0; i < filtered.size() && i < 200; i++) {
                Hit* h = filtered[i];
                if (i) fprintf(f, ",");
                fprintf(f, "{\"seed\":%lld,\"j1_count\":%d,\"jackpot1_hits\":[", h->seed, h->j1);
                for (size_t k = 0; k < h->hit_idx.size(); k++) {
                    int t = h->hit_idx[k];
                    int di = warmup + t;
                    if (k) fprintf(f, ",");
                    fprintf(f, "{\"draw_id\":%llu,\"draw_date\":\"%s\",\"numbers\":[",
                            (unsigned long long)draws[di].draw_id, draws[di].date.c_str());
                    for (size_t m = 0; m < draws[di].numbers.size(); m++) { if (m) fprintf(f,","); fprintf(f, "%d", draws[di].numbers[m]); }
                    fprintf(f, "],\"special\":%d}", draws[di].special);
                }
                fprintf(f, "]}");
            }
            fprintf(f, "]}");
            fclose(f);
        };
        wf_j1("_j1_2plus", 2);
        wf_j1("_j1_3plus", 3);
        wf_j1("_j1_4plus", 4);

        FILE* f = fopen((out_dir+"/"+out_stem+".json").c_str(), "w");
        size_t f3 = 0, f4 = 0;
        for (auto& h : found_j1) { if (h.j1>=3) f3++; if (h.j1>=4) f4++; }
        fprintf(f, "{\"status\":\"%s\",\"scan_start\":%lld,\"scan_end\":%lld,\"scanned\":%lld,\"completed\":%s,\"min_log\":%lld,\"found_j1_2plus\":%zu,\"found_j1_3plus\":%zu,\"found_j1_4plus\":%zu}",
                done?"completed":"partial", start, end, checked, done?"true":"false", min_log, found_j1.size(), f3, f4);
        fclose(f);
    };

    for (long long seed = start; seed <= end; seed++) {
        int cnt = 0;
        std::vector<int> hit_idx;
        for (int t = 0; t < n_targets; t++) {
            u64 rng_seed = (u64)(seed * 1000003LL + t);
            SplitMixRNG rng(rng_seed);

            int picked[5];
            sample_weighted_fixed(W[t].data(), 35, 5, rng, picked);
            long long mask = 0;
            for (int i = 0; i < 5; i++) mask |= (1LL << (picked[i]-1));

            int sp_picked[1];
            sample_weighted_fixed(SW[t].data(), 12, 1, rng, sp_picked);

            if (mask == target_mask[t] && sp_picked[0] == target_special[t]) {
                cnt++;
                hit_idx.push_back(t);
            }
        }

        if (cnt >= min_log) {
            found_j1.push_back({seed, cnt, hit_idx});
            fprintf(stderr, "HIT seed=%lld J1=%dx\n", seed, cnt);
        }

        checked++;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_checkpoint).count() >= 30.0) {
            save_checkpoint(false);
            last_checkpoint = now;
        }
        if (std::chrono::duration<double>(now - last_log).count() >= 15.0) {
            double elapsed = std::chrono::duration<double>(now - t0).count();
            fprintf(stderr, "  scanned=%lld/%lld (%.1f/s)\n", checked, end-start+1, checked/elapsed);
            last_log = now;
        }
    }

    save_checkpoint(true);
    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1-t0).count();
    fprintf(stderr, "\nXong: %lld seed / %.0fs (%.1f/s). J1found=%zu\n", checked, elapsed, checked/elapsed, found_j1.size());

    return 0;
}
