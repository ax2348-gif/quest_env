#pragma once
// Meshes on the GPU, drawn per body (robot_utils docs/scene3d.md): each mesh uploaded once (vertices, triangles,
// outline edges), each instance a mesh on a body with its matrix in the body and a colour; per frame the caller
// sets every body's matrix (render space = the XR reference space the views are located in) and draws each eye.
//
// Styles over the same data:
//   Wire        edges only, every edge visible (today's AR look, now on the GPU)
//   HiddenLine  triangles into depth only (no colour: passthrough shows through), then the edges depth-tested:
//               near surfaces hide far lines
//   Flat        triangles shaded by their face normal (screen-space derivatives: no normals stored)
//   FlatEdges   Flat plus darker edges
// Colour: each instance's own, or its body's override for this frame (setBodyColor: a server-picked colour, e.g. a
// goal ghost by its error). Alpha < 1 is drawn see-through after everything opaque (blended, no depth writes, so it
// never hides what is behind it); alpha 0 is not drawn.
// Needs a current GLES 3.0 context; the caller binds the eye's framebuffer (with depth) and viewport.
#include "mesh_scene.h"
#include <GLES3/gl3.h>
#include <algorithm>
#include <vector>

class MeshRenderer {
  public:
    enum Style { Wire, HiddenLine, Flat, FlatEdges };
    static const char *styleName(Style s) {
        return s == Wire ? "wire" : s == HiddenLine ? "hidden_line" : s == Flat ? "flat" : "flat_edges";
    }
    struct Stats {
        int drawCalls = 0;
        long triangles = 0, edges = 0;
    };

    bool init() {
        const char *vs = "#version 300 es\n"
                         "layout(location=0) in vec3 p;uniform mat4 vp;uniform mat4 model;out vec3 wp;"
                         "void main(){vec4 w=model*vec4(p,1.0);wp=w.xyz;gl_Position=vp*w;}";
        const char *fs = "#version 300 es\nprecision highp float;"
                         "in vec3 wp;uniform vec4 color;uniform float shade;out vec4 o;"
                         "void main(){vec3 n=normalize(cross(dFdx(wp),dFdy(wp)));"
                         "float l=mix(1.0,0.45+0.55*abs(dot(n,normalize(vec3(0.3,0.9,0.4)))),shade);"
                         "o=vec4(color.rgb*l,color.a);}";
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
        if (!v || !f)
            return false;
        program_ = glCreateProgram();
        glAttachShader(program_, v);
        glAttachShader(program_, f);
        glLinkProgram(program_);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok = 0;
        glGetProgramiv(program_, GL_LINK_STATUS, &ok);
        if (!ok)
            return false;
        vpLoc_ = glGetUniformLocation(program_, "vp");
        modelLoc_ = glGetUniformLocation(program_, "model");
        colorLoc_ = glGetUniformLocation(program_, "color");
        shadeLoc_ = glGetUniformLocation(program_, "shade");
        return glGetError() == GL_NO_ERROR;
    }
    void shutdown() {
        clearMeshes();
        if (program_)
            glDeleteProgram(program_);
        program_ = 0;
    }

