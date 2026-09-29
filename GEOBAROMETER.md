# Geobarometer tab in easyMelts

This build of easyMelts adds a **Geobarometer** tab that does what the "Qtz + Fspars P Calc"
of MELTS_Excel does: it finds the pressure at which a melt (glass) composition is saturated,
at its liquidus, in the phases you choose (quartz + two feldspars, plagioclase + orthopyroxene
+ quartz, and so on). It runs locally with the same rhyolite-MELTS 1.0.x engine, so no
internet connection and no Excel are needed.

**Use release v0.3.0-geobarometer.3 or later.** In v0.3.0-geobarometer.1 the easyMelts window ran rhyolite-MELTS without its adjustment to sanidine and gave wrong saturation temperatures (for the Bishop Tuff sample it found no pressure instead of 377.3 / 372.5 MPa); the command-line tool was right. Up to v0.3.0-geobarometer.2, MELTS could crash part way through a batch on glasses that contain none of an oxide (MgO- or TiO2-free glasses, for example).

## Quick start

1. `Init > Version > MELTS_v1.0.x` (the published geobarometer uses rhyolite-MELTS 1.0.x).
2. In **Input/Output**, open *Set initial composition*, type the glass composition in wt%
   and put **H2O = 13** (excess water keeps the melt fluid-saturated at every pressure, as in
   Ruefer et al. 2025). The FeO/Fe2O3 split does not matter: the fO2 buffer resets it.
3. Open the **Geobarometer** tab, keep *Input/Output tab* as the source, choose the buffer
   (NNO by default) and the offsets, e.g. `-1, -0.75, -0.5, 0, 0.5`, choose the three phases,
   press **Run geobarometer**.
4. Click a row of the results table to see its saturation curves and residuals.
   **Export CSV** writes `<name>_summary.csv` and `<name>_detail.csv` next to easyMelts.

For many glasses, pick *CSV file (batch)*: one row per sample, a header with oxide names
(`Sample, SiO2, TiO2, Al2O3, FeO, MnO, MgO, CaO, Na2O, K2O, P2O5, H2O`; FeOt is read as FeO).
Rows without H2O get the amount typed in *H2O added when missing* (13 by default). CSV files
in the easyMelts folder are listed in the *CSV in this folder* menu.

Each row can also carry its own conditions in optional columns: `P_start`, `P_end`, `P_step` (MPa), `T_start`, `T_end`, `T_step` (C), `buffer`, `offsets` (one or more, for example `-1 -0.5 0`), `phase1`, `phase2`, `phase3`, `rule` (`any` or `phase1`) and `threshold` (C). The labels of MELTS_Excel work too (`P1 (MPa)`, `T1 (C)`, `fO2 value`, `Phase 1`, `Formula`, and so on), and a blank cell takes the value set in the tab, so one file can hold runs at different pressures, temperatures, fO2 and phase assemblages. [`docs/conditions_template.csv`](docs/conditions_template.csv) is an example.

For thousands of compositions (a Monte Carlo ensemble, for example), `jobs=N` makes the command-line tool cut the file into N parts, run them at once (one per processor core) and join the results into one summary and one detail file, in the order of the input file:

    geobarometer_cli.exe glasses.csv jobs=8 p_start=600 p_end=25 t_end=650 rule=phase1 out=glasses

On Windows, [`docs/run_batch_example.bat`](docs/run_batch_example.bat) does the same with a double-click once the file name and settings at its top are set. The window runs a batch one composition after another, and writes the summary of every finished composition to `geobarometer_autosave_summary.csv` as it goes, so a long batch is not lost if it is stopped.

## What is calculated (same rules as MELTS_Excel)

* Pressure grid 500 to 25 MPa in 25 MPa steps, temperature 1100 to 700 C in 1 C steps
  (all editable; use 10 MPa steps below 50 MPa as Ruefer et al. did).
* For each pressure: the wet (fluid-saturated) liquidus is found, the melt is cooled from the
  first grid temperature at or above it, one equilibrium per step, until the melt is below 10 %
  of the system mass. Amphibole and biotite are suppressed, as in MELTS_Excel.
