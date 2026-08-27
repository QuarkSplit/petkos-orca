#include "PetkosPerfDriver.hpp"
#include "PetkosPerf.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>

#include <wx/event.h>
#include <wx/timer.h>

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
//The picker phase measures the native dialog against the web guide it replaces, so it needs both.
#include "PodPrinterPicker.hpp"
#include "WebGuideDialog.hpp"
//Tab: the acceptance run pushes a filament-colour change the way the colour picker does, which
//goes through the printer tab as well as the plater.
#include "Tab.hpp"

namespace Slic3r { namespace GUI {

namespace {

struct Spec
{
    int         plates  = -1;
    //Which plate the Assign and Slice phases act on, 1-based. 0 keeps the old behaviour
    //(Assign iterates from plate 1, Slice takes plate 1). Added so a driven run can slice
    //a specific plate of a loaded project - the single-colour plate of a multi-colour
    //project is the case that needed it.
    int         plate   = 0;
    int         warmup  = 60;
    int         orbit   = 600;
    int         switches = 10;
    int         assigns = 4;
    int         board   = 200;
    int         drags   = 5;
    int         preview = 0;
    int         pick    = 1;
    //PetkosOrca: the per-plate settings check. Cheap, so it runs unless switched off.
    int         scope   = 1;
    //The check that the project printer is gone. On by default: it costs one resolve
    //per plate and it is the assertion the whole per-plate architecture rests on.
    int         context = 1;
    //Where to write plate 1's G-code, if anywhere. Off by default because a real slice is
    //the most expensive thing this driver can do; on when the run is a correctness check
    //rather than a latency measurement. It is the only artefact that cannot be wrong about
    //what a plate was sliced with, because it IS what gets made.
    std::string gcode;
    bool        quit    = true;
    std::string printer;
    //Where the run writes the project it built, so persistence can be inspected without the app.
    std::string save;
    //PetkosOrca stage 1: a real bounded cut, on whatever the run loaded. Off by default because
    //it is a mesh boolean on the whole object and nothing else in a timing run wants that.
    int         cut     = 0;
    //Where the plane sits, as a fraction of the object's height, and how much of its footprint
    //in X the region covers. Both are fractions so the same spec works on any fixture.
    double      cutz    = 0.5;
    double      cutspan = 0.5;
    int         cutdist = 1;
    //Where the resulting parts are written as STL. Evidence, not the check.
    std::string cutout;
    //The printer-picker phase. picker=1 builds the native picker and reports what it cost;
    //walk=1 or 2 additionally opens the inherited web guide so the two are measured back to
    //back in one process, which is the only comparison the memory notes accept as valid.
    //  walk=1  the guide's printer page (filaments and processes skipped)
    //  walk=2  the guide's full walk, which is what shipped
    int         picker  = 0;
    int         walk    = 0;
    //"Vendor/Model/Nozzle" - installs through the picker's own code path, so a headless run
    //adds a printer exactly the way a click does.
    std::string install;
    //"Vendor/Model" - removes every variant of it.
    std::string uninstall;
};

Spec parse_spec(const std::string &s)
{
    Spec spec;
    std::vector<std::string> parts;
    boost::split(parts, s, boost::is_any_of(","), boost::token_compress_on);
    for (const std::string &part : parts) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = part.substr(0, eq);
        const std::string val = part.substr(eq + 1);
        auto              num = [&val]() { return std::atoi(val.c_str()); };
        if (key == "plates")       spec.plates = num();
        else if (key == "plate")   spec.plate = num();
        else if (key == "warmup")  spec.warmup = num();
        else if (key == "orbit")   spec.orbit = num();
        else if (key == "switch")  spec.switches = num();
        else if (key == "assign")  spec.assigns = num();
        else if (key == "board")   spec.board = num();
        else if (key == "drag")    spec.drags = num();
        else if (key == "pick")    spec.pick = num();
        else if (key == "scope")   spec.scope = num();
        else if (key == "context") spec.context = num();
        else if (key == "gcode")   spec.gcode = val;
        else if (key == "save")    spec.save = val;
        else if (key == "preview") spec.preview = num();
        else if (key == "quit")    spec.quit = num() != 0;
        else if (key == "printer") spec.printer = val;
        else if (key == "cut")     spec.cut = num();
        else if (key == "cutz")    spec.cutz = std::atof(val.c_str());
        else if (key == "cutspan") spec.cutspan = std::atof(val.c_str());
        else if (key == "cutdist") spec.cutdist = num();
        else if (key == "cutout")  spec.cutout = val;
        else if (key == "picker")  spec.picker = num();
        else if (key == "walk")    spec.walk = num();
        else if (key == "install")   spec.install = val;
        else if (key == "uninstall") spec.uninstall = val;
    }
    return spec;
}

//The run is a state machine on idle rather than a timer, because a Windows timer's default
//resolution is ~15.6 ms - which would cap a frame-time measurement at 64 fps and hide
//exactly the range we are trying to measure.
class PerfDriver : public wxEvtHandler
{
public:
    explicit PerfDriver(Spec spec) : m_spec(std::move(spec))
    {
        Bind(wxEVT_IDLE, &PerfDriver::on_idle, this);
        //Idle events go to windows, not to a loose handler, so the driver rides the app's
        //own idle by pushing itself onto the main frame's handler chain.
        if (wxWindow *frame = static_cast<wxWindow *>(wxGetApp().mainframe); frame != nullptr)
            frame->PushEventHandler(this);
    }

private:
    enum class Phase { Settle0, Build, Warmup, Orbit, Preview, Switch, Assign, Pick, Scope, Context, Cut, Slice, SliceWait, Board, Drag, Picker, Finish, Done };

