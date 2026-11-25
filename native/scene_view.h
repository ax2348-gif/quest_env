#pragma once
// The rigid scene in the AR view (GL thread): scene_client snapshots -> MeshRenderer meshes and instances, per
// frame every body's matrix in the XR LOCAL space through the AR placement (sim world -> operator FLU, scaled,
// -> LOCAL), the frame's colour overrides, and the style the server asks for.
//
// Geoms: role "auto" draws the robot's (and its ghosts') visual geoms (the URDF meshes) and everything else's
// collision geoms (what the physics touches); "visual" / "collision" pick one everywhere; a body without any of
// the chosen role falls back to its others. Planes are left out (the view marks the floor itself).
#include "mesh_renderer.h"
#include "robot_ar.h"
#include "scene_client.h"
#include <map>

class SceneView {
  public:
    bool init() { return renderer_.init(); }
    void shutdown() { renderer_.shutdown(); }
    // Takes the newest snapshot; false when there is nothing to draw (no scene or frame yet, not placed).
    bool update(const scene_client::Snapshot &s, const robot_ar::Placement &pl, bool stale) {
        if (!s.scene || !pl.placed)
            return false;
        if (s.sceneVersion != version_ || s.view.role != role_)
            rebuild(*s.scene, s.view.role, s.sceneVersion);
        style_ = s.view.style == "wire" ? MeshRenderer::Wire : s.view.style == "flat" ? MeshRenderer::Flat
                 : s.view.style == "flat_edges" ? MeshRenderer::FlatEdges : MeshRenderer::HiddenLine;
        const auto &sc = *s.scene;
        auto &bm = renderer_.bodies();
        bm.resize(sc.bodies.size());
        // sim world -> LOCAL: questToBase^T (FLU -> LOCAL) * placement (sim -> FLU, scaled about its origin)
        mesh_scene::Mat4 C = toMat(teleop::transpose(teleop::questToBase()), {0, 0, 0});
        teleop::Pose w = pl.world();
        mesh_scene::Mat4 P = mesh_scene::mul(mesh_scene::mul(C, toMat(w.r, w.p)),
                                             mesh_scene::scale(float(pl.scale), float(pl.scale), float(pl.scale)));
        const mesh_scene::Frame *f = s.frame.get();
        std::vector<int> slotOf(sc.bodies.size(), -1);
        for (size_t k = 0; k < sc.dynamic.size(); k++)
            slotOf[sc.dynamic[k]] = int(k);
        renderer_.clearBodyColors();
        for (size_t b = 0; b < sc.bodies.size(); b++) {
            const float *pose = sc.bodies[b].pose;
            int slot = slotOf[b];
            if (f && slot >= 0 && size_t(slot) * 7 + 7 <= f->poses.size())
                pose = &f->poses[size_t(slot) * 7];
            bm[b] = mesh_scene::mul(P, mesh_scene::fromPose(pose));
        }
        if (f)
            for (const auto &c : f->colors)
                if (c.slot < sc.dynamic.size())
                    renderer_.setBodyColor(sc.dynamic[c.slot], c.rgba);
        if (stale) // the stream is late: dim everything, as the description view does
            for (size_t b = 0; b < sc.bodies.size(); b++) {
                float dim[4] = {.3f, .3f, .3f, 1};
                renderer_.setBodyColor(int(b), dim);
            }
        renderer_.beginFrame();
        return true;
    }
    MeshRenderer::Stats draw(const mesh_scene::Mat4 &viewProj) { return renderer_.draw(viewProj, style_); }
    long triangles() const { return triangles_; }

  private:
    static mesh_scene::Mat4 toMat(const teleop::M &r, const teleop::V &p) {
        mesh_scene::Mat4 m;
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++)
                m.m[col * 4 + row] = float(r(row, col));
        m.m[12] = float(p.x), m.m[13] = float(p.y), m.m[14] = float(p.z);
        return m;
    }
    static bool wants(const std::string &geomRole, const std::string &want) {
        return geomRole == want || geomRole == "both";
    }
    int primitive(const std::string &key, double h = 0) {
        auto it = prims_.find(key);
        if (it != prims_.end())
            return it->second;
        mesh_scene::Mesh m = key == "box" ? mesh_scene::boxMesh() : key == "cylinder" ? mesh_scene::cylinderMesh()
                             : key == "sphere" ? mesh_scene::sphereMesh() : mesh_scene::capsuleMesh(h);
        int id = renderer_.upload(m);
        prims_[key] = id;
        return id;
    }
    void rebuild(const mesh_scene::Scene &sc, const std::string &role, long version) {
        renderer_.clearMeshes();
        prims_.clear();
        std::vector<int> meshIds;
        for (const auto &m : sc.meshes)
            meshIds.push_back(renderer_.upload(m));
        triangles_ = 0;
        for (const auto &b : sc.bodies) {
            std::string want = role;
            if (role != "visual" && role != "collision")
                want = (b.kind == "robot" || b.kind == "ghost") ? "visual" : "collision";
            bool any = false;
            for (const auto &g : b.geoms)
                any = any || wants(g.role, want);
            for (const auto &g : b.geoms) {
                if (any && !wants(g.role, want))
                    continue;
                mesh_scene::Mat4 local;
                if (!mesh_scene::geomMatrix(g, local))
                    continue; // a plane
                int mesh;
                if (g.type == "mesh") {
                    mesh = meshIds[g.mesh];
                    triangles_ += long(sc.meshes[g.mesh].ntri());
                } else if (g.type == "capsule") {
                    double h = g.radius > 0 ? g.halfLength / g.radius : 0;
                    char key[48];
                    std::snprintf(key, sizeof key, "capsule:%.3f", h);
                    mesh = primitive(key, h);
                } else
                    mesh = primitive(g.type == "ellipsoid" ? "sphere" : g.type);
                renderer_.addInstance(mesh, b.id, local, g.rgba);
            }
        }
        version_ = version;
        role_ = role;
    }
    MeshRenderer renderer_;
    MeshRenderer::Style style_ = MeshRenderer::HiddenLine;
    std::map<std::string, int> prims_;
    long version_ = -1, triangles_ = 0;
    std::string role_;
};
