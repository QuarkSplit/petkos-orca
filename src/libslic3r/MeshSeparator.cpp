#include "MeshSeparator.hpp"

#include <cmath>
#include <unordered_map>

namespace Slic3r { namespace MeshSeparator {

static inline Vec3f face_centroid(const indexed_triangle_set &its, int face)
{
    const stl_triangle_vertex_indices &t = its.indices[face];
    return (its.vertices[t[0]] + its.vertices[t[1]] + its.vertices[t[2]]) / 3.f;
}

std::vector<char> flood_fill(const indexed_triangle_set &its,
                             const std::vector<Vec3i32> &face_neighbors,
                             const std::vector<Vec3f>   &face_normals,
                             int                         seed_face,
                             const FillParams           &params)
{
    const int n = int(its.indices.size());
    std::vector<char> selected(n, 0);
    if (seed_face < 0 || seed_face >= n)
        return selected;

    const float clamped = std::clamp(params.angle_threshold_deg, 0.f, 180.f);
    // cos is monotone decreasing, so "angle <= threshold" reads "dot >= cos(threshold)".
    const float cos_threshold = std::cos(clamped * float(M_PI) / 180.f) - 1e-6f;

    std::vector<int> queue;
    queue.reserve(256);
    queue.push_back(seed_face);
    selected[seed_face] = 1;
    while (!queue.empty()) {
        const int face = queue.back();
        queue.pop_back();
        for (int j = 0; j < 3; ++j) {
            const int nb = face_neighbors[face][j];
            if (nb < 0 || selected[nb])
                continue; // an open edge is already a boundary
            const float dot = std::clamp(face_normals[face].dot(face_normals[nb]), -1.f, 1.f);
            bool crossable = dot >= cos_threshold;
            if (!crossable && params.concave_only) {
                //The crease is sharp, but sharp is only a wall when it is a valley. The
                //neighbour's centroid sitting below this face's plane means the surface folds
                //away from its own outside - a convex corner - and the fill walks on.
                Vec3f d = face_centroid(its, nb) - face_centroid(its, face);
                const float len = d.norm();
                if (len > 0.f && face_normals[face].dot(d / len) < -1e-4f)
                    crossable = true;
            }
            if (crossable) {
                selected[nb] = 1;
                queue.push_back(nb);
            }
        }
    }
    return selected;
}

double signed_volume(const indexed_triangle_set &its)
{
    double volume = 0.;
    for (const stl_triangle_vertex_indices &t : its.indices) {
        const Vec3d a = its.vertices[t[0]].cast<double>();
        const Vec3d b = its.vertices[t[1]].cast<double>();
        const Vec3d c = its.vertices[t[2]].cast<double>();
        volume += a.dot(b.cross(c));
    }
    return volume / 6.;
}

// ---- seam loops --------------------------------------------------------------------------

//The seam as the SELECTED faces see it: directed edges (u -> v) in those faces' winding. On a
//consistently wound manifold surface every such edge appears exactly once, in-degree equals
//out-degree at every vertex, and the edges decompose into closed cycles.
struct SeamEdge
{
    int  from;
    int  to;
    bool used{false};
};

static void chain_seam_loops(std::vector<SeamEdge>        &edges,
                             std::vector<std::vector<int>> &loops_out,
                             size_t                        &open_chains_out)
{
    std::unordered_map<int, std::vector<int>> outgoing;
    outgoing.reserve(edges.size());
    for (int i = 0; i < int(edges.size()); ++i)
        outgoing[edges[i].from].push_back(i);

    for (int start = 0; start < int(edges.size()); ++start) {
        if (edges[start].used)
            continue;
        std::vector<int> loop;
        int  edge_idx = start;
        bool closed   = false;
        // Bounded by the edge count: every step consumes an edge for good.
        while (true) {
            SeamEdge &e = edges[edge_idx];
            e.used      = true;
            loop.push_back(e.from);
            if (e.to == edges[start].from) {
                closed = true;
                break;
            }
            auto it = outgoing.find(e.to);
            int next = -1;
            if (it != outgoing.end())
                for (int cand : it->second)
                    if (!edges[cand].used) { next = cand; break; }
            if (next < 0)
                break; // a hole or a non-manifold pinch ended the walk
            edge_idx = next;
        }
        if (closed && loop.size() >= 3)
            loops_out.emplace_back(std::move(loop));
        else
            ++open_chains_out;
    }
}

// ---- capping -----------------------------------------------------------------------------

//A patch spanning one loop. Triangle indices below `loop_size` name loop vertices; an index of
//exactly `loop_size` names the one extra point a fallback fan adds at the loop's centre.
struct CapPatch
{
    std::vector<Vec3i32> triangles; // wound WITH the loop's direction
    bool                 has_centre{false};
    Vec3f                centre{Vec3f::Zero()};
    bool                 used_fallback{false};
};

static inline bool point_in_triangle_2d(const Vec2d &p, const Vec2d &a, const Vec2d &b, const Vec2d &c)
{
    auto side = [](const Vec2d &p, const Vec2d &q, const Vec2d &r) {
        return (q.x() - p.x()) * (r.y() - p.y()) - (q.y() - p.y()) * (r.x() - p.x());
    };
    const double eps = -1e-12;
    const double d1 = side(a, b, p), d2 = side(b, c, p), d3 = side(c, a, p);
    return d1 > eps && d2 > eps && d3 > eps;
}

// Fan the polygon `order` (indices into `pts`) from its centre point, which is emitted as
// triangle index `centre_index` - the caller's contract is loop_size for that slot.
static void fan_from_centre(const std::vector<Vec3f> &pts, const std::vector<int> &order, int centre_index, CapPatch &out)
{
    Vec3f centre = Vec3f::Zero();
    for (int i : order)
        centre += pts[i];
    centre /= float(order.size());
    out.has_centre    = true;
    out.centre        = centre;
    out.used_fallback = true;
    for (size_t k = 0; k < order.size(); ++k)
        out.triangles.emplace_back(order[k], order[(k + 1) % order.size()], centre_index);
}

//Triangulate one closed loop. Ear clipping in the loop's own best-fit plane keeps the patch as
//flat as the seam allows; when the projected polygon is degenerate or the clipping stalls (a
//self-overlapping projection can starve it of ears), the remainder is fanned from its centre -
//uglier, but always closed, and both parts wear the same patch so nothing leaks either way.
static CapPatch triangulate_loop(const std::vector<Vec3f> &pts)
{
    CapPatch out;
    const int n = int(pts.size());
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i)
        order[i] = i;