* The saturation temperature of a phase is the highest temperature at which it is present.
* `delta_3` = highest minus lowest of the three saturation temperatures (all three present).
* `delta_2` = highest minus second highest ("any two phases", the MELTS_Excel default), or
  |T(phase 1) - max(T(phase 2), T(phase 3))| ("require phase 1"). With "any two phases" a
  two-phase pressure only appears where those two phases are the first to crystallise, which
  is the acceptance rule of Ruefer et al. (2025) and Part 5 of the series.
* A parabola is fitted through the smallest residual and up to two grid points on each side;
  its vertex is the pressure, reported only if that smallest residual is at or below the
  threshold (5 C by default; Part 5 used 10 C for magnetite-bearing assemblages).

Phase names: `feldspar1` / `feldspar2` are the first and second coexisting feldspars, exactly
as in MELTS_Excel; `plagioclase` / `sanidine` split feldspar at 25 mol% sanidine component
(the MELTS_Excel rule). `orthopyroxene`, `clinopyroxene`, `spinel`, `rhm-oxide` and every
other MELTS phase can be chosen too.

### Reproducing a MELTS_Excel run

The `init_cond` sheet of a MELTS_Excel workbook holds everything the tab needs: the model (`MELTS_v1.0.x` for rhyolite-MELTS_v1.0.x), the composition with its H2O, T1, T2 and T step (the temperature grid), P1, P2 and P step (the pressure grid), and the fO2 buffer and value (buffer and offset). With those, the tab reproduces the workbook's `Phase_Data` saturation temperatures and its `P_Calc` pressures, which use quartz, feldspar1 and feldspar2 with *any two phases*, the tab's defaults. If the pressures were worked out with other phases or with *require phase 1*, for example feldspar1, quartz and orthopyroxene, enter the phases in the same order: under *require phase 1* the two-phase residual always involves phase 1, so the order changes the two-phase pressure (the three-phase pressure does not depend on it).

*Stop each path once the three phases have appeared* (on by default) ends each cooling path as
soon as the three phases are present. It cannot change any result, because only the highest
temperature at which each phase appears is used; untick it to follow every path to the end.

The summary notes flag pressures where the wet-liquidus search failed (the path then starts at
the top of the temperature grid, as in MELTS_Excel), where MELTS failed part way down, or where
a chosen phase was already present at the first step.

## Checked against MELTS_Excel

The exact requests MELTS_Excel (2025Aug11) sends to its web service were replayed for its
default Bishop Tuff composition at NNO, at 12 pressures between 500 and 25 MPa, and compared
with this tab:

* saturation temperatures: 115 of 116 identical (every phase that appeared: quartz, both
  feldspars, orthopyroxene, clinopyroxene, rhm-oxide, garnet...). The one exception is the
  fluid at 25 MPa, 1 C apart, because the two paths start one grid step apart there;
* wet liquidus: within 0.01 C, except 0.4 C at 25 MPa. The search repeats an equilibration and a liquidus
  calculation until two estimates agree within 0.5 C, so its result depends on its route; started at 930 C
  instead of 1100 C, both programs return 918.87 C. It only sets where a cooling path starts;
* the wet-liquidus search fails at 475 MPa in both, and both then start that path at 1100 C;
* pressures from this tab for that composition: 377.3 MPa (quartz + 2 feldspars) and
  372.5 MPa (quartz + feldspar1).

The Geobarometer tab gives the same tables as the command-line tool: runs replayed through the tab on Windows match it value for value, and a test of the tab compiled exactly like the released program checks the Bishop Tuff result on every change.

Two of about 1,400 web-service requests timed out even after MELTS_Excel's 20 s retry. The
local calculation does not depend on a server.

## Speed

One composition at one fO2 (20 pressures from 500 to 25 MPa) takes 43 s on a laptop under Windows, and about 16 s under Linux on the same machine. MELTS_Excel sends the same work to its web service as 2,328 requests: one wet-liquidus search per pressure and one request per cooling step. Replaying one complete MELTS_Excel run for the Bishop Tuff composition took 33 minutes of server time (0.85 s per request, measured on 26 September 2026), before any time Excel spends writing its sheets, so about 45 times longer. The 53 runs of Ruefer et al. (2025) take about 40 minutes here, against at least 29 hours through the web service.

