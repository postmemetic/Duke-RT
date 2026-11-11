#pragma once

#include <cstdint>

class FGameTexture;
namespace nri_scene
{
// The callback retains the original guarded resolver when consolidate is false.
using MaterialAverageColorResolver = bool (*)(FGameTexture*, float*, bool consolidate, bool& eligible);

// Only a synchronous BuildMaterials owns this memo. It never survives a build,
// and never admits canvas, skybox, unresolved or failed results. Engine/game
// mutation does not run inside that build. Nested builds invalidate the parent.
class ScopedMaterialAverageColorCache
{
public:
	ScopedMaterialAverageColorCache();
	~ScopedMaterialAverageColorCache();
	ScopedMaterialAverageColorCache(const ScopedMaterialAverageColorCache&) = delete;
	ScopedMaterialAverageColorCache& operator=(const ScopedMaterialAverageColorCache&) = delete;
private:
	friend bool ResolveMaterialAverageColor(FGameTexture*, float*, MaterialAverageColorResolver);
	struct Entry { FGameTexture* texture; float color[3]; };
	Entry mEntries[64];
	ScopedMaterialAverageColorCache* mPrevious;
	uint64_t mEpoch = 0;
	uint64_t mRequests = 0, mHits = 0, mMisses = 0, mBypasses = 0;
	uint64_t mChecks = 0, mHitChecks = 0, mMismatches = 0, mNestedInvalidations = 0;
	uint32_t mDepth = 0;
	bool mEnabled, mValidate, mAudit, mQuarantined = false;
	void Invalidate();
	bool Invoke(FGameTexture*, float*, bool, bool&, MaterialAverageColorResolver);
};

bool ResolveMaterialAverageColor(FGameTexture* texture, float* outColor, MaterialAverageColorResolver resolver);
}
