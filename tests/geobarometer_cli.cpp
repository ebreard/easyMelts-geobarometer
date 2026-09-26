/*
 easyMelts geobarometer (c) 2026 Eric C. P. Breard. Free software under the GNU General Public License v3 (see LICENSE.txt).

 Command-line driver for the easyMelts geobarometer (batch runs and tests).

   geobarometer_cli [key=value ...] compositions.csv
   geobarometer_cli [key=value ...] --bishop

 Keys: version (1.0.2, 1.1.0, 1.2.0), p_start, p_end, p_step (MPa), t_start, t_end, t_step (C),
       buffer (none, hm, nno, qfm, coh, iw), offsets (comma list), phases (comma list of three),
       rule (any|phase1), threshold (C), h2o (g added when the file has no H2O),
       suppress (comma list, default amphibole,biotite), stop (1|0), out (file prefix)
*/

#include "geobarometer.hpp"
#include "melts_interface.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>

static std::vector<std::string> List(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) out.push_back(item);
    return out;
}

int main(int argc, char **argv) {
    std::map<std::string, std::string> kv;
    std::string file;
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        size_t eq = a.find('=');
        if (eq != std::string::npos) kv[a.substr(0, eq)] = a.substr(eq + 1);
        else file = a;
    }
    auto get = [&](const char *k, const std::string &d) { return kv.count(k) ? kv[k] : d; };

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
    const std::string buffer = get("buffer", "nno");
    const std::vector<std::string> buffers{"none", "hm", "nno", "qfm", "coh", "iw"};
    for (size_t i = 0; i < buffers.size(); ++i)
        if (buffer == buffers[i]) s.fo2_path = (int)i;
    s.fo2_offsets.clear();
    for (const auto &o : List(get("offsets", "0"))) s.fo2_offsets.push_back(std::stod(o));
    std::vector<std::string> ph = List(get("phases", "quartz,feldspar1,feldspar2"));
    for (int i = 0; i < 3 && i < (int)ph.size(); ++i) s.phases[i] = ph[i];
    s.require_phase1 = get("rule", "any") == "phase1";
    s.threshold = std::stod(get("threshold", "5"));
    s.stop_when_found = get("stop", "1") != "0";
    s.quiet = get("quiet", "1") != "0";
    s.step_timeout = std::stod(get("step_timeout", "20"));
    const std::vector<std::string> sup = List(get("suppress", "amphibole,biotite"));
    for (const auto &p : mi.GetPhases())
        for (const auto &name : sup)
            if (p.second == name) s.suppressed.insert(p);
    const double h2o = std::stod(get("h2o", "13"));

    std::vector<std::string> names;
    std::vector<std::array<double, 20>> comps;
    if (file == "--bishop" || file.empty()) {
        names.push_back("Bishop Tuff (MELTS_Excel default)");
        comps.push_back({74.39, 0.180, 13.55, 0.36, 0.000, 0.976, 0.00, 0.5, 0.0, 0.0, 1.43, 3.36, 5.09, 0.00, 10.0, 0.00, 0.00, 0.00, 0.00, 0.00});
    } else {
        std::string err;
        if (!Geobarometer::ReadCompositions(file, h2o, names, comps, err)) {
            std::cerr << "error: " << err << std::endl;
            return 1;
        }
    }

    std::vector<GeobarometerRun> all;
    for (size_t i = 0; i < comps.size(); ++i) {
        s.composition = comps[i];
        auto runs = Geobarometer::Run(s, names[i]);
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
    return 0;
}
