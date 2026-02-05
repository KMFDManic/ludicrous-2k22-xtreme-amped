#include "OpenGL.h"
#include "FrameBuffer.h"
#include "Textures.h"
#include "DepthBuffer.h"

extern "C" void xt_vram_purge_soft(void)
{
    // IMPORTANT: Ensure no in-flight GL work is still referencing objects we delete.
    glFinish();

    // 1) Depth buffers FIRST.
    // Their destructors can call textureCache().removeFrameBufferTexture(...),
    // so TextureCache must still be alive at this point.
    DepthBuffer_Destroy();

    // 2) Drop all FBOs.
    FrameBufferList::get().destroy();

    // 3) Drop + re-init texture cache.
    TextureCache &tc = TextureCache::get();
    tc.destroy();
    tc.init();

    // 4) Re-init depth buffer list (lightweight; does NOT touch gDP).
    DepthBuffer_Init();

    // 5) Force driver to process deletes now.
    glFinish();
}