    void on_idle(wxIdleEvent &evt)
    {
        evt.Skip();
        Plater *plater = wxGetApp().plater();
        if (plater == nullptr || !plater->is_initialized())
            return;
        GLCanvas3D *canvas = plater->get_view3D_canvas3D();
        if (canvas == nullptr)
            return;

        switch (m_phase) {
        case Phase::Settle0:
            //The app opens on the Home page, where the 3D canvas is not shown and render()
            //early-returns - so a run that forgets this measures an idle app and reports it as
            //fast. The first pass of this instrument did exactly that: 40 orbit frames in 15 ms
            //and not one of them drawn. Select the editor first, then let the app settle.
            if (m_tick == 0 && wxGetApp().mainframe != nullptr) {
                wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                //Frame time is fill-rate sensitive, so a run at one window size cannot be
                //compared with a run at another. Two runs of IDENTICAL code differed by 60%
                //on the opaque pass (42.6 vs 68.1 ms at 36 plates) while the overlays - which
                //are fill-rate light - stayed at 22 ms in both. The size is pinned here so the
                //comparison is valid by construction rather than by luck, and recorded so a
                //stray result can be told apart from a real one.
                wxGetApp().mainframe->SetSize(wxSize(1600, 1000));
            }
            if (++m_tick > 30) {
                if (!plater->is_view3D_shown()) {
                    //Say so rather than producing a fast-looking number from an empty canvas.
                    BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: the 3D editor is not shown; "
                                                "frame times from this run would be meaningless";
                }
                Perf::mark("driver.begin", plater->is_view3D_shown() ? 1 : 0);
                //width*10000+height, so one integer carries the geometry every frame time in
                //this run was measured at
                const Size cs = canvas->get_canvas_size();
                Perf::mark("driver.canvas_size", cs.get_width() * 10000 + cs.get_height());
                enter(Phase::Build);
            }
            evt.RequestMore();
            break;

        case Phase::Build:
            build_plates(plater);
            Perf::mark("driver.plates_built", plater->get_partplate_list().get_plate_count());
            enter(Phase::Warmup);
            evt.RequestMore();
            break;

        case Phase::Warmup:
            orbit_step(plater, canvas);
            if (++m_tick >= m_spec.warmup) {
                Perf::mark("driver.orbit_begin", plater->get_partplate_list().get_plate_count());
                enter(Phase::Orbit);
            }
            evt.RequestMore();
            break;

        case Phase::Orbit:
            orbit_step(plater, canvas);
            if (++m_tick >= m_spec.orbit) {
                Perf::mark("driver.orbit_end", m_spec.orbit);
                enter(m_spec.preview > 0 ? Phase::Preview : Phase::Switch);
            }
            evt.RequestMore();
            break;

        case Phase::Preview:
            //The plate strip - and with it is_slice_result_valid per plate per frame -
            //exists only on the Preview canvas, so a View3D-only run cannot see that cost.
            if (m_tick == 0) {
                Perf::mark("driver.preview_begin", plater->get_partplate_list().get_plate_count());
                plater->select_view_3D("Preview");
            }
            if (GLCanvas3D *preview_canvas = plater->get_preview_canvas3D(); preview_canvas != nullptr)
                preview_canvas->set_as_dirty();
            if (++m_tick >= m_spec.preview) {
                Perf::mark("driver.preview_end", m_spec.preview);
                plater->select_view_3D("3D");
                enter(Phase::Switch);
            }
            evt.RequestMore();
            break;

        case Phase::Switch:
            //Each switch is followed by a quiet stretch so the settle half of the
            //interaction can close before the next one opens.
            if (m_quiet > 0) {
                --m_quiet;
            } else if (m_done >= m_spec.switches) {
                Perf::mark("driver.switch_end", m_done);
                enter(Phase::Assign);
            } else {
                const int count = plater->get_partplate_list().get_plate_count();
                if (count > 1) {
                    const int target = (plater->get_partplate_list().get_curr_plate_index() + 1) % count;
                    plater->select_plate(target);
                }
                ++m_done;
                m_quiet = 12;
            }
            evt.RequestMore();
            break;

        case Phase::Assign:
            if (m_quiet > 0) {
                --m_quiet;
            } else if (m_assign_presets.empty()) {
                //An assign phase that does nothing must not look like an assign phase that ran.
                //This transition used to be silent, so a run with assign=1 produced no ASSIGN line
                //of any kind and every later check quietly tested an UNASSIGNED plate.
                BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: ASSIGN SKIPPED - no printer preset to assign; "
                                            "nothing was reassigned and any later check runs against the plates "
                                            "as they were";
                Perf::mark("driver.assign_end", m_done);
                enter(m_spec.pick > 0 ? Phase::Pick
                                      : (m_spec.scope > 0 ? Phase::Scope
                                                          : (m_spec.context > 0 ? Phase::Context : next_after_context())));
            } else if (m_done >= m_spec.assigns) {
                BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: ASSIGN phase done - " << m_assigned << " of "
                                           << m_done << " iteration(s) changed a plate's machine";
                Perf::mark("driver.assign_end", m_done);
                enter(m_spec.pick > 0 ? Phase::Pick
                                      : (m_spec.scope > 0 ? Phase::Scope
                                                          : (m_spec.context > 0 ? Phase::Context : next_after_context())));
            } else {
                const int count = plater->get_partplate_list().get_plate_count();
                const int plate = m_spec.plate > 0 && m_spec.plate <= count ? m_spec.plate - 1
                                                                            : (count > 0 ? (m_done % count) : 0);
                //A REAL printer, and one this plate does not already have.
                //
                //Two earlier versions of this line each timed nothing. The first alternated a named
                //preset with an empty string on the theory that clearing was the second write path;
                //there is no such path - a plate names its own machine or it is unresolved - so every
                //other iteration measured set_plate_printer refusing to write. The second alternated
                //two real presets by iteration index, and m_assign_presets[0] is the CURRENTLY
                //SELECTED printer, which is the one the plates were completed to - so with assigns=1
                //the single iteration assigned a plate the machine it already had, timed nothing, and
                //left the context check testing a plate that had never been assigned at all.
                //
                //What the phase is for is the cost of a CHANGE of machine, so it picks a name that is
                //a change for this plate, and says out loud when it cannot.
                const std::string current = plater->get_partplate_list().get_plate(plate) != nullptr
                                                ? plater->get_partplate_list().get_plate(plate)->get_printer_preset_name()
                                                : std::string();
                std::string       name;
                for (size_t i = 0; i < m_assign_presets.size(); ++i) {
                    const std::string &candidate = m_assign_presets[(m_done + i) % m_assign_presets.size()];
                    if (candidate != current) {
                        name = candidate;
                        break;
                    }
                }
                if (name.empty()) {
                    BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: ASSIGN SKIPPED - plate " << (plate + 1)
                                             << " already names '" << current
                                             << "' and no other printer preset is installed, so this iteration "
                                                "cannot time a change of machine";
                } else if (plater->set_plate_printer(plate, name)) {
                    BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: ASSIGN plate " << (plate + 1) << " '" << current
                                               << "' -> '" << name << "'";
                    ++m_assigned;
                } else {
                    BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: ASSIGN SKIPPED - plate " << (plate + 1)
                                             << " was not reassigned from '" << current << "' to '" << name
                                             << "'; this iteration timed no assignment";
                }
                ++m_done;
                m_quiet = 20;
            }
            evt.RequestMore();
            break;

        case Phase::Pick:
            pick_step(plater, canvas);
            evt.RequestMore();
            break;

        case Phase::Scope:
            scope_step(plater);
            evt.RequestMore();
            break;

        case Phase::Context:
            context_step(plater);
            evt.RequestMore();
            break;

        case Phase::Cut:
            cut_step(plater);
            evt.RequestMore();
            break;

        case Phase::Slice:
            slice_step(plater);
            evt.RequestMore();
            break;

        case Phase::SliceWait:
            slice_wait_step(plater);
            evt.RequestMore();
            break;

        case Phase::Board:
            board_motion(plater);
            if (++m_tick >= m_spec.board) {
                Perf::mark("driver.board_end", m_spec.board);
                enter(Phase::Drag);
            }
            evt.RequestMore();
            break;

        case Phase::Drag:
            if (m_done >= m_spec.drags) {
                Perf::mark("driver.drag_end", m_done);
                enter(Phase::Picker);
            } else {
                board_drag(plater);
                ++m_done;
            }
            evt.RequestMore();
            break;

        case Phase::Picker:
            picker_step();
            evt.RequestMore();
            break;

        case Phase::Finish: {
            //save= is an OUTPUT, not a property of the scope test. The scope phase saves
            //mid-test to prove override persistence; every other run that asked for a save
            //gets it here, whatever phases were on - a fixture builder with scope=0 used to
            //quit having saved nothing, silently.
            if (!m_spec.save.empty() && !m_saved) {
                Plater *plater = wxGetApp().plater();
                if (plater != nullptr) {
                    const int written = plater->export_3mf(boost::filesystem::path(m_spec.save));
                    m_saved = true;
                    BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: wrote " << m_spec.save << " (rc=" << written << ")";
                }
            }
            Perf::mark("driver.end", 0);
            Perf::flush();
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: run complete, wrote "
                                       << Perf::output_path() << ".csv";
            m_phase = Phase::Done;
            if (m_spec.quit && wxGetApp().mainframe != nullptr)
                wxGetApp().mainframe->Close(true);
            break;
        }

        case Phase::Done:
            break;
        }
    }

    void enter(Phase p)
    {
        m_phase = p;
        m_tick  = 0;
        m_done  = 0;
        m_quiet = 0;
        if (p == Phase::Assign) {
            m_assigned = 0;
            collect_assign_presets();
        }
    }

    //PetkosOrca: the half of correctness a timing run cannot show.
    //
    //A frame can be fast while a plate slices with a different plate's settings, which is
    //precisely what this fork shipped. The Process panel edits the PROJECT's process preset; a
    //plate reassigned to another printer has been moved onto that printer's own default process;
    //so every value typed into the panel reached a preset that plate does not use, and the panel
    //went on displaying the project's preset name as though it had worked. No latency run can
    //see that, and the screenshot that finally caught it was a person using the app.
    //
    //So this asks the resolver the only question that matters - what would this plate ACTUALLY
    //slice with - and checks two things a whitelist and a stale copy each used to break: that a
    //per-plate override reaches the plate it was set on, and that it reaches no other plate.
    void scope_step(Plater *plater)
    {
        PartPlateList &list = plater->get_partplate_list();
        if (list.get_plate_count() < 2) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SCOPE CHECK SKIPPED - needs two plates, have "
                                     << list.get_plate_count();
            enter(Phase::Board);
            return;
        }

        PartPlate *subject = list.get_plate(0);
        PartPlate *control = list.get_plate(1);
        std::string error;

        //Read the inherited value first. An assertion against a number the preset already carries
        //proves nothing, so the target is deliberately something no preset here is using.
        ResolvedPlateSlicingConfig before;
        if (subject == nullptr || control == nullptr ||
            !plater->resolve_plate_slicing_config(subject, before, error)) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SCOPE CHECK FAILED - plate 1 does not resolve: " << error;
            enter(Phase::Board);
            return;
        }
        const ConfigOptionInt *inherited = before.config.option<ConfigOptionInt>("wall_loops");
        const int              want      = (inherited != nullptr ? inherited->value : 2) + 3;

        //Written straight onto the plate, which is what the Plate scope in the Process panel does
        //through TabPrintPlate::on_value_change. What is under test is everything downstream of
        //that write: the resolver applying it, and applying it to one plate only.
        subject->config()->set_key_value("wall_loops", new ConfigOptionInt(want));

        bool ok = true;
        ResolvedPlateSlicingConfig after;
        if (!plater->resolve_plate_slicing_config(subject, after, error)) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SCOPE CHECK FAILED - plate 1 stopped resolving: " << error;
            ok = false;
        } else {
            const ConfigOptionInt *got = after.config.option<ConfigOptionInt>("wall_loops");
            const int              val = got != nullptr ? got->value : -1;
            if (val != want) {
                BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SCOPE CHECK FAILED - plate 1 override not applied: "
                                            "set wall_loops=" << want << ", plate would slice with " << val;
                ok = false;
            }
        }

        ResolvedPlateSlicingConfig other;
        if (!plater->resolve_plate_slicing_config(control, other, error)) {
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: plate 2 does not resolve, cannot check isolation: " << error;
        } else {
            const ConfigOptionInt *got = other.config.option<ConfigOptionInt>("wall_loops");
            const int              val = got != nullptr ? got->value : -1;
            if (val == want) {
                BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SCOPE CHECK FAILED - plate 1's override leaked onto "
                                            "plate 2, both read wall_loops=" << val;
                ok = false;
            }
        }

        if (ok)
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: scope check passed - plate 1 slices with wall_loops="
                                       << want << ", plate 2 unaffected";

