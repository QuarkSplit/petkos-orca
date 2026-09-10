# Podslicer

Podslicer is a Windows FDM fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer).
One project can hold plates for different printers. Each plate owns its printer, process,
filament slots, colours and physical machine assignment.

The development branch is `per-plate-machines`. The checkout still uses the name `petkos-orca`.

## Working with projects

The Home library browses project folders and recent files. Add a folder to index its supported
models recursively, use Refresh after files change, and use the folder action to find a file on
disk. Indexing does not move your files. Search and group filters also apply to recent entries.

Use **My projects** for your own design folders, **Print jobs** for prepared work, and
**Downloaded models** for the source model library. **All files** searches across them.
A file card's **Add folder to My projects** creates a shortcut to its parent folder; a
CAD-only folder can be added with **Add folder**, then **My project** under Library folders.
Personal folders remain reachable through **Show folder** even without a 3MF. Removing a
shortcut leaves its files intact. Personal folders, downloads and All files include loose
meshes automatically.

Library cards use saved project pictures or generate a preview from the model's geometry and
colours. Visible cards load first, with the remaining previews cached in the background. A
refresh repairs missing cached images. Unreadable files show an explanation in the card.

In a project, **Plate assigned materials** shows the selected plate's actual materials above
the independent **Spool pool**. Use **Change** or the plate board's material strip to replace
an assignment. Material selection offers an explicit scope: one slot, matching slots on chosen
plates, or matching slots across the project. Pool edits also offer **Spool pool only**.
Cancel keeps the earlier selection, spool colours and plate assignments. Other materials and
colours stay assigned, and refreshing a selector preserves unrelated plates' valid cached slices.
Printer, process and Plate support controls follow the selected plate's effective settings.

Changing a plate's printer carries process choices and translates its materials to compatible
presets where possible. An unresolved material is reported explicitly. Save as 3MF to retain
plate contexts and painting; STL cannot store these project properties.

The live mesh tools include **Knife** (a planar cut) and **Separate** (connected face-region
separation). Exact face operations preserve facet annotations by identity. Tools that rebuild
the surface transfer annotations geometrically, so the result can be an approximation at paint
boundaries. See [the current audit](docs/AUDIT-2026-09-08.md) for tested paths and limitations.

## Building and checking changes

This fork is maintained and verified as **Windows x64, Release, C++17**. Upstream's platform
instructions are reference material, not a claim that this fork is tested on other platforms.

An existing configured checkout uses:

```powershell
python tools\pod.py status
pwsh -NoProfile -File tools\petkos-dev-build.ps1 -DryRun
pwsh -NoProfile -File tools\petkos-dev-build.ps1
```

Finish source edits and lightweight checks before compiling. The build helper uses one MSBuild
worker, a small compiler budget based on available memory, Idle priority and a lock against
concurrent builds. `-Jobs 1` requests one compiler. A full rebuild is not required for every edit.
The legacy full-build batch file and `tools\petkos-ninja-build.bat` use the same guarded helper.
The full-build path also bounds nested CMake build jobs and compiler fan-out. For initial setup see
`build_release_vs2022.bat`; the retained [upstream README](README.orcaslicer.md) is reference material.

Build relevant test targets with the same helper, for example:

```powershell
& tools\petkos-dev-build.ps1 -Targets libslic3r_tests,slic3rutils_tests,fff_print_tests
& build\tests\libslic3r\Release\libslic3r_tests.exe --order rand --warn NoAssertions
& build\tests\slic3rutils\Release\slic3rutils_tests.exe --order rand --warn NoAssertions
& build\tests\fff_print\Release\fff_print_tests.exe --order rand --warn NoAssertions
```

The 10 September 2026 Release build completed. All 60,768 actual assertions in the native run
passed, but the strict suites remain nonzero: five inherited cases lack assertions and 34
utility cases skipped. See the [audit results](docs/AUDIT-2026-09-08.md#verification) for coverage
and limitations.

Launch through `run-petkos-orca.bat`, which supplies the isolated data directory. A running
instance continues using its current binary after a rebuild; the next launch uses the new one.

## Documentation and attribution

- [Project architecture and development rules](PODSLICER.md)
- [September library, mesh and material audit](docs/AUDIT-2026-09-08.md)
- [August work log](docs/WORKLOG-2026-08.md), a historical record rather than current guarantees
- [Wave-overhang limitations](docs/LIMITATIONS.md), specific to that feature
- [Upstream README and attribution](README.orcaslicer.md)

Podslicer inherits OrcaSlicer's AGPL-3.0 licence; see [LICENSE.txt](LICENSE.txt).
OrcaSlicer is not affiliated with this fork. Upstream download links install OrcaSlicer, not
Podslicer. `SLIC3R_APP_NAME` intentionally remains `OrcaSlicer` for firmware G-code recognition;
the displayed application name is Podslicer.
