#include "CutRegion.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <utility>

#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Geometry.hpp"
#include "boost/log/trivial.hpp"

namespace Slic3r {

namespace {

inline Vec3f face_normal(const indexed_triangle_set &its, int f)
{
    const stl_triangle_vertex_indices &t = its.indices[size_t(f)];
    const Vec3f &a = its.vertices[size_t(t[0])];
    const Vec3f &b = its.vertices[size_t(t[1])];
    const Vec3f &c = its.vertices[size_t(t[2])];
    const Vec3f  n = (b - a).cross(c - a);
    const float  l = n.norm();
    return l > 0.f ? Vec3f(n / l) : Vec3f(0.f, 0.f, 0.f);
}

inline Vec3f face_centroid(const indexed_triangle_set &its, int f)
{
    const stl_triangle_vertex_indices &t = its.indices[size_t(f)];
    return (its.vertices[size_t(t[0])] + its.vertices[size_t(t[1])] + its.vertices[size_t(t[2])]) / 3.f;
}

// A crease is a seam when the surface turns by more than the threshold AND turns inwards.
// Outwards is a corner, and a corner is not a place a body ends.
struct SeamTest
{
    const indexed_triangle_set &its;
    const std::vector<Vec3f>   &normals;
    float                       cos_limit;
    bool                        concave_only;

    bool is_seam(int f, int g) const
    {
        const Vec3f &n1 = normals[size_t(f)];
        const Vec3f &n2 = normals[size_t(g)];
        if (n1.squaredNorm() == 0.f || n2.squaredNorm() == 0.f)
            return false;
        if (n1.dot(n2) >= cos_limit)
            return false;
        if (!concave_only)
            return true;
        // Concave when the neighbour sits on the side its own outward normal points away
        // from: walking from f to g goes INTO the material.
        const Vec3f delta = face_centroid(its, g) - face_centroid(its, f);
        return delta.dot(n1) > 0.f;
    }
};

std::vector<Vec3f> all_face_normals(const indexed_triangle_set &its)
{
    std::vector<Vec3f> normals(its.indices.size());
    for (size_t i = 0; i < its.indices.size(); ++i)
        normals[i] = face_normal(its, int(i));
    return normals;
}

} // namespace

std::vector<char> fill_region_from_face(const indexed_triangle_set &its,
                                        const std::vector<Vec3i32> &face_neighbors,
                                        int                         seed_face,
                                        const RegionFillParams     &params)
{
    std::vector<char> in_region(its.indices.size(), 0);
    if (seed_face < 0 || seed_face >= int(its.indices.size()) || face_neighbors.size() != its.indices.size())
        return in_region;

    const std::vector<Vec3f> normals = all_face_normals(its);
    const SeamTest seam{its, normals, float(std::cos(Geometry::deg2rad(double(params.crease_angle_deg)))), params.concave_only};

    std::vector<int> stack{seed_face};
    in_region[size_t(seed_face)] = 1;
    while (!stack.empty()) {
        const int f = stack.back();
        stack.pop_back();
        for (int e = 0; e < 3; ++e) {
            const int g = face_neighbors[size_t(f)][e];
            // An open edge is a boundary already, so there is nothing on the far side to reach.
            if (g < 0 || in_region[size_t(g)])
                continue;
            if (seam.is_seam(f, g))
                continue;
            in_region[size_t(g)] = 1;
            stack.push_back(g);
        }
    }
    return in_region;
}

int label_regions_by_crease(const indexed_triangle_set &its,
                            const std::vector<Vec3i32> &face_neighbors,
                            const RegionFillParams     &params,
                            std::vector<int>           &labels_out)
{
    labels_out.assign(its.indices.size(), -1);
    if (its.indices.empty() || face_neighbors.size() != its.indices.size())
        return 0;

    const std::vector<Vec3f> normals = all_face_normals(its);
    const SeamTest seam{its, normals, float(std::cos(Geometry::deg2rad(double(params.crease_angle_deg)))), params.concave_only};

    int              label = 0;
    std::vector<int> stack;
    for (size_t s = 0; s < its.indices.size(); ++s) {
        if (labels_out[s] >= 0)
            continue;
        stack.clear();
        stack.push_back(int(s));
        labels_out[s] = label;
        while (!stack.empty()) {
            const int f = stack.back();
            stack.pop_back();
            for (int e = 0; e < 3; ++e) {
                const int g = face_neighbors[size_t(f)][e];
                if (g < 0 || labels_out[size_t(g)] >= 0)
                    continue;
                if (seam.is_seam(f, g))
                    continue;
                labels_out[size_t(g)] = label;
                stack.push_back(g);
            }
        }
        ++label;
    }
    return label;
}

int label_regions_by_paint(const indexed_triangle_set                    &its,
                           const TriangleSelector::TriangleSplittingData &painting,
                           std::vector<int>                              &labels_out,
                           std::vector<EnforcerBlockerType>              *states_out)
{
    labels_out.assign(its.indices.size(), 0);
    if (states_out)
        states_out->clear();

    if (its.indices.empty())
        return 0;
    if (painting.bitstream.empty()) {
        if (states_out)
            states_out->push_back(EnforcerBlockerType::NONE);
        return 1;
    }

    TriangleMesh     mesh(its);
    TriangleSelector selector(mesh);
    selector.deserialize(painting, true);
    const std::vector<EnforcerBlockerType> states = selector.get_facet_states();

    // A dense labelling in first-appearance order, so the caller can name each part by the
    // filament its colour stands for.
    std::map<int, int>               state_to_label;
    std::vector<EnforcerBlockerType> label_states;
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const int  st       = i < states.size() ? int(states[i]) : int(EnforcerBlockerType::NONE);
        const auto inserted = state_to_label.emplace(st, int(label_states.size()));
        if (inserted.second)
            label_states.push_back(EnforcerBlockerType(st));
        labels_out[i] = inserted.first->second;
    }

