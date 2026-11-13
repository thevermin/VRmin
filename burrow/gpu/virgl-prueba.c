// Checks the Mac side of Burrow's GPU without Linux: starts virglrenderer with Venus (MoltenVK → Metal), asks for
// the Venus capabilities and creates a Venus context, like a Linux program's first Vulkan call would.
#include <stdio.h>
#include <stdint.h>
#include <virglrenderer.h>

static void fence(void *c, uint32_t f) { (void)c; (void)f; }
static void ctx_fence(void *c, uint32_t ctx, uint32_t ring, uint64_t f) { (void)c; (void)ctx; (void)ring; (void)f; }

int main(void) {
  struct virgl_renderer_callbacks cb = {0};
  cb.version = 3;
  cb.write_fence = fence;
  cb.write_context_fence = ctx_fence;
  int r = virgl_renderer_init(NULL, VIRGL_RENDERER_VENUS | VIRGL_RENDERER_NO_VIRGL, &cb);
  if (r) { printf("FAIL init: %d\n", r); return 1; }
  uint32_t ver = 0, size = 0;
  virgl_renderer_get_cap_set(4 /* VIRGL_RENDERER_CAPSET_VENUS */, &ver, &size);
  printf("venus capset: version %u, %u bytes\n", ver, size);
  if (!size) { printf("FAIL: no Venus capset (MoltenVK not found?)\n"); return 1; }
  r = virgl_renderer_context_create_with_flags(1, 4, 6, "prueba");
  printf("venus context: %d\n", r);
  if (r) { printf("FAIL context\n"); return 1; }
  virgl_renderer_context_destroy(1);
  virgl_renderer_cleanup(NULL);
  printf("VIRGL: TODO BIEN\n");
  return 0;
}
