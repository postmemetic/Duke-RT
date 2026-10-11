#pragma once

#include "lightoverlay.h"
#include <cstdint>

inline uint64_t HashLightOverlayText(uint64_t hash, const char* text)
{
	if (text == nullptr)
	{
		return hash;
	}

	for (const unsigned char* cursor = (const unsigned char*)text; *cursor != '\0'; ++cursor)
	{
		hash ^= (uint64_t)(*cursor);
		hash *= 1099511628211ull;
	}
	return hash;
}

inline uint32_t BuildResolvedLightOverlayRuleId(const char* id, const char* classOrMapName, const LightOverlaySourceLocation& source)
{
	uint64_t hash = 1469598103934665603ull;
	hash = HashLightOverlayText(hash, id);
	hash = HashLightOverlayText(hash, classOrMapName);
	hash = HashLightOverlayText(hash, source.sourceName.GetChars());
	hash ^= (uint64_t)source.orderIndex + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
	const uint32_t ruleId = (uint32_t)(hash ^ (hash >> 32));
	return ruleId != 0 ? ruleId : 1u;
}
