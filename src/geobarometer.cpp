/*
 easyMelts geobarometer (c) 2026 Eric C. P. Breard. Free software under the GNU General Public License v3 (see LICENSE.txt).

 easyMelts geobarometer, see geobarometer.hpp.

 The procedure mirrors MELTS_Excel (Call_MELTS "Draw_Phase_Diagram" + Estimate_Pressure):
  - one isobaric path per pressure, each on a freshly initialised system;
  - a wet-liquidus search first, then the path starts at the grid temperature
    T_start - Int((T_start - T_liquidus) / dT) * dT and cools in steps of dT;
  - the path ends when the liquid falls below 10 % of the system mass or on the first failure;
  - saturation temperature = highest path temperature at which the phase is present;
  - delta_3 = max - min of the three saturation temperatures (all three present);
  - delta_2 = highest minus second highest ("any two phases") or |T1 - max(T2, T3)|
    ("require phase 1"), with an absent phase counted as 0 as the spreadsheet does;
  - parabola through the first minimum and up to two points each side, pressure = vertex,
    only when the minimum is <= threshold and the parabola opens upwards.
*/

#include "geobarometer.hpp"
#include "melts_interface.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

#include <gsl/gsl_errno.h>

#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#define GB_NULL_DEVICE "NUL"
#else
#include <unistd.h>
#define GB_NULL_DEVICE "/dev/null"
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include "status.h"
#ifdef __cplusplus
}
#endif

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// Guard against an equilibration that runs on: converged equilibrations take up to ~2,000 silmin()
// steps (0.1 s), but a rare one needs ~10^6 steps and a minute. MELTS_Excel waits 2 s and then 20 s
// for the web service before it gives up on a step, so a step is abandoned after step_timeout
// seconds (20 by default) or 2,000,000 silmin() steps.
const int kMaxSilminCalls = 2000000;
// silmin() argument that only resets its step machine (see silmin.c).
const int kSilminReset = -2;

// For the duration of a run: no melts.out or tables on every equilibration (thousands of them),
// and, when quiet, the MELTS console chatter goes to the null device.
class RunGuard {
public:
    explicit RunGuard(bool quiet) : m_skip(skipOutputFiles), m_gsl(gsl_set_error_handler_off()) {
        silmin(kSilminReset); // a stopped equilibration in the main tab may have left silmin() part way
        skipOutputFiles = 1;
        if (!quiet) return;
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        int nul = open(GB_NULL_DEVICE, O_WRONLY);
        if (nul < 0) return;
        m_out = dup(1);
        m_err = dup(2);
        dup2(nul, 1);
        dup2(nul, 2);
        close(nul);
    }
    ~RunGuard() {
        skipOutputFiles = m_skip;
        gsl_set_error_handler(m_gsl);
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        if (m_out >= 0) {
            dup2(m_out, 1);
            close(m_out);
        }
        if (m_err >= 0) {
            dup2(m_err, 2);
            close(m_err);
        }
    }

private:
    int m_skip;
    gsl_error_handler_t *m_gsl;
    int m_out = -1;
    int m_err = -1;
};

struct PathResult {
    double liquidus = kNaN;
    int steps = 0;
    bool cancelled = false;
    std::map<std::string, double> tsat; // highest temperature at which each name was present
    std::string note;
};

void Record(std::map<std::string, double> &m, const std::string &name, double t) {
    if (m.find(name) == m.end()) m[name] = t; // paths cool, so the first hit is the highest temperature
}

void AddNote(std::string &note, const std::string &text) {
    if (!note.empty()) note += "; ";
    note += text;
}

std::string Fmt(double v, int prec = 1) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}

double LiquidMass(const SilminState *st) {
    double mass = 0.0;
    for (int nl = 0; nl < st->nLiquidCoexist; ++nl)
        for (int i = 0; i < nc; ++i) {
            double moles = 0.0;
            for (int j = 0; j < nlc; ++j) moles += st->liquidComp[nl][j] * liquid[j].liqToOx[i];
            mass += moles * bulkSystem[i].mw;
        }
    return mass;
}

double SolidMass(const SilminState *st) {
    double mass = 0.0;
    for (int j = 0; j < npc; ++j) {
        if (solids[j].type != PHASE) continue;
        for (int ns = 0; ns < st->nSolidCoexist[j]; ++ns) {
            if (solids[j].na == 1)
                mass += st->solidComp[j][ns] * solids[j].mw;
            else
                for (int i = 0; i < solids[j].na; ++i) mass += st->solidComp[j + 1 + i][ns] * solids[j + 1 + i].mw;
        }
    }
    return mass;
}