    // A mesh to the GPU; returns its id (instances refer to it).
    int upload(const mesh_scene::Mesh &m) {
        Gpu g;
        glGenVertexArrays(1, &g.vao);
        glGenBuffers(3, g.buf);
        glBindVertexArray(g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g.buf[0]);
        glBufferData(GL_ARRAY_BUFFER, m.v.size() * sizeof(float), m.v.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr);
        glBindVertexArray(0);
        g.u32 = m.nvert() > 65535;
        auto indices = [&](GLuint buf, const std::vector<uint32_t> &ix) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf);
            if (g.u32) {
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, ix.size() * 4, ix.data(), GL_STATIC_DRAW);
            } else {
                std::vector<uint16_t> s(ix.begin(), ix.end());
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, s.size() * 2, s.data(), GL_STATIC_DRAW);
            }
        };
        indices(g.buf[1], m.tri);
        indices(g.buf[2], m.edge);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        g.ntri = GLsizei(m.tri.size());
        g.nedge = GLsizei(m.edge.size());
        meshes_.push_back(g);
        return int(meshes_.size()) - 1;
    }
    void clearMeshes() {
        for (auto &g : meshes_) {
            glDeleteBuffers(3, g.buf);
            glDeleteVertexArrays(1, &g.vao);
        }
        meshes_.clear();
        instances_.clear();
    }

    // A mesh on body `body` at `local` (its matrix in the body, scale included) in colour rgba.
    void addInstance(int mesh, int body, const mesh_scene::Mat4 &local, const float rgba[4]) {
        Instance in;
        in.mesh = mesh, in.body = body, in.local = local;
        for (int i = 0; i < 4; i++)
            in.rgba[i] = rgba[i];
        instances_.push_back(in);
        if (body >= int(bodies_.size()))
            bodies_.resize(body + 1);
    }
    void clearInstances() { instances_.clear(); }
    // Every body's matrix in render space, set before drawing a frame.
    std::vector<mesh_scene::Mat4> &bodies() { return bodies_; }
    // This frame's colour for a body (every instance on it), or back to the instances' own (rgba null).
    void setBodyColor(int body, const float *rgba) {
        if (body >= int(bodyColor_.size()))
            bodyColor_.resize(body + 1);
        auto &c = bodyColor_[body];
        c.set = rgba != nullptr;
        if (rgba)
            for (int i = 0; i < 4; i++)
                c.rgba[i] = rgba[i];
    }
    void clearBodyColors() { bodyColor_.clear(); }
    size_t instanceCount() const { return instances_.size(); }

    // One eye. The caller has bound its framebuffer (colour + depth) and cleared it.
    Stats draw(const mesh_scene::Mat4 &viewProj, Style style, float edgeDim = .55f) {
        Stats st;
        if (!program_)
            return st;
        if (models_.size() != instances_.size() || modelsFrame_ != frame_) {
            models_.resize(instances_.size());
            for (size_t i = 0; i < instances_.size(); i++)
                models_[i] = mesh_scene::mul(bodies_[instances_[i].body], instances_[i].local);
            modelsFrame_ = frame_;
        }
        glUseProgram(program_);
        glUniformMatrix4fv(vpLoc_, 1, GL_FALSE, viewProj.m);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LEQUAL);
        glDisable(GL_CULL_FACE);
        const bool tris = style != Wire, edges = style != Flat;
        for (int pass = 0; pass < 2; pass++) { // opaque, then see-through
            const bool see = pass == 1;
            if (see) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
            }
            if (tris) {
                // hidden-line: opaque surfaces only write depth (passthrough shows through); see-through ones are
                // tinted surfaces so their volume reads
                const bool depthOnly = style == HiddenLine && !see;
                if (depthOnly)
                    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glEnable(GL_POLYGON_OFFSET_FILL); // surfaces slightly back: their own edges stay in front
                glPolygonOffset(1.f, 1.f);
                glUniform1f(shadeLoc_, 1.f);
                for (size_t i = 0; i < instances_.size(); i++)
                    if (transparent(i) == see)
                        st.triangles += drawOne(i, true, 1.f, st);
                glDisable(GL_POLYGON_OFFSET_FILL);
                if (depthOnly)
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }
            if (edges || see) {
                glUniform1f(shadeLoc_, 0.f);
                for (size_t i = 0; i < instances_.size(); i++)
                    if (transparent(i) == see)
                        st.edges += drawOne(i, false, style == FlatEdges ? edgeDim : 1.f, st);
            }
            if (see) {
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            }
        }
        glBindVertexArray(0);
        glUseProgram(0);
        return st;
    }
    // Call once per frame after setting bodies(): instance matrices are recomputed once, shared by both eyes.
    void beginFrame() { frame_++; }

  private:
    struct Gpu {
        GLuint vao = 0, buf[3] = {0, 0, 0};
        GLsizei ntri = 0, nedge = 0;
        bool u32 = false;
    };
    struct Instance {
        int mesh = 0, body = 0;
        mesh_scene::Mat4 local;
        float rgba[4] = {1, 1, 1, 1};
    };
    struct BodyColor {
        bool set = false;
        float rgba[4] = {1, 1, 1, 1};
    };
    const float *color(size_t i) const {
        const Instance &in = instances_[i];
        if (in.body < int(bodyColor_.size()) && bodyColor_[in.body].set)
            return bodyColor_[in.body].rgba;
        return in.rgba;
    }
    bool transparent(size_t i) const { return color(i)[3] < .999f; }
    long drawOne(size_t i, bool tris, float dim, Stats &st) {
        const Instance &in = instances_[i];
        const Gpu &g = meshes_[in.mesh];
        GLsizei n = tris ? g.ntri : g.nedge;
        const float *c = color(i);
        if (!n || c[3] <= 0)
            return 0;
        // see-through: surfaces faint, edges at the full alpha so the outline reads
        float a = c[3] < .999f && !tris ? std::min(1.f, c[3] * 2.f) : c[3];
        glUniformMatrix4fv(modelLoc_, 1, GL_FALSE, models_[i].m);
        glUniform4f(colorLoc_, c[0] * dim, c[1] * dim, c[2] * dim, a);
        glBindVertexArray(g.vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.buf[tris ? 1 : 2]);
        glDrawElements(tris ? GL_TRIANGLES : GL_LINES, n, g.u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, nullptr);
        st.drawCalls++;
        return tris ? n / 3 : n / 2;
    }
    static GLuint compile(GLenum type, const char *text) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &text, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            glDeleteShader(s);
            return 0;
        }
        return s;
    }
    GLuint program_ = 0;
    GLint vpLoc_ = -1, modelLoc_ = -1, colorLoc_ = -1, shadeLoc_ = -1;
    std::vector<Gpu> meshes_;
    std::vector<Instance> instances_;
    std::vector<mesh_scene::Mat4> bodies_, models_;
    std::vector<BodyColor> bodyColor_;
    long frame_ = 0, modelsFrame_ = -1;
};
