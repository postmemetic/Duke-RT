#include "texturemanager.h"
#include "nri_scene_texture_utils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#endif

namespace nri_scene
{
bool IsUsableGameTexturePointer(FGameTexture* texture)
{
#ifndef _WIN32
	return TexMan.OwnsTexture(texture);
#else
	const uintptr_t value = (uintptr_t)texture;
	if (value <= 0x10000 ||
		value == (uintptr_t)-1 ||
		(value & (sizeof(void*) - 1)) != 0)
	{
		return false;
	}

	MEMORY_BASIC_INFORMATION pointerInfo = {};
	if (VirtualQuery(texture, &pointerInfo, sizeof(pointerInfo)) != sizeof(pointerInfo) ||
		pointerInfo.State != MEM_COMMIT ||
		(pointerInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
	{
		return false;
	}

	void* vtable = nullptr;
#ifdef _WIN32
	__try
#endif
	{
		vtable = *(void**)texture;
	}
#ifdef _WIN32
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		vtable = nullptr;
	}
#endif

	if (vtable == nullptr)
	{
		return false;
	}

	MEMORY_BASIC_INFORMATION vtableInfo = {};
	return VirtualQuery(vtable, &vtableInfo, sizeof(vtableInfo)) == sizeof(vtableInfo) &&
		vtableInfo.State == MEM_COMMIT &&
		(vtableInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
#endif
}
}