    if (states_out)
        *states_out = label_states;
    return int(label_states.size());
}

bool regions_from_paint(const indexed_triangle_set                    &its,
                        const TriangleSelector::TriangleSplittingData &painting,
                        PaintedRegions                                &out)
{
    out = PaintedRegions();
    if (its.indices.empty() || painting.bitstream.empty())
        return false;

    TriangleMesh     mesh(its);
    TriangleSelector selector(mesh);
    selector.deserialize(painting, true);

    std::vector<EnforcerBlockerType> states;
    out.mesh = selector.get_facets_conforming(states, out.source_face);
    if (out.mesh.indices.empty() || states.size() != out.mesh.indices.size())
        return false;

    // A dense labelling in first-appearance order, so a caller can name each part by its filament.
    std::map<int, int> state_to_label;
    out.labels.resize(out.mesh.indices.size());
    for (size_t i = 0; i < out.mesh.indices.size(); ++i) {
        const int  st       = int(states[i]);
        const auto inserted = state_to_label.emplace(st, int(out.label_states.size()));
        if (inserted.second)
            out.label_states.push_back(EnforcerBlockerType(st));
        out.labels[i] = inserted.first->second;
    }
    out.label_count = int(out.label_states.size());
    return out.label_count > 1;
}

namespace {

// The directed edges of one part that have nothing on the other side, chained into loops.
// A loop comes back in the direction that part's own faces traverse it, which is what the
// caller needs to wind a lid the right way round.
std::vector<std::vector<int>> chain_loops(const std::vector<std::pair<int, int>> &edges)
{
    std::vector<std::vector<int>> loops;
    if (edges.empty())
        return loops;

    // A vertex can start more than one boundary edge where the surface pinches, so the
    // successors are a multimap and each is consumed once.
    std::unordered_multimap<int, size_t> from;
    from.reserve(edges.size() * 2);
    for (size_t i = 0; i < edges.size(); ++i)
        from.emplace(edges[i].first, i);

    std::vector<char> used(edges.size(), 0);
    for (size_t start = 0; start < edges.size(); ++start) {
        if (used[start])
            continue;
        std::vector<int> loop;
        size_t           cur    = start;
        bool             closed = false;
        while (true) {
            used[cur] = 1;
            loop.push_back(edges[cur].first);
            const int next_vertex = edges[cur].second;
            if (next_vertex == edges[start].first) {
                closed = true;
                break;
            }
            const auto range = from.equal_range(next_vertex);
            size_t     pick  = size_t(-1);
            for (auto it = range.first; it != range.second; ++it)
                if (!used[it->second]) {
                    pick = it->second;
                    break;
                }
            if (pick == size_t(-1))
                // An open chain rather than a loop: the mesh was already torn here. It is
                // not spannable, so it is dropped rather than closed wrongly.
                break;
            cur = pick;
        }
        if (closed && loop.size() >= 3)
            loops.push_back(std::move(loop));
    }
    return loops;
}

} // namespace