// Records every phase present at temperature t. Coexisting instances are numbered in the order
// MELTS lists them, as MELTS_Excel does when it splits a phase sheet into phase1 and phase2.
void RecordPhases(const SilminState *st, double t, std::map<std::string, double> &tsat) {
    for (int j = 0; j < npc; ++j) {
        if (solids[j].type != PHASE) continue;
        int n = st->nSolidCoexist[j];
        if (n <= 0) continue;
        std::string label(solids[j].label);
        Record(tsat, label, t);
        if (solids[j].na > 1) {
            Record(tsat, label + "1", t);
            if (n >= 2) Record(tsat, label + "2", t);
        }
        if (label == "feldspar" && solids[j].na > 1) {
            int isan = -1;
            for (int i = 0; i < solids[j].na; ++i)
                if (!strcmp(solids[j + 1 + i].label, "sanidine")) isan = i;
            if (isan < 0) continue;
            for (int ns = 0; ns < n; ++ns) {
                double total = 0.0;
                for (int i = 0; i < solids[j].na; ++i) total += st->solidComp[j + 1 + i][ns];
                double xsan = total > 0.0 ? st->solidComp[j + 1 + isan][ns] / total : 0.0;
                Record(tsat, xsan < 0.25 ? "plagioclase" : "sanidine", t);
            }
        }
    }
}

void Setup(MeltsInterface &mi, const GeobarometerSettings &s, double p_bar, double offset, double t) {
    mi.CreateSilminState();
    mi.SetComposition(s.composition);
    mi.SetFO2Path(s.fo2_path);
    if (s.fo2_path != FO2_NONE) mi.SetFO2Offset(offset);
    mi.SetSuppressedPhases(s.suppressed);
    mi.SetInitialTP(t, p_bar);
    mi.SetCalcSteps(0);
}

// One equilibration at the temperature already set in the state. Returns false on failure.
bool EquilibrateOnce(SilminState *st, int &calc_index, double max_seconds, std::atomic<bool> *cancel) {
    silminState = st;
    meltsStatus.status = GENERIC_INTERNAL_ERROR;
    auto t0 = std::chrono::steady_clock::now();
    int calls = 0;
    int done = 0;
    while (!done) {
        done = silmin(calc_index);
        ++calc_index;
        if (calc_index == INT_MAX) calc_index = 1;
        if (++calls > kMaxSilminCalls) break;
        if ((calls & 63) == 0) {
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (sec > max_seconds || (cancel && cancel->load())) break;
        }
    }
    if (!done) silmin(kSilminReset); // abandoned part way: the next call must start afresh
    return done && meltsStatus.status == SILMIN_SUCCESS;
}

PathResult RunPath(const GeobarometerSettings &s, double p_mpa, double offset, std::atomic<bool> *cancel) {
    PathResult out;
    const double p_bar = p_mpa * 10.0;
    const double t1 = std::max(s.t_start, s.t_end);
    const double t2 = std::min(s.t_start, s.t_end);
    const double dt = std::fabs(s.t_step) > 0.0 ? std::fabs(s.t_step) : 1.0;

    // Wet liquidus on its own system, as MELTS_Excel does with a separate request.
    {
        MeltsInterface mi;
        Setup(mi, s, p_bar, offset, t1);
        mi.WetLiquidus();
        out.liquidus = mi.GetLiquidusT();
    }

    double k0 = 0.0;
    if (std::isfinite(out.liquidus)) {
        if (out.liquidus > t1) {
            k0 = -std::ceil((out.liquidus - t1) / dt);
            AddNote(out.note, "wet liquidus " + Fmt(out.liquidus) + " C is above T start");
        } else {
            k0 = std::floor((t1 - out.liquidus) / dt);
        }
    } else {
        AddNote(out.note, "wet liquidus not found, path starts at T start");
    }

    MeltsInterface mi;
    Setup(mi, s, p_bar, offset, t1 - k0 * dt);
    SilminState *st = mi.GetSilminState();
    int calc_index = 0;

    for (int k = 0;; ++k) {
        const double t = t1 - (k0 + k) * dt;
        if (t < t2 - 1e-9) break;
        if (cancel && cancel->load()) {
            out.cancelled = true;
            break;
        }
        mi.SetInitialTP(t, p_bar);
        const auto t_step = std::chrono::steady_clock::now();
        if (!EquilibrateOnce(st, calc_index, s.step_timeout, cancel)) {
            if (cancel && cancel->load()) {
                out.cancelled = true;
                break;
            }
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_step).count();
            AddNote(out.note, (sec >= s.step_timeout ? "MELTS did not converge within " + Fmt(s.step_timeout, 0) + " s at "
                                                      : std::string("MELTS failed at ")) + Fmt(t) + " C, path stopped");
            break;
        }
        ++out.steps;

        const bool had_solids = !out.tsat.empty();
        RecordPhases(st, t, out.tsat);
        if (k == 0 && !had_solids) {
            for (const auto &name : s.phases)
                if (out.tsat.count(name))
                    AddNote(out.note, name + " already present at the first step (" + Fmt(t) + " C)");
        }

        const double ml = LiquidMass(st);
        const double ms = SolidMass(st);
        if (ml + ms > 0.0 && ml / (ml + ms) < s.min_liquid_fraction) break;

        if (s.stop_when_found) {
            bool all = true;
            for (const auto &name : s.phases)
                if (!name.empty() && !out.tsat.count(name)) all = false;
            if (all) break;
        }
    }
    silminState = nullptr;
    return out;
}

