/*
 easyMelts geobarometer: Excel workbook of a geobarometer run.
 Copyright (c) 2026 Eric C. P. Breard.
 Part of easyMelts, distributed under the GNU General Public License version 3 (see LICENSE.txt).

 The workbook is written from the summary and detail CSV files, so it can be made for any run of the
 window or the command-line tool, including the files of earlier releases. It has three sheets:
 Viewer (pick a run from a list; its results and two native Excel charts follow the choice),
 Summary (one row per run, with its flags, filterable) and Detail (one row per run and pressure).
*/

#include "geobarometer.hpp"
extern "C" {
#include "xlsxwriter.h"
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

// CSV as the geobarometer writes it: commas, fields with a comma, quote or line break in quotes.
bool ReadTable(const std::string &path, std::vector<std::string> &head, std::vector<std::vector<std::string>> &rows) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::vector<std::string>> all;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else
                    quoted = false;
            } else
                field += c;
        } else if (c == '"')
            quoted = true;
        else if (c == ',') {
            row.push_back(field);
            field.clear();
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            row.push_back(field);
            field.clear();
            if (!(row.size() == 1 && row[0].empty())) all.push_back(row);
            row.clear();
        } else
            field += c;
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        all.push_back(row);
    }
    if (all.empty()) return false;
    head = all[0];
    if (!head.empty() && head[0].size() >= 3 && (unsigned char)head[0][0] == 0xEF) head[0] = head[0].substr(3); // BOM
    rows.assign(all.begin() + 1, all.end());
    for (auto &r : rows) r.resize(head.size());
    return true;
}

bool Number(const std::string &s, double &v) {
    if (s.empty()) return false;
    char *end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

std::string Col(int c) { // 0 -> A
    std::string s;
    for (++c; c > 0; c = (c - 1) / 26) s.insert(s.begin(), char('A' + (c - 1) % 26));
    return s;
}

int Find(const std::vector<std::string> &head, const std::string &name) {
    auto it = std::find(head.begin(), head.end(), name);
    return it == head.end() ? -1 : (int)(it - head.begin());
}

void WriteCell(lxw_worksheet *ws, int r, int c, const std::string &v, lxw_format *num) {
    double x;
    if (Number(v, x)) worksheet_write_number(ws, r, c, x, num);
    else if (!v.empty()) worksheet_write_string(ws, r, c, v.c_str(), nullptr);
}

lxw_chart_line Line(lxw_color_t color, float width, uint8_t dash = LXW_CHART_LINE_DASH_SOLID) {
    lxw_chart_line l = {};
    l.color = color;
    l.width = width;
    l.dash_type = dash;
    return l;
}

} // namespace

