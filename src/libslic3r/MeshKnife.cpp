#include "MeshKnife.hpp"

#include "libslic3r/TriangleMeshSlicer.hpp"

#include <cmath>

namespace Slic3r { namespace MeshKnife {

std::vector<std::pair<Vec3f, Vec3f>> contour(const indexed_triangle_set &its,
                                             const Vec3f                &point,
                                             const Vec3f                &normal)
{
    std::vector<std::pair<Vec3f, Vec3f>> segments;
    const Vec3f n = normal.normalized();
    const float offset = n.dot(point);

    std::vector<float> dist(its.vertices.size());
    for (size_t i = 0; i < its.vertices.size(); ++i) {
        float d = n.dot(its.vertices[i]) - offset;
        //A vertex exactly on the plane would make its edges neither cross nor not-cross;
        //nudging it to one side costs nothing visible and keeps every case binary.
        dist[i] = d == 0.f ? 1e-8f : d;
    }

    for (const stl_triangle_vertex_indices &t : its.indices) {
        Vec3f crossing[3];
        int   found = 0;
        for (int e = 0; e < 3 && found < 3; ++e) {
            const int   a = t[e], b = t[(e + 1) % 3];
            const float da = dist[a], db = dist[b];
            if (da * db < 0.f) {
                const float s = da / (da - db);
                crossing[found++] = its.vertices[a] + s * (its.vertices[b] - its.vertices[a]);
            }
        }
        if (found == 2)
            segments.emplace_back(crossing[0], crossing[1]);
    }
    return segments;
}

CutResult planar_cut(const indexed_triangle_set &its, const Vec3f &point, const Vec3f &normal)
{
    CutResult result;

    // Rotate the world so the knife lies flat, cut horizontally, rotate the halves back.
    const Eigen::Quaternionf rotation = Eigen::Quaternionf::FromTwoVectors(normal.normalized(), Vec3f::UnitZ());
    indexed_triangle_set     rotated  = its;
    for (Vec3f &v : rotated.vertices)
        v = rotation * v;
    const float z = (rotation * point).z();

    cut_mesh(rotated, z, &result.upper, &result.lower, true);

    const Eigen::Quaternionf back = rotation.conjugate();
    for (Vec3f &v : result.upper.vertices)
        v = back * v;
    for (Vec3f &v : result.lower.vertices)
        v = back * v;

    result.cut = !result.upper.indices.empty() && !result.lower.indices.empty();
    return result;
}

}} // namespace Slic3r::MeshKnife
