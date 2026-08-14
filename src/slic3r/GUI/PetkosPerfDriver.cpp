#include "PetkosPerfDriver.hpp"
#include "PetkosPerf.hpp"

#include <chrono>
#include <map>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>

#include <wx/event.h>
#include <wx/timer.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
//Tab: the acceptance run pushes a filament-colour change the way the colour picker does, which
//goes through the printer tab as well as the plater.
#include "Tab.hpp"

namespace Slic3r { namespace GUI {

namespace {

struct Spec
{
    int         plates  = -1;
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
    bool        quit    = true;
    std::string printer;
    //Where the run writes the project it built, so persistence can be inspected without the app.
    std::string save;
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
        else if (key == "warmup")  spec.warmup = num();
        else if (key == "orbit")   spec.orbit = num();
        else if (key == "switch")  spec.switches = num();
        else if (key == "assign")  spec.assigns = num();
        else if (key == "board")   spec.board = num();
        else if (key == "drag")    spec.drags = num();
        else if (key == "pick")    spec.pick = num();
        else if (key == "scope")   spec.scope = num();
        else if (key == "save")    spec.save = val;
        else if (key == "preview") spec.preview = num();
        else if (key == "quit")    spec.quit = num() != 0;
        else if (key == "printer") spec.printer = val;
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
    enum class Phase { Settle0, Build, Warmup, Orbit, Preview, Switch, Assign, Pick, Scope, Board, Drag, Finish, Done };

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
            } else if (m_done >= m_spec.assigns || m_assign_presets.empty()) {
                Perf::mark("driver.assign_end", m_done);
                enter(m_spec.pick > 0 ? Phase::Pick : (m_spec.scope > 0 ? Phase::Scope : Phase::Board));
            } else {
                const int count = plater->get_partplate_list().get_plate_count();
                const int plate = count > 0 ? (m_done % count) : 0;
                //Alternate between the two collected presets. This used to alternate a named
                //printer with an EMPTY name to time the "clear back to the project" path as well;
                //a plate always names a printer now, so that path does not exist and an empty name
                //is refused. Two real presets keep the assignment being timed a real one.
                const std::string &name = m_assign_presets[m_done % m_assign_presets.size()];
                plater->set_plate_printer(plate, name);
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
                enter(Phase::Finish);
            } else {
                board_drag(plater);
                ++m_done;
            }
            evt.RequestMore();
            break;

        case Phase::Finish: {
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
        if (p == Phase::Assign)
            collect_assign_presets();
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
            BOOST_LOG_TRIVIAL(warning) << "PETKOS_PERF_SCRIPT: wrote " << m_spec.save << " (rc=" << written << ")";
        }

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
            enter(m_spec.scope > 0 ? Phase::Scope : Phase::Board);
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
        //Stop when the list stops growing, not when the target is reached. create_plate refuses
        //past MAX_PLATE_COUNT and says so by returning without adding one, so a target above
        //that turns a loop on the count into a hang - which is exactly what a mistyped argument
        //produced the first time this ran.
        for (int previous = -1; list.get_plate_count() < m_spec.plates &&
                                list.get_plate_count() > previous;) {
            previous = list.get_plate_count();
            list.create_plate(true);
        }
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

    void collect_assign_presets()
    {
        m_assign_presets.clear();
        if (!m_spec.printer.empty()) {
            m_assign_presets.push_back(m_spec.printer);
            return;
        }
        PresetBundle *bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr)
            return;
        const std::string current = bundle->printers.get_selected_preset_name();
        for (const Preset &p : bundle->printers) {
            if (!p.is_visible || p.name == current)
                continue;
            m_assign_presets.push_back(p.name);
            break;
        }
        if (m_assign_presets.empty())
            BOOST_LOG_TRIVIAL(warning)
                << "PETKOS_PERF_SCRIPT: no second printer preset installed; the assign phase "
                   "will be skipped and its row will be missing from the summary";
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
    int                      m_tick    = 0;
    int                      m_done    = 0;
    int                      m_quiet   = 0;
    int                      m_board_y = 0;
    int                      m_pick_ok = 0;
    int                      m_pick_miss = 0;
    int                      m_pick_off = 0;
    std::vector<std::string> m_assign_presets;
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
            if (plater->resolve_plate_slicing_config(list.get_plate(0), resolved, error) && resolved.printer_preset)
                a_name = resolved.printer_preset->name;
            else
                fail("assign", "plate 1 does not resolve: " + error);
            if (!m_failed) {
                if (plater->resolve_plate_slicing_config(list.get_plate(1), resolved, error) && resolved.printer_preset)
                    b_name = resolved.printer_preset->name;
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
