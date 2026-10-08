#include <ElfBug/process/Process.h>
#include <ElfBug/process/ProcFs.h>
#include <fcntl.h>
#include <unistd.h>

namespace ElfBug
{
    Process::Process(const pid_t pid)
        : pid(pid)
    {
    }

    Process::~Process()
    {
        if(mMemFd != -1)
            close(mMemFd);
    }

    Thread* Process::FindThread(const pid_t tid) const
    {
        const auto it = threads.find(tid);
        return it != threads.end() ? it->second.get() : nullptr;
    }

    ssize_t Process::memPwrite(const void* buffer, const size_t size, const off_t offset) const
    {
        std::lock_guard lock(mMemFdMutex);
        if(mMemFd == -1)
            mMemFd = open(procfs::Path(pid, "mem").c_str(), O_RDWR | O_CLOEXEC);
        return mMemFd == -1 ? -1 : pwrite(mMemFd, buffer, size, offset);
    }

    void Process::ResetMemFd() const
    {
        std::lock_guard lock(mMemFdMutex);
        if(mMemFd != -1)
        {
            close(mMemFd);
            mMemFd = -1;
        }
    }
}
