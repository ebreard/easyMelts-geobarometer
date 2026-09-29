/*
 easyMelts geobarometer (c) 2026 Eric C. P. Breard. Free software under the GNU General Public License v3 (see LICENSE.txt).

 Command-line driver for the easyMelts geobarometer (batch runs and tests).

   geobarometer_cli [key=value ...] compositions.csv
   geobarometer_cli [key=value ...] --bishop

 Keys: version (1.0.2, 1.1.0, 1.2.0), p_start, p_end, p_step (MPa), t_start, t_end, t_step (C),
       buffer (none, hm, nno, qfm, coh, iw), offsets (comma list), phases (comma list of three),
       rule (any|phase1), threshold (C), h2o (g added when the file has no H2O),
       suppress (comma list, default amphibole,biotite), stop (1|0), out (file prefix),
       negative (zero|skip: a negative oxide amount counts as 0, or the composition is not calculated),
       xlsx (1: also write <out>.xlsx, an Excel workbook with a run viewer and charts),
       jobs (run the file in that many parts at once, one per processor core, and join the results)

 Rows of the CSV file can override the grid, fO2, phases, rule and threshold in their own columns
 (P_start, P_end, P_step, T_start, T_end, T_step, buffer, offsets, phase1-3, rule, threshold; see
 Geobarometer::ReadCompositions).
*/

#include "geobarometer.hpp"
#include "melts_interface.hpp"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

static std::vector<std::string> List(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) out.push_back(item);
    return out;
}