    // Newell's normal of the polygon.
    Vec3d normal = Vec3d::Zero();
    for (int i = 0; i < n; ++i) {
        const Vec3d a = pts[i].cast<double>();
        const Vec3d b = pts[(i + 1) % n].cast<double>();
        normal.x() += (a.y() - b.y()) * (a.z() + b.z());
        normal.y() += (a.z() - b.z()) * (a.x() + b.x());
        normal.z() += (a.x() - b.x()) * (a.y() + b.y());
    }
    // A huge loop makes O(n^2) clipping noticeable and its projection is rarely simple anyway.
    if (normal.norm() < 1e-12 || n > 10000) {
        fan_from_centre(pts, order, n, out);
        return out;
    }
    normal.normalize();
    Vec3d u = std::abs(normal.z()) < 0.9 ? normal.cross(Vec3d::UnitZ()) : normal.cross(Vec3d::UnitX());
    u.normalize();
    const Vec3d v = normal.cross(u);

    std::vector<Vec2d> p2(n);
    for (int i = 0; i < n; ++i) {
        const Vec3d p = pts[i].cast<double>();
        p2[i] = Vec2d(p.dot(u), p.dot(v));
    }
    double area2 = 0.;
    for (int i = 0; i < n; ++i) {
        const Vec2d &a = p2[i], &b = p2[(i + 1) % n];
        area2 += a.x() * b.y() - b.x() * a.y();
    }
    if (std::abs(area2) < 1e-12) {
        fan_from_centre(pts, order, n, out);
        return out;
    }
    //Mirror the test space rather than the loop: convexity tests below assume a CCW polygon,
    //while the emitted triangles keep the loop's real order, which is what the winding
    //contract with the caller is written in.
    if (area2 < 0.)
        for (Vec2d &p : p2)
            p.y() = -p.y();

    std::vector<int> rem = order;
    out.triangles.reserve(n - 2);
    while (rem.size() > 3) {
        bool clipped = false;
        for (size_t i = 0; i < rem.size(); ++i) {
            const size_t i_prev = (i + rem.size() - 1) % rem.size();
            const size_t i_next = (i + 1) % rem.size();
            const Vec2d &a = p2[rem[i_prev]], &b = p2[rem[i]], &c = p2[rem[i_next]];
            const double cross = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
            if (cross <= 1e-12)
                continue; // reflex or collinear corner, not an ear
            bool contains_other = false;
            for (size_t k = 0; k < rem.size() && !contains_other; ++k)
                if (k != i && k != i_prev && k != i_next && point_in_triangle_2d(p2[rem[k]], a, b, c))
                    contains_other = true;
            if (contains_other)
                continue;
            out.triangles.emplace_back(rem[i_prev], rem[i], rem[i_next]);
            rem.erase(rem.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) {
            // The ears already clipped stand; the stubborn remainder is fanned closed.
            fan_from_centre(pts, rem, n, out);
            return out;
        }
    }
    if (rem.size() == 3)
        out.triangles.emplace_back(rem[0], rem[1], rem[2]);
    return out;
}

// ---- the split ---------------------------------------------------------------------------

//One side of the split under construction: the remap from source vertex ids into this part's
//own vertex array, built lazily as faces arrive.
struct PartBuilder
{
    Part            &part;
    std::vector<int> vertex_map;
    const indexed_triangle_set &src;