        //Persistence is the other half, and it is the one a whitelist breaks: an override that
        //works in this session and vanishes on save is a silent yes. The file is left for the
        //harness to read, because proving a round-trip from inside the process that wrote it is
        //the weaker test.
        if (!m_spec.save.empty()) {
            const int written = plater->export_3mf(boost::filesystem::path(m_spec.save));
            m_saved = true;
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: wrote " << m_spec.save << " (rc=" << written << ")";
        }

        enter(m_spec.context > 0 ? Phase::Context : next_after_context());
    }

    //IS THE PROJECT PRINTER REALLY GONE?
    //
    //Three questions, in rising strength, all asked of the code slicing itself reads.
    //
    //  1. Does every plate carry its own complete context? A plate without one used to be
    //     legal and meant "use the project's", which is the state being deleted.
    //  2. Does an EMPTY context refuse to resolve? That refusal is the deletion. While the
    //     resolver answered an empty context by reading the globally selected preset, every
    //     plate in the project had that preset standing behind it.
    //  3. Does moving the global selection change what a plate slices with? This is the one
    //     that cannot be faked by a well-behaved caller: the cursor is moved underneath the
    //     plate and the plate must not notice. It is restored afterwards.
    void context_step(Plater *plater)
    {
        PartPlateList &list   = plater->get_partplate_list();
        PresetBundle * bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CONTEXT CHECK SKIPPED - no preset bundle";
            enter(Phase::Board);
            return;
        }

        bool ok = true;

        // 1. every plate owns a complete context
        for (int i = 0; i < list.get_plate_count(); ++i) {
            PartPlate *plate = list.get_plate(i);
            if (plate == nullptr)
                continue;
            const PlateSlicingContext context = plate->get_slicing_context();
            if (!context.is_complete()) {
                BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CONTEXT CHECK FAILED - plate " << (i + 1)
                                         << " has no context of its own (printer='" << context.printer_preset_name
                                         << "', process='" << context.print_preset_name << "', "
                                         << context.filament_preset_names.size() << " filament(s))";
                ok = false;
            }
        }

        // 1b. and it must not be a PLACEHOLDER. "Complete" is not the same as "right": the
        //     first attempt at this filled every plate in before the project's own presets
        //     were installed, so each one came out complete and named "Default Setting" /
        //     "Default Filament" - which capped the flow rate at 2 and turned a 6h45m plate
        //     into 21h03m. Every in-app check passed on that build. This is the cheap
        //     in-app half of the check that would have caught it; the other half is reading
        //     filament_settings_id and filament_max_volumetric_speed out of the G-code.
        for (int i = 0; i < list.get_plate_count(); ++i) {
            PartPlate *plate = list.get_plate(i);
            if (plate == nullptr)
                continue;
            const PlateSlicingContext context = plate->get_slicing_context();
            auto names_a_default = [bundle](const PresetCollection &collection, const std::string &name) {
                const Preset *preset = collection.find_preset(name, false);
                return preset != nullptr && preset->is_default;
            };
            if (names_a_default(bundle->prints, context.print_preset_name)) {
                BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CONTEXT CHECK FAILED - plate " << (i + 1)
                                         << " names the default process '" << context.print_preset_name
                                         << "', which is a placeholder rather than a choice";
                ok = false;
            }
            for (const std::string &filament : context.filament_preset_names) {
                if (names_a_default(bundle->filaments, filament)) {
                    BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CONTEXT CHECK FAILED - plate " << (i + 1)
                                             << " names the default filament '" << filament
                                             << "', which caps the flow rate and is a placeholder rather than a choice";
                    ok = false;
                }
            }
        }

        // 2. an empty context is an error, not an inheritance
        {
            PlateSlicingContext        empty;
            ResolvedPlateSlicingConfig resolved;
            std::string                error;
            if (bundle->resolve_plate_slicing_config(empty, std::nullopt, std::nullopt, resolved, error)) {
                BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CONTEXT CHECK FAILED - an empty context still "
                                            "resolves, so the project printer is still standing behind it";
                ok = false;
            }
        }

        // 3. the cursor moves, the plate does not
        PartPlate *subject = list.get_plate(0);
        if (subject != nullptr) {
            ResolvedPlateSlicingConfig before;
            std::string                error;
            if (!plater->resolve_plate_slicing_config(subject, before, error)) {
                BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CONTEXT CHECK FAILED - plate 1 does not resolve: "
                                         << error;
                ok = false;
            } else {
                const std::string original = bundle->printers.get_selected_preset_name();
                std::string       other;
                for (const Preset &preset : bundle->printers) {
                    if (preset.is_visible && !preset.is_default && preset.printer_technology() == ptFFF &&
                        preset.name != original) {
                        other = preset.name;
                        break;
                    }
                }
                if (other.empty()) {
                    BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: only one printer preset is installed, so the "
                                                  "cursor cannot be moved out from under plate 1; questions 1 and 2 "
                                                  "still stand";
                } else {
                    bundle->printers.select_preset_by_name(other, false);
                    ResolvedPlateSlicingConfig after;
                    if (!plater->resolve_plate_slicing_config(subject, after, error)) {
                        BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CONTEXT CHECK FAILED - plate 1 stopped "
                                                    "resolving when the global selection moved: " << error;
                        ok = false;
                    } else {
                        //The NAMES, not the preset pointers. find_preset hands back
                        //&m_edited_preset for whichever preset is selected, and
                        //select_preset_by_name overwrites that buffer - so reading
                        //before.printer_preset->name after the move read the machine the cursor
                        //had just been pointed at, and this check reported a plate changing
                        //machine when nothing about the plate had changed. It is the one test
                        //that moves the cursor on purpose, so it is the one place guaranteed to
                        //hit it. See ResolvedPlateSlicingConfig.
                        const std::string &was = before.printer_preset_name;
                        const std::string &now = after.printer_preset_name;
                        if (was != now) {
                            BOOST_LOG_TRIVIAL(error)
                                << "PETKOS_PERF_SCRIPT: CONTEXT CHECK FAILED - moving the global selection to '"
                                << other << "' changed what plate 1 slices with, from '" << was << "' to '" << now
                                << "'";
                            ok = false;
                        }
                    }
                    bundle->printers.select_preset_by_name(original, false);
                }
            }
        }

        if (ok)
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: context check passed - every plate owns its context, "
                                          "an empty one is an error, and the global selection reaches no plate";

        enter(next_after_context());
    }

    //its_volume() accumulates in FLOAT. Two million signed tetrahedra, each of magnitude ~1e6,
    //summing to ~3e5 lose their low digits to cancellation: on the revolver that is ~250 mm3 of
    //pure measurement error - 0.09%, which is more than a boolean would have to lose before
    //this check could see it. A check whose noise floor is above the fault it is looking for is
    //not a check, so the sum is done in double and the tolerance can then mean something.
    static double mesh_volume(const indexed_triangle_set &its)
    {
        double v = 0.0;
        for (const Vec3i32 &f : its.indices) {
            const Vec3d a = its.vertices[f[0]].cast<double>();
            const Vec3d b = its.vertices[f[1]].cast<double>();
            const Vec3d c = its.vertices[f[2]].cast<double>();
            v += a.dot(b.cross(c));
        }
        return v / 6.0;
    }

    Phase next_after_context() const
    {
        if (m_spec.cut > 0)
            return Phase::Cut;
        return m_spec.gcode.empty() ? Phase::Board : Phase::Slice;
    }

    //PETKOSORCA STAGE 1: A REAL BOUNDED CUT, CHECKED THE ONLY WAY THAT CANNOT BE WRONG.
    //
    //Two halves that look right on screen prove nothing - the failure mode of a boolean is a
    //part with a hole in it, and a hole does not show from outside. So the verdict is
    //arithmetic: both parts closed (its_num_open_edges == 0) and their volumes adding up to
    //the source's. Both are computed off the meshes the app is holding, after the cut has
    //actually gone through Cut and Plater::apply_cut_object_to_model, so what is measured is
    //the production path rather than a copy of it.
    //
    //The booleans run inline here rather than through BoundedCutJob, deliberately: this phase
    //is checking geometry, and a driver that hands work to a worker and returns to idle has to
    //re-enter to find out what happened. What the Job is for - a UI that keeps painting - is
    //not what a headless run can observe.
    void cut_step(Plater *plater)
    {
        Model &model = plater->model();
        if (model.objects.empty()) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CUT CHECK FAILED - there is no object to cut";
            enter(m_spec.gcode.empty() ? Phase::Board : Phase::Slice);
            return;
        }

        ModelObject *mo = model.objects.front();
        if (mo->instances.empty()) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CUT CHECK FAILED - '" << mo->name << "' has no instance";
            enter(m_spec.gcode.empty() ? Phase::Board : Phase::Slice);
            return;
        }

        const int           instance_idx = 0;
        const BoundingBoxf3 wbb          = mo->instance_bounding_box(instance_idx);
        const Vec3d         inst_offset  = mo->instances[instance_idx]->get_offset();
        if (!wbb.defined || wbb.size().z() <= EPSILON) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CUT CHECK FAILED - '" << mo->name << "' has no height to cut";
            enter(m_spec.gcode.empty() ? Phase::Board : Phase::Slice);
            return;
        }

        //The plane, expressed the way the gizmo expresses it: a centre in the instance's own
        //space, no rotation. The region is then plane-local by construction.
        const Vec3d plane_centre(wbb.center().x(), wbb.center().y(), wbb.min.z() + m_spec.cutz * wbb.size().z());
        const Transform3d cut_matrix = Geometry::translation_transform(plane_centre - inst_offset);

        //A rectangle covering the middle `cutspan` of the footprint in X and the whole of it in
        //Y, with margin. The point of the fixture is that the walls in X pass through material
        //and the walls in Y pass through air, so the result is testable in one number: the part
        //that comes off is smaller than the whole slab above the plane.
        const double half_x = 0.5 * m_spec.cutspan * wbb.size().x();
        const double half_y = 0.5 * wbb.size().y() + 10.0;
        const CutBounds bounds = CutBounds::make_rectangle(Vec2d(-half_x, -half_y), Vec2d(half_x, half_y));

        const std::vector<CutBoundedInput> inputs = collect_bounded_cut_inputs(*mo, instance_idx);
        if (inputs.empty()) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CUT CHECK FAILED - '" << mo->name
                                     << "' has no solid volume to cut";
            enter(m_spec.gcode.empty() ? Phase::Board : Phase::Slice);
            return;
        }

        //The source volume, measured in the same space the split is measured in, so the
        //conservation check is a comparison rather than a coincidence.
        //The source's OWN open edges are reported with its volume, because a downloaded mesh is
        //routinely not closed and the volume of a mesh that is not closed is not a number - it
        //is a divergence sum over a surface with holes in it. Without this line a conservation
        //check that drifts reads as a boolean losing material, when what actually happened is
        //that corefinement returned a closed solid for an input that never had one volume.
        double source_volume = 0.0;
        size_t source_faces  = 0;
        size_t source_open   = 0;
        for (const CutBoundedInput &in : inputs) {
            if (!in.mesh)
                continue;
            TriangleMesh m(*in.mesh);
            m.transform(in.matrix, true);
            source_volume += mesh_volume(m.its);
            source_faces  += m.its.indices.size();
            source_open   += its_num_open_edges(m.its);
        }

        BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: cutting '" << mo->name << "' - " << inputs.size()
                                   << " volume(s), " << source_faces << " faces, " << source_volume
                                   << " mm3, " << source_open << " open edge(s) in the SOURCE; plane at z="
                                   << plane_centre.z() << ", region " << (2.0 * half_x) << " x " << (2.0 * half_y)
                                   << " mm";

        CutBoundedSplits splits;
        std::string      failure;
        const auto       t0 = std::chrono::steady_clock::now();
        const bool       ok = compute_bounded_splits(inputs, cut_matrix, bounds, splits, failure);
        const double     boolean_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

        if (!ok) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CUT CHECK FAILED - the boolean refused after "
                                     << boolean_ms << " ms: " << failure;
            enter(m_spec.gcode.empty() ? Phase::Board : Phase::Slice);
            return;
        }

        //THE NUMBER THAT SAYS THE BOUND DID ANYTHING. Watertight parts that add up prove the
        //boolean is sound; they say nothing about whether the region mattered. So the same
        //plane is run unbounded, through the code path the unbounded cut actually uses, and
        //the two upper volumes are compared. If they are equal the bound is decoration.
        double unbounded_upper = 0.0;
        {
            const Transform3d to_cut = cut_space_transform(cut_matrix);
            for (const CutBoundedInput &in : inputs) {
                if (!in.mesh)
                    continue;
                TriangleMesh m(*in.mesh);
                m.transform(to_cut * in.matrix, true);
                indexed_triangle_set up, lo;
                cut_mesh(m.its, 0.0f, &up, &lo);
                unbounded_upper += mesh_volume(up);
            }
        }

        bool   check_ok    = true;
        double parts_volume = 0.0;
        for (const auto &kv : splits) {
            const size_t open_up = its_num_open_edges(kv.second.upper);
            const size_t open_lo = its_num_open_edges(kv.second.lower);
            if (open_up != 0 || open_lo != 0) {
                BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CUT CHECK FAILED - volume " << kv.first
                                         << " came out open: " << open_up << " open edge(s) above, " << open_lo
                                         << " below";
                check_ok = false;
            }
            parts_volume += mesh_volume(kv.second.upper) + mesh_volume(kv.second.lower);
        }

        //One part in a thousand. Tessellation moves a volume by far less than that; a boolean
        //that lost a piece moves it by percent.
        //One part in a hundred thousand. Double summation puts the noise floor far below that,
        //so anything this catches is geometry rather than arithmetic.
        const double tolerance = std::max(1e-5 * std::abs(source_volume), 1e-6);
        if (std::abs(parts_volume - source_volume) > tolerance) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CUT CHECK FAILED - volume not conserved: source "
                                     << source_volume << " mm3, parts " << parts_volume << " mm3 (difference "
                                     << (parts_volume - source_volume) << ")";
            check_ok = false;
        }

        //Now the cut for real, through the same Cut the gizmo drives, with the geometry it has
        //just been handed. A check that stops at the meshes would never touch clone_for_cut,
        //the config copy, the paint restore or the plate placement.
        const int object_idx = 0;
        ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper |
                                              ModelObjectCutAttribute::KeepLower |
                                              ModelObjectCutAttribute::KeepPaint;
        Cut cut(mo, instance_idx, cut_matrix, attributes);
        cut.set_precomputed_splits(splits);
        const ModelObjectPtrs &new_objects = cut.perform_with_bounded_plane(bounds);

        if (!cut.failure().empty() || new_objects.size() < 2) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CUT CHECK FAILED - the cut produced "
                                     << new_objects.size() << " object(s)"
                                     << (cut.failure().empty() ? "" : (": " + cut.failure()));
            check_ok = false;
        }
        else {
            if (!m_spec.cutout.empty()) {
                boost::system::error_code ec;
                boost::filesystem::create_directories(boost::filesystem::path(m_spec.cutout), ec);
                for (size_t i = 0; i < new_objects.size(); ++i) {
                    TriangleMesh part = new_objects[i]->mesh();
                    const std::string path =
                        (boost::filesystem::path(m_spec.cutout) / ("cut-part-" + std::to_string(i + 1) + ".stl")).string();
                    if (store_stl(path.c_str(), &part, true))
                        BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: wrote " << path << " ("
                                                   << part.its.indices.size() << " faces)";
                    else
                        BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: could not write " << path;
                }
            }
            const size_t parts = new_objects.size();
            plater->apply_cut_object_to_model(object_idx, new_objects, m_spec.cutdist != 0);
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: applied " << parts << " part(s) to the model"
                                       << (m_spec.cutdist != 0 ? ", distributed to the plate" : "");
        }

        double bounded_upper = 0.0;
        for (const auto &kv : splits)
            bounded_upper += mesh_volume(kv.second.upper);
        if (bounded_upper >= unbounded_upper - std::max(1e-5 * std::abs(unbounded_upper), 1e-6)) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: CUT CHECK FAILED - the region took "
                                     << bounded_upper << " mm3 and the infinite plane at the same height would "
                                        "have taken " << unbounded_upper << " mm3; the bound did nothing";
            check_ok = false;
        }

        if (check_ok)
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: cut check passed - both parts watertight, volume "
                                          "conserved (source " << source_volume << " mm3, parts " << parts_volume
                                       << " mm3), the region took " << bounded_upper << " mm3 where the infinite "
                                          "plane would have taken " << unbounded_upper << " mm3, boolean "
                                       << boolean_ms << " ms";
        Perf::mark("driver.cut_ms", int(boolean_ms));

        enter(m_spec.gcode.empty() ? Phase::Board : Phase::Slice);
    }

    //THE PRINTER PICKER, AND THE DIALOG IT REPLACES.
    //
    //Both are exercised inside one process, on one disk-cache state, in one run, because the
    //only comparison worth reporting is a back-to-back one - a number from yesterday's run
    //against a number from today's says as much about the machine as about the code.
    //
    //The old dialog is not simulated: a real GuideFrame is constructed the way run_wizard
    //constructs it, and its own profile walk is what gets timed.
    void picker_step()
    {
        if (m_spec.picker == 0 && m_spec.walk == 0) {
            enter(Phase::Finish);
            return;
        }

        //--- the inherited web guide
        if (m_spec.walk > 0 && !m_guide_done) {
            if (m_guide == nullptr) {
                m_guide = new GuideFrame(&wxGetApp(), wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU);
                m_guide->SetStartPage(m_spec.walk >= 2 ? GuideFrame::BBL_FILAMENT_ONLY
                                                       : GuideFrame::BBL_MODELS_ONLY);
                BOOST_LOG_TRIVIAL(warning)
                    << "PETKOS_PERF_SCRIPT: PICKER opened the inherited web guide ("
                    << (m_spec.walk >= 2 ? "full walk, as shipped" : "printer page, filaments skipped")
                    << "), waiting for its profile walk";
                m_tick = 0;
                return;
            }
            if (!m_guide->walk_finished()) {
                //Idle ticks, not wall clock, so a stalled event loop is what this notices.
                if (++m_tick > 200000) {
                    BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: PICKER FAILED - the guide's profile "
                                                "walk never finished, so there is no comparison to report";
                    m_guide->Destroy();
                    m_guide     = nullptr;
                    m_guide_done = true;
                }
                return;
            }
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: PICKER old=web-guide files_parsed="
                                       << m_guide->files_parsed()
                                       << " walk_ms=" << int(m_guide->walk_ms());
            //Destroy(), not delete: the walk queues a CallAfter that touches this object, and a
            //deferred destroy is processed after that idle event rather than before it.
            m_guide->Destroy();
            m_guide      = nullptr;
            m_guide_done = true;
            m_tick       = 0;
            return;
        }

        //--- the native picker
        if (m_spec.picker != 0) {
            const int64_t t0 = Perf::now();
            auto dlg = std::make_unique<PrinterPickerDialog>(wxGetApp().mainframe);
            const double build_ms = Perf::ticks_to_ms(Perf::now() - t0);
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: PICKER new=native models="
                                       << dlg->model_count() << " files_parsed=0"
                                       << " build_ms=" << build_ms;

            bool ok = true;
            if (!m_spec.install.empty()) {
                std::vector<std::string> f;
                boost::split(f, m_spec.install, boost::is_any_of("/"));
                if (f.size() != 3) {
                    BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: PICKER FAILED - install= wants "
                                                "Vendor/Model/Nozzle, got '" << m_spec.install << "'";
                    ok = false;
                } else if (!dlg->install_headless(f[0], f[1], f[2])) {
                    BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: PICKER FAILED - the index has no "
                                             << m_spec.install;
                    ok = false;
                }
            }
            if (ok && !m_spec.uninstall.empty()) {
                std::vector<std::string> f;
                boost::split(f, m_spec.uninstall, boost::is_any_of("/"));
                if (f.size() != 2 || !dlg->uninstall_headless(f[0], f[1])) {
                    BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: PICKER FAILED - cannot remove "
                                             << m_spec.uninstall;
                    ok = false;
                }
            }
            if (ok && (!m_spec.install.empty() || !m_spec.uninstall.empty())) {
                const int64_t c0 = Perf::now();
                if (!dlg->commit()) {
                    BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: PICKER FAILED - commit refused";
                    ok = false;
                } else {
                    BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: PICKER committed in "
                                               << int(Perf::ticks_to_ms(Perf::now() - c0)) << " ms";
                    //The check is not "the dialog said yes", it is that AppConfig now agrees.
                    if (!m_spec.install.empty()) {
                        std::vector<std::string> f;
                        boost::split(f, m_spec.install, boost::is_any_of("/"));
                        const bool present = wxGetApp().app_config->get_variant(f[0], f[1], f[2]);
                        BOOST_LOG_TRIVIAL(warning)
                            << "PETKOS_PERF_SCRIPT: PICKER CHECK app_config has " << m_spec.install
                            << ": " << (present ? "passed" : "FAILED");
                        //And that a printer preset for it is actually visible, which is the
                        //thing the user is trying to get - an AppConfig line nobody can select
                        //would pass the check above and still be useless.
                        const std::string want = f[1] + " " + f[2] + " nozzle";
                        const Preset      *p    = wxGetApp().preset_bundle->printers.find_preset(want, false);
                        BOOST_LOG_TRIVIAL(warning)
                            << "PETKOS_PERF_SCRIPT: PICKER CHECK preset '" << want << "' visible: "
                            << (p != nullptr && p->is_visible ? "passed" : "FAILED");
                    }
                }
            }
            if (!ok)
                Perf::mark("driver.picker_failed", 1);
        }

        enter(Phase::Finish);
    }

    //SLICE IT, AND KEEP WHAT CAME OUT.
    //
    //Every check above this one is the app marking its own homework. They all passed on the
    //build that quoted 21h03m for a 6h45m plate and reported 0.00 g, because a plate pinned
    //to a placeholder preset is a COMPLETE plate - it is simply pinned to the wrong thing.
    //The emitted G-code is the only artefact that cannot be wrong about what was used, so
    //the run produces one and the harness reads it without the app.
    //1-based spec.plate, clamped to "plate 1" when unset.
    int slice_plate_index() const { return m_spec.plate > 0 ? m_spec.plate - 1 : 0; }

    void slice_step(Plater *plater)
    {
        const int  idx   = slice_plate_index();
        PartPlate *plate = plater->get_partplate_list().get_plate(idx);
        if (plate == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SLICE SKIPPED - there is no plate " << (idx + 1);
            enter(Phase::Board);
            return;
        }
        plater->select_plate(idx);
        BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: slicing plate " << (idx + 1) << " on '"
                                   << plate->get_printer_preset_name() << "' with process '"
                                   << plate->get_print_preset_name() << "'";
        plater->reslice();
        m_slice_started   = std::chrono::steady_clock::now();
        m_slice_requested = plater->is_background_process_slicing();
        if (!m_slice_requested)
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: reslice() returned without a running background "
                                          "process; either the plate was already sliced or the slice never started";
        m_tick = 0;
        enter(Phase::SliceWait);
    }

    void slice_wait_step(Plater *plater)
    {
        //BOUNDED ON THE WALL CLOCK, NOT ON IDLE TICKS. Idle events fire as fast as the loop
        //turns, so a tick count measures how busy the UI thread is rather than how long
        //slicing has had: the same bound is minutes on an idle app and seconds on a busy
        //one, and a big model "times out" while still working. A harness that can hang is a
        //harness that gets killed, and a killed process writes no CSV and no verdict, so
        //there is still a bound - twenty minutes of real time.
        static constexpr auto slice_bound = std::chrono::minutes(20);
        const auto elapsed = std::chrono::steady_clock::now() - m_slice_started;
        const bool timed_out = elapsed > slice_bound;
        if (plater->is_background_process_slicing()) {
            m_slice_requested = true;   //it is running, whatever reslice() reported
            if (!timed_out)
                return;
        }
        const long long secs = (long long) std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        PartPlate *plate = plater->get_partplate_list().get_plate(slice_plate_index());
        //THREE DIFFERENT FAILURES, SAID APART. They used to share one sentence because one
        //test answered all three, which is the test's convenience rather than the reader's:
        //a slice that never started, a slice still running when the bound expired, and a
        //slice that finished and produced nothing valid are three different things to go and
        //look at.
        if (plate == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SLICE FAILED - there is no plate 1 to take a result from";
            enter(Phase::Board);
            return;
        }
        if (timed_out && plater->is_background_process_slicing()) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SLICE TIMED OUT - plate 1 was still slicing after "
                                     << secs << "s (bound is " << slice_bound.count() << " minutes)";
            enter(Phase::Board);
            return;
        }
        if (!m_slice_requested && !plate->is_slice_result_valid()) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SLICE NEVER STARTED - no background slice ran for plate 1 "
                                        "and it has no result from before; check that the plate resolves and has "
                                        "printable instances";
            enter(Phase::Board);
            return;
        }
        if (!plate->is_slice_result_valid()) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SLICE COMPLETED WITHOUT A VALID RESULT - plate 1 finished "
                                        "after " << secs << "s and has nothing that can be exported";
            enter(Phase::Board);
            return;
        }

        //The result lives at a temp path the plate owns. Copying rather than moving, because
        //the plate still needs it: the retained slice is a per-plate asset in this fork.
        const std::string src = plate->get_tmp_gcode_path();
        boost::system::error_code ec;
        if (src.empty() || !boost::filesystem::exists(src)) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SLICE FAILED - plate 1 reports a valid result but its "
                                        "G-code is not at '" << src << "'";
            enter(Phase::Board);
            return;
        }
        boost::filesystem::copy_file(src, boost::filesystem::path(m_spec.gcode),
                                     boost::filesystem::copy_option::overwrite_if_exists, ec);
        if (ec)
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: SLICE FAILED - could not copy the G-code to "
                                     << m_spec.gcode << ": " << ec.message();
        else
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: wrote " << m_spec.gcode
                                       << " - read it with tools/petkos-gcode-check.py";
        enter(Phase::Board);
    }

    void orbit_step(Plater *plater, GLCanvas3D *canvas)
    {
        //A real orbit is a drag, which is a stream of small rotations. 0.01 rad a frame is
        //about the speed a hand moves a mouse, and keeps the whole scene on screen so the
        //frame cost is not accidentally measured against a view that culled everything.
        plater->get_camera().rotate_on_sphere(0.012, 0.0, false);
        canvas->set_as_dirty();
    }

    //Where a world point lands on the canvas, in the pixel space the raycaster reads
    //mouse positions in. Returns false when the point is off screen, which is a skip
    //rather than a failure.
    static bool project_to_canvas(const Vec3d &world, Vec2d &out)
    {
        const Camera &camera = wxGetApp().plater()->get_camera();
        const Vec4d clip = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix() *
                           Vec4d(world.x(), world.y(), world.z(), 1.0);
        if (clip.w() == 0.0)
            return false;
        const Vec3d ndc(clip.x() / clip.w(), clip.y() / clip.w(), clip.z() / clip.w());
        if (ndc.x() < -1.0 || ndc.x() > 1.0 || ndc.y() < -1.0 || ndc.y() > 1.0)
            return false;
        const std::array<int, 4> &vp = camera.get_viewport();
        out = Vec2d(vp[0] + (ndc.x() * 0.5 + 0.5) * vp[2], vp[1] + (0.5 - ndc.y() * 0.5) * vp[3]);
        return true;
    }

    //Point at every plate in turn and check the app picks THAT plate. This is the half of
    //correctness a screenshot cannot show: a plate draws through its own frame and is
    //picked through the same frame, and the two can disagree silently - a click would
    //select the wrong plate and nothing on screen would look wrong. Run after the assign
    //phase on purpose, because that is what moves plates around.
    void pick_step(Plater *plater, GLCanvas3D *canvas)
    {
        PartPlateList &list  = plater->get_partplate_list();
        const int      count = list.get_plate_count();

        if (m_tick == 0) {
            //straight down, so no plate's own cube can stand in front of another plate,
            //and wide enough that every plate is on screen to be pointed at
            canvas->select_view("top");
            canvas->zoom_to_volumes();
            canvas->set_as_dirty();
            Perf::mark("pick.begin", count);
        }
        //the camera change needs a frame to reach the viewport the projection reads
        if (++m_tick < 12)
            return;

        if (m_done >= count) {
            Perf::mark("pick.ok", m_pick_ok);
            Perf::mark("pick.miss", m_pick_miss);
            Perf::mark("pick.offscreen", m_pick_off);
            if (m_pick_miss > 0)
                BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: PICK CHECK FAILED - " << m_pick_miss
                                         << " of " << count << " plates picked the wrong plate";
            else
                BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: pick check passed - " << m_pick_ok
                                           << " of " << count << " plates picked themselves ("
                                           << m_pick_off << " off screen)";
            enter(m_spec.scope > 0 ? Phase::Scope : (m_spec.context > 0 ? Phase::Context : next_after_context()));
            return;
        }

        PartPlate *plate = list.get_plate(m_done);
        if (plate == nullptr) {
            ++m_done;
            return;
        }

        //a quarter of the bed away from the middle: clear of the cube parked at the centre,
        //and clear of the icon column and the labels along the edges
        const BoundingBoxf3 &bb   = plate->get_bounding_box();
        const Vec3d          size = bb.size();
        const Vec3d target(bb.center().x() + 0.25 * size.x(), bb.center().y() - 0.25 * size.y(), 0.0);

        Vec2d at;
        if (!project_to_canvas(target, at)) {
            ++m_pick_off;
            ++m_done;
            return;
        }

        const SceneRaycaster::HitResult hit = canvas->pick_at(at);
        const int picked = (hit.is_valid() && hit.type == SceneRaycaster::EType::Bed)
                               ? hit.raycaster_id / PartPlate::GRABBER_COUNT
                               : -1;
        if (picked == m_done) {
            ++m_pick_ok;
        } else {
            ++m_pick_miss;
            //name both plates: which one was pointed at and which one answered is the whole
            //diagnosis when a frame and its raycaster have drifted apart
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: pointing at plate " << m_done
                                     << " picked plate " << picked << " (raycaster id "
                                     << hit.raycaster_id << ")";
            Perf::mark("pick.mismatch", m_done * 1000 + (picked + 1));
        }
        ++m_done;
    }

    void build_plates(Plater *plater)
    {
        if (m_spec.plates <= 0)
            return;
        PartPlateList &list = plater->get_partplate_list();
        //A PLATE IS SEEDED BY WHOEVER ASKED FOR IT, and this driver is asking. create_plate
        //deliberately seeds nothing, so a run that built 36 plates built 35 that named no
        //printer - and then measured, and then asserted a context check against them. The
        //holder is the plate the app opened on, exactly as it is for "add a plate".
        PlateSlicingContext seed;
        if (const PartPlate *current = list.get_curr_plate())
            seed = current->get_slicing_context();
        //Stop when the list stops growing, not when the target is reached. create_plate refuses
        //past MAX_PLATE_COUNT and says so by returning without adding one, so a target above
        //that turns a loop on the count into a hang - which is exactly what a mistyped argument
        //produced the first time this ran.
        for (int previous = -1; list.get_plate_count() < m_spec.plates &&
                                list.get_plate_count() > previous;) {
            previous = list.get_plate_count();
            list.create_plate(true);
        }
        if (seed.is_complete())
            list.complete_plate_contexts(seed);
        else
            list.complete_plate_contexts();
        list.apply_printer_assignments();
        if (list.get_plate_count() < m_spec.plates)
            BOOST_LOG_TRIVIAL(warning)
                << "PETKOS_PERF_SCRIPT: asked for " << m_spec.plates << " plates, the list caps at "
                << list.get_plate_count() << "; measuring that instead";

        //One cube per plate. A scene of empty plates would measure the plate chrome alone
        //and flatter every result, because an empty plate skips most of what a real
        //project makes the renderer do.
        Model &model = plater->model();
        for (int i = 0; i < list.get_plate_count(); ++i) {
            PartPlate *plate = list.get_plate(i);
            if (plate == nullptr)
                continue;
            const Vec3d origin = plate->get_origin();
            const Vec2d size   = plate->get_size();

            ModelObject *obj = model.add_object();
            obj->name        = "petkos-perf-cube";
            obj->add_volume(TriangleMesh(its_make_cube(40.0, 40.0, 40.0)));
            ModelInstance *inst = obj->add_instance();
            inst->set_offset(Vec3d(origin.x() + size.x() * 0.5, origin.y() + size.y() * 0.5, 0.0));
            obj->ensure_on_bed();
        }
        list.reload_all_objects();
        plater->update();
    }

    //Two real printers, because that is what the assign phase writes. The phase alternates
    //between them, so both write paths are exercised, and neither of them is an empty name -
    //an empty name is not a value this app accepts, and driving one through set_plate_printer
    //measured a refusal rather than an assignment.
    void collect_assign_presets()
    {
        m_assign_presets.clear();
        PresetBundle *bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: ASSIGN FAILED - no preset bundle";
            return;
        }

        //A name given on the command line is checked before anything is built on it. An
        //unknown one used to be assigned anyway, refused deep inside the write path, and
        //reported as a fast assign phase.
        if (!m_spec.printer.empty()) {
            const Preset *named = bundle->printers.find_preset(m_spec.printer, false);
            if (named == nullptr || !named->is_visible || named->is_default) {
                BOOST_LOG_TRIVIAL(error) << "PETKOS_PERF_SCRIPT: ASSIGN FAILED - no visible printer preset named '"
                                         << m_spec.printer << "'";
                return;
            }
            m_assign_presets.push_back(named->name);
        }

        const std::string current = bundle->printers.get_selected_preset_name();
        //The current printer is one of the two, so a single extra installed preset is enough
        //to alternate between two real machines.
        if (m_assign_presets.empty() && !current.empty())
            m_assign_presets.push_back(current);
        for (const Preset &p : bundle->printers) {
            if (m_assign_presets.size() >= 2)
                break;
            if (!p.is_visible || p.is_default || p.printer_technology() != ptFFF)
                continue;
            if (std::find(m_assign_presets.begin(), m_assign_presets.end(), p.name) != m_assign_presets.end())
                continue;
            m_assign_presets.push_back(p.name);
        }
        //Say what was collected, always. This function reported only its failures, so the ordinary
        //case produced no line at all - and "no ASSIGN line in the log" then meant either that it
        //had worked perfectly or that the phase had done nothing, with no way to tell which.
        if (m_assign_presets.empty()) {
            BOOST_LOG_TRIVIAL(error)
                << "PETKOS_PERF_SCRIPT: ASSIGN FAILED - no visible printer preset to assign; the assign phase "
                   "will be skipped and its row will be missing from the summary";
            return;
        }
        std::string names;
        for (const std::string &name : m_assign_presets)
            names += (names.empty() ? "" : ", ") + ("'" + name + "'");
        BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: ASSIGN will alternate between " << names;
        if (m_assign_presets.size() < 2)
            BOOST_LOG_TRIVIAL(warning)
                << "PETKOS_PERF_SCRIPT: fewer than two visible printer presets are installed, so the assign "
                   "phase can only write one machine; a plate already on it cannot be reassigned at all, and "
                   "any iteration that lands on one will say ASSIGN SKIPPED";
    }

    wxWindow *board(Plater *plater)
    {
        Sidebar &sidebar = plater->sidebar();
        return sidebar.get_plate_board_window();
    }

    void board_motion(Plater *plater)
    {
        wxWindow *b = board(plater);
        if (b == nullptr || !b->IsShownOnScreen())
            return;
        const wxSize sz = b->GetClientSize();
        if (sz.GetHeight() <= 0)
            return;
        //A slow sweep down the rows, which is what makes hover state change on nearly
        //every event - the case a hover repaint has to survive.
        m_board_y = (m_board_y + 3) % sz.GetHeight();
        Perf::begin_interaction(Perf::Interaction::BoardHover, m_board_y);
        wxMouseEvent evt(wxEVT_MOTION);
        evt.SetPosition(wxPoint(sz.GetWidth() / 2, m_board_y));
        evt.SetEventObject(b);
        b->GetEventHandler()->ProcessEvent(evt);
    }

    void board_drag(Plater *plater)
    {
        wxWindow *b = board(plater);
        if (b == nullptr || !b->IsShownOnScreen())
            return;
        const wxSize sz = b->GetClientSize();
        if (sz.GetHeight() <= 40)
            return;
        const int x = sz.GetWidth() / 2;
        Perf::begin_interaction(Perf::Interaction::BoardDrag, 0);

        wxMouseEvent down(wxEVT_LEFT_DOWN);
        down.SetPosition(wxPoint(x, 30));
        down.SetEventObject(b);
        b->GetEventHandler()->ProcessEvent(down);

        for (int y = 30; y < sz.GetHeight() - 5; y += 7) {
            wxMouseEvent move(wxEVT_MOTION);
            move.SetPosition(wxPoint(x, y));
            move.SetLeftDown(true);
            move.SetEventObject(b);
            b->GetEventHandler()->ProcessEvent(move);
        }

        wxMouseEvent up(wxEVT_LEFT_UP);
        up.SetPosition(wxPoint(x, sz.GetHeight() - 5));
        up.SetEventObject(b);
        b->GetEventHandler()->ProcessEvent(up);
    }

    Spec                     m_spec;
    Phase                    m_phase   = Phase::Settle0;
    bool                     m_saved   = false;
    //The web guide under measurement, and whether its half of the comparison is already done.
    GuideFrame              *m_guide      = nullptr;
    bool                     m_guide_done = false;
    int                      m_tick    = 0;
    int                      m_done    = 0;
    int                      m_quiet   = 0;
    int                      m_board_y = 0;
    int                      m_pick_ok = 0;
    int                      m_pick_miss = 0;
    int                      m_pick_off = 0;
    //How many assign iterations actually changed a plate's machine, as against how many ran. The
    //two were assumed equal, and a run in which they were 0 and 1 read as a phase that had worked.
    int                      m_assigned = 0;
    std::vector<std::string> m_assign_presets;
    //When the slice this run asked for actually started. The wait below is bounded on the
    //wall clock rather than on idle ticks, so the bound means the same thing whatever the
    //UI thread is doing. Unset means no slice was ever started.
    std::chrono::steady_clock::time_point m_slice_started {};
    bool                     m_slice_requested = false;
};

} // namespace

