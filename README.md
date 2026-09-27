# easyMelts with a phase-equilibrium geobarometer #

This fork of [magmasource/MAGMA](https://github.com/magmasource/MAGMA) adds a **Geobarometer** tab to easyMelts. It finds the pressure at which a melt (glass) composition is saturated, at its liquidus, in the minerals you choose: quartz and two feldspars, or plagioclase, quartz and orthopyroxene, for example. The calculation follows the rhyolite-MELTS geobarometer of Gualda & Ghiorso (2014) and Harmon et al. (2018) exactly as the "Qtz + Fspars P Calc" of MELTS_Excel (Gualda & Ghiorso 2015) performs it, with the same rhyolite-MELTS 1.0.x model, but it runs on your own computer: no Excel, no internet connection and no web service.

This is not an official easyMelts release, and it is not associated with or endorsed by the authors of MELTS, MELTS_Excel or easyMelts.

## Download ##

**Windows:** download the zip under **Releases**, unzip it and double-click `easyMelts.exe`. The zip also contains `geobarometer_cli.exe` (the same calculation from the command line), the [user guide (PDF)](docs/easyMelts_geobarometer_guide.pdf), a CSV template, the licence and the source patch.

**macOS and Linux:** build from source (below).

## Quick start ##

1. `Init > Version > MELTS_v1.0.x`.
2. In the **Input/Output** tab, enter the glass composition in wt% with **H2O = 13**. The excess water keeps the melt saturated with fluid at every pressure, as in Ruefer et al. (2025).
3. In the **Geobarometer** tab, choose three phases (default quartz, feldspar1, feldspar2), the fO2 buffer and one or more offsets (for example `-1, -0.75, -0.5, 0, 0.5`), then click **Run geobarometer**.
4. Click a row of the results table to see the saturation curves and the residuals, and use **Export CSV** to save them.

To run many glasses at once, choose *CSV file (batch)* and load a CSV file with one row per composition and a header of oxide names (`Sample, SiO2, TiO2, Al2O3, FeO, MnO, MgO, CaO, Na2O, K2O, P2O5, H2O`). The [user guide](docs/easyMelts_geobarometer_guide.pdf) describes every setting and output, and [GEOBAROMETER.md](GEOBAROMETER.md) gives the technical notes.

## What is calculated ##

For each pressure of a grid (500 to 25 MPa in 25 MPa steps by default):

* MELTS finds the fluid-saturated liquidus of the composition, and the melt is cooled from the first grid temperature at or above it in fixed steps (1 °C by default), one equilibrium calculation per step, until less than 10 % melt is left;
* the saturation temperature of each phase is the highest temperature at which it is present;
* two residuals are formed from the saturation temperatures of the three chosen phases: the spread of all three, and the gap between the two highest ("any two phases", the MELTS_Excel default) or between phase 1 and the higher of the other two ("require phase 1").

A least-squares parabola through the smallest residual and up to two grid points on each side gives the pressure (its vertex), reported only if that smallest residual is at or below 5 °C. Amphibole and biotite are suppressed, as in MELTS_Excel.

## Checked against MELTS_Excel ##

The exact requests that MELTS_Excel (version of 11 August 2025) sends to its MELTS web service were replayed for its Bishop Tuff test composition at NNO, at 12 pressures between 500 and 25 MPa:

* 115 of 116 phase saturation temperatures are identical (quartz, both feldspars, orthopyroxene, clinopyroxene, rhombohedral oxide, garnet and fluid). The exception is the fluid at 25 MPa, 1 °C apart, where the two cooling paths start one grid step apart;
* the wet liquidus agrees to 0.01 °C except at 25 MPa (0.4 °C). That search repeats an equilibration and a liquidus calculation until two estimates agree within 0.5 °C, so its result depends on its route: started at 930 °C instead of 1100 °C, both programs return 918.87 °C. It only sets where a cooling path starts;
* for that composition the tab gives 377.3 MPa (quartz and two feldspars) and 372.5 MPa (quartz and the first feldspar).

The Windows and Linux builds give identical results.

## Speed ##

One composition at one fO2 (20 pressures from 500 to 25 MPa) takes 43 s on a laptop under Windows, and about 16 s under Linux on the same machine. MELTS_Excel sends the same work to its web service as 2,328 requests: one wet-liquidus search per pressure and one request per cooling step. Replaying one complete MELTS_Excel run for the Bishop Tuff composition took 33 minutes of server time (0.85 s per request, measured on 26 September 2026), before any time Excel spends writing its sheets, so about 45 times longer. The 53 runs of Ruefer et al. (2025) take about 40 minutes here, against at least 29 hours through the web service.

## Building from source ##

The two new source files are part of the `easyMelts` rule in `Makefile.common`, so the usual makefiles build the tab. Compilers from 2024 on need the old MELTS C sources compiled as C17, for example `CC="gcc -std=gnu17"`.

On Windows the release was built with [MSYS2](https://www.msys2.org) (MINGW64 shell) and a static build of [libxlsxwriter](https://github.com/jmcnamara/libxlsxwriter):

```bash
pacman -S make mingw-w64-x86_64-gcc mingw-w64-x86_64-glfw mingw-w64-x86_64-libxml2 \
          mingw-w64-x86_64-gsl mingw-w64-x86_64-fmt mingw-w64-x86_64-zlib
git clone https://github.com/jmcnamara/libxlsxwriter.git && make -C libxlsxwriter
M=/mingw64; X=$PWD/libxlsxwriter
make -f Makefiles/Makefile.Windows easyMelts CC="gcc -std=gnu17" \
  INCC=$M/include INCM=$M/include INCX=$X/include INCXML=$M/include/libxml2 \
  PUBLIBS="$M/lib/libglfw3.a -lopengl32 -lgdi32 $M/lib/libfmt.a $X/lib/libxlsxwriter.a $M/lib/libz.a -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive" \
  LIBBATCH="$M/lib/libgsl.a $M/lib/libgslcblas.a $M/lib/libxml2.a $M/lib/libz.a $M/lib/libiconv.a -static-libstdc++ -static-libgcc -Wl,-Bdynamic -lws2_32 -luuid -lbcrypt -lm -lmingw32"
```

The command-line tool is built against the library that step produces:

```bash
g++ -O2 -std=c++17 -DBATCH_VERSION -DRHYOLITE_ADJUSTMENTS -DEASYMELTS_UPDATE_SYSTEM -DMINGW -DLIBXML_STATIC \
  -I./include -I./includes -I$M/include -I$M/include/libxml2 -o geobarometer_cli.exe \
  tests/geobarometer_cli.cpp src/geobarometer.cpp src/melts_interface.cpp src/e_utility.cpp src/file_utility.cpp \
  libMELTSbatch.a $M/lib/libgsl.a $M/lib/libgslcblas.a $M/lib/libxml2.a $M/lib/libz.a $M/lib/libiconv.a \
  -static -static-libstdc++ -static-libgcc -lws2_32 -luuid -lbcrypt -lm
```

## Changes in this fork (September 2026) ##

* New: `include/geobarometer.hpp`, `src/geobarometer.cpp` (the calculation), `src/geobarometer_tab.cpp` (the tab), `tests/geobarometer_cli.cpp` (command line), `tests/geobarometer_gui_test.cpp` (drives the tab in a hidden window), `GEOBAROMETER.md` and `docs/`.
* `src/imgui_opengl_melts_version.cpp`, `include/imgui_opengl.hpp`, `Makefile.common`: the new tab and sources.
* `sources/silmin.c`, `includes/silmin.h`: a switch that stops MELTS writing `melts.out` and the tables on every step of a run, and `silmin(calc_index < -1)`, which resets the solver's step counter after an interrupted equilibration (a wet-liquidus search after an interrupted run otherwise resumed a stale step and crashed).
* `sources/liquidus.c`: the GSL error handler is switched off in `liquidus()` as it already is in `silmin()`, so a singular matrix fails the search instead of aborting the program.
* `sources/melts_support.c`: a loop bound in `InitComputeDataStruct` is checked before the array read.

## Licence and citation ##

easyMelts is free software under the GNU General Public License v3 ([LICENSE.txt](LICENSE.txt)): easyMelts (c) 2020-2024 Einari Suikkanen and (c) 2025 Paula Antoshechkina; MELTS (c) Mark S. Ghiorso, BSD 3-clause; the geobarometer additions (c) 2026 Eric C. P. Breard.

If you publish results from this fork, please cite the MELTS and geobarometer papers below and easyMelts, and say that this fork was used; the easyMelts licence asks for a description of any changes that may affect model results, which the list above provides.

* Gualda, G.A.R., Ghiorso, M.S., Lemons, R.V., Carley, T.L. (2012) Rhyolite-MELTS: a modified calibration of MELTS optimized for silica-rich, fluid-bearing magmatic systems. *Journal of Petrology* 53, 875-890.
* Gualda, G.A.R., Ghiorso, M.S. (2014) Phase-equilibrium geobarometers for silicic rocks based on rhyolite-MELTS. Part 1: Principles, procedures, and evaluation of the method. *Contributions to Mineralogy and Petrology* 168, 1033.
* Gualda, G.A.R., Ghiorso, M.S. (2015) MELTS_Excel: A Microsoft Excel-based MELTS interface for research and teaching of magma properties and evolution. *Geochemistry, Geophysics, Geosystems* 16, 315-324.
* Harmon, L.J., Cowlyn, J., Gualda, G.A.R., Ghiorso, M.S. (2018) Phase-equilibrium geobarometers for silicic rocks based on rhyolite-MELTS. Part 4: Plagioclase, orthopyroxene, clinopyroxene, glass geobarometer, and application to Mt. Ruapehu, New Zealand. *Contributions to Mineralogy and Petrology* 173, 7.
* Ruefer, A.C., et al. (2025) In one step: Insights into shallow differentiation from basalt to rhyolite at Cordón Caulle from rhyolite-MELTS simulations. *Journal of Volcanology and Geothermal Research* 462, 108305.

---

*The README of the upstream MAGMA repository follows.*

# MELTS software #

**MELTS** is a software package designed to facilitate thermodynamic modeling of phase equilibria in magmatic systems. The original MELTS model could compute equilibrium phase relations for igneous systems over the temperature range 500-2000 °C and the pressure range 0-2 GPa. The [Magma Chamber Simulator](https://mcs.geol.ucsb.edu/) ([MCS](https://mcs.geol.ucsb.edu/)) is the marriage between a computational thermodynamic engine and an executive brain. The thermodynamic engine (Melts-batch.exe) implements one of the MELTS models - rhyolite-MELTS v1.2.x, 1.1.x, 1.0.x, or pMELTS - for performing phase equilibria calculations. The executive brain or command and control element of the MCS is called IGOR and implements a particular recharge-assimilation-fractional crystallization (RnAFC) scenario specified by the user in an Excel input file.

This repository is a fork of the **MAGMA** branch of Mark Ghiorso's **[xMELTS](https://gitlab.com/ENKI-portal/xMELTS)** repository on GitLab. It is maintained by Paula Antoshechkina in collaboration with the Magma Chamber Simulator (MCS) developer team. It also includes edits made for the MELTS software support previously hosted at [MAGMA@Caltech](https://magmasource.caltech.edu) (including early work on [MELTS for MATLAB](https://ui.adsabs.harvard.edu/abs/2018AGUFMED44B..23A%2F/abstract)).

In this repository the original standalone rhyolite-MELTS (v 1.0.2, 1.1.x, 1.2.x) and pMELTS (v 5.6.1) graphical user interface (GUI) is replaced by Einari Suikkanen's easyMelts software, which has the same capabilities - plus working plots - in a modern and easy-to-use package. For additional documentation on the original GUI, visit the [MELTS website](http://melts.ofm-research.org/index.html). There you can find preconfigured executables for MacOS and Ubuntu platforms (though your mileage may vary, especially with the Linux ones).

Both easyMelts and Melts-batch are provided for all macOS, Linux and Windows platforms. The Melts-batch executable can be used outside of MCS to simulate the MELTS web services (see the [schema documentation](http://melts.ofm-research.org/web0services.html)) and will be integrated in an upcoming release of the [GeoChemical Data toolkit](https://gcdkit.org/index.php) ([GCDkit](https://gcdkit.org/index.php)). No installation should be required for easyMelts or Melts-batch but more details will be provided in the Wiki soon.

## Downloading and running the software (MacOS, Linux or Windows) ##

Compiled executables for easyMelts and Melts-batch can be found by following the links below **Releases** to the right. Clicking the [Latest](https://github.com/magmasource/MAGMA/releases/latest) button will take you to the latest stable release for Intel (x86_64) and ARM64 (also known as AArch64) processors, including Apple silicon, running macOS, Windows 11 or Linux. Click on **Assets** to see the list of the zipped archives and checksums. The Linux executables were built on Ubuntu but are known to work on some Fedora-based systems.

Clicking the link below (e.g. [+4 releases](https://github.com/magmasource/MAGMA/releases)) will take you to the most recent beta version for testing. Not all OS and chipsets will be posted. If your combination is not listed and you are interested in trying out the newest software, please [contact Paula](mailto:psmith@gps.caltech.edu).

Information on running easyMelts and Melts-batch can be found in the repository Wiki linked at the top of the page.