// Starts the commands together and waits for all of them; returns their exit codes.
static std::vector<int> RunTogether(const std::vector<std::vector<std::string>> &commands) {
    std::vector<int> codes(commands.size(), -1);
#ifdef _WIN32
    std::vector<HANDLE> procs(commands.size(), NULL);
    for (size_t k = 0; k < commands.size(); ++k) {
        std::string line;
        for (const auto &a : commands[k]) {
            if (!line.empty()) line += ' ';
            line += a.find_first_of(" \t\"") == std::string::npos ? a : "\"" + a + "\"";
        }
        std::vector<char> buf(line.begin(), line.end());
        buf.push_back('\0');
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        ZeroMemory(&pi, sizeof pi);
        if (CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            procs[k] = pi.hProcess;
            CloseHandle(pi.hThread);
        }
    }
    for (size_t k = 0; k < procs.size(); ++k) {
        if (!procs[k]) continue;
        WaitForSingleObject(procs[k], INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(procs[k], &code);
        codes[k] = (int)code;
        CloseHandle(procs[k]);
    }
#else
    std::vector<pid_t> pids(commands.size(), -1);
    for (size_t k = 0; k < commands.size(); ++k) {
        std::vector<char *> av;
        for (const auto &a : commands[k]) av.push_back(const_cast<char *>(a.c_str()));
        av.push_back(nullptr);
        pid_t pid;
        if (posix_spawnp(&pid, av[0], nullptr, nullptr, av.data(), environ) == 0) pids[k] = pid;
    }
    for (size_t k = 0; k < pids.size(); ++k) {
        if (pids[k] < 0) continue;
        int status = 0;
        waitpid(pids[k], &status, 0);
        codes[k] = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    }
#endif
    return codes;
}

// jobs=N: the file is cut into N consecutive parts that run as N copies of this program at once, and
// their results are joined into one summary and one detail file.
static int RunInParts(const char *self, const std::string &file, int jobs, const std::vector<std::string> &keys,
                      const std::string &out) {
    std::ifstream in(file);
    std::string header, line;
    if (!in || !std::getline(in, header)) {
        std::cerr << "error: cannot read " << file << std::endl;
        return 1;
    }
    std::vector<std::string> rows;
    while (std::getline(in, line))
        if (line.find_first_not_of(" \t\r,;") != std::string::npos) rows.push_back(line);
    const int n = (int)rows.size(), parts = std::max(1, std::min(jobs, n));
    std::string exe(self);
#ifdef _WIN32
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH)) exe = path;
#endif
    std::vector<std::vector<std::string>> commands;
    std::vector<std::string> names;
    for (int k = 0; k < parts; ++k) {
        char tag[32];
        std::snprintf(tag, sizeof tag, "_part%02d", k + 1);
        const std::string base = out + tag;
        std::ofstream p(base + ".csv");
        p << header << "\n";
        for (int i = k * n / parts; i < (k + 1) * n / parts; ++i) p << rows[i] << "\n";
        std::vector<std::string> cmd{exe, base + ".csv"};
        cmd.insert(cmd.end(), keys.begin(), keys.end());
        cmd.push_back("jobs=1");
        cmd.push_back("out=" + base);
        commands.push_back(cmd);
        names.push_back(base);
    }
    std::cerr << n << " compositions in " << parts << " parts running at once" << std::endl;
    const std::vector<int> codes = RunTogether(commands);
    std::vector<std::string> summaries, details;
    int failed = 0;
    for (int k = 0; k < parts; ++k) {
        if (codes[k] != 0) {
            ++failed;
            std::cerr << "part " << k + 1 << " (" << names[k] << ".csv) stopped with code " << codes[k]
                      << "; its finished compositions are in " << names[k] << "_summary.csv" << std::endl;
            summaries.push_back(names[k] + "_summary.csv");
            continue;
        }
        summaries.push_back(names[k] + "_summary.csv");
        details.push_back(names[k] + "_detail.csv");
    }
    const bool ok = Geobarometer::MergeCSV(summaries, out + "_summary.csv") && Geobarometer::MergeCSV(details, out + "_detail.csv");
    if (ok && failed == 0)
        for (const auto &b : names)
            for (const char *ext : {".csv", "_summary.csv", "_detail.csv"}) std::remove((b + ext).c_str());
    std::cerr << (ok ? "results in " + out + "_summary.csv and " + out + "_detail.csv" : std::string("could not join the parts")) << std::endl;
    return ok && failed == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    std::map<std::string, std::string> kv;
    std::string file;
    bool xlsx_only = false;
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--xlsx") {
            xlsx_only = true;
            continue;
        }
        size_t eq = a.find('=');
        if (eq != std::string::npos) kv[a.substr(0, eq)] = a.substr(eq + 1);
        else file = a;
    }
    auto get = [&](const char *k, const std::string &d) { return kv.count(k) ? kv[k] : d; };
    // Excel workbook of <out>_summary.csv and <out>_detail.csv
    auto workbook = [](const std::string &out) {
        std::string err;
        if (!Geobarometer::WriteWorkbook(out + "_summary.csv", out + "_detail.csv", out + ".xlsx", err)) {
            std::cerr << "error: " << err << std::endl;
            return false;
        }
        std::cerr << "workbook " << out << ".xlsx" << std::endl;
        return true;
    };
    if (xlsx_only) { // geobarometer_cli --xlsx <prefix>: workbook of an earlier run
        std::string out = file.empty() ? get("out", "geobarometer") : file;
        for (const char *ext : {"_summary.csv", "_detail.csv", ".csv"})
            if (out.size() > std::string(ext).size() && out.compare(out.size() - std::string(ext).size(), std::string::npos, ext) == 0) {
                out.erase(out.size() - std::string(ext).size());
                break;
            }
        return workbook(out) ? 0 : 1;
    }
    const bool xlsx = get("xlsx", "0") != "0";

    const int jobs = std::stoi(get("jobs", "1"));
    if (jobs > 1 && !file.empty() && file != "--bishop") {
        std::vector<std::string> keys;
        for (const auto &p : kv)
            if (p.first != "jobs" && p.first != "out" && p.first != "xlsx") keys.push_back(p.first + "=" + p.second);
        const int code = RunInParts(argv[0], file, jobs, keys, get("out", "geobarometer"));
        if (xlsx && !workbook(get("out", "geobarometer"))) return 1;
        return code;
    }

    const std::string version = get("version", "1.0.2");
    int mode = MODE__MELTS;
    if (version == "1.1.0") mode = MODE__MELTSandCO2;
    else if (version == "1.2.0") mode = MODE__MELTSandCO2_H2O;

    MeltsInterface mi;
    mi.InitializeMelts(mode);

    GeobarometerSettings s;
    s.p_start = std::stod(get("p_start", "500"));
    s.p_end = std::stod(get("p_end", "25"));
    s.p_step = std::stod(get("p_step", "25"));
    s.t_start = std::stod(get("t_start", "1100"));
    s.t_end = std::stod(get("t_end", "700"));
    s.t_step = std::stod(get("t_step", "1"));
    const int buffer = Geobarometer::BufferIndex(get("buffer", "nno"));
    if (buffer >= 0) s.fo2_path = buffer;
    s.fo2_offsets.clear();
    for (const auto &o : List(get("offsets", "0"))) s.fo2_offsets.push_back(std::stod(o));
    std::vector<std::string> ph = List(get("phases", "quartz,feldspar1,feldspar2"));
    for (int i = 0; i < 3 && i < (int)ph.size(); ++i) s.phases[i] = ph[i];
    s.require_phase1 = get("rule", "any") == "phase1";
    s.threshold = std::stod(get("threshold", "5"));
    s.stop_when_found = get("stop", "1") != "0";
    const std::string negative = get("negative", "zero");
    if (negative != "zero" && negative != "skip") {
        std::cerr << "error: negative is 'zero' or 'skip', not '" << negative << "'" << std::endl;
        return 1;
    }
    s.negative_to_zero = negative == "zero";
    s.quiet = get("quiet", "1") != "0";
    s.step_timeout = std::stod(get("step_timeout", "20"));
    const std::vector<std::string> sup = List(get("suppress", "amphibole,biotite"));
    for (const auto &p : mi.GetPhases())
        for (const auto &name : sup)
            if (p.second == name) s.suppressed.insert(p);
    const double h2o = std::stod(get("h2o", "13"));

    std::vector<std::string> names;
    std::vector<std::array<double, 20>> comps;
    std::vector<std::map<std::string, std::string>> conds; // conditions given in the file, row by row
    if (file == "--bishop" || file.empty()) {
        names.push_back("Bishop Tuff (MELTS_Excel default)");
        comps.push_back({74.39, 0.180, 13.55, 0.36, 0.000, 0.976, 0.00, 0.5, 0.0, 0.0, 1.43, 3.36, 5.09, 0.00, 10.0, 0.00, 0.00, 0.00, 0.00, 0.00});
    } else {
        std::string err;
        if (!Geobarometer::ReadCompositions(file, h2o, names, comps, err, &conds)) {
            std::cerr << "error: " << err << std::endl;
            return 1;
        }
    }

    conds.resize(comps.size());
    std::vector<GeobarometerRun> all;
    for (size_t i = 0; i < comps.size(); ++i) {
        GeobarometerSettings c = s;
        c.composition = comps[i];
        std::string err;
        if (!Geobarometer::ApplyConditions(conds[i], c, err)) {
            std::cerr << "error: " << names[i] << ": " << err << std::endl;
            return 1;
        }
        auto runs = Geobarometer::Run(c, names[i]);
        Geobarometer::AppendSummaryCSV(get("out", "geobarometer") + "_summary.csv", runs, i == 0); // kept as the batch goes
        for (const auto &r : runs) {
            char p3[32] = "-", p2[32] = "-";
            if (r.fit3.estimated) std::snprintf(p3, sizeof p3, "%.1f", r.fit3.p_est);
            if (r.fit2.estimated) std::snprintf(p2, sizeof p2, "%.1f", r.fit2.p_est);
            std::fprintf(stderr, "%-30s offset %+5.2f  P3 %7s (min %6.2f C at %5.0f MPa)  P2 %7s (min %6.2f C, %s)  %d eq\n",
                         r.sample.c_str(), r.fo2_offset, p3, r.fit3.min_residual, r.fit3.p_at_min,
                         p2, r.fit2.min_residual, r.fit2.phases_at_min.c_str(), r.equilibrations);
            all.push_back(r);
        }
    }
    const std::string out = get("out", "geobarometer");
    Geobarometer::WriteSummaryCSV(out + "_summary.csv", s, all);
    Geobarometer::WriteDetailCSV(out + "_detail.csv", s, all);
    if (xlsx && !workbook(out)) return 1;
    return 0;
}
