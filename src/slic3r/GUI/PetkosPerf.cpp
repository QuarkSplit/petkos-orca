#include "PetkosPerf.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI { namespace Perf {

namespace {

//Big enough that a 36-plate orbit of a few thousand frames never wraps, small enough to be
//an unremarkable allocation. Only allocated when the instrument is switched on.
constexpr size_t RING_CAPACITY = 1u << 20;

struct Sample
{
    int64_t t0;
    int64_t t1;
    int32_t aux;
    uint8_t probe;
    uint8_t kind; //0 = probe span, 1 = interaction result, 2 = marker
    uint8_t pad[2];
};
static_assert(sizeof(Sample) == 24, "Sample is meant to stay a small POD");

//Interaction results reuse the Sample record, with kind=1 and the probe byte holding the
//Interaction. The second timestamp is first-paint; settle rides in aux as microseconds so a
//single flat record covers both halves without a second table.
struct Interactions
{
    int64_t start[(size_t) Interaction::Count]{};
    int64_t first_paint[(size_t) Interaction::Count]{};
    int32_t aux[(size_t) Interaction::Count]{};
    bool    open[(size_t) Interaction::Count]{};
};

struct State
{
    std::vector<Sample>                       ring;
    std::atomic<size_t>                       head{0};
    Interactions                              inter;
    std::mutex                                inter_mutex;
    std::vector<std::pair<std::string, Sample>> marks;
    std::mutex                                marks_mutex;
    std::string                               out_path;
    int64_t                                   epoch = 0;
    std::atomic<bool>                         flushed{false};
};

State &state()
{
    static State s;
    return s;
}

bool read_enabled()
{
    const char *v = std::getenv("PETKOS_PERF");
    if (v == nullptr || v[0] == '\0')
        return false;
    if (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0)
        return false;
    return true;
}

std::string default_out_path()
{
    if (const char *v = std::getenv("PETKOS_PERF_OUT"); v != nullptr && v[0] != '\0')
        return std::string(v);

    boost::filesystem::path dir;
    try {
        dir = boost::filesystem::path(data_dir()) / "perf";
        boost::filesystem::create_directories(dir);
    } catch (...) {
        //data_dir() is unset in some early-startup and CLI paths; the cwd is always writable
        //enough to not lose a measurement over it.
        dir = boost::filesystem::path(".");
    }
    return (dir / "petkos-perf").string();
}

double ticks_per_ms()
{
    using clock = std::chrono::steady_clock;
    return double(clock::period::den) / double(clock::period::num) / 1000.0;
}

const char *const PROBE_NAMES[] = {
    "CanvasRender",     "CanvasSwap",       "PlateListRender",  "PlateRender",
    "PlateListRenderLock", "ResolvePrinterBed", "SetShapes",     "ApplyPrinterToPlate",
    "BoardPaint",       "BoardThumbHeal",   "BoardRowLayout",   "BoardRowRefresh",
    "BoardReloadItems", "BoardReloadSize",  "BoardScopeRefresh",
    "PlateSetShapeGeom", "PlateReflow",
    "SetPlatePrinter",
    "SppSnapshot",      "SppReresolve",     "SppApply",         "SppBedUpdate",
    "SppOutsideState",  "SppBoardRefresh",  "SppBackgroundProcess", "SelectPlate",
    "UpdateBedForPlate", "RenderThumbnail", "MouseMove",
    "PickingPass",      "MouseTo3d",        "SceneRaycasterHit", "PlateSliceValid",
    "ResolvePlateContext", "ResolvePlatePresets", "SelectPlateSnapshot",
    "RenderBed",        "RenderObjects",    "RenderShadows",     "RenderGizmos",
    "RenderOverlays",   "RenderImGui",      "RenderSsaoFxaa",
    "OvIconScale",      "OvToolbars",       "OvPlateStrip",   "OvPaintToolbar",
    "OvGizmosOverlay",  "OvLabels",         "OvNavigator",    "OvCanvasToolbar",
    "OvAssemble",
    "TabSelectPreset",  "TabLoadCurrentPreset", "TabUpdateVisibility",
    "PlaterSetBedShape", "BundleFullConfig",    "LoadBedtypeTextures",
    "FollowPlatePresets",
    "PickerIndexBuild", "PickerBuildUi",
    "GuideProfileWalk", "GuideProfileFamily", "GuideProfileDump",
};
static_assert(sizeof(PROBE_NAMES) / sizeof(PROBE_NAMES[0]) == (size_t) Probe::Count,
              "probe name table is out of step with the Probe enum");

const char *const INTERACTION_NAMES[] = {
    "PlateSwitch", "PrinterAssign", "BoardDrag", "BoardHover", "ProjectLoad", "AppStartup",
};
static_assert(sizeof(INTERACTION_NAMES) / sizeof(INTERACTION_NAMES[0]) == (size_t) Interaction::Count,
              "interaction name table is out of step with the Interaction enum");

void push(const Sample &s) noexcept
{
    State &st = state();
    if (st.ring.empty())
        return;
    const size_t i = st.head.fetch_add(1, std::memory_order_relaxed);
    st.ring[i % RING_CAPACITY] = s;
}

} // namespace

