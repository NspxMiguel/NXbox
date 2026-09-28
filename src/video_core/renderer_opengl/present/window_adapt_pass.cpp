// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 Torzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/nxbox_gl_readback.h"
#include "common/settings.h"
#include "video_core/framebuffer_config.h"

#ifdef _WIN32
#include <algorithm>
#include <fstream>
#include <vector>
#include "common/fs/path_util.h"
#include "common/logging.h"
// Declared directly: windows.h in GL code clashes with glad.
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char*, char*,
                                                                                 unsigned long);
#endif
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
            // NXBOX diagnostic: save a small thumbnail of the presented layer texture, at most
            // 10 times, at the same place window_adapt_pass.cpp used before (proven to write
            // successfully): LocalState\eden\log.
            static unsigned draws = 0;
            static unsigned saved = 0;
            // Keeps sampling for as long as the game runs, cycling through 4 files so a late
            // thumbnail (after a long intro) can still be pulled.
            if (++draws % 300 == 1) {
                NxboxPackBufferGuard pack_guard;
                GLint tex_w = 0;
                GLint tex_h = 0;
                GLint tex_format = 0;
                GLint tex_levels = 0;
                GLint tex_samples = 0;
                glGetTextureLevelParameteriv(textures[i], 0, GL_TEXTURE_WIDTH, &tex_w);
                glGetTextureLevelParameteriv(textures[i], 0, GL_TEXTURE_HEIGHT, &tex_h);
                glGetTextureLevelParameteriv(textures[i], 0, GL_TEXTURE_INTERNAL_FORMAT,
                                             &tex_format);
                glGetTextureParameteriv(textures[i], GL_TEXTURE_IMMUTABLE_LEVELS, &tex_levels);
                glGetTextureLevelParameteriv(textures[i], 0, GL_TEXTURE_SAMPLES, &tex_samples);
                LOG_CRITICAL(Render_OpenGL,
                             "NXBOX texture={} format={:#x} levels={} samples={} error={:#x}",
                             textures[i], tex_format, tex_levels, tex_samples, glGetError());
                if (tex_w > 0 && tex_h > 0) {
                    constexpr int kW = 16;
                    constexpr int kH = 9;
                    std::vector<unsigned char> thumb(kW * kH * 3);
                    const auto sample = [&] {
                        unsigned peak = 0;
                        for (int y = 0; y < kH; ++y) {
                            for (int x = 0; x < kW; ++x) {
                                unsigned char px[4]{};
                                glGetTextureSubImage(textures[i], 0, x * tex_w / kW,
                                                     y * tex_h / kH, 0, 1, 1, 1, GL_RGBA,
                                                     GL_UNSIGNED_BYTE, 4, px);
                                peak = std::max<unsigned>(peak, std::max({px[0], px[1], px[2]}));
                                const size_t o = (static_cast<size_t>(y) * kW + x) * 3;
                                thumb[o] = px[0];
                                thumb[o + 1] = px[1];
                                thumb[o + 2] = px[2];
                            }
                        }
                        return peak;
                    };
                    // Sync experiment: if only the synced samples are non-zero, the game's draws
                    // land but the D3D12 backend reads the resource before they are visible.
                    const unsigned peak_raw = sample();
                    glMemoryBarrier(GL_ALL_BARRIER_BITS);
                    const unsigned peak_barrier = sample();
                    glFinish();
                    const unsigned peak = sample();
                    // Also read what this pass just drew into the presented framebuffer.
                    unsigned char out_px[4]{};
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_draw_fb);
                    glReadPixels(static_cast<GLint>(layout.width / 2),
                                 static_cast<GLint>(layout.height / 2), 1, 1, GL_RGBA,
                                 GL_UNSIGNED_BYTE, out_px);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fb);
                    // Published by the patched Mesa d3d12 driver (tools/nxbox/patch_mesa_uwp.py).
                    char pso_report[96] = "unavailable";
                    GetEnvironmentVariableA("NXBOX_D3D12_PSO", pso_report, sizeof(pso_report));
                    char dxil_report[96] = "unavailable";
                    GetEnvironmentVariableA("NXBOX_DXIL", dxil_report, sizeof(dxil_report));
                    char validate_report[96] = "unavailable";
                    GetEnvironmentVariableA("NXBOX_DXIL_VALIDATE", validate_report,
                                            sizeof(validate_report));
                    // Readback self test: clear a private texture to a known color and read it
                    // back. If this fails, none of the peak/out_center numbers can be trusted.
                    {
                        GLuint probe_tex = 0;
                        GLuint probe_fb = 0;
                        glCreateTextures(GL_TEXTURE_2D, 1, &probe_tex);
                        glTextureStorage2D(probe_tex, 1, GL_RGBA8, 4, 4);
                        glCreateFramebuffers(1, &probe_fb);
                        glNamedFramebufferTexture(probe_fb, GL_COLOR_ATTACHMENT0, probe_tex, 0);
                        const GLfloat probe_color[4]{37 / 255.0f, 99 / 255.0f, 201 / 255.0f, 1.0f};
                        glClearNamedFramebufferfv(probe_fb, GL_COLOR, 0, probe_color);
                        glFinish();
                        unsigned char probe_px[4]{};
                        glGetTextureSubImage(probe_tex, 0, 1, 1, 0, 1, 1, 1, GL_RGBA,
                                             GL_UNSIGNED_BYTE, 4, probe_px);
                        LOG_CRITICAL(Render_OpenGL, "NXBOX readback_probe={},{},{} expected=37,99,201",
                                     probe_px[0], probe_px[1], probe_px[2]);
                        glDeleteFramebuffers(1, &probe_fb);
                        glDeleteTextures(1, &probe_tex);
                    }
                    char draw_report[400] = "unavailable";
                    GetEnvironmentVariableA("NXBOX_D3D12_DRAW", draw_report, sizeof(draw_report));
                    char quad_report[400] = "unavailable";
                    GetEnvironmentVariableA("NXBOX_D3D12_QUAD", quad_report, sizeof(quad_report));
                    char batch_report[160] = "unavailable";
                    GetEnvironmentVariableA("NXBOX_D3D12_BATCH", batch_report, sizeof(batch_report));
                    char message_report[400] = "none";
                    GetEnvironmentVariableA("NXBOX_D3D12_MESSAGE", message_report,
                                            sizeof(message_report));
                    LOG_CRITICAL(Render_OpenGL, "NXBOX batch=[{}] message=[{}]", batch_report,
                                 message_report);
                    char first_failure[64] = "none";
                    GetEnvironmentVariableA("NXBOX_D3D12_FIRST_FAILURE", first_failure,
                                            sizeof(first_failure));
                    char first_message[1000] = "none";
                    GetEnvironmentVariableA("NXBOX_D3D12_FIRST_MESSAGE", first_message,
                                            sizeof(first_message));
                    char reset_report[128] = "none";
                    GetEnvironmentVariableA("NXBOX_D3D12_RESET", reset_report, sizeof(reset_report));
                    LOG_CRITICAL(Render_OpenGL, "NXBOX first_failure=[{}] reset=[{}] first_message={}",
                                 first_failure, reset_report, first_message);
                    LOG_CRITICAL(Render_OpenGL, "NXBOX draw=[{}]", draw_report);
                    LOG_CRITICAL(Render_OpenGL, "NXBOX quad=[{}]", quad_report);
                    static bool dxil_error_logged = false;
                    char dxil_error[400] = "";
                    if (!dxil_error_logged &&
                        GetEnvironmentVariableA("NXBOX_DXIL_ERROR", dxil_error, sizeof(dxil_error))) {
                        dxil_error_logged = true;
                        LOG_CRITICAL(Render_OpenGL, "NXBOX dxil_error=[{}]", dxil_error);
                    }
                    LOG_CRITICAL(Render_OpenGL,
                                 "NXBOX sync peak_raw={} peak_barrier={} peak_finish={} "
                                 "out_center={},{},{} draw_fb={} err={:#x} pso=[{}] dxil=[{}] validate=[{}]",
                                 peak_raw, peak_barrier, peak, out_px[0], out_px[1], out_px[2],
                                 old_draw_fb, glGetError(), pso_report, dxil_report,
                                 validate_report);
                    const unsigned slot = saved % 4;
                    std::ofstream out(Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                                          fmt::format("thumb_{}.ppm", slot),
                                      std::ios::binary);
                    out << "P6\n" << kW << ' ' << kH << "\n255\n";
                    out.write(reinterpret_cast<const char*>(thumb.data()),
                              static_cast<std::streamsize>(thumb.size()));
                    LOG_CRITICAL(Render_OpenGL,
                                 "NXBOX thumb_{} (save #{}) tex={}x{} peak={} layer={}/{} "
                                 "invert_y={}",
                                 slot, saved, tex_w, tex_h, peak, i, layer_count, invert_y);
                    ++saved;
                }
            }
        }
#endif
    }
}

} // namespace OpenGL
