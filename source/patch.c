/* patch.c -- game-specific patches for libgodot_android.so (SMB1R arm64-v8a).
 *
 * Empty for now: no crash has required a binary hook yet. If hardware
 * bring-up hits a null-deref or similar bug in the engine itself (as
 * happened in smwr_nx with GodotJavaViewWrapper::can_capture_pointer()),
 * add a GamePatch entry here the same way -- validate the original
 * instruction word before hooking so a patch silently no-ops instead of
 * corrupting a binary it wasn't written for.
 *
 * MIT license; see LICENSE. */

#include <stdint.h>
#include <string.h>

#include "patch.h"
#include "so_util.h"
#include "util.h"

typedef struct { uint32_t vaddr_word0; uint32_t expect; uintptr_t vaddr; void *repl; const char *name; } GamePatch;

void so_patch(so_module *mod) {
  static const GamePatch patches[] = {
    // { 0, EXPECTED_WORD, VADDR, (void *)&replacement_fn, "name" },
  };

  for (unsigned i = 0; i < sizeof(patches) / sizeof(*patches); i++) {
    const GamePatch *p = &patches[i];
    // hooks are written into the RW backing (load_base) before so_finalize
    uint32_t *insn = (uint32_t *)((uintptr_t)mod->load_base + p->vaddr);
    if (*insn != p->expect) {
      debugPrintf("[patch] %s: unexpected bytes %08x at 0x%lx, skipping\n",
                  p->name, *insn, (unsigned long)p->vaddr);
      continue;
    }
    hook_arm64((uintptr_t)insn, (uintptr_t)p->repl);
    debugPrintf("[patch] %s hooked at vaddr 0x%lx\n", p->name, (unsigned long)p->vaddr);
  }
}