double Value(const std::array<double, 3> &v, int i) {
    return std::isfinite(v[i]) ? v[i] : 0.0;
}

} // namespace

namespace Geobarometer {

std::vector<double> PressureGrid(const GeobarometerSettings &s) {
    const double p1 = std::max(s.p_start, s.p_end);
    const double p2 = std::min(s.p_start, s.p_end);
    const double dp = std::fabs(s.p_step) > 0.0 ? std::fabs(s.p_step) : 1.0;
    const int n = (int)std::floor((p1 - p2) / dp + 1e-9) + 1;
    std::vector<double> p;
    for (int j = 0; j < n; ++j) p.push_back(p1 - j * dp);
    return p;
}

std::vector<GeobarometerRun> Run(const GeobarometerSettings &s, const std::string &sample,
                                 std::atomic<int> *pressures_done, std::atomic<bool> *cancel) {
    RunGuard guard(s.quiet);
    std::vector<GeobarometerRun> runs;
    const std::vector<double> grid = PressureGrid(s);
    std::vector<double> offsets = s.fo2_offsets;
    if (offsets.empty() || s.fo2_path == FO2_NONE) offsets = {0.0};

    // Compositions MELTS cannot calculate get a row that says why. A negative amount of an oxide (a
    // Monte Carlo draw below zero, for example) gives a liquid component a negative mole fraction, and
    // no wet liquidus is found. An fO2 buffer acts on FeO and Fe2O3; with neither, MELTS writes past the
    // end of its constraint matrix (getEqualityConstraints).
    static const char *oxides[] = {"SiO2", "TiO2", "Al2O3", "Fe2O3", "Cr2O3", "FeO", "MnO", "MgO", "NiO", "CoO",
                                   "CaO", "Na2O", "K2O", "P2O5", "H2O", "CO2", "SO3", "Cl2O-1", "F2O-1"};
    std::string refused;
    for (int i = 0; i < 19; ++i)
        if (s.composition[i] < 0.0)
            refused += std::string(refused.empty() ? "negative " : ", ") + oxides[i] + " (" + Fmt(s.composition[i], 3) + ")";
    if (!refused.empty()) refused += " in the composition, not calculated";
    else if (s.fo2_path != FO2_NONE && !(s.composition[3] > 0.0) && !(s.composition[5] > 0.0))
        refused = "no FeO or Fe2O3: an fO2 buffer cannot be applied (choose buffer none), not calculated";
    if (!refused.empty()) {
        for (double offset : offsets) {
            GeobarometerRun run;
            run.sample = sample;
            run.fo2_offset = offset;
            run.settings = s;
            run.message = refused;
            Evaluate(s, run);
            runs.push_back(run);
        }
        return runs;
    }

    for (double offset : offsets) {
        GeobarometerRun run;
        run.sample = sample;
        run.fo2_offset = offset;
        std::vector<std::map<std::string, double>> seen;
        std::string first_failure; // note of the first pressure that gave no result

        for (double p : grid) {
            if (cancel && cancel->load()) {
                run.cancelled = true;
                break;
            }
            PathResult pr = RunPath(s, p, offset, cancel);
            if (pressures_done) ++(*pressures_done);
            run.equilibrations += pr.steps;
            if (pr.cancelled) run.cancelled = true;
            if (pr.steps == 0) { // MELTS_Excel keeps only pressures that returned results
                if (first_failure.empty()) first_failure = Fmt(p, 0) + " MPa: " + (pr.note.empty() ? std::string("path starts below T end") : pr.note);
                continue;
            }
            run.pressure.push_back(p);
            run.liquidus.push_back(pr.liquidus);
            std::array<double, 3> t{{kNaN, kNaN, kNaN}};
            for (int i = 0; i < 3; ++i) {
                auto it = pr.tsat.find(s.phases[i]);
                if (it != pr.tsat.end()) t[i] = it->second;
            }
            run.tsat.push_back(t);
            run.note.push_back(pr.note);
            seen.push_back(pr.tsat);
            if (run.cancelled) break;
        }

        std::set<std::string> names;
        for (const auto &m : seen)
            for (const auto &kv : m) names.insert(kv.first);
        for (const auto &name : names) {
            std::vector<double> col;
            for (const auto &m : seen) {
                auto it = m.find(name);
                col.push_back(it == m.end() ? kNaN : it->second);
            }
            run.all_tsat[name] = col;
        }

        if (run.pressure.empty() && !run.cancelled)
            run.message = "no result at any pressure" + (first_failure.empty() ? std::string() : " (first, " + first_failure + ")");
        run.settings = s;
        Evaluate(s, run);
        runs.push_back(run);
        if (run.cancelled) break;
    }
    return runs;
}

void Evaluate(const GeobarometerSettings &s, GeobarometerRun &run) {
    const size_t n = run.pressure.size();
    run.delta3.assign(n, kNaN);
    run.delta2.assign(n, kNaN);
    for (size_t i = 0; i < n; ++i) {
        const auto &v = run.tsat[i];
        if (std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]))
            run.delta3[i] = std::max({v[0], v[1], v[2]}) - std::min({v[0], v[1], v[2]});
        if (s.require_phase1) {
            run.delta2[i] = std::fabs(Value(v, 0) - std::max(Value(v, 1), Value(v, 2)));
        } else {
            std::array<double, 3> z{{Value(v, 0), Value(v, 1), Value(v, 2)}};
            std::sort(z.begin(), z.end(), std::greater<double>());
            if (z[0] != 0.0 && z[1] != 0.0) run.delta2[i] = z[0] - z[1];
        }
    }

    run.fit3 = FitResidual(run.pressure, run.delta3, s.threshold);
    run.fit2 = FitResidual(run.pressure, run.delta2, s.threshold);
    if (run.fit3.index_at_min >= 0)
        run.fit3.phases_at_min = s.phases[0] + " + " + s.phases[1] + " + " + s.phases[2];
    if (run.fit2.index_at_min >= 0) {
        const auto &v = run.tsat[run.fit2.index_at_min];
        if (s.require_phase1) {
            int other = Value(v, 1) >= Value(v, 2) ? 1 : 2;
            run.fit2.phases_at_min = s.phases[0] + " + " + s.phases[other];
        } else {
            std::array<int, 3> idx{{0, 1, 2}};
            std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return Value(v, a) > Value(v, b); });
            int first = std::min(idx[0], idx[1]), second = std::max(idx[0], idx[1]);
            run.fit2.phases_at_min = s.phases[first] + " + " + s.phases[second];
        }
    }
}

