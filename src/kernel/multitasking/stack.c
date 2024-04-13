#include <elf.h>
#include <paging.h>
#include <stack.h>
#include <string.h>
#include <util.h>

// Stack creation for userland & kernelspace tasks
// Copyright (C) 2024 Panagiotis

void stackGenerateMutual(Task *task) {
  if (GetPageDirectory() != task->pagedir) {
    debugf("[stack] Do NOT run stackGenerateMutual() without changing to the "
           "correct page directory first!\n");
    panic();
  }

  // Map the user stack (for variables & such)
  for (int i = 0; i < USER_STACK_PAGES; i++) {
    VirtualMap(USER_STACK_BOTTOM - USER_STACK_PAGES * 0x1000 + i * 0x1000,
               BitmapAllocatePageframe(&physical), PF_SYSTEM | PF_USER | PF_RW);
  }
}

void stackGenerateUser(Task *target, uint32_t argc, char **argv, uint8_t *out,
                       size_t filesize, void *elf_ehdr_ptr) {
  Elf64_Ehdr *elf_ehdr = (Elf64_Ehdr *)(elf_ehdr_ptr);

  // yeah, we will need to construct a stackframe...
  void *oldPagedir = GetPageDirectory();
  ChangePageDirectory(target->pagedir);

  stackGenerateMutual(target);

#define PUSH_TO_STACK(a, b, c)                                                 \
  a -= sizeof(b);                                                              \
  *((b *)(a)) = c

  int *randomByteStart = target->heap_end;
  taskAdjustHeap(target, target->heap_end + 16);
  for (int i = 0; i < 4; i++)
    randomByteStart[i] = rand();

  // aux: AT_NULL
  PUSH_TO_STACK(target->registers.usermode_rsp, size_t, (size_t)0);
  PUSH_TO_STACK(target->registers.usermode_rsp, size_t, (size_t)0);
  // aux: AT_RANDOM
  PUSH_TO_STACK(target->registers.usermode_rsp, size_t,
                (size_t)randomByteStart);
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t, 25);
  // aux: AT_PAGESZ
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t, 4096);
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t, 6);
  // aux: AT_PHNUM
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t, elf_ehdr->e_phnum);
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t, 5);
  // aux: AT_PHENT
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t,
                elf_ehdr->e_shentsize);
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t, 4);
  // aux: AT_PHDR
  void *phstuffStart = target->heap_end;
  taskAdjustHeap(target, target->heap_end + filesize);
  memcpy(phstuffStart, out, filesize);
  PUSH_TO_STACK(target->registers.usermode_rsp, size_t,
                (size_t)phstuffStart + elf_ehdr->e_phoff);
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t, 3);

  // Store argument contents
  uint32_t argSpace = 0;
  for (int i = 0; i < argc; i++)
    argSpace += strlength(argv[i]) + 1; // null terminator
  uint8_t *argStart = target->heap_end;
  taskAdjustHeap(target, target->heap_end + argSpace);
  size_t ellapsed = 0;
  for (int i = 0; i < argc; i++) {
    uint32_t len = strlength(argv[i]) + 1; // null terminator
    memcpy((size_t)argStart + ellapsed, argv[i], len);
    ellapsed += len;
  }

  // todo: Proper environ
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t, 0);

  // end of argv
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t, 0);

  // Store argument pointers (directly in stack)
  size_t finalEllapsed = 0;
  // ellapsed already has the full size lol
  for (int i = argc - 1; i >= 0; i--) {
    target->registers.usermode_rsp -= sizeof(uint64_t);
    uint64_t *finalArgv = target->registers.usermode_rsp;

    uint32_t len = strlength(argv[i]) + 1; // null terminator
    finalEllapsed += len;
    *finalArgv = (size_t)argStart + (ellapsed - finalEllapsed);
  }

  // argc
  PUSH_TO_STACK(target->registers.usermode_rsp, uint64_t, argc);

  /*for (uint64_t i = target->registers.usermode_rsp; i < USER_STACK_BOTTOM;
       i += sizeof(uint64_t))
    debugf("[%lx] %lx\n", i, *((uint64_t *)i));*/

  ChangePageDirectory(oldPagedir);
}

void stackGenerateKernel(Task *target, uint64_t parameter) {
  void *oldPagedir = GetPageDirectory();
  ChangePageDirectory(target->pagedir);

  stackGenerateMutual(target);
  target->registers.rdi = parameter;

  ChangePageDirectory(oldPagedir);
}