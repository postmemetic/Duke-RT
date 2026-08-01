#pragma once

#include "model.h"
#include "i_modelvertexbuffer.h"
#include "tarray.h"
#include "xs_Float.h"

struct FVoxel;
struct kvxslab_t;
class FModelRenderer;
class FGameTexture;

struct FVoxelVertexHash
{
	// Returns the hash value for a key.
	hash_t Hash(const FModelVertex &key) 
	{ 
		int ix = int(key.x);
		int iy = int(key.y);
		int iz = int(key.z);
		return (hash_t)(ix + (iy<<9) + (iz<<18));
	}

	// Compares two keys, returning zero if they are the same.
	int Compare(const FModelVertex &left, const FModelVertex &right) 
	{ 
		return left.x != right.x || left.y != right.y || left.z != right.z || left.u != right.u || left.v != right.v;
	}
};

struct FIndexInit
{
	void Init(unsigned int &value)
	{
		value = 0xffffffff;
	}
};

typedef TMap<FModelVertex, unsigned int, FVoxelVertexHash, FIndexInit> FVoxelMap;

struct FVoxelMeshData
{
	TArray<FModelVertex> vertices;
	TArray<unsigned int> indices;
};

struct FVoxelRawSlabRecord
{
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t zTop = 0;
	uint32_t cullMask = 0;
	uint32_t zLength = 0;
	uint32_t colorRunCount = 0;
	uint32_t colorRunOffset = 0;
	uint32_t capExposureMask = 0;
};

struct FVoxelRawColorRunRecord
{
	uint32_t zOffset = 0;
	uint32_t zLength = 0;
	uint32_t color = 0;
	uint32_t lateralExposureMask = 0;
};

struct FVoxelRawFaceRecord
{
	int32_t x[4] = {};
	int32_t y[4] = {};
	int32_t z[4] = {};
	uint32_t color = 0;
	uint32_t materialIndex = 0;
};

struct FVoxelRawMeshStats
{
	uint32_t sizeX = 0;
	uint32_t sizeY = 0;
	uint32_t sizeZ = 0;
	uint32_t slabCount = 0;
	uint32_t voxelCount = 0;
	uint32_t topFaceCount = 0;
	uint32_t bottomFaceCount = 0;
	uint32_t sideFaceSpanCount = 0;
	uint32_t coalescedFaceCount = 0;
	uint32_t noDedupeVertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t unitSurfaceFaceCount = 0;
	uint32_t unitSurfaceVertexCount = 0;
	uint32_t unitSurfaceIndexCount = 0;
	uint32_t adjacencySurfaceFaceCount = 0;
	uint32_t adjacencySurfaceVertexCount = 0;
	uint32_t adjacencySurfaceIndexCount = 0;
	uint64_t rawByteCount = 0;
	uint64_t contentHash = 0;
	float pivotX = 0.0f;
	float pivotY = 0.0f;
	float pivotZ = 0.0f;
};

class FVoxelModel : public FModel
{
protected:
	FVoxel *mVoxel;
	bool mOwningVoxel;	// if created through MODELDEF deleting this object must also delete the voxel object
	FTextureID mPalette;
	unsigned int mNumIndices;
	TArray<FModelVertex> mVertices;
	TArray<unsigned int> mIndices;

	void BuildMesh(TArray<FModelVertex>& vertices, TArray<unsigned int>& indices) const;
	void MakeSlabPolys(int x, int y, kvxslab_t *voxptr, FVoxelMap &check, TArray<FModelVertex>& vertices, TArray<unsigned int>& indices) const;
	void AddFace(int x1, int y1, int z1, int x2, int y2, int z2, int x3, int y3, int z3, int x4, int y4, int z4, uint8_t color, FVoxelMap &check, TArray<FModelVertex>& vertices, TArray<unsigned int>& indices) const;
	unsigned int AddVertex(FModelVertex &vert, FVoxelMap &check, TArray<FModelVertex>& vertices) const;

public:
	FVoxelModel(FVoxel *voxel, bool owned);
	~FVoxelModel();
	bool Load(const char * fn, int lumpnum, const char * buffer, int length) override;
	void Initialize();
	void BuildCpuMesh(FVoxelMeshData& outMesh) const;
	void BuildRawMeshStats(
		FVoxelRawMeshStats& outStats,
		TArray<FVoxelRawSlabRecord>* outSlabs = nullptr,
		TArray<FVoxelRawFaceRecord>* outFaces = nullptr,
		TArray<FVoxelRawColorRunRecord>* outColorRuns = nullptr) const;
	virtual int FindFrame(const char* name, bool nodefault) override;
	virtual void RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frame, int frame2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, const TArray<VSMatrix>& boneData, int boneStartPosition) override;
	virtual void AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids) override;
	FTextureID GetPaletteTexture() const { return mPalette; }
	void BuildVertexBuffer(FModelRenderer *renderer) override;
	float getAspectFactor(float vscale) override;
};