    PartBuilder(Part &p, const indexed_triangle_set &s) : part(p), src(s)
    {
        vertex_map.assign(s.vertices.size(), -1);
    }
    int remap(int src_vertex)
    {
        int &m = vertex_map[src_vertex];
        if (m < 0) {
            m = int(part.mesh.vertices.size());
            part.mesh.vertices.emplace_back(src.vertices[src_vertex]);
        }
        return m;
    }
    void add_surface_face(const stl_triangle_vertex_indices &t)
    {
        part.mesh.indices.emplace_back(remap(t[0]), remap(t[1]), remap(t[2]));
        ++part.surface_faces;
    }
    int add_point(const Vec3f &p)
    {
        part.mesh.vertices.emplace_back(p);
        return int(part.mesh.vertices.size()) - 1;
    }
};

bool separate(const indexed_triangle_set &its,
              const std::vector<Vec3i32> &face_neighbors,
              const std::vector<char>    &selected,
              Result                     &out)
{
    out = Result{};
    const int n = int(its.indices.size());
    if (n == 0 || int(selected.size()) != n) {
        out.error = "no mesh";
        return false;
    }
    size_t count = 0;
    for (char s : selected)
        count += s != 0;
    if (count == 0) {
        out.error = "the selection is empty";
        return false;
    }
    if (count == size_t(n)) {
        out.error = "the selection covers the whole mesh; there is nothing to detach";
        return false;
    }

    // The seam, directed as the selected faces wind it.
    std::vector<SeamEdge> seam;
    for (int f = 0; f < n; ++f) {
        if (!selected[f])
            continue;
        for (int j = 0; j < 3; ++j) {
            const int nb = face_neighbors[f][j];
            if (nb >= 0 && !selected[nb])
                seam.push_back({its.indices[f][j], its.indices[f][(j + 1) % 3], false});
        }
    }
    std::vector<std::vector<int>> loops;
    chain_seam_loops(seam, loops, out.open_chains);
    out.seam_loops = loops.size();

    PartBuilder region(out.region, its);
    PartBuilder rest(out.rest, its);
    for (int f = 0; f < n; ++f)
        (selected[f] ? region : rest).add_surface_face(its.indices[f]);

    //One patch per loop, appended to BOTH parts: the rest keeps the emitted winding (its faces
    //see the seam running opposite to the selection's, so the patch as wound closes it), the
    //region takes the same triangles reversed. Identical geometry on both sides is what makes
    //the parts mate exactly and the cap contributions cancel out of the volume sum.
    for (const std::vector<int> &loop : loops) {
        std::vector<Vec3f> pts;
        pts.reserve(loop.size());
        for (int src_vertex : loop)
            pts.emplace_back(its.vertices[src_vertex]);
        CapPatch patch = triangulate_loop(pts);
        if (patch.used_fallback)
            ++out.fallback_fans;

        const int loop_size = int(loop.size());
        int centre_region = -1, centre_rest = -1;
        if (patch.has_centre) {
            centre_region = region.add_point(patch.centre);
            centre_rest   = rest.add_point(patch.centre);
        }
        auto region_idx = [&](int k) { return k == loop_size ? centre_region : region.remap(loop[k]); };
        auto rest_idx   = [&](int k) { return k == loop_size ? centre_rest : rest.remap(loop[k]); };
        for (const Vec3i32 &t : patch.triangles) {
            out.rest.mesh.indices.emplace_back(rest_idx(t[0]), rest_idx(t[1]), rest_idx(t[2]));
            ++out.rest.cap_faces;
            out.region.mesh.indices.emplace_back(region_idx(t[0]), region_idx(t[2]), region_idx(t[1]));
            ++out.region.cap_faces;
        }
    }

    out.volume_source = signed_volume(its);
    out.volume_region = signed_volume(out.region.mesh);
    out.volume_rest   = signed_volume(out.rest.mesh);

    const double tolerance = 1e-6 * std::max(1.0, std::abs(out.volume_source));
    if (out.open_chains == 0 &&
        std::abs(out.volume_region + out.volume_rest - out.volume_source) > tolerance) {
        out.error = "the parts do not add up to the source; refusing the split";
        return false;
    }
    if (std::abs(out.volume_region) <= tolerance) {
        out.error = "the selected region is flat - it has no thickness to detach";
        return false;
    }
    return true;
}

}} // namespace Slic3r::MeshSeparator