//=============================================================================================
// PetkosOrca: the acceptance run.
//
// The latency work could be measured and the per-plate work could be unit-checked, but neither
// answers the question the user actually asked, which is whether a real job can be got through
// this application without wasting his time. So this drives one: a five-part model, split across
// two plates, on two DIFFERENT printers, in two different materials, sliced and saved.
//
// It uses the application's own APIs and nothing else - load_files, add_to_plate, set_plate_printer,
// set_plate_filaments, reslice, export_3mf. Constructing the 3MF externally would have been easier
// and would have proved nothing, because the thing under test is the app, not the file format.
//
//   PETKOS_ACCEPT="project=<path>,printerA=<name>,printerB=<name>,filamentA=<name>,filamentB=<name>,out=<path>"
//
// Every stage says PASS or FAIL by name. A stage that cannot run says so and the run stops there
// rather than continuing to a green summary it has not earned.
//=============================================================================================

struct AcceptSpec
{
    std::string project;
    std::string printerA, printerB;
    std::string filamentA, filamentB;
    //The two spools, as colour and finish. Silver is grey PLUS metallic, which is the whole
    //reason filament_finish exists - #C0C0C0 on its own is just light grey.
    std::string colourA { "#C0C0C0" }, colourB { "#FFF144" };
    std::string finishA { "metallic" }, finishB { "standard" };
    //Deliberately DIFFERENT process settings per plate. This is the thing per-plate process
    //exists for and the only way to show it works: two plates, one project, one process preset
    //between them, and two different answers in the G-code.
    int         wallsA { 2 }, wallsB { 6 };
    int         infillA { 10 }, infillB { 45 };
    std::string out;
};