namespace Geobarometer {

bool WriteWorkbook(const std::string &summary_csv, const std::string &detail_csv, const std::string &xlsx, std::string &error) {
    std::vector<std::string> sh, dh;
    std::vector<std::vector<std::string>> srows, drows;
    if (!ReadTable(summary_csv, sh, srows)) {
        error = "cannot read " + summary_csv;
        return false;
    }
    if (!ReadTable(detail_csv, dh, drows)) {
        error = "cannot read " + detail_csv;
        return false;
    }
    const int s_sample = Find(sh, "sample"), s_off = Find(sh, "fO2_offset");
    const int d_sample = Find(dh, "sample"), d_off = Find(dh, "fO2_offset"), d_p = Find(dh, "P_MPa");
    const int d_liq = Find(dh, "wet_liquidus_C"), d_d3 = Find(dh, "delta_3"), d_d2 = Find(dh, "delta_2");
    if (s_sample < 0 || s_off < 0 || d_sample < 0 || d_off < 0 || d_p < 0 || d_liq < 0 || d_d3 < 0 || d_d2 < 0 ||
        d_liq + 3 >= (int)dh.size() || srows.empty()) {
        error = "not a geobarometer summary and detail pair";
        return false;
    }

    // One label per run (sample and offset), in the order of the summary; repeats get #2, #3...
    std::vector<std::string> labels;
    std::map<std::string, int> seen;
    for (const auto &r : srows) {
        std::string base = r[s_sample] + " (" + (r[s_off].size() && r[s_off][0] == '-' ? "" : "+") + r[s_off] + ")";
        const int k = ++seen[base];
        labels.push_back(k == 1 ? base : base + " #" + std::to_string(k));
    }
    std::vector<std::string> dlabels(drows.size());
    {
        std::map<std::string, int> occurrence;
        std::string prev;
        for (size_t i = 0; i < drows.size(); ++i) {
            const std::string key = drows[i][d_sample] + "\x1f" + drows[i][d_off];
            if (key != prev) ++occurrence[key];
            prev = key;
            const std::string base = drows[i][d_sample] + " (" + (drows[i][d_off].size() && drows[i][d_off][0] == '-' ? "" : "+") + drows[i][d_off] + ")";
            dlabels[i] = occurrence[key] == 1 ? base : base + " #" + std::to_string(occurrence[key]);
        }
    }
    // fixed axis ranges from all runs (Excel cannot follow the chosen run): pressures, and the temperatures
    double pmin = 1e300, pmax = -1e300, tmin = 1e300, tmax = -1e300;
    for (const auto &r : drows) {
        double v;
        if (Number(r[d_p], v)) pmin = std::min(pmin, v), pmax = std::max(pmax, v);
        for (int c = d_liq; c <= d_liq + 3; ++c)
            if (Number(r[c], v)) tmin = std::min(tmin, v), tmax = std::max(tmax, v);
    }
    int max_rows = 1; // pressures per run
    {
        std::map<std::string, int> count;
        for (const auto &l : dlabels) max_rows = std::max(max_rows, ++count[l]);
    }

    lxw_workbook *wb = workbook_new(xlsx.c_str());
    if (!wb) {
        error = "cannot create " + xlsx;
        return false;
    }
    lxw_worksheet *view = workbook_add_worksheet(wb, "Viewer");
    lxw_worksheet *sum = workbook_add_worksheet(wb, "Summary");
    lxw_worksheet *det = workbook_add_worksheet(wb, "Detail");
    lxw_format *head = workbook_add_format(wb);
    format_set_bold(head);
    format_set_bg_color(head, 0xE8EEF4);
    format_set_bottom(head, LXW_BORDER_THIN);
    lxw_format *bold = workbook_add_format(wb);
    format_set_bold(bold);
    lxw_format *note = workbook_add_format(wb);
    format_set_italic(note);
    format_set_font_color(note, 0x52514E);
    lxw_format *grey = workbook_add_format(wb);
    format_set_font_color(grey, 0x8A8A8A);
    lxw_format *wrap = workbook_add_format(wb);
    format_set_text_wrap(wrap);
    lxw_format *num = nullptr;

    // Summary and Detail: the CSV tables, with the run label in front
    auto table = [&](lxw_worksheet *ws, const std::vector<std::string> &h, const std::vector<std::vector<std::string>> &rows,
                     const std::vector<std::string> &lab) {
        worksheet_write_string(ws, 0, 0, "run", head);
        for (size_t c = 0; c < h.size(); ++c) worksheet_write_string(ws, 0, (lxw_col_t)(c + 1), h[c].c_str(), head);
        for (size_t r = 0; r < rows.size(); ++r) {
            worksheet_write_string(ws, (lxw_row_t)(r + 1), 0, lab[r].c_str(), nullptr);
            for (size_t c = 0; c < h.size(); ++c) WriteCell(ws, (int)r + 1, (int)c + 1, rows[r][c], num);
        }
        worksheet_freeze_panes(ws, 1, 1);
        worksheet_autofilter(ws, 0, 0, (lxw_row_t)rows.size(), (lxw_col_t)h.size());
        worksheet_set_column(ws, 0, 0, 34, nullptr);
        worksheet_set_column(ws, 1, (lxw_col_t)h.size(), 12, nullptr);
    };
    table(sum, sh, srows, labels);
    table(det, dh, drows, dlabels);

    // Viewer: the run chosen in B1, its results, and the values the charts draw
    const std::string sref = "=Summary!$A$2:$A$" + std::to_string(srows.size() + 1);
    worksheet_write_string(view, 0, 0, "Run", bold);
    worksheet_write_string(view, 0, 1, labels[0].c_str(), bold);
    lxw_data_validation dv = {};
    dv.validate = LXW_VALIDATION_TYPE_LIST_FORMULA;
    dv.value_formula = sref.c_str();
    worksheet_data_validation_cell(view, 0, 1, &dv);
    worksheet_write_string(view, 1, 0,
                           "Pick a run in B1 (arrow at its right). Its results and the two charts follow it. Summary lists "
                           "every run with its flags (filter a column with its header arrow); Detail has every pressure.",
                           note);
    worksheet_write_string(view, 2, 0, "summary row", grey);
    worksheet_write_formula(view, 2, 1, "=MATCH($B$1,Summary!$A:$A,0)", grey);
    worksheet_write_string(view, 2, 2, "detail row", grey);
    worksheet_write_formula(view, 2, 3, "=MATCH($B$1,Detail!$A:$A,0)", grey);

    auto scol = [&](const std::string &name) { const int i = Find(sh, name); return i < 0 ? std::string() : Col(i + 1); };
    auto sval = [&](const std::string &name) { // value of a summary column for the chosen run, "" if blank
        const std::string c = scol(name);
        if (c.empty()) return std::string("\"\"");
        const std::string ix = "INDEX(Summary!$" + c + ":$" + c + ",$B$3)";
        return "IF(" + ix + "=\"\",\"\"," + ix + ")";
    };
    const std::pair<const char *, std::string> fields[] = {
        {"sample", "sample"}, {"fO2 offset", "fO2_offset"}, {"P3 (MPa)", "P_3phase_MPa"}, {"min dT3 (C)", "min_dT_3phase_C"},
        {"flags, P3", "flags_3phase"}, {"P2 (MPa)", "P_2phase_MPa"}, {"min dT2 (C)", "min_dT_2phase_C"},
        {"two-phase pair", "phases_2phase"}, {"flags, P2", "flags_2phase"}, {"two-phase rule", "two_phase_rule"},
        {"threshold (C)", "threshold_C"}, {"notes", "notes"}};
    int row = 4;
    for (const auto &f : fields) {
        worksheet_write_string(view, row, 0, f.first, bold);
        worksheet_write_formula(view, row, 1, ("=" + sval(f.second)).c_str(), wrap);
        ++row;
    }
    worksheet_write_string(view, row, 0, "phases", bold);
    worksheet_write_formula(view, row, 1, ("=" + sval("phase1") + "&\", \"&" + sval("phase2") + "&\", \"&" + sval("phase3")).c_str(), nullptr);
    worksheet_set_column(view, 0, 0, 16, nullptr);
    worksheet_set_column(view, 1, 1, 46, nullptr);
    worksheet_set_column(view, 2, 12, 9, nullptr);

    // values of the chosen run at each pressure (columns S to Z), #N/A where there is none
    const int c0 = 18; // S, clear of the two charts
    const char *vh[] = {"detail row", "P (MPa)", "", "", "", "wet liquidus", "delta_3", "delta_2"};
    worksheet_write_string(view, 3, c0 - 1, "values the charts use", grey);
    for (int k = 0; k < 8; ++k)
        if (vh[k][0]) worksheet_write_string(view, 3, c0 + k, vh[k], head);
    for (int k = 0; k < 3; ++k)
        worksheet_write_formula(view, 3, c0 + 2 + k, ("=\"T \"&" + sval("phase" + std::to_string(k + 1))).c_str(), head);
    const std::string rowcol = "$" + Col(c0);
    auto dget = [&](int dcol, const std::string &r) {
        const std::string c = Col(dcol + 1), ix = "INDEX(Detail!$" + c + ":$" + c + "," + r + ")";
        return "IF(INDEX(Detail!$A:$A," + r + ")<>$B$1,NA(),IF(" + ix + "=\"\",NA()," + ix + "))";
    };
    const int dcols[7] = {d_p, d_liq + 1, d_liq + 2, d_liq + 3, d_liq, d_d3, d_d2};
    for (int k = 0; k < max_rows; ++k) {
        const int r = 4 + k;
        const std::string rr = rowcol + std::to_string(r + 1);
        worksheet_write_formula(view, r, c0, ("=$D$3+" + std::to_string(k)).c_str(), grey);
        for (int j = 0; j < 7; ++j) worksheet_write_formula(view, r, c0 + 1 + j, ("=" + dget(dcols[j], rr)).c_str(), nullptr);
    }

    // the two parabolas (columns AB to AI): coefficients, fitted range and vertex, then 41 points each
    const int f0 = 27; // AB
    worksheet_write_string(view, 3, f0, "3-phase fit", head);
    worksheet_write_string(view, 3, f0 + 1, "2-phase fit", head);
    const char *pn[] = {"a", "b", "c", "P min", "P max", "P", "from", "to"};
    for (int k = 0; k < 8; ++k) worksheet_write_string(view, 4 + k, f0 - 1, pn[k], grey);
    const char *kinds[2] = {"3phase", "2phase"};
    for (int j = 0; j < 2; ++j) {
        const std::string kind = kinds[j], cc = Col(f0 + j);
        const std::string src[6] = {"fit_a_" + kind, "fit_b_" + kind, "fit_c_" + kind, "fit_P_min_" + kind + "_MPa",
                                    "fit_P_max_" + kind + "_MPa", "P_" + kind + "_MPa"};
        for (int k = 0; k < 6; ++k) {
            const std::string c = scol(src[k]);
            const std::string f = c.empty() ? "=NA()"
                                            : "=IF(INDEX(Summary!$" + c + ":$" + c + ",$B$3)=\"\",NA(),INDEX(Summary!$" + c + ":$" + c + ",$B$3))";
            worksheet_write_formula(view, 4 + k, f0 + j, f.c_str(), nullptr);
        }
        // drawn over the fitted pressures, and out to the vertex if it lies beyond them (as in the tab)
        const std::string P = "$" + cc + "$10", lo = "$" + cc + "$8", hi = "$" + cc + "$9";
        worksheet_write_formula(view, 10, f0 + j, ("=IF(ISNA(" + P + ")," + lo + ",MIN(" + lo + "," + P + "))").c_str(), nullptr);
        worksheet_write_formula(view, 11, f0 + j, ("=IF(ISNA(" + P + ")," + hi + ",MAX(" + hi + "," + P + "))").c_str(), nullptr);
    }
    const int p0 = 29; // AD: x and y of the 3-phase curve, then of the 2-phase curve, then the vertices
    worksheet_write_string(view, 3, p0, "P, fit 3", head);
    worksheet_write_string(view, 3, p0 + 1, "fit 3 phases", head);
    worksheet_write_string(view, 3, p0 + 2, "P, fit 2", head);
    worksheet_write_string(view, 3, p0 + 3, "fit 2 phases", head);
    worksheet_write_string(view, 3, p0 + 4, "P vertex", head);
    worksheet_write_string(view, 3, p0 + 5, "vertex", head);
    for (int j = 0; j < 2; ++j) {
        const std::string cc = "$" + Col(f0 + j);
        const std::string a = cc + "$5", b = cc + "$6", c = cc + "$7", from = cc + "$11", to = cc + "$12";
        for (int k = 0; k <= 40; ++k) {
            const int r = 4 + k;
            const std::string x = Col(p0 + 2 * j) + std::to_string(r + 1);
            worksheet_write_formula(view, r, p0 + 2 * j,
                                    ("=IF(ISNA(" + a + "),NA(),IF(" + a + "<=0,NA()," + from + "+(" + to + "-" + from + ")*" +
                                     std::to_string(k) + "/40))").c_str(), nullptr);
            worksheet_write_formula(view, r, p0 + 2 * j + 1,
                                    ("=IF(ISNA(" + x + "),NA()," + a + "*" + x + "^2+" + b + "*" + x + "+" + c + ")").c_str(), nullptr);
        }
        const std::string P = cc + "$10", vx = Col(p0 + 4) + std::to_string(5 + j);
        worksheet_write_formula(view, 4 + j, p0 + 4, ("=" + P).c_str(), nullptr);
        worksheet_write_formula(view, 4 + j, p0 + 5, ("=IF(ISNA(" + vx + "),NA()," + a + "*" + vx + "^2+" + b + "*" + vx + "+" + c + ")").c_str(), nullptr);
    }

    auto range = [&](int col, int r1, int r2) {
        return "=Viewer!$" + Col(col) + "$" + std::to_string(r1 + 1) + ":$" + Col(col) + "$" + std::to_string(r2 + 1);
    };
    const int last = 4 + max_rows - 1;
    const lxw_color_t colors[3] = {0x2A78D6, 0xEB6834, 0x1BAF7A}, dark[2] = {0x184D8F, 0xA8401A};

    lxw_chart *ct = workbook_add_chart(wb, LXW_CHART_SCATTER_STRAIGHT_WITH_MARKERS);
    for (int k = 0; k < 3; ++k) {
        lxw_chart_series *se = chart_add_series(ct, range(c0 + 1, 4, last).c_str(), range(c0 + 2 + k, 4, last).c_str());
        chart_series_set_name(se, ("=Viewer!$" + Col(c0 + 2 + k) + "$4").c_str());
        lxw_chart_line l = Line(colors[k], 1.5);
        chart_series_set_line(se, &l);
        chart_series_set_marker_type(se, LXW_CHART_MARKER_CIRCLE);
        chart_series_set_marker_size(se, 5);
        lxw_chart_fill fl = {};
        fl.color = colors[k];
        chart_series_set_marker_fill(se, &fl);
        chart_series_set_marker_line(se, &l);
    }
    {
        lxw_chart_series *se = chart_add_series(ct, range(c0 + 1, 4, last).c_str(), range(c0 + 5, 4, last).c_str());
        chart_series_set_name(se, "wet liquidus");
        lxw_chart_line l = Line(0x8A8A8A, 1.0, LXW_CHART_LINE_DASH_DASH);
        chart_series_set_line(se, &l);
        chart_series_set_marker_type(se, LXW_CHART_MARKER_NONE);
    }
    chart_title_set_name(ct, "Saturation temperatures");
    chart_axis_set_name(ct->x_axis, "P (MPa)");
    chart_axis_set_name(ct->y_axis, "T (C)");
    if (tmin < tmax) {
        chart_axis_set_min(ct->y_axis, std::floor(tmin / 25.0) * 25.0);
        chart_axis_set_max(ct->y_axis, std::ceil(tmax / 25.0) * 25.0);
    }
    chart_legend_set_position(ct, LXW_CHART_LEGEND_BOTTOM);

    lxw_chart *cr = workbook_add_chart(wb, LXW_CHART_SCATTER_STRAIGHT_WITH_MARKERS);
    const char *rn[2] = {"delta_3 (3 phases)", "delta_2 (2 phases)"};
    for (int k = 0; k < 2; ++k) {
        lxw_chart_series *se = chart_add_series(cr, range(c0 + 1, 4, last).c_str(), range(c0 + 6 + k, 4, last).c_str());
        chart_series_set_name(se, rn[k]);
        lxw_chart_line l = Line(colors[k], 1.25);
        chart_series_set_line(se, &l);
        chart_series_set_marker_type(se, LXW_CHART_MARKER_CIRCLE);
        chart_series_set_marker_size(se, 5);
        lxw_chart_fill fl = {};
        fl.color = colors[k];
        chart_series_set_marker_fill(se, &fl);
        chart_series_set_marker_line(se, &l);
    }
    const char *fn[2] = {"fit, 3 phases", "fit, 2 phases"}, *vn[2] = {"P3", "P2"};
    for (int k = 0; k < 2; ++k) {
        lxw_chart_series *se = chart_add_series(cr, range(p0 + 2 * k, 4, 44).c_str(), range(p0 + 2 * k + 1, 4, 44).c_str());
        chart_series_set_name(se, fn[k]);
        lxw_chart_line l = Line(dark[k], 2.25);
        chart_series_set_line(se, &l);
        chart_series_set_marker_type(se, LXW_CHART_MARKER_NONE);
    }
    for (int k = 0; k < 2; ++k) {
        lxw_chart_series *se = chart_add_series(cr, range(p0 + 4, 4 + k, 4 + k).c_str(), range(p0 + 5, 4 + k, 4 + k).c_str());
        chart_series_set_name(se, vn[k]);
        lxw_chart_line none = {};
        none.none = LXW_TRUE;
        chart_series_set_line(se, &none);
        chart_series_set_marker_type(se, LXW_CHART_MARKER_DIAMOND);
        chart_series_set_marker_size(se, 10);
        lxw_chart_fill fl = {};
        fl.color = dark[k];
        chart_series_set_marker_fill(se, &fl);
        lxw_chart_line ml = Line(0xFFFFFF, 1.0);
        chart_series_set_marker_line(se, &ml);
    }
    chart_title_set_name(cr, "Residuals and parabola fits");
    chart_axis_set_name(cr->x_axis, "P (MPa)");
    chart_axis_set_name(cr->y_axis, "residual (C)");
    chart_axis_set_min(cr->y_axis, 0);
    if (pmin < pmax)
        for (lxw_chart *c : {ct, cr}) {
            chart_axis_set_min(c->x_axis, std::floor(pmin / 50.0) * 50.0);
            chart_axis_set_max(c->x_axis, std::ceil(pmax / 50.0) * 50.0);
        }
    chart_legend_set_position(cr, LXW_CHART_LEGEND_BOTTOM);

    lxw_chart_options opt = {};
    opt.x_scale = 1.45;
    opt.y_scale = 1.35;
    worksheet_insert_chart_opt(view, 18, 0, ct, &opt);
    worksheet_insert_chart_opt(view, 18, 6, cr, &opt);
    worksheet_activate(view);

    const lxw_error e = workbook_close(wb);
    if (e != LXW_NO_ERROR) {
        error = std::string("cannot write ") + xlsx + ": " + lxw_strerror(e);
        return false;
    }
    return true;
}

} // namespace Geobarometer
