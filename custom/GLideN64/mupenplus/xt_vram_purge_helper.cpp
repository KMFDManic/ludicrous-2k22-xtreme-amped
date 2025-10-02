#include "OpenGL.h"
#include "FrameBuffer.h"
#include "Textures.h"

extern "C" void xt_vram_purge_soft(void)
{
    // 1) Drop all FBOs
    FrameBufferList::get().destroy();

    // 2) Drop and re-init all GL textures (cache + FB textures)
    TextureCache &tc = TextureCache::get();
    tc.destroy();
    tc.init();

    // 3) Force driver to process deletes now (prevents deferred frees)
    glFinish();
}