GeobarometerFit FitResidual(const std::vector<double> &p, const std::vector<double> &r, double threshold) {
    GeobarometerFit f;
    const int n = (int)std::min(p.size(), r.size());
    double best = std::numeric_limits<double>::infinity();
    for (int i = 0; i < n; ++i)
        if (std::isfinite(r[i]) && r[i] < best) {
            best = r[i];
            f.index_at_min = i;
        }
    if (f.index_at_min < 0) return f;
    f.min_residual = best;
    f.p_at_min = p[f.index_at_min];
    if (!(best <= threshold)) return f;

    const int lo = std::max(f.index_at_min - 2, 0);
    const int hi = std::min(f.index_at_min + 2, n - 1);
    for (int i = lo; i <= hi; ++i)
        if (!std::isfinite(r[i])) return f; // a gap in the window: the spreadsheet fit fails too
    f.n_fit = hi - lo + 1;
    if (f.n_fit < 3) return f;
    f.index_lo = lo;
    f.index_hi = hi;

    // Least squares y = A u^2 + B u + C on centred, scaled pressures u = (P - m) / h
    double m = 0.0;
    for (int i = lo; i <= hi; ++i) m += p[i];
    m /= f.n_fit;
    double h = 0.0;
    for (int i = lo; i <= hi; ++i) h = std::max(h, std::fabs(p[i] - m));
    if (h == 0.0) return f;
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, t0 = 0, t1 = 0, t2 = 0;
    for (int i = lo; i <= hi; ++i) {
        const double u = (p[i] - m) / h, y = r[i];
        s0 += 1;
        s1 += u;
        s2 += u * u;
        s3 += u * u * u;
        s4 += u * u * u * u;
        t0 += y;
        t1 += u * y;
        t2 += u * u * y;
    }
    // Normal equations [s4 s3 s2; s3 s2 s1; s2 s1 s0] [A B C] = [t2 t1 t0], by Cramer's rule
    auto det3 = [](double a, double b, double c, double d, double e, double g, double k, double l, double q) {
        return a * (e * q - g * l) - b * (d * q - g * k) + c * (d * l - e * k);
    };
    const double D = det3(s4, s3, s2, s3, s2, s1, s2, s1, s0);
    if (std::fabs(D) < 1e-12) return f;
    const double A = det3(t2, s3, s2, t1, s2, s1, t0, s1, s0) / D;
    const double B = det3(s4, t2, s2, s3, t1, s1, s2, t0, s0) / D;
    const double C = det3(s4, s3, t2, s3, s2, t1, s2, s1, t0) / D;

    f.a = A / (h * h);
    f.b = B / h - 2.0 * A * m / (h * h);
    f.c = C - B * m / h + A * m * m / (h * h);
    if (f.a > 0.0) {
        f.p_est = m - B * h / (2.0 * A);
        f.residual_at_p_est = C - B * B / (4.0 * A);
        f.estimated = true;
    }
    return f;
}

