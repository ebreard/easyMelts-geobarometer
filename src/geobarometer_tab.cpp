/*
 easyMelts geobarometer (c) 2026 Eric C. P. Breard. Free software under the GNU General Public License v3 (see LICENSE.txt).

 easyMelts "Geobarometer" tab: GUI for the phase-equilibrium geobarometer (geobarometer.hpp).
*/

#include "imgui_opengl.hpp"

#include "imgui/imgui.h"
#include "imgui/implot.h"

#include "file_utility.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace {

void Help(const char *text) {
    ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1.f), "(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool StringCombo(const char *label, std::string &value, const std::vector<std::string> &choices) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.c_str())) {
        for (const auto &c : choices) {
            const bool selected = (c == value);
            if (ImGui::Selectable(c.c_str(), selected)) {
                value = c;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::vector<double> ParseList(const char *text) {
    std::vector<double> out;
    std::string s(text);
    for (char &c : s)
        if (c == ';' || c == ' ') c = ',';
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        char *end = nullptr;
        double v = std::strtod(item.c_str(), &end);
        if (end != item.c_str()) out.push_back(v);
    }
    return out;
}

std::string Num(double v, const char *fmt = "%.0f") {
    if (!std::isfinite(v)) return "-";
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, v);
    return buf;
}

// Finite points of a series as float arrays for ImPlot.
void Series(const std::vector<double> &x, const std::vector<double> &y, std::vector<float> &xs, std::vector<float> &ys) {
    xs.clear();
    ys.clear();
    for (size_t i = 0; i < x.size() && i < y.size(); ++i)
        if (std::isfinite(x[i]) && std::isfinite(y[i])) {
            xs.push_back((float)x[i]);
            ys.push_back((float)y[i]);
        }
}

void Parabola(const GeobarometerRun &r, const GeobarometerFit &f, std::vector<float> &xs, std::vector<float> &ys) {
    xs.clear();
    ys.clear();
    if (!(std::isfinite(f.a) && f.a > 0.0) || f.index_at_min < 0) return;
    const int n = (int)r.pressure.size();
    double lo = r.pressure[std::max(f.index_at_min - 2, 0)], hi = r.pressure[std::min(f.index_at_min + 2, n - 1)];
    if (lo > hi) std::swap(lo, hi);
    if (std::isfinite(f.p_est)) {
        lo = std::min(lo, f.p_est);
        hi = std::max(hi, f.p_est);
    }
    for (int i = 0; i <= 60; ++i) {
        const double p = lo + (hi - lo) * i / 60.0;
        xs.push_back((float)p);
        ys.push_back((float)(f.a * p * p + f.b * p + f.c));
    }
}

// Okabe-Ito colours, distinguishable with the common forms of colour blindness.
const ImVec4 kPhaseColor[3] = {ImVec4(0.34f, 0.71f, 0.91f, 1.f), ImVec4(0.84f, 0.37f, 0.f, 1.f), ImVec4(0.f, 0.62f, 0.45f, 1.f)};
const ImVec4 kResidualColor[4] = {ImVec4(0.34f, 0.71f, 0.91f, 1.f), ImVec4(0.84f, 0.37f, 0.f, 1.f),
                                  ImVec4(0.f, 0.45f, 0.70f, 1.f), ImVec4(0.90f, 0.62f, 0.f, 1.f)};

void KeyEntry(const char *label, const ImVec4 &color) {
    ImGui::ColorButton((std::string("##key") + label).c_str(), color,
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop, ImVec2(12.f, 12.f));
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
}

