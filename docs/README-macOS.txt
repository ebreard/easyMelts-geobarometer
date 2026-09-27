easyMelts with the geobarometer, macOS (14 Sonoma or later)

1. Unzip the download. Pick the arm64 zip for Apple silicon Macs (M1, M2, M3, M4...) and the
   x86_64 zip for Intel Macs (Apple menu > About This Mac shows which one you have).

2. The first time only: the program is not signed with an Apple developer certificate, so macOS
   blocks it until you allow it. Open Terminal and type (drag the unzipped folder onto the
   Terminal window instead of typing its path):

       xattr -dr com.apple.quarantine ~/Downloads/easyMelts_geobarometer_macos_arm64

   Alternatively, double-click easyMelts.command, then open System Settings > Privacy & Security
   and click "Open Anyway" (you may have to do this twice, once for easyMelts.command and once
   for easyMelts).

3. Double-click easyMelts.command. A Terminal window opens with easyMelts; keep it open while
   you work. Exported files are written to the unzipped folder.

The user guide (easyMelts_geobarometer_guide.pdf) describes the Geobarometer tab. The
command-line tool runs from Terminal in the same folder, for example

    ./geobarometer_cli offsets=-1,-0.75,-0.5,0,0.5 phases=quartz,feldspar,orthopyroxene glasses.csv