const char *probe_name(Probe p) noexcept
{
    const size_t i = (size_t) p;
    return i < (size_t) Probe::Count ? PROBE_NAMES[i] : "?";
}

const char *interaction_name(Interaction k) noexcept
{
    const size_t i = (size_t) k;
    return i < (size_t) Interaction::Count ? INTERACTION_NAMES[i] : "?";
}

bool enabled() noexcept
{
    //Function-local static: initialised once, thread-safe under C++11, and after that a
    //load with an already-initialised guard. This is the branch every probe pays.
    static const bool on = [] {
        const bool e = read_enabled();
        if (e) {
            State &st = state();
            try {
                st.ring.resize(RING_CAPACITY);
            } catch (...) {
                return false;
            }
            st.out_path = default_out_path();
            st.epoch    = std::chrono::steady_clock::now().time_since_epoch().count();
        }
        return e;
    }();
    return on;
}

int64_t now() noexcept { return std::chrono::steady_clock::now().time_since_epoch().count(); }

double ticks_to_ms(int64_t ticks) noexcept { return double(ticks) / ticks_per_ms(); }

void record(Probe p, int64_t t0, int64_t t1, int32_t aux) noexcept
{
    if (!enabled())
        return;
    Sample s{};
    s.t0    = t0;
    s.t1    = t1;
    s.aux   = aux;
    s.probe = (uint8_t) p;
    s.kind  = 0;
    push(s);
}

void mark(const char *label, int32_t aux) noexcept
{
    if (!enabled() || label == nullptr)
        return;
    Sample s{};
    s.t0   = now();
    s.t1   = s.t0;
    s.aux  = aux;
    s.kind = 2;
    State &st = state();
    try {
        const std::lock_guard<std::mutex> lock(st.marks_mutex);
        st.marks.emplace_back(std::string(label), s);
    } catch (...) {
    }
}

void begin_interaction(Interaction k, int32_t aux) noexcept
{
    if (!enabled() || k >= Interaction::Count)
        return;
    State &st = state();
    const std::lock_guard<std::mutex> lock(st.inter_mutex);
    const size_t i        = (size_t) k;
    st.inter.start[i]     = now();
    st.inter.first_paint[i] = 0;
    st.inter.aux[i]       = aux;
    st.inter.open[i]      = true;
}

void note_painted(int32_t surface) noexcept
{
    if (!enabled())
        return;
    State &st = state();
    const std::lock_guard<std::mutex> lock(st.inter_mutex);
    const int64_t t = now();
    for (size_t i = 0; i < (size_t) Interaction::Count; ++i) {
        if (st.inter.open[i] && st.inter.first_paint[i] == 0)
            st.inter.first_paint[i] = t;
    }
    (void) surface;
}

void note_idle() noexcept
{
    if (!enabled())
        return;
    State &st = state();
    const std::lock_guard<std::mutex> lock(st.inter_mutex);
    const int64_t t = now();
    for (size_t i = 0; i < (size_t) Interaction::Count; ++i) {
        if (!st.inter.open[i])
            continue;
        //An interaction that reached idle without ever painting is still a real result: it
        //means the click changed nothing on screen, which is its own kind of bug.
        Sample s{};
        s.t0    = st.inter.start[i];
        s.t1    = st.inter.first_paint[i];
        s.probe = (uint8_t) i;
        s.kind  = 1;
        //settle, in microseconds, so one flat record carries both halves
        s.aux = (int32_t) std::min<int64_t>(int64_t(ticks_to_ms(t - st.inter.start[i]) * 1000.0),
                                            int64_t(INT32_MAX));
        push(s);
        st.inter.open[i] = false;
    }
}

std::string output_path()
{
    if (!enabled())
        return {};
    return state().out_path;
}

