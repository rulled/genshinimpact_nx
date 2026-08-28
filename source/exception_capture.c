/* Last-resort Horizon user-exception capture.
 *
 * An OOM or native data abort stops Unity with no other record of where it
 * failed.  libnx invokes this strong handler on a dedicated stack and then
 * still raises the ordinary fatal break, so this only appends a report to
 * fatal.txt; it never attempts recovery or changes the faulting control
 * flow. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "libc_shim.h"

#define NX_EXCEPTION_STACK_BYTES 0x8000u
#define NX_EXCEPTION_MAX_FRAMES  16u

__attribute__((aligned(16))) u8 __nx_exception_stack[NX_EXCEPTION_STACK_BYTES];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

extern void _start(void);
extern char __bss_end__[];

static void exception_print_address(FILE *file, const char *label,
                                    uint64_t raw) {
  const uintptr_t address = (uintptr_t)raw;
  const uintptr_t host_base = (uintptr_t)&_start;
  const uintptr_t host_end = (uintptr_t)__bss_end__;
  if (g_il2cpp_base && address >= g_il2cpp_base &&
      address - g_il2cpp_base < g_il2cpp_size) {
    fprintf(file, "%s=0x%llx guest+0x%llx\n", label,
            (unsigned long long)raw,
            (unsigned long long)(address - g_il2cpp_base));
  } else if (address >= host_base && address < host_end) {
    fprintf(file, "%s=0x%llx host+0x%llx\n", label,
            (unsigned long long)raw,
            (unsigned long long)(address - host_base));
  } else {
    fprintf(file, "%s=0x%llx absolute\n", label,
            (unsigned long long)raw);
  }
}

static int exception_read_frame(uint64_t frame_pointer,
                                uint64_t frame[2]) {
  const uintptr_t address = (uintptr_t)frame_pointer;
  if (!address || (address & 0xfu) ||
      address > UINTPTR_MAX - sizeof(uint64_t) * 2u)
    return 0;
  MemoryInfo info;
  u32 page_info = 0;
  if (R_FAILED(svcQueryMemory(&info, &page_info, address)) ||
      !(info.perm & Perm_R) || info.type == MemType_Unmapped ||
      address < (uintptr_t)info.addr ||
      sizeof(uint64_t) * 2u > (size_t)info.size ||
      address - (uintptr_t)info.addr >
        (size_t)info.size - sizeof(uint64_t) * 2u)
    return 0;
  memcpy(frame, (const void *)address, sizeof(uint64_t) * 2u);
  return 1;
}

void __libnx_exception_handler(ThreadExceptionDump *context) {
  if (!context) return;

  /* Append so an immediately preceding guest abort message is retained.  The
   * normal boot path removes fatal.txt before each new run. */
  FILE *file = fopen(GAME_HOME "/fatal.txt", "ab");
  if (!file) return;

  u64 total_memory = 0, used_memory = 0;
  u64 total_non_system = 0, used_non_system = 0;
  u64 system_resource_total = 0, system_resource_used = 0;
  const Result total_result = svcGetInfo(
    &total_memory, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  const Result used_result = svcGetInfo(
    &used_memory, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  const Result non_system_total_result = svcGetInfo(
    &total_non_system, InfoType_TotalNonSystemMemorySize,
    CUR_PROCESS_HANDLE, 0);
  const Result non_system_used_result = svcGetInfo(
    &used_non_system, InfoType_UsedNonSystemMemorySize,
    CUR_PROCESS_HANDLE, 0);
  const Result system_total_result = svcGetInfo(
    &system_resource_total, InfoType_SystemResourceSizeTotal,
    CUR_PROCESS_HANDLE, 0);
  const Result system_used_result = svcGetInfo(
    &system_resource_used, InfoType_SystemResourceSizeUsed,
    CUR_PROCESS_HANDLE, 0);

  fprintf(file, "\nlibnx user exception\n");
  fprintf(file,
          "error_desc=0x%x pstate=0x%x esr=0x%x afsr0=0x%x afsr1=0x%x aarch64=%u\n",
          context->error_desc, context->pstate, context->esr,
          context->afsr0, context->afsr1,
          threadExceptionIsAArch64(context) ? 1u : 0u);
  fprintf(file,
          "memory total=0x%llx used=0x%llx result=0x%x/0x%x non_system=0x%llx/0x%llx result=0x%x/0x%x system_resource=0x%llx/0x%llx result=0x%x/0x%x\n",
          (unsigned long long)total_memory,
          (unsigned long long)used_memory,
          (unsigned)total_result, (unsigned)used_result,
          (unsigned long long)total_non_system,
          (unsigned long long)used_non_system,
          (unsigned)non_system_total_result,
          (unsigned)non_system_used_result,
          (unsigned long long)system_resource_total,
          (unsigned long long)system_resource_used,
          (unsigned)system_total_result, (unsigned)system_used_result);
  fprintf(file, "guest_base=0x%llx guest_size=0x%llx host_base=0x%llx\n",
          (unsigned long long)g_il2cpp_base,
          (unsigned long long)g_il2cpp_size,
          (unsigned long long)(uintptr_t)&_start);
  exception_print_address(file, "pc", context->pc.x);
  exception_print_address(file, "lr", context->lr.x);
  exception_print_address(file, "sp", context->sp.x);
  exception_print_address(file, "fp", context->fp.x);
  exception_print_address(file, "far", context->far.x);
  for (unsigned index = 0; index < 29u; ++index)
    fprintf(file, "x%u=0x%llx\n", index,
            (unsigned long long)context->cpu_gprs[index].x);

  exception_print_address(file, "bt0", context->pc.x);
  exception_print_address(file, "bt1", context->lr.x);
  uint64_t frame_pointer = context->fp.x;
  for (unsigned index = 2; index < NX_EXCEPTION_MAX_FRAMES; ++index) {
    uint64_t frame[2];
    if (!exception_read_frame(frame_pointer, frame) ||
        !frame[1] || frame[0] <= frame_pointer)
      break;
    char label[16];
    snprintf(label, sizeof(label), "bt%u", index);
    exception_print_address(file, label, frame[1]);
    frame_pointer = frame[0];
  }
  fflush(file);
  fclose(file);
}