## Where this differs from MELTS_Excel

* An equilibration that has not converged after 20 s stops that pressure path, with a note
  (MELTS_Excel gives up on a request after 2 s plus a 20 s retry). Converged equilibrations
  take well under a second; a rare one needs about a minute.
* A chosen phase that never appears is reported as absent at those pressures; MELTS_Excel's
  P_Calc sheet shows errors or drops the residual in that case.
* MELTS no longer writes `melts.out` and the `tables` folder on every step of a run.

Five faults in the MELTS library that could close easyMelts were fixed for this: a singular
matrix during a liquidus search called GSL's default handler, which aborts the program (now
handled as in `silmin()`); a wet-liquidus search after an interrupted equilibration resumed a
stale step and crashed (the step counter is now reset); and a loop bound in
`InitComputeDataStruct` read one element past the phase table; and for melts that contain none of an oxide (MgO-free glasses, for example), round-off left a trace of the matching liquid component (1e-22 mol Mg2SiO4 once garnet had crystallised) that the constraint matrix of `getEqualityConstraints` had no column for, so MELTS wrote past the end of the matrix and the program crashed part way through a batch; such components are now set to zero before the matrix is built; and when an fO2 buffer was on and the bulk composition held no FeO or no Fe2O3 (after MELTS dropped the liquid and no phase held ferric iron), `getEqualityConstraints` combined the two iron rows through an index of -1, and `subsolidusmuO2` then wrote to a null pointer; the buffer is now removed in that case, as MELTS itself does when its subsolidus buffering reaction fails. A melt with no FeO and no Fe2O3 at all is not calculated with a buffer; its row says so.

Up to v0.3.0-geobarometer.1, easyMelts compiled its own copy of the MELTS solid-phase tables (in `melts_interface.cpp`) without `RHYOLITE_ADJUSTMENTS`, the switch the MELTS library is built with, so the window ran rhyolite-MELTS without the sanidine adjustment. The switch is now set in `melts_interface.hpp`; the upstream makefiles compile the C++ sources without it.

## Command-line version

`geobarometer_cli.exe` runs the same calculation on a CSV file without the window, e.g.

    geobarometer_cli.exe offsets=-1,-0.75,-0.5,0,0.5 phases=quartz,feldspar,orthopyroxene glasses.csv

Keys: `version`, `p_start`, `p_end`, `p_step`, `t_start`, `t_end`, `t_step`, `buffer`
(none, hm, nno, qfm, coh, iw), `offsets`, `phases`, `rule` (any or phase1), `threshold`,
`h2o`, `suppress`, `stop`, `quiet`, `step_timeout`, `out`, `jobs`.

## Source code and licence

easyMelts is GPL v3 (see LICENSE.txt). `source/geobarometer.patch` holds every change, against
https://github.com/magmasource/MAGMA at commit 705a0fb (`git apply geobarometer.patch`). The
Windows build used MSYS2 MINGW64 with gcc, glfw, libxml2, gsl, fmt and zlib, a static build of
libxlsxwriter, and `make -f Makefiles/Makefile.Windows easyMelts` with `CC="gcc -std=gnu17"`
(recent gcc rejects the old-style C declarations of the MELTS sources otherwise). The macOS and
Linux makefiles pick up the two new source files the same way. If results from this build are
published, the easyMelts licence asks for the modified source or a description of the changes;
this file and the patch provide both.

## References

Gualda & Ghiorso (2014) Contrib Mineral Petrol 168:1033. Gualda & Ghiorso (2015) G-cubed 16:315.
Harmon et al. (2018) Contrib Mineral Petrol 173:7. Smithies et al. (2025, Part 5) Contrib Mineral
Petrol, doi:10.1007/s00410-025-02274-w.
Ruefer et al. (2025) J Volcanol Geotherm Res 462:108305.