static AcceptSpec parse_accept(const std::string &s)
{
    AcceptSpec spec;
    std::vector<std::string> parts;
    boost::split(parts, s, boost::is_any_of(","), boost::token_compress_on);
    for (const std::string &part : parts) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = part.substr(0, eq);
        const std::string val = part.substr(eq + 1);
        if (key == "project")        spec.project = val;
        else if (key == "printerA")  spec.printerA = val;
        else if (key == "printerB")  spec.printerB = val;
        else if (key == "filamentA") spec.filamentA = val;
        else if (key == "filamentB") spec.filamentB = val;
        else if (key == "colourA")   spec.colourA = val;
        else if (key == "colourB")   spec.colourB = val;
        else if (key == "finishA")   spec.finishA = val;
        else if (key == "finishB")   spec.finishB = val;
        else if (key == "wallsA")    spec.wallsA = std::atoi(val.c_str());
        else if (key == "wallsB")    spec.wallsB = std::atoi(val.c_str());
        else if (key == "infillA")   spec.infillA = std::atoi(val.c_str());
        else if (key == "infillB")   spec.infillB = std::atoi(val.c_str());
        else if (key == "out")       spec.out = val;
    }
    return spec;
}

class AcceptDriver : public wxEvtHandler
{
public:
    explicit AcceptDriver(AcceptSpec spec) : m_spec(std::move(spec))
    {
        wxGetApp().mainframe->PushEventHandler(this);
        Bind(wxEVT_IDLE, &AcceptDriver::on_idle, this);
    }

private:
    enum class Step { Settle, Load, Materials, Split, Assign, Process, SliceA, SliceB, Save, Report, Done };