namespace {

double signed_volume(const indexed_triangle_set &its)
{
    double v = 0.;
    for (const stl_triangle_vertex_indices &t : its.indices) {
        const Vec3d a = its.vertices[size_t(t[0])].cast<double>();
        const Vec3d b = its.vertices[size_t(t[1])].cast<double>();
        const Vec3d c = its.vertices[size_t(t[2])].cast<double>();
        v += a.dot(b.cross(c));
    }
    return v / 6.;
}

//A lid may not be made of the model's own skin.
//
//A minimal surface spanning a loop is chosen from the loop alone, and it knows nothing about
//the solid it is supposed to be inside. When every vertex of a region's boundary is also a
//vertex of the surface around it, the cheapest surface spanning that loop can BE that
//surface - and the cut then hands back one part with the whole model in it and one with
//nothing, both watertight, both wrong. So a patch that reuses faces the model already has is
//not a lid, and the fan below is used instead.
struct SourceFaceLookup
{
    // Only the faces touching the loops matter, so only those are indexed.
    std::unordered_map<int64_t, char> faces;

    static int64_t key(int a, int b, int c)
    {
        int v[3] = {a, b, c};
        std::sort(std::begin(v), std::end(v));
        return (int64_t(v[0]) * 1000003LL + int64_t(v[1])) * 1000003LL + int64_t(v[2]);
    }

    SourceFaceLookup(const indexed_triangle_set &src, const std::vector<char> &is_loop_vertex)
    {
        for (const stl_triangle_vertex_indices &t : src.indices)
            if (is_loop_vertex[size_t(t[0])] && is_loop_vertex[size_t(t[1])] && is_loop_vertex[size_t(t[2])])
                faces.emplace(key(t[0], t[1], t[2]), 1);
    }

    bool contains(int a, int b, int c) const { return faces.find(key(a, b, c)) != faces.end(); }
};

} // namespace

