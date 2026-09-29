/*
 easyMelts geobarometer (c) 2026 Eric C. P. Breard. Free software under the GNU General Public License v3 (see LICENSE.txt).

 easyMelts geobarometer: phase-equilibrium pressures from glass compositions, following the
 "Qtz + Fspars P Calc" of MELTS_Excel (Gualda and Ghiorso 2014, 2015; Harmon et al. 2018).

 For every pressure of a grid the engine finds the fluid-saturated liquidus, cools the melt in
 fixed steps from the first grid temperature at or above it, and records the highest temperature
 at which each phase is present. Two residuals are formed at each pressure from the saturation
 temperatures of three chosen phases, and a parabola through the residual minimum and its two
 neighbours on each side gives the pressure, exactly as the P_Calc sheet of MELTS_Excel does.
*/

#ifndef GEOBAROMETER_HPP
#define GEOBAROMETER_HPP

#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <string>
#include <vector>

struct GeobarometerSettings {
    std::array<double, 20> composition{};   // grams of each oxide, easyMelts order (H2O included)
    double p_start = 500.0;                 // MPa; the grid runs from the larger to the smaller value
    double p_end = 25.0;
    double p_step = 25.0;
    double t_start = 1100.0;                // C
    double t_end = 700.0;
    double t_step = 1.0;
    int fo2_path = 2;                       // FO2_NNO
    std::vector<double> fo2_offsets{0.0};
    std::array<std::string, 3> phases{{"quartz", "feldspar1", "feldspar2"}};
    bool require_phase1 = false;            // two-phase residual |T1 - max(T2, T3)| instead of highest minus second highest
    double threshold = 5.0;                 // C
    double min_liquid_fraction = 0.1;       // a path stops once liquid falls below this mass fraction of the system
    bool stop_when_found = true;            // stop a path once the three phases have appeared (same result, faster)
    bool negative_to_zero = true;           // a negative oxide amount counts as 0; false: the composition is not calculated
    bool quiet = true;                      // silence the MELTS console output during the run
    double step_timeout = 20.0;             // s; a path stops at an equilibration that has not converged by then
    std::map<int, std::string> suppressed;  // MELTS phase index -> label
};

struct GeobarometerFit {
    bool estimated = false;
    double min_residual = std::numeric_limits<double>::quiet_NaN();
    double p_at_min = std::numeric_limits<double>::quiet_NaN();
    int index_at_min = -1;
    int n_fit = 0;
    int index_lo = -1, index_hi = -1; // the parabola was fitted to the run's pressures index_lo..index_hi
    double a = std::numeric_limits<double>::quiet_NaN(); // residual = a P^2 + b P + c, P in MPa
    double b = std::numeric_limits<double>::quiet_NaN();
    double c = std::numeric_limits<double>::quiet_NaN();
    double p_est = std::numeric_limits<double>::quiet_NaN();
    double residual_at_p_est = std::numeric_limits<double>::quiet_NaN();
    std::string phases_at_min; // phases whose saturation temperatures set the residual at the minimum
    std::vector<std::string> flags; // reasons to look at this pressure before using it (set when it is estimated)
};

struct GeobarometerRun {
    std::string sample;
    double fo2_offset = 0.0;
    std::vector<double> pressure;              // MPa, only pressures that returned at least one equilibrium
    std::vector<double> liquidus;              // wet liquidus (C), NaN where the search failed
    std::vector<std::array<double, 3>> tsat;   // saturation temperature of the three phases (C), NaN if absent
    std::vector<double> delta3, delta2;        // residuals (C), NaN where undefined
    std::vector<std::string> note;
    std::string message;                       // about the whole run (e.g. why it was not calculated)
    std::map<std::string, std::vector<double>> all_tsat; // every phase seen, NaN where absent
    GeobarometerFit fit3, fit2;
    int equilibrations = 0;
    bool cancelled = false;
    GeobarometerSettings settings; // what this run used, conditions of its batch row included
};

