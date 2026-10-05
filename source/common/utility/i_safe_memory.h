#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif

// The caller owns the destination. Failure may leave a partial copy there;
// never consume it unless this returns true. This does not validate C++ objects.
inline bool I_TryReadMemory(void* destination, const void* source, size_t size)
{
	if (!destination || !source || size == 0 ||
		size > (std::numeric_limits<intptr_t>::max)() ||
		(uintptr_t)source > (std::numeric_limits<uintptr_t>::max)() - size)
		return false;
#ifdef _WIN32
	SIZE_T bytesRead = 0;
	return ReadProcessMemory(GetCurrentProcess(), source, destination, size, &bytesRead) && bytesRead == size;
#elif defined(__linux__)
	iovec local = { destination, size };
	iovec remote = { const_cast<void*>(source), size };
	return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
#else
	return false;
#endif
}