void flush() noexcept
{
    if (!enabled())
        return;
    State &st = state();
    bool expected = false;
    if (!st.flushed.compare_exchange_strong(expected, true))
        return;

    try {
        const size_t written = st.head.load(std::memory_order_relaxed);
        const size_t count   = std::min(written, RING_CAPACITY);
        const size_t first   = written > RING_CAPACITY ? written - RING_CAPACITY : 0;

        std::ofstream csv(st.out_path + ".csv", std::ios::out | std::ios::trunc);
        csv << "kind,name,start_ms,dur_ms,aux\n";
        csv.setf(std::ios::fixed);
        csv.precision(4);
        for (size_t n = 0; n < count; ++n) {
            const Sample &s = st.ring[(first + n) % RING_CAPACITY];
            const double  start_ms = ticks_to_ms(s.t0 - st.epoch);
            if (s.kind == 1) {
                const double paint_ms = s.t1 == 0 ? -1.0 : ticks_to_ms(s.t1 - s.t0);
                csv << "interaction," << interaction_name((Interaction) s.probe) << ','
                    << start_ms << ',' << paint_ms << ',' << (double(s.aux) / 1000.0) << '\n';
            } else {
                csv << "span," << probe_name((Probe) s.probe) << ',' << start_ms << ','
                    << ticks_to_ms(s.t1 - s.t0) << ',' << s.aux << '\n';
            }
        }
        {
            const std::lock_guard<std::mutex> lock(st.marks_mutex);
            for (const auto &m : st.marks)
                csv << "mark," << m.first << ',' << ticks_to_ms(m.second.t0 - st.epoch) << ",0,"
                    << m.second.aux << '\n';
        }
        csv.close();

        //A summary beside the raw rows, because a 200k-row CSV answers nothing on its own and
        //the whole point of the instrument is a number you can compare against another number.
        std::vector<std::vector<double>> per_probe((size_t) Probe::Count);
        std::vector<std::vector<double>> per_inter_paint((size_t) Interaction::Count);
        std::vector<std::vector<double>> per_inter_settle((size_t) Interaction::Count);
        for (size_t n = 0; n < count; ++n) {
            const Sample &s = st.ring[(first + n) % RING_CAPACITY];
            if (s.kind == 0 && s.probe < (uint8_t) Probe::Count)
                per_probe[s.probe].push_back(ticks_to_ms(s.t1 - s.t0));
            else if (s.kind == 1 && s.probe < (uint8_t) Interaction::Count) {
                if (s.t1 != 0)
                    per_inter_paint[s.probe].push_back(ticks_to_ms(s.t1 - s.t0));
                per_inter_settle[s.probe].push_back(double(s.aux) / 1000.0);
            }
        }

        auto pct = [](std::vector<double> &v, double p) {
            if (v.empty())
                return 0.0;
            const size_t i = std::min(v.size() - 1, (size_t) (p * double(v.size())));
            std::nth_element(v.begin(), v.begin() + (long) i, v.end());
            return v[i];
        };

        std::ofstream sum(st.out_path + ".summary.txt", std::ios::out | std::ios::trunc);
        sum.setf(std::ios::fixed);
        sum.precision(3);
        sum << "petkos-orca perf summary  (all times ms)\n\n";
        sum << "span                    n        mean     p50     p95     p99     max   total\n";
        for (size_t i = 0; i < per_probe.size(); ++i) {
            auto &v = per_probe[i];
            if (v.empty())
                continue;
            double total = 0.0, max = 0.0;
            for (double d : v) {
                total += d;
                max = std::max(max, d);
            }
            sum.width(22);
            sum << std::left << probe_name((Probe) i) << std::right;
            sum.width(7);
            sum << v.size() << "  ";
            sum.width(8);
            sum << total / double(v.size());
            sum.width(8);
            sum << pct(v, 0.50);
            sum.width(8);
            sum << pct(v, 0.95);
            sum.width(8);
            sum << pct(v, 0.99);
            sum.width(8);
            sum << max;
            sum.width(8);
            sum << total << '\n';
        }
        sum << "\ninteraction             n   first_paint_p50  first_paint_max   settle_p50   settle_max\n";
        for (size_t i = 0; i < per_inter_settle.size(); ++i) {
            auto &settle = per_inter_settle[i];
            if (settle.empty())
                continue;
            auto & paint     = per_inter_paint[i];
            double paint_max = 0.0, settle_max = 0.0;
            for (double d : paint)
                paint_max = std::max(paint_max, d);
            for (double d : settle)
                settle_max = std::max(settle_max, d);
            sum.width(22);
            sum << std::left << interaction_name((Interaction) i) << std::right;
            sum.width(4);
            sum << settle.size();
            sum.width(18);
            sum << pct(paint, 0.50);
            sum.width(17);
            sum << paint_max;
            sum.width(13);
            sum << pct(settle, 0.50);
            sum.width(13);
            sum << settle_max << '\n';
        }
        sum.close();
    } catch (...) {
        //A measurement run must not become a crash report about the measuring.
    }
}

}}} // namespace Slic3r::GUI::Perf
