// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glad/glad.h>

// Diagnostic readbacks write into client memory only while no pixel pack buffer is bound and the
// pack layout is plain. Eden leaves a pack buffer and non-default pack row/skip state behind, which
// makes glReadPixels and glGetTextureImage store the pixels somewhere else than the client array.
struct NxboxPackBufferGuard {
    NxboxPackBufferGuard() {
        // Wait for all queued GPU work so the readback below sees finished pixels.
        glFinish();
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &buffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_PACK_IMAGE_HEIGHT, &image_height);
        glGetIntegerv(GL_PACK_SKIP_IMAGES, &skip_images);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
        glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
    }
    ~NxboxPackBufferGuard() {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(buffer));
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
        glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, image_height);
        glPixelStorei(GL_PACK_SKIP_IMAGES, skip_images);
    }
    GLint buffer = 0;
    GLint alignment = 4;
    GLint row_length = 0;
    GLint skip_pixels = 0;
    GLint skip_rows = 0;
    GLint image_height = 0;
    GLint skip_images = 0;
};