std::vector<std::string> PhaseChoices() {
    std::vector<std::string> first{"quartz", "feldspar", "feldspar1", "feldspar2", "plagioclase", "sanidine",
                                   "orthopyroxene", "clinopyroxene", "spinel", "rhm-oxide"};
    std::vector<std::string> out;
    std::set<std::string> available;
    for (int j = 0; j < npc; ++j)
        if (solids[j].type == PHASE) available.insert(solids[j].label);
    for (const auto &name : first) {
        std::string base = name;
        if (name == "feldspar1" || name == "feldspar2" || name == "plagioclase" || name == "sanidine") base = "feldspar";
        if (available.count(base)) out.push_back(name);
    }
    for (const auto &name : available) {
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    for (int j = 0; j < npc; ++j)
        if (solids[j].type == PHASE && solids[j].na > 1) {
            std::string label(solids[j].label);
            if (label == "feldspar") continue;
            out.push_back(label + "1");
            out.push_back(label + "2");
        }
    return out;
}

namespace {

std::string Trim(const std::string &x) {
    size_t a = x.find_first_not_of(" \t\r\n\"");
    size_t b = x.find_last_not_of(" \t\r\n\"");
    return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
}

std::vector<std::string> Split(const std::string &line, char sep) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') quoted = !quoted;
        else if (c == sep && !quoted) {
            out.push_back(Trim(cur));
            cur.clear();
        } else cur += c;
    }
    out.push_back(Trim(cur));
    return out;
}

int OxideIndex(std::string h) {
    std::string k;
    for (char c : h)
        if (c != ' ' && c != '_') k += (char)std::tolower((unsigned char)c);
    static const std::vector<std::string> ox{"sio2", "tio2", "al2o3", "fe2o3", "cr2o3", "feo", "mno", "mgo", "nio", "coo",
                                             "cao", "na2o", "k2o", "p2o5", "h2o", "co2", "so3", "cl2o-1", "f2o-1"};
    for (size_t i = 0; i < ox.size(); ++i)
        if (k == ox[i]) return (int)i;
    if (k == "feot" || k == "feo*" || k == "feotot" || k == "feo(t)" || k == "feototal") return 5;
    return -1;
}

// Canonical key of a condition column, or "" (letters and digits only, lower case, so that
// "P1 (MPa)", "p_start" and "P start" all match).
std::string ConditionKey(const std::string &h) {
    std::string k;
    for (char c : h)
        if (std::isalnum((unsigned char)c)) k += (char)std::tolower((unsigned char)c);
    static const std::map<std::string, std::string> keys{
        {"pstart", "p_start"}, {"pstartmpa", "p_start"}, {"p1", "p_start"}, {"p1mpa", "p_start"},
        {"pend", "p_end"}, {"pendmpa", "p_end"}, {"p2", "p_end"}, {"p2mpa", "p_end"},
        {"pstep", "p_step"}, {"pstepmpa", "p_step"},
        {"tstart", "t_start"}, {"tstartc", "t_start"}, {"t1", "t_start"}, {"t1c", "t_start"},
        {"tend", "t_end"}, {"tendc", "t_end"}, {"t2", "t_end"}, {"t2c", "t_end"},
        {"tstep", "t_step"}, {"tstepc", "t_step"},
        {"buffer", "buffer"}, {"fo2buffer", "buffer"},
        {"offset", "offsets"}, {"offsets", "offsets"}, {"fo2offset", "offsets"}, {"fo2offsets", "offsets"},
        {"fo2value", "offsets"},
        {"phase1", "phase1"}, {"phase2", "phase2"}, {"phase3", "phase3"},
        {"rule", "rule"}, {"twophaserule", "rule"}, {"twophaseresidual", "rule"}, {"formula", "rule"},
        {"threshold", "threshold"}, {"thresholdc", "threshold"}, {"residualthreshold", "threshold"},
        {"residualthresholdc", "threshold"}};
    auto it = keys.find(k);
    return it == keys.end() ? std::string() : it->second;
}

// A minus sign typed or pasted as U+2212 becomes '-'.
std::string AsciiMinus(std::string v) {
    for (size_t i = v.find("\xE2\x88\x92"); i != std::string::npos; i = v.find("\xE2\x88\x92", i)) v.replace(i, 3, "-");
    return v;
}

std::string Csv(double v, int prec = 2) {
    if (!std::isfinite(v)) return "";
    return Fmt(v, prec);
}

// Parabola coefficients need significant digits rather than decimals (a is of order 1e-4 C/MPa^2).
std::string CsvSig(double v, int digits = 10) {
    if (!std::isfinite(v)) return "";
    std::ostringstream o;
    o << std::setprecision(digits) << v;
    return o.str();
}

std::string Quote(const std::string &s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) {
        if (c == '"') q += '"';
        q += c;
    }
    return q + "\"";
}

} // namespace

const std::vector<std::string> &BufferNames() {
    static const std::vector<std::string> names{"none", "HM", "NNO", "QFM", "COH", "IW"};
    return names;
}

int BufferIndex(const std::string &name) {
    std::string k;
    for (char c : name)
        if (!std::isspace((unsigned char)c)) k += (char)std::toupper((unsigned char)c);
    if (k == "FMQ") k = "QFM";
    for (size_t i = 0; i < BufferNames().size(); ++i) {
        std::string b = BufferNames()[i];
        for (char &c : b) c = (char)std::toupper((unsigned char)c);
        if (k == b) return (int)i;
    }
    return -1;
}

