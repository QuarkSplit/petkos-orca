#ifndef slic3r_GUI_PetkosPerf_hpp_
#define slic3r_GUI_PetkosPerf_hpp_

#include <cstdint>
#include <string>

//Petko's Orca: a latency instrument for the GUI, not for the slicer.
//
//Carried because the headless slicing rig cannot see a GUI stall and the in-repo Shiny
//profiler is invasive enough to change the thing it measures. What a "feels laggy" report
//needs is two honest wall-clock numbers - steady-state frame time, and input-to-painted-frame
//latency - taken at several plate counts, because the fork's whole risk is that cost scales
//with the number of plates.
//
//Off by default and free when off: every entry point early-returns on one cached bool, so an
//un-instrumented build and an instrumented-but-disabled build differ by a predictable branch.
//Turn it on with PETKOS_PERF=1; send the CSV somewhere else with PETKOS_PERF_OUT=<path>.
//
//Samples land in a preallocated ring buffer rather than the log, because BOOST_LOG formats
//and locks per line, which is a per-frame cost large enough to be mistaken for the bug.

namespace Slic3r { namespace GUI { namespace Perf {

//Probes are an enum rather than a string so the hot path never touches a map or an
//allocation. Add to the end; probe_name() must be kept in step.
enum class Probe : uint8_t {
    CanvasRender,          //whole GLCanvas3D::render, aux = canvas type
    CanvasSwap,            //SwapBuffers alone, aux = canvas type
    PlateListRender,       //PartPlateList::render, aux = plate count drawn
    PlateRender,           //one PartPlate::render, aux = plate index
    PlateListRenderLock,   //time spent waiting for m_plates_mutex in render
    ResolvePrinterBed,     //one resolve_printer_bed call, aux = 1 on cache hit
    SetShapes,             //PartPlateList::set_shapes, aux = plate count
    ApplyPrinterToPlate,   //PartPlateList::apply_printer_to_plate, aux = plate index
    BoardPaint,            //PlateBoard::on_paint, aux = rows drawn
    BoardThumbHeal,        //the offscreen plate render inside a board paint, aux = plate index
    BoardRowLayout,        //PlateBoard row/group layout recompute, aux = row count
    BoardRowRefresh,       //ONE board row recomputed in place + totals re-derived, aux = plate index
    BoardReloadItems,      //  sync_glyph_targets + rebuild_items + clamp_scroll
    BoardReloadSize,       //  the best-size check and any parent Layout it triggers
    BoardScopeRefresh,     //  Sidebar::refresh_plate_scope, incl. the inspector
    PlateSetShapeGeom,     //  PartPlate::set_shape - one plate's outline/grid rebuild
    PlateReflow,           //  PartPlateList::reflow_layout, aux = plate count
    SetPlatePrinter,       //whole Plater::set_plate_printer, aux = plate index
    SppSnapshot,           //  its undo snapshot
    SppReresolve,          //  its preset re-resolution
    SppApply,              //  its apply_printer_to_plate
    SppBedUpdate,          //  its bed update
    SppOutsideState,       //  its update_instances_outside_state
    SppBoardRefresh,       //  its board refresh
    SppBackgroundProcess,  //  its schedule_background_process
    SelectPlate,           //whole plate-switch path, aux = plate index
    UpdateBedForPlate,     //Plater::update_bed_for_selected_plate
    RenderThumbnail,       //GLCanvas3D::render_thumbnail, aux = plate index
    MouseMove,             //GLCanvas3D::on_mouse motion handling
    PickingPass,           //GLCanvas3D::_picking_pass - the FIRST raycast of the frame
    MouseTo3d,             //the SECOND raycast, ungated by dragging, so it runs while orbiting
    SceneRaycasterHit,     //SceneRaycaster::hit, aux = registered bed-item count
    PlateSliceValid,       //PartPlate::is_slice_result_valid, aux = plate index
    ResolvePlateContext,   //tier-2: a whole DynamicPrintConfig composed for one plate
    ResolvePlatePresets,   //tier-1: the plate's presets found, no config composed
    SelectPlateSnapshot,   //the undo snapshot taken on the plate-switch path
    RenderBed,             //Bed3D::render
    RenderObjects,         //_render_objects, both passes
    RenderShadows,         //_render_shadows depth pass
    RenderGizmos,          //sidebar hints + current gizmo
    RenderOverlays,        //_render_overlays: toolbars, plate strip, notifications
    RenderImGui,           //the ImGui draw-list submission at the end of the frame
    RenderSsaoFxaa,        //the optional post passes
    OvIconScale,           //_check_and_update_toolbar_icon_scale
    OvToolbars,            //the four GLToolbar renders
    OvPlateStrip,          //_render_imgui_select_plate_toolbar
    OvPaintToolbar,        //_render_paint_toolbar
    OvGizmosOverlay,       //_render_gizmos_overlay
    OvLabels,              //m_labels.render
    OvNavigator,           //_render_3d_navigator
    OvCanvasToolbar,       //_render_canvas_toolbar
    OvAssemble,            //_render_assemble_control + info + separators
    Count
};

[[nodiscard]] const char *probe_name(Probe p) noexcept;

//An interaction is a click the user makes; its latency is measured to the first frame that
//shows the result and again to the moment the loop goes idle. First-paint is what the eye
//catches; settle is when the app is usable again, and the two diverge exactly where an app
//"feels" slow while claiming a good frame rate.
enum class Interaction : uint8_t {
    PlateSwitch,
    PrinterAssign,
    BoardDrag,
    BoardHover,
    ProjectLoad,
    AppStartup,
    Count
};

[[nodiscard]] const char *interaction_name(Interaction k) noexcept;

//True once PETKOS_PERF is set to something other than 0/empty. Cached after the first call,
//so this is a load and a branch.
[[nodiscard]] bool enabled() noexcept;

//Monotonic ticks. QueryPerformanceCounter on Windows via steady_clock, ~20ns per call.
[[nodiscard]] int64_t now() noexcept;
[[nodiscard]] double ticks_to_ms(int64_t ticks) noexcept;

void record(Probe p, int64_t t0, int64_t t1, int32_t aux) noexcept;

//A one-off labelled marker, for anything that is an instant rather than a span.
void mark(const char *label, int32_t aux) noexcept;

void begin_interaction(Interaction k, int32_t aux) noexcept;
//Called after a frame has actually been presented, and after a board paint. Closes the
//first-paint half of any open interaction.
void note_painted(int32_t surface) noexcept;
//Called when the event loop has nothing left to do. Closes the settle half.
void note_idle() noexcept;

//Writes <out>.csv (every sample) and <out>.summary.txt (count/mean/p50/p95/p99/max per probe).
//Safe to call more than once; safe to call when disabled, where it does nothing.
void flush() noexcept;

//Where the CSV will land, so a driver can report it.
[[nodiscard]] std::string output_path();

//Scoped span. Reads the enabled flag once in the constructor and remembers it, so a disabled
//build pays one branch on entry and one on exit.
class Scope
{
public:
    explicit Scope(Probe p, int32_t aux = 0) noexcept : m_p(p), m_aux(aux), m_t0(enabled() ? now() : 0) {}
    ~Scope() noexcept
    {
        if (m_t0 != 0)
            record(m_p, m_t0, now(), m_aux);
    }
    Scope(const Scope &)            = delete;
    Scope &operator=(const Scope &) = delete;

    //Some spans only learn what they were measuring partway through (how many plates were
    //actually drawn, whether a lookup hit the cache).
    void set_aux(int32_t aux) noexcept { m_aux = aux; }

private:
    Probe   m_p;
    int32_t m_aux;
    int64_t m_t0;
};

}}} // namespace Slic3r::GUI::Perf

#define PETKOS_PERF_CAT2(a, b) a##b
#define PETKOS_PERF_CAT(a, b) PETKOS_PERF_CAT2(a, b)
//One line at a probe site. The variable name is line-derived so two probes may share a scope.
#define PETKOS_PERF_SCOPE(probe) ::Slic3r::GUI::Perf::Scope PETKOS_PERF_CAT(pp_scope_, __LINE__)(probe)
#define PETKOS_PERF_SCOPE_AUX(probe, aux) ::Slic3r::GUI::Perf::Scope PETKOS_PERF_CAT(pp_scope_, __LINE__)(probe, aux)
//Named form, for the sites that need to revise their aux value before the scope ends.
#define PETKOS_PERF_SCOPE_NAMED(name, probe, aux) ::Slic3r::GUI::Perf::Scope name(probe, aux)

#endif // slic3r_GUI_PetkosPerf_hpp_
