// The on-screen controls' renderer. See iTouchOverlay.h.

#include <rwcore.h>

#include "rw.h"

#include "backend.h"

#include "iTouchOverlay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(RW_GL3) || defined(RW_VULKAN)
namespace
{
    // 5x7, top row first, most significant of the five bits on the left.
    struct Glyph
    {
        char c;
        U8 rows[7];
    };

    const Glyph kGlyphs[] = {
        { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
        { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
        { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
        { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
        { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
        { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
        { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
        { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
        { '>', { 0x10, 0x18, 0x1C, 0x1E, 0x1C, 0x18, 0x10 } },
        { '=', { 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x1F, 0x00 } },
    };

    const Glyph* FindGlyph(char c)
    {
        for (U32 i = 0; i < sizeof(kGlyphs) / sizeof(kGlyphs[0]); i++)
        {
            if (kGlyphs[i].c == c)
            {
                return &kGlyphs[i];
            }
        }
        return NULL;
    }

#ifdef RW_GL3
    // Every shape, circle or box, filled or outlined, is one signed distance to
    // a rounded box, so there is one program and one draw call. The Vulkan
    // device has the same shader, in librw's shaders/overlay.frag.
    const char* kVertexSrc =
        "uniform vec2 u_screen;\n"
        "VSIN(0) vec2 in_pos;\n"
        "VSIN(1) vec2 in_local;\n"
        "VSIN(2) vec2 in_half;\n"
        "VSIN(3) vec2 in_shape;\n"
        "VSIN(4) vec4 in_color;\n"
        "VSOUT vec2 v_local;\n"
        "VSOUT vec2 v_half;\n"
        "VSOUT vec2 v_shape;\n"
        "VSOUT vec4 v_color;\n"
        "void main(void)\n"
        "{\n"
        "    gl_Position = vec4(in_pos.x / u_screen.x * 2.0 - 1.0,\n"
        "                       1.0 - in_pos.y / u_screen.y * 2.0, 0.0, 1.0);\n"
        "    v_local = in_local;\n"
        "    v_half = in_half;\n"
        "    v_shape = in_shape;\n"
        "    v_color = in_color;\n"
        "}\n";

    const char* kFragmentSrc =
        "#ifndef GL2\n"
        "out vec4 fragColor;\n"
        "#endif\n"
        "FSIN vec2 v_local;\n"
        "FSIN vec2 v_half;\n"
        "FSIN vec2 v_shape;\n"
        "FSIN vec4 v_color;\n"
        "void main(void)\n"
        "{\n"
        "    vec2 q = abs(v_local) - (v_half - vec2(v_shape.x));\n"
        "    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_shape.x;\n"
        "    if(v_shape.y > 0.0)\n"
        "        d = abs(d + v_shape.y * 0.5) - v_shape.y * 0.5;\n"
        "    float a = clamp(0.5 - d, 0.0, 1.0);\n"
        "    FRAGCOLOR(vec4(v_color.rgb, v_color.a * a));\n"
        "}\n";
#endif

    // Twelve floats, which is also rw::d3d::implvk::drawPresentOverlay's vertex.
    struct Vertex
    {
        F32 x, y;
        F32 lx, ly;
        F32 hw, hh;
        F32 corner, stroke;
        F32 r, g, b, a;
    };

    // Shapes plus up to 35 cells per label.
    const S32 kMaxQuads = ITOUCHOVERLAY_MAX_SHAPES * (1 + 35);

    Vertex sVertices[kMaxQuads * 6];

#ifdef RW_GL3
    GLuint sProgram;
    GLint sScreenLoc = -1;
    GLuint sVbo;
    GLuint sVao;
    bool sBuilt;
    bool sFailed;

    GLuint Compile(GLenum type, const char* body)
    {
        const char* parts[2] = { rw::gl3::shaderDecl, body };
        GLuint s = glCreateShader(type);
        glShaderSource(s, 2, parts, NULL);
        glCompileShader(s);

        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[1024];
            glGetShaderInfoLog(s, sizeof(log), NULL, log);
            printf("bfbb: touch overlay shader did not compile: %s\n", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    bool Build()
    {
        GLuint vs = Compile(GL_VERTEX_SHADER, kVertexSrc);
        GLuint fs = Compile(GL_FRAGMENT_SHADER, kFragmentSrc);
        if (vs == 0 || fs == 0)
        {
            return false;
        }

        sProgram = glCreateProgram();
        glAttachShader(sProgram, vs);
        glAttachShader(sProgram, fs);

        // GLSL ES 1.00 and 1.20 have no layout qualifiers; VSIN drops the index
        // there, so the locations are bound by name as well.
        glBindAttribLocation(sProgram, 0, "in_pos");
        glBindAttribLocation(sProgram, 1, "in_local");
        glBindAttribLocation(sProgram, 2, "in_half");
        glBindAttribLocation(sProgram, 3, "in_shape");
        glBindAttribLocation(sProgram, 4, "in_color");

        glLinkProgram(sProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint ok = 0;
        glGetProgramiv(sProgram, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[1024];
            glGetProgramInfoLog(sProgram, sizeof(log), NULL, log);
            printf("bfbb: touch overlay shader did not link: %s\n", log);
            return false;
        }

        sScreenLoc = glGetUniformLocation(sProgram, "u_screen");

        glGenBuffers(1, &sVbo);
        if (rw::gl3::gl3Caps.glversion >= 30)
        {
            glGenVertexArrays(1, &sVao);
        }
        return true;
    }
#endif

    void Quad(Vertex*& v, F32 cx, F32 cy, F32 hw, F32 hh, F32 corner, F32 stroke, F32 r,
              F32 g, F32 b, F32 a)
    {
        // One pixel beyond the shape, for the antialiased edge.
        const F32 ex = hw + 1.0f;
        const F32 ey = hh + 1.0f;
        static const F32 kCorners[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 },
                                            { -1, -1 }, { 1, 1 },  { -1, 1 } };

        for (S32 i = 0; i < 6; i++)
        {
            v->lx = kCorners[i][0] * ex;
            v->ly = kCorners[i][1] * ey;
            v->x = cx + v->lx;
            v->y = cy + v->ly;
            v->hw = hw;
            v->hh = hh;
            v->corner = corner;
            v->stroke = stroke;
            v->r = r;
            v->g = g;
            v->b = b;
            v->a = a;
            v++;
        }
    }

    // The submitted shapes as triangles in sVertices, labels included. Returns
    // the vertex count.
    S32 BuildVertices()
    {
        const iTouchOverlayShape* shapes;
        S32 shapeCount = iTouchOverlayShapes(&shapes);

        Vertex* v = sVertices;
        for (S32 i = 0; i < shapeCount; i++)
        {
            const iTouchOverlayShape& s = shapes[i];
            Quad(v, s.x, s.y, s.halfWidth, s.halfHeight, s.corner, s.stroke, s.r / 255.0f,
                 s.g / 255.0f, s.b / 255.0f, s.a / 255.0f);

            const Glyph* glyph = FindGlyph(s.label);
            if (glyph == NULL)
            {
                continue;
            }

            // Seven cells tall in half the shape's shorter side.
            F32 half = s.halfWidth < s.halfHeight ? s.halfWidth : s.halfHeight;
            F32 cell = half / 7.0f;
            F32 left = s.x - cell * 2.5f;
            F32 top = s.y - cell * 3.5f;
            F32 alpha = s.a < 128 ? 0.6f : 0.95f;

            for (S32 row = 0; row < 7; row++)
            {
                for (S32 col = 0; col < 5; col++)
                {
                    if (glyph->rows[row] & (0x10 >> col))
                    {
                        Quad(v, left + (col + 0.5f) * cell, top + (row + 0.5f) * cell,
                             cell * 0.5f, cell * 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, alpha);
                    }
                }
            }
        }

        return (S32)(v - sVertices);
    }

#ifdef RW_GL3
    rw::bool32 DrawGL(rw::int32 winWidth, rw::int32 winHeight)
    {
        iTouchOverlaySetWindowSize(winWidth, winHeight);

        const iTouchOverlayShape* shapes;
        if (iTouchOverlayShapes(&shapes) == 0 || sFailed)
        {
            return 0;
        }

        if (!sBuilt)
        {
            sBuilt = true;
            if (!Build())
            {
                sFailed = true;
                return 1;
            }
        }

        GLsizei vertexCount = (GLsizei)BuildVertices();

        glViewport(0, 0, winWidth, winHeight);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(sProgram);
        glUniform2f(sScreenLoc, (GLfloat)winWidth, (GLfloat)winHeight);

        if (sVao != 0)
        {
            glBindVertexArray(sVao);
        }
        glBindBuffer(GL_ARRAY_BUFFER, sVbo);
        glBufferData(GL_ARRAY_BUFFER, vertexCount * sizeof(Vertex), sVertices, GL_STREAM_DRAW);

        const GLsizei stride = sizeof(Vertex);
        const GLint sizes[5] = { 2, 2, 2, 2, 4 };
        GLint offset = 0;
        for (GLuint i = 0; i < 5; i++)
        {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, stride,
                                  (void*)(uintptr_t)offset);
            offset += sizes[i] * sizeof(F32);
        }

        glDrawArrays(GL_TRIANGLES, 0, vertexCount);

        for (GLuint i = 0; i < 5; i++)
        {
            glDisableVertexAttribArray(i);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return 1;
    }
#endif

#ifdef RW_VULKAN
    rw::bool32 DrawVulkan(rw::int32 winWidth, rw::int32 winHeight)
    {
        iTouchOverlaySetWindowSize(winWidth, winHeight);

        S32 vertexCount = BuildVertices();
        if (vertexCount == 0)
        {
            return 0;
        }

        rw::d3d::implvk::drawPresentOverlay((const rw::float32*)sVertices, vertexCount);
        return 1;
    }
#endif
} // namespace
#endif

void iTouchOverlayInstall()
{
#ifdef RW_GL3
    if (iBackendIsGL3())
    {
        rw::gl3::setPresentOverlay(DrawGL);
    }
#endif
#ifdef RW_VULKAN
    if (iBackendIsVulkan())
    {
        rw::d3d::implvk::setPresentOverlay(DrawVulkan);
    }
#endif
}