bool ApplyConditions(const std::map<std::string, std::string> &c, GeobarometerSettings &s, std::string &error) {
    auto number = [&](const char *key, double &out) {
        auto it = c.find(key);
        if (it == c.end()) return true;
        const std::string v = AsciiMinus(it->second);
        char *end = nullptr;
        const double x = std::strtod(v.c_str(), &end);
        if (end == v.c_str() || !std::isfinite(x)) {
            error = std::string(key) + " '" + it->second + "' is not a number";
            return false;
        }
        out = x;
        return true;
    };
    if (!number("p_start", s.p_start) || !number("p_end", s.p_end) || !number("p_step", s.p_step) ||
        !number("t_start", s.t_start) || !number("t_end", s.t_end) || !number("t_step", s.t_step) ||
        !number("threshold", s.threshold))
        return false;
    if (s.p_step <= 0.0 || s.t_step <= 0.0) {
        error = "P_step and T_step must be positive";
        return false;
    }
    auto it = c.find("buffer");
    if (it != c.end()) {
        const int b = BufferIndex(it->second);
        if (b < 0) {
            error = "unknown fO2 buffer '" + it->second + "' (none, HM, NNO, QFM, COH or IW)";
            return false;
        }
        s.fo2_path = b;
    }
    it = c.find("offsets");
    if (it != c.end()) {
        std::string v = AsciiMinus(it->second);
        for (char &ch : v)
            if (ch == '|' || ch == ';' || ch == ',' || ch == '/') ch = ' ';
        std::istringstream in(v);
        std::vector<double> off;
        std::string item;
        while (in >> item) {
            char *end = nullptr;
            const double x = std::strtod(item.c_str(), &end);
            if (end == item.c_str() || *end != '\0') {
                error = "offsets '" + it->second + "' are not numbers";
                return false;
            }
            off.push_back(x);
        }
        if (!off.empty()) s.fo2_offsets = off;
    }
    const std::vector<std::string> choices = PhaseChoices();
    for (int k = 0; k < 3; ++k) {
        it = c.find("phase" + std::to_string(k + 1));
        if (it == c.end()) continue;
        std::string v;
        for (char ch : it->second) v += (char)std::tolower((unsigned char)ch);
        if (std::find(choices.begin(), choices.end(), v) == choices.end()) {
            error = "unknown phase '" + it->second + "'";
            return false;
        }
        s.phases[k] = v;
    }
    it = c.find("rule");
    if (it != c.end()) {
        std::string k;
        for (char ch : it->second)
            if (std::isalnum((unsigned char)ch)) k += (char)std::tolower((unsigned char)ch);
        if (k == "any" || k == "anytwo" || k == "anytwophases") s.require_phase1 = false;
        else if (k == "phase1" || k == "requirephase1") s.require_phase1 = true;
        else {
            error = "rule '" + it->second + "' is neither 'any' nor 'phase1'";
            return false;
        }
    }
    return true;
}

bool ReadCompositions(const std::string &path, double default_h2o, std::vector<std::string> &names,
                      std::vector<std::array<double, 20>> &comps, std::string &error,
                      std::vector<std::map<std::string, std::string>> *conditions) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::string header;
    if (!std::getline(in, header)) {
        error = "empty file";
        return false;
    }
    if (header.size() >= 3 && (unsigned char)header[0] == 0xEF) header = header.substr(3); // UTF-8 BOM
    char sep = ',';
    if (std::count(header.begin(), header.end(), ';') > std::count(header.begin(), header.end(), ',')) sep = ';';
    if (std::count(header.begin(), header.end(), '\t') > std::count(header.begin(), header.end(), sep)) sep = '\t';
    std::vector<std::string> cols = Split(header, sep);
    std::vector<int> map(cols.size(), -1);
    std::vector<std::string> ckey(cols.size());
    int name_col = -1;
    bool any = false;
    for (size_t i = 0; i < cols.size(); ++i) {
        map[i] = OxideIndex(cols[i]);
        if (map[i] >= 0) any = true;
        else if (!(ckey[i] = ConditionKey(cols[i])).empty()) continue;
        else if (name_col < 0) name_col = (int)i;
    }
    if (!any) {
        error = "no oxide columns found in the header (SiO2, TiO2, Al2O3, ...)";
        return false;
    }
    bool has_h2o = std::find(map.begin(), map.end(), 14) != map.end();
    std::string line;
    int row = 1;
    while (std::getline(in, line)) {
        ++row;
        if (Trim(line).empty()) continue;
        std::vector<std::string> f = Split(line, sep);
        std::array<double, 20> c{};
        bool h2o_set = false;
        double total = 0.0;
        for (size_t i = 0; i < f.size() && i < map.size(); ++i) {
            if (map[i] < 0 || f[i].empty()) continue;
            std::string v = f[i];
            if (sep == ';') std::replace(v.begin(), v.end(), ',', '.');
            char *end = nullptr;
            double x = std::strtod(v.c_str(), &end);
            if (end == v.c_str()) continue;
            c[map[i]] += x;
            total += x;
            if (map[i] == 14) h2o_set = true;
        }
        if (total <= 0.0) continue;
        if (!has_h2o || !h2o_set) c[14] = default_h2o;
        if (conditions) {
            std::map<std::string, std::string> cond;
            for (size_t i = 0; i < f.size() && i < ckey.size(); ++i) {
                if (ckey[i].empty() || f[i].empty()) continue;
                std::string v = f[i];
                if (sep == ';' && ckey[i].compare(0, 5, "phase") != 0 && ckey[i] != "buffer" && ckey[i] != "rule")
                    std::replace(v.begin(), v.end(), ',', '.'); // decimal commas
                cond[ckey[i]] = v;
            }
            conditions->push_back(cond);
        }
        std::string name = (name_col >= 0 && name_col < (int)f.size()) ? f[name_col] : std::string();
        if (name.empty()) name = "row " + std::to_string(row);
        names.push_back(name);
        comps.push_back(c);
    }
    if (comps.empty()) {
        error = "no composition rows found";
        return false;
    }
    return true;
}

