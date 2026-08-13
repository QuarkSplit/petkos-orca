#include "PetkosPerfDriver.hpp"
#include "PetkosPerf.hpp"

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/log/trivial.hpp>

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
    bool        quit    = true;
    std::string printer;
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
    enum class Phase { Settle0, Build, Warmup, Orbit, Preview, Switch, Assign, Board, Drag, Finish, Done };

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
                enter(Phase::Board);
            } else {
                const int count = plater->get_partplate_list().get_plate_count();
                const int plate = count > 0 ? (m_done % count) : 0;
                //alternate between a named printer and clearing back to the project, so
                //both write paths get timed rather than only the one
                const std::string &name = (m_done % 2 == 0) ? m_assign_presets.front() : m_empty;
                plater->set_plate_printer(plate, name);
                ++m_done;
                m_quiet = 20;
            }
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

    void orbit_step(Plater *plater, GLCanvas3D *canvas)
    {
        //A real orbit is a drag, which is a stream of small rotations. 0.01 rad a frame is
        //about the speed a hand moves a mouse, and keeps the whole scene on screen so the
        //frame cost is not accidentally measured against a view that culled everything.
        plater->get_camera().rotate_on_sphere(0.012, 0.0, false);
        canvas->set_as_dirty();
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
    std::vector<std::string> m_assign_presets;
    const std::string        m_empty;
};

} // namespace

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
