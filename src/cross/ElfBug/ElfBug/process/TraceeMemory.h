#pragma once

#include <sys/types.h>
#include <ElfBug/types/ElfBug.h>

namespace ElfBug
{
    // Bytes read from another process by pid, or -1; a read process_vm_readv refuses at its first page goes through /proc/<pid>/mem. Needs no Process or engine lock.
    ssize_t ReadTraceeMemory(pid_t pid, ptr address, void* buffer, ptr size);
}