namespace {

void SummaryHeader(std::ostream &o) {
    o << "sample,fO2_offset,P_3phase_MPa,min_dT_3phase_C,P_at_min_3phase_MPa,points_3phase,"
         "fit_P_min_3phase_MPa,fit_P_max_3phase_MPa,fit_a_3phase,fit_b_3phase,fit_c_3phase,"
         "P_2phase_MPa,min_dT_2phase_C,P_at_min_2phase_MPa,points_2phase,phases_2phase,"
         "fit_P_min_2phase_MPa,fit_P_max_2phase_MPa,fit_a_2phase,fit_b_2phase,fit_c_2phase,"
         "phase1,phase2,phase3,two_phase_rule,threshold_C,"
         "P_start_MPa,P_end_MPa,P_step_MPa,T_start_C,T_end_C,T_step_C,fO2_buffer,equilibrations,notes\n";
}

void SummaryRows(std::ostream &o, const std::vector<GeobarometerRun> &runs) {
    for (const auto &r : runs) {
        const GeobarometerSettings &rs = r.settings;
        std::string notes = r.message;
        for (size_t i = 0; i < r.note.size(); ++i)
            if (!r.note[i].empty()) notes += std::string(notes.empty() ? "" : " | ") + Fmt(r.pressure[i], 0) + " MPa: " + r.note[i];
        if (r.cancelled) notes = "stopped by user" + std::string(notes.empty() ? "" : " | ") + notes;
        if (r.fit2.estimated && r.fit2.index_lo >= 0) {
            const double lo = std::min(r.pressure[r.fit2.index_lo], r.pressure[r.fit2.index_hi]);
            const double hi = std::max(r.pressure[r.fit2.index_lo], r.pressure[r.fit2.index_hi]);
            if (r.fit2.p_est < lo || r.fit2.p_est > hi) notes += std::string(notes.empty() ? "" : " | ") + "2-phase vertex outside the fitted points";
        }
        // pressure range of the points each parabola was fitted to, and its coefficients
        // (residual in C = a P^2 + b P + c, P in MPa), so that every curve can be redrawn
        auto fit_columns = [&](const GeobarometerFit &f) {
            if (!std::isfinite(f.a) || f.index_lo < 0) return std::string(",,,,");
            const double p1 = r.pressure[f.index_lo], p2 = r.pressure[f.index_hi];
            return Csv(std::min(p1, p2), 1) + "," + Csv(std::max(p1, p2), 1) + "," + CsvSig(f.a) + "," + CsvSig(f.b) + "," + CsvSig(f.c);
        };
        o << Quote(r.sample) << "," << Csv(r.fo2_offset, 3) << ","
          << (r.fit3.estimated ? Csv(r.fit3.p_est, 1) : "") << "," << Csv(r.fit3.min_residual, 2) << ","
          << Csv(r.fit3.p_at_min, 1) << "," << r.fit3.n_fit << "," << fit_columns(r.fit3) << ","
          << (r.fit2.estimated ? Csv(r.fit2.p_est, 1) : "") << "," << Csv(r.fit2.min_residual, 2) << ","
          << Csv(r.fit2.p_at_min, 1) << "," << r.fit2.n_fit << "," << Quote(r.fit2.phases_at_min) << ","
          << fit_columns(r.fit2) << ","
          << Quote(rs.phases[0]) << "," << Quote(rs.phases[1]) << "," << Quote(rs.phases[2]) << ","
          << (rs.require_phase1 ? "require phase 1" : "any two phases") << "," << Csv(rs.threshold, 1) << ","
          << Csv(rs.p_start, 1) << "," << Csv(rs.p_end, 1) << "," << Csv(rs.p_step, 1) << "," << Csv(rs.t_start, 1) << ","
          << Csv(rs.t_end, 1) << "," << Csv(rs.t_step, 2) << ","
          << (rs.fo2_path >= 0 && rs.fo2_path < (int)BufferNames().size() ? BufferNames()[rs.fo2_path] : std::string()) << ","
          << r.equilibrations << "," << Quote(notes) << "\n";
    }
}

} // namespace