namespace Geobarometer {

// Pressures of the grid in calculation order (MPa, largest first).
std::vector<double> PressureGrid(const GeobarometerSettings &s);

// Runs one composition at every fO2 offset of the settings. MELTS must already be initialised
// (Init menu); the calibration in use is the one selected there. Safe to call from a worker
// thread as long as nothing else drives MELTS at the same time.
std::vector<GeobarometerRun> Run(const GeobarometerSettings &s, const std::string &sample,
                                 std::atomic<int> *pressures_done = nullptr,
                                 std::atomic<bool> *cancel = nullptr);

// Residuals and parabola fits from the saturation temperatures already in the run.
void Evaluate(const GeobarometerSettings &s, GeobarometerRun &run);

// MELTS_Excel parabola: least squares through the first grid minimum and up to two points on
// each side, reported only if the minimum is at or below the threshold and the parabola opens up.
GeobarometerFit FitResidual(const std::vector<double> &p, const std::vector<double> &r, double threshold);

// Phase names accepted in the settings: MELTS phases, their first and second coexisting
// instances (e.g. feldspar1, feldspar2), and plagioclase / sanidine split at 25 mol% sanidine.
std::vector<std::string> PhaseChoices();

// fO2 buffers in the order of the MELTS constants: none, HM, NNO, QFM, COH, IW.
const std::vector<std::string> &BufferNames();
int BufferIndex(const std::string &name); // case-insensitive, -1 if unknown

// Batch input: CSV with a header row of oxide names, an optional sample column and optional
// condition columns. FeOt, FeO* and FeOT are read as FeO. Missing H2O is set to default_h2o.
// Condition columns, each optional, with blank cells meaning "use the settings of the run":
//   P_start, P_end, P_step (MPa), T_start, T_end, T_step (C), buffer, offsets (one or more,
//   separated by spaces, | or ;), phase1, phase2, phase3, rule (any or phase1), threshold (C).
// The labels of MELTS_Excel work too: P1 (MPa), P2 (MPa), P step (MPa), T1 (C), T2 (C),
// T step (C), fO2 buffer, fO2 value, Phase 1, Phase 2, Phase 3, Formula.
bool ReadCompositions(const std::string &path, double default_h2o,
                      std::vector<std::string> &names, std::vector<std::array<double, 20>> &comps,
                      std::string &error, std::vector<std::map<std::string, std::string>> *conditions = nullptr);

// Applies the condition cells of one batch row (keys as read by ReadCompositions) to settings s.
// MELTS must be initialised, because phase names are checked against the phases it knows.
bool ApplyConditions(const std::map<std::string, std::string> &conditions, GeobarometerSettings &s, std::string &error);

bool WriteSummaryCSV(const std::string &path, const GeobarometerSettings &s, const std::vector<GeobarometerRun> &runs);
// Adds the summary rows of runs to a file, with the header first if header is true, so that a long
// batch can keep its results on disk as it goes.
bool AppendSummaryCSV(const std::string &path, const std::vector<GeobarometerRun> &runs, bool header);

// Joins CSV files written by the functions above (parts of one batch) into one file, in order. Their
// columns are united by name (a detail file lists only the phases its runs met); notes stay last.
bool MergeCSV(const std::vector<std::string> &parts, const std::string &path);

// Excel workbook from a summary and detail pair (geobarometer_xlsx.cpp): a Viewer sheet where a run is
// picked from a list and its results and two charts follow, plus the Summary and Detail tables.
bool WriteWorkbook(const std::string &summary_csv, const std::string &detail_csv, const std::string &xlsx, std::string &error);
bool WriteDetailCSV(const std::string &path, const GeobarometerSettings &s, const std::vector<GeobarometerRun> &runs);

} // namespace Geobarometer

#endif /* GEOBAROMETER_HPP */
