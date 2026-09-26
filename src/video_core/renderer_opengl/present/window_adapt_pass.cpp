// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 Torzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "common/nxbox_gl_readback.h"
#include <fstream>
#include <vector>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/framebuffer_config.h"
#include "video_core/host_shaders/opengl_present_vert.h"
#include "video_core/renderer_opengl/gl_device.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_opengl/gl_shader_util.h"
#include "video_core/renderer_opengl/present/layer.h"
#include "video_core/renderer_opengl/present/present_uniforms.h"
#include "video_core/renderer_opengl/present/window_adapt_pass.h"

namespace OpenGL {

WindowAdaptPass::WindowAdaptPass(const Device& device_, OGLSampler&& sampler_,
                                 std::string_view frag_source)
    : device(device_), sampler(std::move(sampler_)) {
    vert = CreateProgram(HostShaders::OPENGL_PRESENT_VERT, GL_VERTEX_SHADER);
    frag = CreateProgram(frag_source, GL_FRAGMENT_SHADER);

    // Generate VBO handle for drawing
    vertex_buffer.Create();

    // Attach vertex data to VAO
    glNamedBufferData(vertex_buffer.handle, sizeof(ScreenRectVertex) * 4, nullptr, GL_STREAM_DRAW);

    // Query vertex buffer address when the driver supports unified vertex attributes
    if (device.HasVertexBufferUnifiedMemory()) {
        glMakeNamedBufferResidentNV(vertex_buffer.handle, GL_READ_ONLY);
        glGetNamedBufferParameterui64vNV(vertex_buffer.handle, GL_BUFFER_GPU_ADDRESS_NV,
                                         &vertex_buffer_address);
    }
}

WindowAdaptPass::~WindowAdaptPass() = default;

void WindowAdaptPass::DrawToFramebuffer(ProgramManager& program_manager, std::list<Layer>& layers,
                                        std::span<const Tegra::FramebufferConfig> framebuffers,
                                        const Layout::FramebufferLayout& layout, bool invert_y) {
    GLint old_read_fb;
    GLint old_draw_fb;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fb);

    const size_t layer_count = framebuffers.size();
    std::vector<GLuint> textures(layer_count);
    std::vector<std::array<GLfloat, 3 * 2>> matrices(layer_count);
    std::vector<std::array<ScreenRectVertex, 4>> vertices(layer_count);

    auto layer_it = layers.begin();
    for (size_t i = 0; i < layer_count; i++) {
        textures[i] = layer_it->ConfigureDraw(matrices[i], vertices[i], program_manager,
                                              framebuffers[i], layout, invert_y);
        layer_it++;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw_fb);

    program_manager.BindPresentPrograms(vert.handle, frag.handle);

    glDisable(GL_FRAMEBUFFER_SRGB);
    glViewportIndexedf(0, 0.0f, 0.0f, static_cast<GLfloat>(layout.width),
                       static_cast<GLfloat>(layout.height));

    glEnableVertexAttribArray(PositionLocation);
    glEnableVertexAttribArray(TexCoordLocation);
    glVertexAttribDivisor(PositionLocation, 0);
    glVertexAttribDivisor(TexCoordLocation, 0);
    glVertexAttribFormat(PositionLocation, 2, GL_FLOAT, GL_FALSE,
                         offsetof(ScreenRectVertex, position));
    glVertexAttribFormat(TexCoordLocation, 2, GL_FLOAT, GL_FALSE,
                         offsetof(ScreenRectVertex, tex_coord));
    glVertexAttribBinding(PositionLocation, 0);
    glVertexAttribBinding(TexCoordLocation, 0);
    if (device.HasVertexBufferUnifiedMemory()) {
        glBindVertexBuffer(0, 0, 0, sizeof(ScreenRectVertex));
        glBufferAddressRangeNV(GL_VERTEX_ATTRIB_ARRAY_ADDRESS_NV, 0, vertex_buffer_address,
                               sizeof(decltype(vertices)::value_type));
    } else {
        glBindVertexBuffer(0, vertex_buffer.handle, 0, sizeof(ScreenRectVertex));
    }

    glBindSampler(0, sampler.handle);

    // Update background color before drawing
    glClearColor(Settings::values.bg_red.GetValue() / 255.0f,
                 Settings::values.bg_green.GetValue() / 255.0f,
                 Settings::values.bg_blue.GetValue() / 255.0f, 1.0f);

    glClear(GL_COLOR_BUFFER_BIT);

    for (size_t i = 0; i < layer_count; i++) {
        switch (framebuffers[i].blending) {
        case Tegra::BlendMode::Opaque:
        default:
            glDisablei(GL_BLEND, 0);
            break;
        case Tegra::BlendMode::Premultiplied:
            glEnablei(GL_BLEND, 0);
            glBlendFuncSeparatei(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
            break;
        case Tegra::BlendMode::Coverage:
            glEnablei(GL_BLEND, 0);
            glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
            break;
        }

        glBindTextureUnit(0, textures[i]);
        glProgramUniformMatrix3x2fv(vert.handle, ModelViewMatrixLocation, 1, GL_FALSE, matrices[i].data());
        glNamedBufferSubData(vertex_buffer.handle, 0, sizeof(vertices[i]), std::data(vertices[i]));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
#ifdef _WIN32
        {
            // NXbox diagnostic: report what the presentation pass sampled, every 120 frames.
            static unsigned present_draws = 0;
            if (++present_draws % 120 == 1) {
                NxboxPackBufferGuard pack_guard;
                GLint tex_w = 0;
                GLint tex_h = 0;
                glGetTextureLevelParameteriv(textures[i], 0, GL_TEXTURE_WIDTH, &tex_w);
                glGetTextureLevelParameteriv(textures[i], 0, GL_TEXTURE_HEIGHT, &tex_h);
                unsigned peak = 0;
                for (int k = 0; k < 16 && tex_w > 0 && tex_h > 0; ++k) {
                    unsigned char px[4]{};
                    glGetTextureSubImage(textures[i], 0, (tex_w * (2 * (k % 4) + 1)) / 8,
                                         (tex_h * (2 * (k / 4) + 1)) / 8, 0, 1, 1, 1, GL_RGBA,
                                         GL_UNSIGNED_BYTE, 4, px);
                    peak = std::max<unsigned>(peak, std::max({px[0], px[1], px[2]}));
                }
                static unsigned dumped = 0;
                if (peak > 100 && dumped < 8 && tex_w > 0 && tex_h > 0) {
                    // Save frames that have visible content as PPM so the picture can be inspected.
                    std::vector<unsigned char> rgba(static_cast<size_t>(tex_w) * tex_h * 4);
                    glGetTextureImage(textures[i], 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                      static_cast<GLsizei>(rgba.size()), rgba.data());
                    std::ofstream out(Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                                          fmt::format("present_shot_{}.ppm", dumped++),
                                      std::ios::binary);
                    out << "P6\n" << tex_w << ' ' << tex_h << "\n255\n";
                    for (size_t px = 0; px < rgba.size(); px += 4) {
                        out.write(reinterpret_cast<const char*>(&rgba[px]), 3);
                    }
                }
                LOG_CRITICAL(Render_OpenGL,
                             "NXBOX present tex={} {}x{} peak={} verts=({:.0f},{:.0f})-({:.0f},{:.0f}) "
                             "layout={}x{} error={:#x}",
                             textures[i], tex_w, tex_h, peak, vertices[i][0].position[0],
                             vertices[i][0].position[1], vertices[i][3].position[0],
                             vertices[i][3].position[1], layout.width, layout.height, glGetError());
            }
        }
#endif
    }
}

} // namespace OpenGL