    void fail(const std::string &stage, const std::string &why)
    {
        BOOST_LOG_TRIVIAL(error) << "PETKOS_ACCEPT: FAIL " << stage << " - " << why;
        m_failed = true;
        m_step   = Step::Report;
    }

    void on_idle(wxIdleEvent &evt)
    {
        Plater *plater = wxGetApp().plater();
        if (plater == nullptr || !plater->is_initialized()) {
            evt.RequestMore();
            return;
        }

        switch (m_step) {
        case Step::Settle:
            //The app opens on Home, where the canvas does not draw and the plater is not the
            //thing on screen. A run that forgets this measures an idle app and calls it fast.
            if (m_tick == 0)
                wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
            if (++m_tick >= 30)
                m_step = Step::Load;
            evt.RequestMore();
            break;

        case Step::Load: {
            if (m_spec.project.empty()) { fail("load", "no project= given"); evt.RequestMore(); break; }
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: loading " << m_spec.project;
            const std::vector<size_t> loaded = plater->load_files(std::vector<std::string>{m_spec.project});
            m_objects = (int) wxGetApp().model().objects.size();
            if (m_objects <= 0) { fail("load", "the project produced no objects"); evt.RequestMore(); break; }
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: PASS load - " << m_objects << " object(s), "
                                       << loaded.size() << " file(s)";
            m_step = Step::Materials;
            m_tick = 0;
            evt.RequestMore();
            break;
        }

        case Step::Materials: {
            //Two spools, because the job is two colours. The project arrives with one slot, so the
            //extruder numbers the split step is about to assign would have nothing to point at -
            //everything would quietly come out in slot 1's colour, which is exactly the silent
            //yes this fork forbids.
            PresetBundle *bundle = wxGetApp().preset_bundle;
            if (bundle == nullptr) { fail("materials", "no preset bundle"); evt.RequestMore(); break; }

            bundle->set_num_filaments(2, std::vector<std::string>{m_spec.colourA, m_spec.colourB});

            //set_num_filaments applies its colour list to the slots it ADDS, so growing 1 -> 2 left
            //slot 1 with whatever the project came with and put the first requested colour in slot
            //2. The finishes below are written by index, so the two fell out of step and the job
            //came out with a yellow slot marked metallic. Both are stated by index here, so colour
            //and finish cannot disagree about which spool is which.
            if (auto *colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour"))
                colours->values = { m_spec.colourA, m_spec.colourB };

            //Finish sits beside colour in the project config, per slot, because two slots can hold
            //the same preset and differ only in what is on the spool.
            static const std::map<std::string, FilamentFinish> names {
                {"standard", ffStandard}, {"matte", ffMatte}, {"glossy", ffGlossy},
                {"silk", ffSilk}, {"metallic", ffMetallic} };
            auto finish_of = [](const std::string &n) {
                const auto it = names.find(n);
                return it == names.end() ? ffStandard : it->second;
            };
            if (auto *finishes = bundle->project_config.option<ConfigOptionEnumsGeneric>("filament_finish")) {
                finishes->values = { (int) finish_of(m_spec.finishA), (int) finish_of(m_spec.finishB) };
            } else {
                fail("materials", "filament_finish is not a project option");
                evt.RequestMore();
                break;
            }

            //Pushed the way the colour picker pushes its own change, so everything downstream -
            //the volume colours, the sidebar, the wipe tower - hears about it once, through the
            //path that already exists.
            DynamicPrintConfig changed = bundle->project_config;
            wxGetApp().get_tab(Preset::TYPE_PRINTER)->load_config(changed);
            plater->on_config_change(changed);

            const auto *colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            const size_t slots = colours != nullptr ? colours->values.size() : 0;
            if (slots < 2) { fail("materials", "only " + std::to_string(slots) + " filament slot(s)"); evt.RequestMore(); break; }
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: PASS materials - slot 1 " << m_spec.colourA
                                       << " " << m_spec.finishA << ", slot 2 " << m_spec.colourB
                                       << " " << m_spec.finishB;
            m_step = Step::Split;
            evt.RequestMore();
            break;
        }

        case Step::Split: {
            //Half the model on each plate, and each half in its own material slot. Which objects
            //make up "half" is a judgement about the model, not about the app, so it is the plain
            //split and it is stated rather than dressed up as anatomy.
            PartPlateList &list = plater->get_partplate_list();
            while (list.get_plate_count() < 2) {
                const int before = list.get_plate_count();
                list.create_plate(false);
                if (list.get_plate_count() == before) { fail("split", "cannot create a second plate"); break; }
            }
            if (m_failed) { evt.RequestMore(); break; }

            const int half = (m_objects + 1) / 2;
            for (int i = 0; i < m_objects; ++i) {
                ModelObject *object = wxGetApp().model().objects[i];
                const int    plate  = i < half ? 0 : 1;
                const int    slot   = plate + 1;   // extruder 1 on plate 1, extruder 2 on plate 2
                object->config.set_key_value("extruder", new ConfigOptionInt(slot));
                for (int inst = 0; inst < (int) object->instances.size(); ++inst)
                    list.add_to_plate(i, inst, plate);
            }
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: PASS split - " << half << " object(s) on plate 1 in slot 1, "
                                       << (m_objects - half) << " on plate 2 in slot 2";
            m_step = Step::Assign;
            evt.RequestMore();
            break;
        }

        case Step::Assign: {
            if (!m_spec.printerA.empty()) plater->set_plate_printer(0, m_spec.printerA);
            if (!m_spec.printerB.empty()) plater->set_plate_printer(1, m_spec.printerB);
            if (!m_spec.filamentA.empty()) plater->set_plate_filaments(0, {m_spec.filamentA});
            if (!m_spec.filamentB.empty()) plater->set_plate_filaments(1, {m_spec.filamentB});

            //The point of the whole exercise: two plates that resolve to two DIFFERENT machines.
            //Asking the resolver is the only honest way to check it, because that is what slicing
            //reads - a label in the sidebar has been wrong about this before.
            PartPlateList &list = plater->get_partplate_list();
            std::string a_name, b_name, error;
            ResolvedPlateSlicingConfig resolved;
            //The resolved NAME, not the preset pointer: the pointer is a view onto a buffer that
            //any preset selection rewrites. See ResolvedPlateSlicingConfig.
            if (plater->resolve_plate_slicing_config(list.get_plate(0), resolved, error) &&
                !resolved.printer_preset_name.empty())
                a_name = resolved.printer_preset_name;
            else
                fail("assign", "plate 1 does not resolve: " + error);
            if (!m_failed) {
                if (plater->resolve_plate_slicing_config(list.get_plate(1), resolved, error) &&
                    !resolved.printer_preset_name.empty())
                    b_name = resolved.printer_preset_name;
                else
                    fail("assign", "plate 2 does not resolve: " + error);
            }
            if (!m_failed && a_name == b_name)
                fail("assign", "both plates resolved to the same machine '" + a_name + "'");
            if (!m_failed) {
                BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: PASS assign - plate 1 on '" << a_name
                                           << "', plate 2 on '" << b_name << "'";
                m_step = Step::Process;
                m_tick = 0;
            }
            evt.RequestMore();
            break;
        }

        case Step::Process: {
            //Per-plate process overrides, written the way TabPrintPlate::on_value_change writes
            //them - straight onto the plate's own config. Both plates still SHARE one process
            //preset; what differs is what each plate says on top of it. Before this existed those
            //values could only be typed into the project's preset, which a reassigned plate does
            //not use, so they reached nothing at all.
            PartPlateList &list = plater->get_partplate_list();
            const int walls[2]  = { m_spec.wallsA, m_spec.wallsB };
            const int infill[2] = { m_spec.infillA, m_spec.infillB };
            for (int i = 0; i < 2; ++i) {
                PartPlate *plate = list.get_plate(i);
                if (plate == nullptr) { fail("process", "no plate " + std::to_string(i + 1)); break; }
                plate->config()->set_key_value("wall_loops", new ConfigOptionInt(walls[i]));
                plate->config()->set_key_value("sparse_infill_density", new ConfigOptionPercent(infill[i]));
            }
            if (m_failed) { evt.RequestMore(); break; }

            //Read back through the resolver, because that is what slicing reads. A value sitting
            //in a config nobody composes is exactly the failure this whole change is about.
            bool ok = true;
            for (int i = 0; i < 2; ++i) {
                ResolvedPlateSlicingConfig resolved;
                std::string                error;
                if (!plater->resolve_plate_slicing_config(list.get_plate(i), resolved, error)) {
                    fail("process", "plate " + std::to_string(i + 1) + " does not resolve: " + error);
                    ok = false;
                    break;
                }
                const auto *w = resolved.config.option<ConfigOptionInt>("wall_loops");
                const auto *d = resolved.config.option<ConfigOptionPercent>("sparse_infill_density");
                const int   gw = w != nullptr ? w->value : -1;
                const int   gd = d != nullptr ? (int) d->value : -1;
                if (gw != walls[i] || gd != infill[i]) {
                    fail("process", "plate " + std::to_string(i + 1) + " would slice with wall_loops=" +
                         std::to_string(gw) + ", infill=" + std::to_string(gd) + "%, not " +
                         std::to_string(walls[i]) + "/" + std::to_string(infill[i]) + "%");
                    ok = false;
                    break;
                }
                BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: plate " << (i + 1) << " will slice with wall_loops="
                                           << gw << ", sparse_infill_density=" << gd << "%";
            }
            if (ok) {
                BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: PASS process - two plates, one preset, "
                                              "different settings each";
                m_step = Step::SliceA;
                m_tick = 0;
            }
            evt.RequestMore();
            break;
        }

        case Step::SliceA:
        case Step::SliceB: {
            const int      index = m_step == Step::SliceA ? 0 : 1;
            PartPlateList &list  = plater->get_partplate_list();
            PartPlate     *plate = list.get_plate(index);
            if (plate == nullptr) { fail("slice", "no plate to slice"); evt.RequestMore(); break; }

            //An empty plate is not a failure and never becomes valid, so waiting for it is waiting
            //forever. Say it was skipped and why, rather than reporting a slice that did not happen.
            if (plate->instance_count() == 0) {
                BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: skip slice - plate " << (index + 1)
                                           << " has nothing on it";
                m_step = m_step == Step::SliceA ? Step::SliceB : Step::Save;
                m_tick = 0;
                evt.RequestMore();
                break;
            }

            if (m_tick == 0) {
                plater->select_plate(index);
                plater->reslice();
                m_slice_started = std::chrono::steady_clock::now();
                BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: slicing plate " << (index + 1) << "...";
            }
            ++m_tick;

            if (plate->is_slice_result_valid()) {
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - m_slice_started).count();
                BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: PASS slice - plate " << (index + 1)
                                           << " sliced in " << secs << "s";
                m_step = m_step == Step::SliceA ? Step::SliceB : Step::Save;
                m_tick = 0;
            } else {
                //Wall clock, not an idle count. Idle events fire as fast as the loop turns, so a
                //count is a measure of how busy the UI thread is rather than of how long slicing
                //has had - and a big model would "time out" in seconds while still working.
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - m_slice_started).count();
                if (secs > 900)
                    fail("slice", "plate " + std::to_string(index + 1) + " did not finish slicing in 15 minutes");
            }
            evt.RequestMore();
            break;
        }

        case Step::Save: {
            if (m_spec.out.empty()) { fail("save", "no out= given"); evt.RequestMore(); break; }
            const boost::filesystem::path out(m_spec.out);
            boost::system::error_code ec;
            boost::filesystem::create_directories(out.parent_path(), ec);
            //WithGcode: a saved project that drops the slice it just made would hand the user the
            //work again, which is the whole complaint this run exists to answer.
            const int rc = plater->export_3mf(out, SaveStrategy::SplitModel | SaveStrategy::WithGcode);
            if (rc < 0 || !boost::filesystem::exists(out))
                fail("save", "export_3mf returned " + std::to_string(rc));
            else
                BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: PASS save - " << out.string() << " ("
                                           << boost::filesystem::file_size(out) << " bytes)";
            if (!m_failed)
                m_step = Step::Report;
            evt.RequestMore();
            break;
        }

        case Step::Report:
            BOOST_LOG_TRIVIAL(warning) << (m_failed ? "PETKOS_ACCEPT: RUN FAILED" : "PETKOS_ACCEPT: RUN PASSED");
            m_step = Step::Done;
            if (wxGetApp().mainframe != nullptr)
                wxGetApp().mainframe->Close(true);
            break;

        case Step::Done:
            break;
        }
    }

