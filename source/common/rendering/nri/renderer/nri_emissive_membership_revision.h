#pragma once

#include <cstdint>

// Published only by the static CPU geometry owner after an ordered material
// membership write. Process-wide identities prevent reset/reconstruction ABA;
// exhaustion is sticky and withholds certification rather than wrapping.
class NRIEmissiveMembershipRevision
{
public:
	void PublishChange()
	{
		mRevision = sLastRevision == UINT64_MAX ? 0 : ++sLastRevision;
	}
	uint64_t Value() const { return mRevision; }

private:
	inline static uint64_t sLastRevision = 0;
	uint64_t mRevision = 0;
};