const char *kAbout =
    "For each pressure of the grid, easyMelts finds the fluid-saturated (wet) liquidus, cools the melt from the first "
    "grid temperature at or above it, and records the highest temperature at which each phase is present. "
    "delta_3 is the spread of the three saturation temperatures; delta_2 is the gap between the two highest "
    "('any two phases') or between phase 1 and the higher of phases 2 and 3 ('require phase 1'). A parabola through "
    "the smallest residual and up to two grid points on each side gives the pressure, reported only if that smallest "
    "residual is at or below the threshold. This is the 'Qtz + Fspars P Calc' of MELTS_Excel "
    "(Gualda & Ghiorso 2014, 2015; Harmon et al. 2018; Smithies et al., Part 5), same defaults: 500 to 25 MPa in 25 MPa "
    "steps, 1100 to 700 C in 1 C steps, amphibole and biotite suppressed, paths stop below 10 % melt.\n\n"
    "feldspar1 / feldspar2 are the first and second coexisting feldspars, as in MELTS_Excel; plagioclase / sanidine "
    "split feldspar at 25 mol% sanidine component.";

} // namespace

// Summary of every finished composition of the current run, written next to easyMelts as the run goes.
static const char *const kAutosave = "geobarometer_autosave_summary.csv";

void ImGuiOpenGL::GeobarometerTab(int melts_version) {
    static char out_buf[128] = "geobarometer";
    static bool suppressed_init = false;
    static bool refit_plots = true;
    static std::vector<std::string> csv_files;
    GeobarometerSettings &s = m_GbSettings;

    ImGui::BeginChild("GbLeft", ImVec2(ImGui::GetWindowContentRegionWidth() * 0.34f, 0));

    ImGui::TextWrapped("Pressure at which a melt (glass) composition is saturated in the chosen phases at its liquidus");
    ImGui::SameLine();
    Help(kAbout);

    if (!_MI.MeltsInitialized()) {
        ImGui::Dummy(ImVec2(0, 5.f));
        ImGui::TextColored(ImVec4(1.f, 0.5f, 0.1f, 1.f), "Initialise MELTS first: Init > Version > MELTS_v1.0.x");
        ImGui::EndChild();
        return;
    }
    const std::string version = melts_version >= 0 && melts_version < (int)_MI.GetMeltsVersions().size() ? _MI.GetMeltsVersions()[melts_version] : "?";
    ImGui::Text("Model: %s", version.c_str());
    if (melts_version != MODE__MELTS - 1) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.5f, 0.1f, 1.f), "(the published geobarometer uses MELTS_v1.0.x)");
    }

    if (!suppressed_init) { // MELTS_Excel default
        int count = 0;
        for (const auto &a : _MI.GetPhases()) {
            m_GbSuppressed[count] = (a.second == "amphibole" || a.second == "biotite");
            ++count;
        }
        suppressed_init = true;
    }

    const std::vector<std::string> choices = Geobarometer::PhaseChoices();
    const float w = ImGui::GetWindowContentRegionWidth();

    ImGui::Dummy(ImVec2(0, 5.f));
    ImGui::Separator();
    ImGui::Text("Melt composition");
    ImGui::RadioButton("Input/Output tab", &m_GbSource, 0);
    ImGui::SameLine();
    ImGui::RadioButton("CSV file (batch)", &m_GbSource, 1);
    if (m_GbSource == 0) {
        std::string line;
        int shown = 0;
        for (int i = 0; i < 16; ++i)
            if (m_Composition[i] > 0.0) {
                line += _MI.GetOxideNames()[i] + " " + Num(m_Composition[i], "%.2f") + ((++shown % 6) ? "   " : "\n");
            }
        if (!line.empty() && line.back() == '\n') line.pop_back();
        ImGui::TextUnformatted(line.c_str());
        ImGui::SameLine();
        Help("The composition entered in the Input/Output tab (grams, H2O included). For water saturation at every pressure "
             "add excess H2O, e.g. 13 g per 100 g of glass as in Ruefer et al. (2025). The fO2 buffer resets the FeO/Fe2O3 split.");
    } else {
        ImGui::PushItemWidth(w * 0.62f);
        ImGui::InputText("##csvpath", m_GbCsvPath, sizeof m_GbCsvPath);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            m_GbNames.clear();
            m_GbComps.clear();
            m_GbConds.clear();
            std::string err;
            if (Geobarometer::ReadCompositions(m_GbCsvPath, m_GbBatchH2O, m_GbNames, m_GbComps, err, &m_GbConds)) {
                int with = 0;
                std::string bad;
                for (size_t i = 0; i < m_GbConds.size(); ++i) {
                    if (m_GbConds[i].empty()) continue;
                    ++with;
                    GeobarometerSettings check = s;
                    if (bad.empty() && !Geobarometer::ApplyConditions(m_GbConds[i], check, err)) bad = m_GbNames[i] + ": " + err;
                }
                if (!bad.empty()) {
                    m_GbMessage = "Could not use the conditions of " + bad;
                    m_GbNames.clear();
                    m_GbComps.clear();
                    m_GbConds.clear();
                } else {
                    m_GbMessage = std::to_string(m_GbComps.size()) + " compositions loaded from " + std::string(m_GbCsvPath) +
                                  (with ? ", " + std::to_string(with) + " with their own conditions" : std::string());
                }
            } else {
                m_GbMessage = "Could not read the file: " + err;
            }
        }
        ImGui::SameLine();
        Help("CSV with a header row: an optional sample name column, then oxide columns named SiO2, TiO2, Al2O3, Fe2O3, "
             "FeO (or FeOt), MnO, MgO, CaO, Na2O, K2O, P2O5, H2O. Comma, semicolon or tab separated. Rows without H2O get "
             "the amount below. Files in the easyMelts folder are listed in the menu.\n"
             "Optional columns give a row its own conditions: P_start, P_end, P_step, T_start, T_end, T_step, buffer, "
             "offsets (e.g. -1 -0.5 0), phase1, phase2, phase3, rule (any or phase1), threshold. MELTS_Excel labels such as "
             "P1 (MPa), T1 (C), fO2 value and Phase 1 work too. Blank cells use the settings on this panel.");
        if (ImGui::BeginCombo("CSV in this folder", "", ImGuiComboFlags_NoPreview)) {
            if (ImGui::IsWindowAppearing()) {
                FileUtility fu;
                csv_files = fu.GetFilesWithName(".csv");
            }
            for (const auto &f : csv_files)
                if (ImGui::Selectable(f.c_str())) std::strncpy(m_GbCsvPath, f.c_str(), sizeof m_GbCsvPath - 1);
            ImGui::EndCombo();
        }
        ImGui::PushItemWidth(w * 0.3f);
        ImGui::InputDouble("H2O added when missing (g)", &m_GbBatchH2O, 0.5, 1.0, "%.2f");
        ImGui::PopItemWidth();
        int with = 0;
        for (const auto &c : m_GbConds) with += !c.empty();
        if (with) ImGui::Text("%d compositions ready, %d with their own conditions", (int)m_GbComps.size(), with);
        else ImGui::Text("%d compositions ready", (int)m_GbComps.size());
    }

    ImGui::Dummy(ImVec2(0, 5.f));
    ImGui::Separator();
    ImGui::PushItemWidth(w * 0.18f);
    ImGui::Text("Pressure (MPa)");
    ImGui::InputDouble("start##gbp", &s.p_start, 0.0, 0.0, "%.0f");
    ImGui::SameLine();
    ImGui::InputDouble("end##gbp", &s.p_end, 0.0, 0.0, "%.0f");
    ImGui::SameLine();
    ImGui::InputDouble("step##gbp", &s.p_step, 0.0, 0.0, "%.0f");
    ImGui::Text("Temperature (C)");
    ImGui::InputDouble("start##gbt", &s.t_start, 0.0, 0.0, "%.0f");
    ImGui::SameLine();
    ImGui::InputDouble("end##gbt", &s.t_end, 0.0, 0.0, "%.0f");
    ImGui::SameLine();
    ImGui::InputDouble("step##gbt", &s.t_step, 0.0, 0.0, "%.1f");
    ImGui::PopItemWidth();
    if (s.p_step <= 0.0) s.p_step = 25.0;
    if (s.t_step <= 0.0) s.t_step = 1.0;

    ImGui::Dummy(ImVec2(0, 5.f));
    ImGui::PushItemWidth(w * 0.35f);
    std::string buffer = _MI.GetFO2Paths().at(std::min(std::max(s.fo2_path, 0), (int)_MI.GetFO2Paths().size() - 1));
    if (StringCombo("fO2 buffer", buffer, _MI.GetFO2Paths()))
        for (size_t i = 0; i < _MI.GetFO2Paths().size(); ++i)
            if (_MI.GetFO2Paths()[i] == buffer) s.fo2_path = (int)i;
    ImGui::InputText("offsets (log units)", m_GbOffsets, sizeof m_GbOffsets);
    ImGui::SameLine();
    Help("One run per offset, e.g. -1, -0.75, -0.5, 0, 0.5. Quartz and feldspar hardly depend on fO2; pyroxenes and oxides do.");

    ImGui::Dummy(ImVec2(0, 5.f));
    ImGui::Text("Phases");
    StringCombo("phase 1", s.phases[0], choices);
    StringCombo("phase 2", s.phases[1], choices);
    StringCombo("phase 3", s.phases[2], choices);
    ImGui::PopItemWidth();
    ImGui::PushItemWidth(w * 0.5f);
    int rule = s.require_phase1 ? 1 : 0;
    if (ImGui::Combo("two-phase residual", &rule, "any two phases\0require phase 1\0\0")) s.require_phase1 = (rule == 1);
    ImGui::SameLine();
    Help("any two phases: highest minus second highest saturation temperature, so a two-phase pressure is only found where "
         "those two phases are the first to crystallise (MELTS_Excel default).\nrequire phase 1: |T(phase 1) - max(T(phase 2), T(phase 3))|.");
    ImGui::PopItemWidth();
    ImGui::PushItemWidth(w * 0.25f);
    ImGui::InputDouble("residual threshold (C)", &s.threshold, 1.0, 5.0, "%.1f");
    ImGui::InputDouble("stop path below melt fraction", &s.min_liquid_fraction, 0.01, 0.1, "%.2f");
    ImGui::PopItemWidth();
    ImGui::Checkbox("Stop each path once the three phases have appeared", &s.stop_when_found);
    ImGui::SameLine();
    Help("Saturation temperatures are the highest temperatures at which a phase is present, so the rest of the path cannot "
         "change them. Untick to follow every path down to the melt fraction limit (slower; fills the all-phase table).");

    if (ImGui::TreeNode("Suppressed phases")) {
        ImGui::Columns(3, NULL, false);
        int count = 0;
        for (const auto &a : _MI.GetPhases()) {
            ImGui::Selectable((a.second + "##gbsup").c_str(), &m_GbSuppressed[count]);
            ++count;
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
        ImGui::TreePop();
    }

    ImGui::Dummy(ImVec2(0, 10.f));
    const bool running = m_GbFuture.valid();
    if (!running && (ImGui::Button("Run geobarometer") || m_GbAutoRun)) {
        m_GbAutoRun = false;
        std::vector<std::array<double, 20>> comps;
        std::vector<std::string> names;
        std::vector<std::map<std::string, std::string>> conds;
        if (m_GbSource == 0) {
            comps.push_back(m_Composition);
            const std::string title = _MI.GetTitle();
            names.push_back(title.empty() || title == "Title" || title == "Default" ? std::string("Input/Output tab") : title);
        } else {
            comps = m_GbComps;
            names = m_GbNames;
            conds = m_GbConds;
        }
        conds.resize(comps.size());
        s.fo2_offsets = ParseList(m_GbOffsets);
        if (s.fo2_offsets.empty()) s.fo2_offsets.push_back(0.0);
        s.suppressed.clear();
        int count = 0;
        for (const auto &a : _MI.GetPhases()) {
            if (m_GbSuppressed[count]) s.suppressed.insert(a);
            ++count;
        }
        // Settings of each run: this panel, then the conditions of its batch row on top
        std::vector<GeobarometerSettings> rows;
        std::string err;
        int total = 0;
        for (size_t i = 0; i < comps.size(); ++i) {
            GeobarometerSettings c = s;
            c.composition = comps[i];
            if (!Geobarometer::ApplyConditions(conds[i], c, err)) {
                m_GbMessage = "Could not use the conditions of " + names[i] + ": " + err;
                rows.clear();
                break;
            }
            total += (int)Geobarometer::PressureGrid(c).size() * (int)(c.fo2_path == FO2_NONE ? 1 : std::max<size_t>(1, c.fo2_offsets.size()));
            rows.push_back(c);
        }
        if (comps.empty()) {
            m_GbMessage = "No composition to run: load a CSV file first.";
        } else if (!rows.empty()) {
            m_GbTotal = total;
            m_GbDone = 0;
            m_GbCancel = false;
            m_GbRunSettings = s;
            m_GbFuture = std::async(std::launch::async, [this, rows, names]() {
                std::vector<GeobarometerRun> all;
                for (size_t i = 0; i < rows.size() && !m_GbCancel.load(); ++i) {
                    std::vector<GeobarometerRun> r = Geobarometer::Run(rows[i], names[i], &m_GbDone, &m_GbCancel);
                    // kept on disk as the batch goes, so that a long batch survives a crash
                    Geobarometer::AppendSummaryCSV(kAutosave, r, i == 0);
                    all.insert(all.end(), r.begin(), r.end());
                }
                return all;
            });
            ImGui::OpenPopup("Geobarometer");
        }
    }

    ImGui::SetNextWindowBgAlpha(0.9f);
    if (ImGui::BeginPopupModal("Geobarometer", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_GbFuture.valid() && m_GbFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            m_GbRuns = m_GbFuture.get();
            m_GbSelected = 0;
            refit_plots = true;
            int found = 0;
            for (const auto &r : m_GbRuns)
                if (r.fit3.estimated || r.fit2.estimated) ++found;
            m_GbMessage = std::to_string(m_GbRuns.size()) + " runs, " + std::to_string(found) + " with a pressure" +
                          (m_GbCancel.load() ? " (stopped early)" : "");
            ImGui::CloseCurrentPopup();
        } else {
            const int done = m_GbDone.load();
            ImGui::Text("Geobarometer: pressure path %d of %d", std::min(done + 1, m_GbTotal), m_GbTotal);
            ImGui::ProgressBar(m_GbTotal > 0 ? (float)done / m_GbTotal : 0.f, ImVec2(320.f, 0.f));
            if (ImGui::Button("Stop")) m_GbCancel = true;
        }
        ImGui::EndPopup();
    }

    if (!m_GbMessage.empty()) ImGui::TextWrapped("%s", m_GbMessage.c_str());

    if (!m_GbRuns.empty()) {
        ImGui::Dummy(ImVec2(0, 5.f));
        ImGui::PushItemWidth(w * 0.4f);
        ImGui::InputText("##gbout", out_buf, sizeof out_buf);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Export CSV")) {
            const std::string base(out_buf[0] ? out_buf : "geobarometer");
            const bool ok = Geobarometer::WriteSummaryCSV(base + "_summary.csv", m_GbRunSettings, m_GbRuns) &&
                            Geobarometer::WriteDetailCSV(base + "_detail.csv", m_GbRunSettings, m_GbRuns);
            m_GbMessage = ok ? "Saved " + base + "_summary.csv and " + base + "_detail.csv in the easyMelts folder"
                             : "Could not write " + base + "_summary.csv";
        }
        ImGui::SameLine();
        Help("summary: one row per composition and fO2 offset (pressures, smallest residuals, phases). detail: saturation "
             "temperatures, residuals and fitted parabolas at every pressure, plus every phase that appeared.");
    }

    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("GbRight", ImVec2(0, 0));
    if (m_GbRuns.empty()) {
        ImGui::TextWrapped("Results appear here: a table of pressures, then the saturation temperatures and residuals of the selected run.");
        ImGui::EndChild();
        return;
    }
    if (m_GbSelected >= (int)m_GbRuns.size()) m_GbSelected = 0;
    const GeobarometerSettings &rs = m_GbRuns[m_GbSelected].settings; // phases and buffer of the selected run

    const float table_h = std::min(ImGui::GetWindowHeight() * 0.3f, 60.f + 22.f * (float)m_GbRuns.size());
    ImGui::BeginChild("GbTable", ImVec2(0, table_h), true);
    ImGui::Columns(8, "gbcols");
    static bool widths_set = false;
    if (!widths_set) {
        const float tw = ImGui::GetWindowContentRegionWidth();
        const float frac[8] = {0.15f, 0.08f, 0.09f, 0.09f, 0.09f, 0.09f, 0.23f, 0.18f};
        for (int c = 0; c < 8; ++c) ImGui::SetColumnWidth(c, tw * frac[c]);
        widths_set = true;
    }
    const char *heads[] = {"sample", "fO2 offset", "P3 (MPa)", "min dT3 (C)", "P2 (MPa)", "min dT2 (C)", "2-phase pair", "notes"};
    for (const char *h : heads) {
        ImGui::Text("%s", h);
        ImGui::NextColumn();
    }
    ImGui::Separator();
    for (int i = 0; i < (int)m_GbRuns.size(); ++i) {
        const auto &r = m_GbRuns[i];
        std::string id = r.sample + "##gbrow" + std::to_string(i);
        if (ImGui::Selectable(id.c_str(), m_GbSelected == i, ImGuiSelectableFlags_SpanAllColumns)) {
            m_GbSelected = i;
            refit_plots = true;
        }
        ImGui::NextColumn();
        ImGui::Text("%s", Num(r.fo2_offset, "%+.2f").c_str());
        ImGui::NextColumn();
        ImGui::Text("%s", r.fit3.estimated ? Num(r.fit3.p_est, "%.1f").c_str() : "-");
        ImGui::NextColumn();
        ImGui::Text("%s", Num(r.fit3.min_residual, "%.1f").c_str());
        ImGui::NextColumn();
        ImGui::Text("%s", r.fit2.estimated ? Num(r.fit2.p_est, "%.1f").c_str() : "-");
        ImGui::NextColumn();
        ImGui::Text("%s", Num(r.fit2.min_residual, "%.1f").c_str());
        ImGui::NextColumn();
        ImGui::Text("%s", r.fit2.phases_at_min.c_str());
        ImGui::NextColumn();
        int notes = 0;
        for (const auto &n : r.note)
            if (!n.empty()) ++notes;
        std::string cell = r.cancelled ? std::string("stopped") : r.message;
        if (!r.cancelled && notes) cell += (cell.empty() ? "" : " | ") + std::to_string(notes) + " (see values)";
        ImGui::Text("%s", cell.c_str());
        ImGui::NextColumn();
    }
    ImGui::Columns(1);
    ImGui::EndChild();

    const GeobarometerRun &r = m_GbRuns[m_GbSelected];
    std::vector<double> col[3];
    for (int k = 0; k < 3; ++k)
        for (const auto &t : r.tsat) col[k].push_back(t[k]);

    double pmin = 1e30, pmax = -1e30, tmin = 1e30, tmax = -1e30, dmax = 0.0;
    for (double p : r.pressure) {
        pmin = std::min(pmin, p);
        pmax = std::max(pmax, p);
    }
    for (int k = 0; k < 3; ++k)
        for (double t : col[k])
            if (std::isfinite(t)) {
                tmin = std::min(tmin, t);
                tmax = std::max(tmax, t);
            }
    for (double d : r.delta3)
        if (std::isfinite(d)) dmax = std::max(dmax, d);
    for (double d : r.delta2)
        if (std::isfinite(d)) dmax = std::max(dmax, d);
    if (tmin > tmax) {
        tmin = 700.0;
        tmax = 1100.0;
    }
    if (pmin > pmax) {
        pmin = 0.0;
        pmax = 500.0;
    }

    // No text is drawn inside the plot areas: the colour key, the pressures found and the cursor
    // position are shown in a strip to the right of each plot, so nothing can cover the data.
    const float plot_h = (ImGui::GetContentRegionAvail().y - 10.f) * 0.5f;
    const float key_w = 190.f;
    const float plot_w = std::max(ImGui::GetContentRegionAvail().x - key_w - 8.f, 200.f);
    const int plot_flags = ImPlotFlags_Default & ~ImPlotFlags_Legend & ~ImPlotFlags_MousePos;
    std::vector<float> xs, ys;
    bool sat_hovered = false, res_hovered = false;
    ImVec2 sat_mouse, res_mouse;

    ImGui::BeginChild("GbSatPlot", ImVec2(plot_w, plot_h));
    ImGui::SetNextPlotRange((float)pmin - 10.f, (float)pmax + 10.f, (float)tmin - 10.f, (float)tmax + 10.f, refit_plots ? ImGuiCond_Always : ImGuiCond_Once);
    std::string title = "Saturation temperatures: " + r.sample + ", " + _MI.GetFO2Paths().at(rs.fo2_path) + " " + Num(r.fo2_offset, "%+.2f") + "##gbsat";
    if (ImGui::BeginPlot(title.c_str(), "P (MPa)", "T (C)", ImVec2(plot_w - 5.f, plot_h - 5.f), plot_flags)) {
        ImGui::PushPlotStyleVar(ImPlotStyleVar_Marker, ImMarker_Circle);
        ImGui::PushPlotStyleVar(ImPlotStyleVar_MarkerSize, 3.f);
        for (int k = 0; k < 3; ++k) {
            Series(r.pressure, col[k], xs, ys);
            ImGui::PushPlotColor(ImPlotCol_Line, kPhaseColor[k]);
            if (!xs.empty()) ImGui::Plot(rs.phases[k].c_str(), xs.data(), ys.data(), (int)xs.size());
            ImGui::PopPlotColor();
        }
        ImGui::PopPlotStyleVar(2);
        if ((sat_hovered = ImGui::IsPlotHovered())) sat_mouse = ImGui::GetPlotMousePos();
        ImGui::EndPlot();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("GbSatKey", ImVec2(key_w, plot_h));
    ImGui::Dummy(ImVec2(0.f, 30.f));
    for (int k = 0; k < 3; ++k) KeyEntry(rs.phases[k].c_str(), kPhaseColor[k]);
    if (sat_hovered) {
        ImGui::Dummy(ImVec2(0.f, 8.f));
        ImGui::Text("cursor: %.0f MPa, %.0f C", sat_mouse.x, sat_mouse.y);
    }
    ImGui::EndChild();

    ImGui::BeginChild("GbResPlot", ImVec2(plot_w, plot_h));
    ImGui::SetNextPlotRange((float)pmin - 10.f, (float)pmax + 10.f, 0.f, (float)std::min(std::max(dmax, 10.0), 80.0) + 2.f, refit_plots ? ImGuiCond_Always : ImGuiCond_Once);
    if (ImGui::BeginPlot("Residuals and parabola fits##gbres", "P (MPa)", "residual (C)", ImVec2(plot_w - 5.f, plot_h - 5.f), plot_flags)) {
        ImGui::PushPlotStyleVar(ImPlotStyleVar_Marker, ImMarker_Circle);
        ImGui::PushPlotStyleVar(ImPlotStyleVar_MarkerSize, 3.f);
        Series(r.pressure, r.delta3, xs, ys);
        ImGui::PushPlotColor(ImPlotCol_Line, kResidualColor[0]);
        if (!xs.empty()) ImGui::Plot("delta_3", xs.data(), ys.data(), (int)xs.size());
        ImGui::PopPlotColor();
        Series(r.pressure, r.delta2, xs, ys);
        ImGui::PushPlotColor(ImPlotCol_Line, kResidualColor[1]);
        if (!xs.empty()) ImGui::Plot("delta_2", xs.data(), ys.data(), (int)xs.size());
        ImGui::PopPlotColor();
        ImGui::PopPlotStyleVar(2);
        ImGui::PushPlotStyleVar(ImPlotStyleVar_LineWeight, 2.f);
        Parabola(r, r.fit3, xs, ys);
        ImGui::PushPlotColor(ImPlotCol_Line, kResidualColor[2]);
        if (!xs.empty()) ImGui::Plot("fit 3 phases", xs.data(), ys.data(), (int)xs.size());
        ImGui::PopPlotColor();
        Parabola(r, r.fit2, xs, ys);
        ImGui::PushPlotColor(ImPlotCol_Line, kResidualColor[3]);
        if (!xs.empty()) ImGui::Plot("fit 2 phases", xs.data(), ys.data(), (int)xs.size());
        ImGui::PopPlotColor();
        ImGui::PopPlotStyleVar();
        // The pressures found: a diamond at each parabola vertex, values listed in the key strip
        const GeobarometerFit *fits[2] = {&r.fit3, &r.fit2};
        for (int k = 0; k < 2; ++k) {
            if (!fits[k]->estimated) continue;
            float vx = (float)fits[k]->p_est, vy = (float)std::max(fits[k]->residual_at_p_est, 0.0);
            ImGui::PushPlotColor(ImPlotCol_Line, kResidualColor[2 + k]);
            ImGui::PushPlotStyleVar(ImPlotStyleVar_Marker, ImMarker_Diamond);
            ImGui::PushPlotStyleVar(ImPlotStyleVar_MarkerSize, 6.f);
            ImGui::Plot(k == 0 ? "P3##vertex" : "P2##vertex", &vx, &vy, 1);
            ImGui::PopPlotStyleVar(2);
            ImGui::PopPlotColor();
        }
        if ((res_hovered = ImGui::IsPlotHovered())) res_mouse = ImGui::GetPlotMousePos();
        ImGui::EndPlot();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("GbResKey", ImVec2(key_w, plot_h));
    ImGui::Dummy(ImVec2(0.f, 30.f));
    const char *res_names[] = {"delta_3 (3 phases)", "delta_2 (2 phases)", "fit, 3 phases", "fit, 2 phases"};
    for (int k = 0; k < 4; ++k) KeyEntry(res_names[k], kResidualColor[k]);
    ImGui::Dummy(ImVec2(0.f, 8.f));
    ImGui::TextColored(kResidualColor[2], "P3 = %s", r.fit3.estimated ? (Num(r.fit3.p_est, "%.1f") + " MPa").c_str() : "none");
    ImGui::TextColored(kResidualColor[3], "P2 = %s", r.fit2.estimated ? (Num(r.fit2.p_est, "%.1f") + " MPa").c_str() : "none");
    if (r.fit2.estimated) ImGui::TextWrapped("(%s)", r.fit2.phases_at_min.c_str());
    if (res_hovered) {
        ImGui::Dummy(ImVec2(0.f, 8.f));
        ImGui::Text("cursor: %.0f MPa, %.1f C", res_mouse.x, res_mouse.y);
    }
    ImGui::EndChild();
    refit_plots = false;

    if (ImGui::TreeNode("Values at each pressure")) {
        ImGui::Columns(8, "gbvals");
        const std::string h3 = "T " + rs.phases[0], h4 = "T " + rs.phases[1], h5 = "T " + rs.phases[2];
        const char *vh[] = {"P (MPa)", "wet liquidus", h3.c_str(), h4.c_str(), h5.c_str(), "delta_3", "delta_2", "note"};
        for (const char *h : vh) {
            ImGui::Text("%s", h);
            ImGui::NextColumn();
        }
        ImGui::Separator();
        for (size_t i = 0; i < r.pressure.size(); ++i) {
            ImGui::Text("%s", Num(r.pressure[i]).c_str());
            ImGui::NextColumn();
            ImGui::Text("%s", Num(r.liquidus[i], "%.1f").c_str());
            ImGui::NextColumn();
            for (int k = 0; k < 3; ++k) {
                ImGui::Text("%s", Num(r.tsat[i][k]).c_str());
                ImGui::NextColumn();
            }
            ImGui::Text("%s", Num(r.delta3[i]).c_str());
            ImGui::NextColumn();
            ImGui::Text("%s", Num(r.delta2[i]).c_str());
            ImGui::NextColumn();
            ImGui::TextWrapped("%s", r.note[i].c_str());
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
        ImGui::TreePop();
    }
    ImGui::EndChild();
}