    AcceptSpec m_spec;
    Step       m_step    { Step::Settle };
    int        m_tick    { 0 };
    int        m_objects { 0 };
    bool       m_failed  { false };
    std::chrono::steady_clock::time_point m_slice_started {};
};

void petkos_acceptance_start()
{
    const char *env = std::getenv("PETKOS_ACCEPT");
    if (env == nullptr || env[0] == '\0')
        return;
    BOOST_LOG_TRIVIAL(warning) << "PETKOS_ACCEPT: starting acceptance run '" << env << "'";
    new AcceptDriver(parse_accept(env));
}

void petkos_perf_driver_start()
{
    const char *spec_env = std::getenv("PETKOS_PERF_SCRIPT");
    if (spec_env == nullptr || spec_env[0] == '\0')
        return;

    if (!Perf::enabled()) {
        //A run that measures nothing is indistinguishable from a fast one, so it is refused
        //rather than allowed to produce a reassuring empty file.
        BOOST_LOG_TRIVIAL(error)
            << "PETKOS_PERF_SCRIPT is set but PETKOS_PERF is not; refusing to run a scripted "
               "pass that would record nothing";
        return;
    }

    BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: starting scripted run '" << spec_env << "'";
    //Owned by the main frame's handler chain for the life of the app; the run ends by
    //closing the frame, which is also what tears this down.
    new PerfDriver(parse_spec(spec_env));
}

}} // namespace Slic3r::GUI