bool split_by_labels(const indexed_triangle_set &src,
                     const std::vector<int>     &labels,
                     int                         label_count,
                     bool                        cap,
                     CutRegionResult            &out,
                     std::string                &failure)
{
    out = CutRegionResult();
    failure.clear();

    if (src.indices.empty()) {
        failure = "the volume has no geometry to cut";
        return false;
    }
    if (labels.size() != src.indices.size()) {
        failure = "the selected region does not describe this mesh";
        return false;
    }
    if (label_count <= 0) {
        failure = "nothing was selected to cut";
        return false;
    }
    if (label_count == 1) {
        failure = "the whole model is one part, so there is nothing to separate";
        return false;
    }

    out.parts.resize(size_t(label_count));

    // Faces first: each part gets its own vertices, in the order its faces ask for them.
    std::vector<std::vector<int>> vertex_map(size_t(label_count), std::vector<int>(src.vertices.size(), -1));
    for (size_t f = 0; f < src.indices.size(); ++f) {
        const int l = labels[f];
        if (l < 0 || l >= label_count)
            continue;
        CutRegionPart    &part = out.parts[size_t(l)];
        std::vector<int> &vm   = vertex_map[size_t(l)];
        Vec3i32           tri;
        for (int k = 0; k < 3; ++k) {
            const int v = src.indices[f][k];
            if (vm[size_t(v)] < 0) {
                vm[size_t(v)] = int(part.mesh.vertices.size());
                part.mesh.vertices.push_back(src.vertices[size_t(v)]);
            }
            tri[k] = vm[size_t(v)];
        }
        part.mesh.indices.emplace_back(tri[0], tri[1], tri[2]);
        part.src_face.push_back(int(f));
    }

    if (!cap)
        return true;

    // The openings: a directed edge whose neighbour lives under another label.
    const std::vector<Vec3i32> neighbors = its_face_neighbors_par(src);

    // Grouped by the ordered pair (lower label, higher label) so each interface is spanned
    // once and both sides are handed the same triangles.
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> interface_edges;
    for (size_t f = 0; f < src.indices.size(); ++f) {
        const int lf = labels[f];
        if (lf < 0 || lf >= label_count)
            continue;
        for (int e = 0; e < 3; ++e) {
            const int g = neighbors[f][e];
            if (g < 0) {
                ++out.parts[size_t(lf)].inherited_open_edges;
                continue;
            }
            const int lg = labels[size_t(g)];
            if (lg == lf || lg < 0 || lg >= label_count)
                continue;
            if (lf > lg)
                continue; // the face on the other side of this edge records it
            // The directed edge as the LOWER label's face traverses it.
            const int u = src.indices[f][e];
            const int v = src.indices[f][(e + 1) % 3];
            interface_edges[std::make_pair(lf, lg)].emplace_back(u, v);
        }
    }

    //Which vertices any loop passes through, so the "is this lid made of the model's own
    //skin" test only has to look at the handful of faces that could possibly be reused.
    std::vector<char> is_loop_vertex(src.vertices.size(), 0);
    for (const auto &entry : interface_edges)
        for (const auto &e : entry.second) {
            is_loop_vertex[size_t(e.first)]  = 1;
            is_loop_vertex[size_t(e.second)] = 1;
        }
    const SourceFaceLookup source_faces(src, is_loop_vertex);

    for (const auto &entry : interface_edges) {
        const int low  = entry.first.first;
        const int high = entry.first.second;
        for (const std::vector<int> &loop : chain_loops(entry.second)) {
            ++out.cut_loops;

            std::vector<Vec3f> pts;
            pts.reserve(loop.size());
            for (int v : loop)
                pts.push_back(src.vertices[size_t(v)]);

            //Indices into `loop`, except for one value past its end, which means "the point
            //added at the centre of the loop" - the fan's apex, and the only new vertex this
            //whole operation ever creates.
            const int            apex = int(loop.size());
            std::vector<Vec3i32> patch;
            std::string          why;
            bool                 fanned = false;

            if (!MeshBoolean::cgal::triangulate_loop(pts, patch, &why)) {
                fanned = true;
            } else {
                for (const Vec3i32 &t : patch)
                    if (source_faces.contains(loop[size_t(t[0])], loop[size_t(t[1])], loop[size_t(t[2])])) {
                        fanned = true;
                        break;
                    }
            }

            if (fanned) {
                patch.clear();
                for (size_t i = 0; i < loop.size(); ++i)
                    patch.emplace_back(int(i), int((i + 1) % loop.size()), apex);
                ++out.fanned_loops;
                BOOST_LOG_TRIVIAL(info) << "CutRegion: a cut outline of " << loop.size()
                                        << " points was closed with a fan from its centre"
                                        << (why.empty() ? "" : (" (" + why + ")"));
            }

            if (patch.empty()) {
                ++out.unspanned_loops;
                BOOST_LOG_TRIVIAL(warning) << "CutRegion: a cut outline of " << loop.size()
                                           << " points could not be spanned: " << why;
                continue;
            }

            Vec3f centre = Vec3f::Zero();
            for (const Vec3f &p : pts)
                centre += p;
            centre /= float(pts.size());

            // One patch, two windings. The lower label's faces walk the loop forwards, so its
            // lid has to walk it backwards to close the surface; the higher label - whose
            // faces walk the same loop backwards - takes the patch as it came.
            auto add_patch = [&](int label, bool flip) {
                CutRegionPart    &part = out.parts[size_t(label)];
                std::vector<int> &vm   = vertex_map[size_t(label)];
                int               apex_local = -1;
                for (const Vec3i32 &t : patch) {
                    Vec3i32 tri;
                    bool    ok = true;
                    for (int k = 0; k < 3; ++k) {
                        const int idx = t[k];
                        if (idx == apex) {
                            if (apex_local < 0) {
                                apex_local = int(part.mesh.vertices.size());
                                part.mesh.vertices.push_back(centre);
                            }
                            tri[k] = apex_local;
                            continue;
                        }
                        if (idx < 0 || idx >= int(loop.size())) {
                            ok = false;
                            break;
                        }
                        const int v = loop[size_t(idx)];
                        if (vm[size_t(v)] < 0) {
                            // A loop vertex belongs to both parts by construction; a missing
                            // one means the labelling and the mesh disagree, and a lid built
                            // on a vertex the part does not own would tear it.
                            ok = false;
                            break;
                        }
                        tri[k] = vm[size_t(v)];
                    }
                    if (!ok || tri[0] == tri[1] || tri[1] == tri[2] || tri[2] == tri[0])
                        continue;
                    if (flip)
                        std::swap(tri[1], tri[2]);
                    part.mesh.indices.emplace_back(tri[0], tri[1], tri[2]);
                    part.src_face.push_back(-1);
                    ++part.cap_faces;
                }
            };
            add_patch(low, true);
            add_patch(high, false);
        }
    }

    //The arithmetic the whole approach rests on, checked rather than assumed: what came out
    //is what went in. A caller that gets volume_conserved == false has been handed geometry
    //it should refuse, and knowing that costs one pass over the triangles.
    out.source_volume = signed_volume(src);
    out.parts_volume  = 0.;
    for (const CutRegionPart &part : out.parts)
        out.parts_volume += signed_volume(part.mesh);
    const double scale = std::max(1e-6, std::abs(out.source_volume));
    out.volume_conserved = std::abs(out.parts_volume - out.source_volume) <= 1e-6 * scale;

    return true;
}

} // namespace Slic3r