bool WriteSummaryCSV(const std::string &path, const GeobarometerSettings &s, const std::vector<GeobarometerRun> &runs) {
    (void)s; // each run carries its own settings
    std::ofstream o(path);
    if (!o) return false;
    SummaryHeader(o);
    SummaryRows(o, runs);
    return (bool)o;
}

bool MergeCSV(const std::vector<std::string> &parts, const std::string &path) {
    std::vector<std::string> cols;
    std::vector<std::vector<std::map<std::string, std::string>>> tables;
    for (const auto &part : parts) {
        std::ifstream in(part);
        std::string line;
        if (!in || !std::getline(in, line)) continue; // a part that wrote nothing
        const std::vector<std::string> head = Split(line, ',');
        for (const auto &c : head)
            if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
        std::vector<std::map<std::string, std::string>> rows;
        while (std::getline(in, line)) {
            if (Trim(line).empty()) continue;
            const std::vector<std::string> f = Split(line, ',');
            std::map<std::string, std::string> row;
            for (size_t i = 0; i < head.size() && i < f.size(); ++i) row[head[i]] = f[i];
            rows.push_back(row);
        }
        tables.push_back(rows);
    }
    for (const char *last : {"note", "notes"}) { // notes stay the last column
        auto it = std::find(cols.begin(), cols.end(), std::string(last));
        if (it != cols.end()) {
            cols.erase(it);
            cols.push_back(last);
        }
    }
    std::ofstream o(path);
    if (!o) return false;
    for (size_t i = 0; i < cols.size(); ++i) o << (i ? "," : "") << cols[i];
    o << "\n";
    for (const auto &rows : tables)
        for (const auto &row : rows) {
            for (size_t i = 0; i < cols.size(); ++i) {
                auto it = row.find(cols[i]);
                o << (i ? "," : "") << (it == row.end() ? std::string() : Quote(it->second));
            }
            o << "\n";
        }
    return (bool)o;
}

bool AppendSummaryCSV(const std::string &path, const std::vector<GeobarometerRun> &runs, bool header) {
    std::ofstream o(path, header ? std::ios::trunc : std::ios::app);
    if (!o) return false;
    if (header) SummaryHeader(o);
    SummaryRows(o, runs);
    return (bool)o;
}

bool WriteDetailCSV(const std::string &path, const GeobarometerSettings &s, const std::vector<GeobarometerRun> &runs) {
    std::ofstream o(path);
    if (!o) return false;
    std::set<std::string> names;
    for (const auto &r : runs)
        for (const auto &kv : r.all_tsat) names.insert(kv.first);
    // Columns named after the phases when every run used the same three; otherwise T_phase1..3,
    // with the phases of each row in their own columns.
    const std::array<std::string, 3> &ph = runs.empty() ? s.phases : runs.front().settings.phases;
    bool mixed = false;
    for (const auto &r : runs) mixed = mixed || r.settings.phases != ph;
    o << "sample,fO2_offset," << (mixed ? "phase1,phase2,phase3," : "") << "P_MPa,wet_liquidus_C,"
      << (mixed ? std::string("T_phase1,T_phase2,T_phase3") : "T_" + ph[0] + ",T_" + ph[1] + ",T_" + ph[2])
      << ",delta_3,delta_2,fit_3,fit_2";
    for (const auto &n : names) o << ",Tsat_" << n;
    o << ",note\n";
    for (const auto &r : runs) {
        for (size_t i = 0; i < r.pressure.size(); ++i) {
            const double p = r.pressure[i];
            auto fitv = [&](const GeobarometerFit &f) { // only at the pressures the parabola was fitted to
                return std::isfinite(f.a) && f.a > 0.0 && (int)i >= f.index_lo && (int)i <= f.index_hi
                           ? f.a * p * p + f.b * p + f.c : kNaN;
            };
            o << Quote(r.sample) << "," << Csv(r.fo2_offset, 3) << ",";
            if (mixed) o << Quote(r.settings.phases[0]) << "," << Quote(r.settings.phases[1]) << "," << Quote(r.settings.phases[2]) << ",";
            o << Csv(p, 1) << "," << Csv(r.liquidus[i], 2) << ","
              << Csv(r.tsat[i][0], 1) << "," << Csv(r.tsat[i][1], 1) << "," << Csv(r.tsat[i][2], 1) << ","
              << Csv(r.delta3[i], 2) << "," << Csv(r.delta2[i], 2) << "," << Csv(fitv(r.fit3), 2) << "," << Csv(fitv(r.fit2), 2);
            for (const auto &n : names) {
                auto it = r.all_tsat.find(n);
                o << "," << (it == r.all_tsat.end() ? "" : Csv(it->second[i], 1));
            }
            o << "," << Quote(r.note[i]) << "\n";
        }
    }
    return true;
}

} // namespace Geobarometer
