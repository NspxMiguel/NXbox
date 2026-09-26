// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glad/glad.h>

// Diagnostic readbacks write into client memory only while no pixel pack buffer is bound; Eden
// leaves one bound, which makes glReadPixels and glGetTextureImage skip the client array.
struct NxboxPackBufferGuard {
    NxboxPackBufferGuard() {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    ~NxboxPackBufferGuard() {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previous));
    }
    GLint previous = 0;
};
