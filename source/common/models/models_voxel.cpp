// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2010-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_voxels.cpp
**
** Voxel management
**
**/

#include "filesystem.h"
#include "colormatcher.h"
#include "bitmap.h"
#include "model_kvx.h"
#include "voxel_surface_emission.h"
#include "image.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "voxels.h"
#include "texturemanager.h"
#include "palettecontainer.h"
#include "textures.h"
#include "imagehelpers.h"
#include <algorithm>
#include <limits>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable:4244) // warning C4244: conversion from 'double' to 'float', possible loss of data
#endif

//===========================================================================
//
// Creates a 16x16 texture from the palette so that we can
// use the existing palette manipulation code to render the voxel
// Otherwise all shaders had to be duplicated and the non-shader code
// would be a lot less efficient.
//
//===========================================================================

class FVoxelTexture : public FImageSource
{
public:
	FVoxelTexture(FVoxel *voxel);

	int CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override;
	PalettedPixels CreatePalettedPixels(int conversion, int frame = 0) override;

protected:
	FVoxel *SourceVox;
};

//===========================================================================
//
// 
//
//===========================================================================

FVoxelTexture::FVoxelTexture(FVoxel *vox)
{
	SourceVox = vox;
	Width = 16;
	Height = 16;
	//bNoCompress = true;
}

//===========================================================================
//
// 
//
//===========================================================================

PalettedPixels FVoxelTexture::CreatePalettedPixels(int conversion, int frame)
{
	// GetPixels gets called when a translated palette is used so we still need to implement it here.
	PalettedPixels Pixels(256);
	uint8_t *pp = SourceVox->Palette.Data();

	if(pp != NULL)
	{
		for(int i=0;i<256;i++, pp+=3)
		{
			PalEntry pe;
			pe.r = (pp[0] << 2) | (pp[0] >> 4);
			pe.g = (pp[1] << 2) | (pp[1] >> 4);
			pe.b = (pp[2] << 2) | (pp[2] >> 4);
			// Alphatexture handling is just for completeness, but rather unlikely to be used ever.
			Pixels[i] = conversion == luminance ? pe.r : ColorMatcher.Pick(pe);

		}
	}
	else 
	{
		for(int i=0;i<256;i++, pp+=3)
		{
			Pixels[i] = (uint8_t)i;
		}
	}  
	ImageHelpers::FlipSquareBlock(Pixels.Data(), Width);
	return Pixels;
}

//===========================================================================
//
// FVoxelTexture::CopyPixels
//
// This creates a dummy 16x16 paletted bitmap and converts that using the
// voxel palette
//
//===========================================================================

int FVoxelTexture::CopyPixels(FBitmap *bmp, int conversion, int frame)
{
	PalEntry pe[256];
	uint8_t bitmap[256];
	uint8_t *pp = SourceVox->Palette.Data();

	if(pp != nullptr)
	{
		for(int i=0;i<256;i++, pp+=3)
		{
			bitmap[i] = (uint8_t)i;
			pe[i].r = (pp[0] << 2) | (pp[0] >> 4);
			pe[i].g = (pp[1] << 2) | (pp[1] >> 4);
			pe[i].b = (pp[2] << 2) | (pp[2] >> 4);
			pe[i].a = 255;
		}
	}
	else 
	{
		for(int i=0;i<256;i++, pp+=3)
		{
			bitmap[i] = (uint8_t)i;
			pe[i] = GPalette.BaseColors[i];
			pe[i].a = 255;
		}
	}    
	bmp->CopyPixelData(0, 0, bitmap, Width, Height, 1, 16, 0, pe);
	return 0;
}	

//===========================================================================
//
// 
//
//===========================================================================

FVoxelModel::FVoxelModel(FVoxel *voxel, bool owned)
{
	mVoxel = voxel;
	mOwningVoxel = owned;
	mPalette = TexMan.AddGameTexture(MakeGameTexture(new FImageTexture(new FVoxelTexture(voxel)), nullptr, ETextureType::Override));
}

//===========================================================================
//
// 
//
//===========================================================================

FVoxelModel::~FVoxelModel()
{
	if (mOwningVoxel) delete mVoxel;
}


//===========================================================================
//
// 
//
//===========================================================================

unsigned int FVoxelModel::AddVertex(FModelVertex &vert, FVoxelMap &check, TArray<FModelVertex>& vertices) const
{
	unsigned int index = check[vert];
	if (index == 0xffffffff)
	{
		index = check[vert] = vertices.Push(vert);
	}
	return index;
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::AddFace(int x1, int y1, int z1, int x2, int y2, int z2, int x3, int y3, int z3, int x4, int y4, int z4, uint8_t col, FVoxelMap &check, TArray<FModelVertex>& vertices, TArray<unsigned int>& indices) const
{
	float PivotX = mVoxel->Mips[0].Pivot.X;
	float PivotY = mVoxel->Mips[0].Pivot.Y;
	float PivotZ = mVoxel->Mips[0].Pivot.Z;
	FModelVertex vert;
	unsigned int indx[4];

	vert.packedNormal = 0;	// currently this is not being used for voxels.
	vert.u = (((col & 15) + 0.5f) / 16.f);
	vert.v = (((col / 16) + 0.5f) / 16.f);

	vert.x =  x1 - PivotX;
	vert.z = -y1 + PivotY;
	vert.y = -z1 + PivotZ;
	indx[0] = AddVertex(vert, check, vertices);

	vert.x =  x2 - PivotX;
	vert.z = -y2 + PivotY;
	vert.y = -z2 + PivotZ;
	indx[1] = AddVertex(vert, check, vertices);

	vert.x =  x4 - PivotX;
	vert.z = -y4 + PivotY;
	vert.y = -z4 + PivotZ;
	indx[2] = AddVertex(vert, check, vertices);

	vert.x =  x3 - PivotX;
	vert.z = -y3 + PivotY;
	vert.y = -z3 + PivotZ;
	indx[3] = AddVertex(vert, check, vertices);


	indices.Push(indx[0]);
	indices.Push(indx[1]);
	indices.Push(indx[3]);
	indices.Push(indx[1]);
	indices.Push(indx[2]);
	indices.Push(indx[3]);
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::MakeSlabPolys(int x, int y, kvxslab_t *voxptr, FVoxelMap &check, TArray<FModelVertex>& vertices, TArray<unsigned int>& indices) const
{
	const uint8_t *col = voxptr->col;
	int zleng = voxptr->zleng;
	int ztop = voxptr->ztop;
	int cull = voxptr->backfacecull;

	if (cull & 16)
	{
		AddFace(x, y, ztop, x+1, y, ztop, x, y+1, ztop, x+1, y+1, ztop, *col, check, vertices, indices);
	}
	int z = ztop;
	while (z < ztop+zleng)
	{
		int c = 0;
		while (z+c < ztop+zleng && col[c] == col[0]) c++;

		if (cull & 1)
		{
			AddFace(x, y, z, x, y+1, z, x, y, z+c, x, y+1, z+c, *col, check, vertices, indices);
		}
		if (cull & 2)
		{
			AddFace(x+1, y+1, z, x+1, y, z, x+1, y+1, z+c, x+1, y, z+c, *col, check, vertices, indices);
		}
		if (cull & 4)
		{
			AddFace(x+1, y, z, x, y, z, x+1, y, z+c, x, y, z+c, *col, check, vertices, indices);
		}
		if (cull & 8)
		{
			AddFace(x, y+1, z, x+1, y+1, z, x, y+1, z+c, x+1, y+1, z+c, *col, check, vertices, indices);
		}	
		z+=c;
		col+=c;
	}
	if (cull & 32)
	{
		int zz = ztop+zleng-1;
		AddFace(x+1, y, zz+1, x, y, zz+1, x+1, y+1, zz+1, x, y+1, zz+1, voxptr->col[zleng-1], check, vertices, indices);
	}
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::BuildMesh(TArray<FModelVertex>& vertices, TArray<unsigned int>& indices) const
{
	FVoxelMap check;
	FVoxelMipLevel *mip = &mVoxel->Mips[0];
	for (int x = 0; x < mip->SizeX; x++)
	{
		uint8_t *slabxoffs = &mip->GetSlabData(false)[mip->OffsetX[x]];
		short *xyoffs = &mip->OffsetXY[x * (mip->SizeY + 1)];
		for (int y = 0; y < mip->SizeY; y++)
		{
			kvxslab_t *voxptr = (kvxslab_t *)(slabxoffs + xyoffs[y]);
			kvxslab_t *voxend = (kvxslab_t *)(slabxoffs + xyoffs[y+1]);
			for (; voxptr < voxend; voxptr = (kvxslab_t *)((uint8_t *)voxptr + voxptr->zleng + 3))
			{
				MakeSlabPolys(x, y, voxptr, check, vertices, indices);
			}
		}
	}
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::Initialize()
{
	mVertices.Clear();
	mIndices.Clear();
	BuildMesh(mVertices, mIndices);
}

//===========================================================================
//
//
//
//===========================================================================

void FVoxelModel::BuildCpuMesh(FVoxelMeshData& outMesh) const
{
	outMesh.vertices.Clear();
	outMesh.indices.Clear();
	BuildMesh(outMesh.vertices, outMesh.indices);
}

//===========================================================================
//
//
//
//===========================================================================

void FVoxelModel::BuildRawMeshStats(
	FVoxelRawMeshStats& outStats,
	TArray<FVoxelRawSlabRecord>* outSlabs,
	TArray<FVoxelRawFaceRecord>* outFaces,
	TArray<FVoxelRawColorRunRecord>* outColorRuns) const
{
	outStats = {};
	if (outSlabs != nullptr)
	{
		outSlabs->Clear();
	}
	if (outFaces != nullptr)
	{
		outFaces->Clear();
	}
	if (outColorRuns != nullptr)
	{
		outColorRuns->Clear();
	}
	if (mVoxel == nullptr)
	{
		return;
	}

	const FVoxelMipLevel* mip = &mVoxel->Mips[0];
	if (mip->SizeX <= 0 || mip->SizeY <= 0 || mip->SizeZ <= 0 || mip->OffsetX == nullptr || mip->OffsetXY == nullptr)
	{
		return;
	}

	outStats.sizeX = (uint32_t)mip->SizeX;
	outStats.sizeY = (uint32_t)mip->SizeY;
	outStats.sizeZ = (uint32_t)mip->SizeZ;
	outStats.pivotX = mip->Pivot.X;
	outStats.pivotY = mip->Pivot.Y;
	outStats.pivotZ = mip->Pivot.Z;
	const uint8_t* slabData = mip->GetSlabData(false);
	if (slabData == nullptr)
	{
		return;
	}

	const size_t sizeX = (size_t)mip->SizeX;
	const size_t sizeY = (size_t)mip->SizeY;
	const size_t sizeZ = (size_t)mip->SizeZ;
	size_t occupancyCount = sizeX;
	if (occupancyCount > std::numeric_limits<size_t>::max() / sizeY)
	{
		return;
	}
	occupancyCount *= sizeY;
	if (occupancyCount > std::numeric_limits<size_t>::max() / sizeZ)
	{
		return;
	}
	occupancyCount *= sizeZ;
	std::vector<uint8_t> occupancy(occupancyCount, 0u);
	const auto occupancyIndex = [sizeY, sizeZ](int x, int y, int z)
	{
		return ((size_t)x * sizeY + (size_t)y) * sizeZ + (size_t)z;
	};
	for (int x = 0; x < mip->SizeX; ++x)
	{
		const uint8_t* slabxoffs = &slabData[mip->OffsetX[x]];
		const short* xyoffs = &mip->OffsetXY[x * (mip->SizeY + 1)];
		for (int y = 0; y < mip->SizeY; ++y)
		{
			kvxslab_t* voxptr = (kvxslab_t*)(slabxoffs + xyoffs[y]);
			kvxslab_t* voxend = (kvxslab_t*)(slabxoffs + xyoffs[y + 1]);
			for (; voxptr < voxend; voxptr = (kvxslab_t*)((uint8_t*)voxptr + voxptr->zleng + 3))
			{
				const int zEnd = std::min<int>(mip->SizeZ, (int)voxptr->ztop + (int)voxptr->zleng);
				for (int z = (int)voxptr->ztop; z < zEnd; ++z)
				{
					occupancy[occupancyIndex(x, y, z)] = 1u;
				}
			}
		}
	}
	const auto isOccupied = [&](int x, int y, int z)
	{
		return
			x >= 0 && x < mip->SizeX &&
			y >= 0 && y < mip->SizeY &&
			z >= 0 && z < mip->SizeZ &&
			occupancy[occupancyIndex(x, y, z)] != 0u;
	};
	const auto buildExposureMask = [&](int x, int y, int z)
	{
		uint32_t occupiedNeighborMask = 0u;
		if (isOccupied(x - 1, y, z)) occupiedNeighborMask |= 1u;
		if (isOccupied(x + 1, y, z)) occupiedNeighborMask |= 2u;
		if (isOccupied(x, y - 1, z)) occupiedNeighborMask |= 4u;
		if (isOccupied(x, y + 1, z)) occupiedNeighborMask |= 8u;
		if (isOccupied(x, y, z - 1)) occupiedNeighborMask |= 16u;
		if (isOccupied(x, y, z + 1)) occupiedNeighborMask |= 32u;
		return VoxelExposureMaskFromOccupiedNeighbors(occupiedNeighborMask);
	};

	const auto pushFace = [&](int x1, int y1, int z1, int x2, int y2, int z2, int x3, int y3, int z3, int x4, int y4, int z4, uint8_t color)
	{
		if (outFaces == nullptr)
		{
			return;
		}

		FVoxelRawFaceRecord face = {};
		face.x[0] = x1;
		face.y[0] = y1;
		face.z[0] = z1;
		face.x[1] = x2;
		face.y[1] = y2;
		face.z[1] = z2;
		face.x[2] = x3;
		face.y[2] = y3;
		face.z[2] = z3;
		face.x[3] = x4;
		face.y[3] = y4;
		face.z[3] = z4;
		face.color = color;
		outFaces->Push(face);
	};

	for (int x = 0; x < mip->SizeX; ++x)
	{
		const uint8_t* slabxoffs = &slabData[mip->OffsetX[x]];
		const short* xyoffs = &mip->OffsetXY[x * (mip->SizeY + 1)];
		outStats.rawByteCount += (uint64_t)std::max<int>(0, xyoffs[mip->SizeY]);
		for (int y = 0; y < mip->SizeY; ++y)
		{
			kvxslab_t* voxptr = (kvxslab_t*)(slabxoffs + xyoffs[y]);
			kvxslab_t* voxend = (kvxslab_t*)(slabxoffs + xyoffs[y + 1]);
			for (; voxptr < voxend; voxptr = (kvxslab_t*)((uint8_t*)voxptr + voxptr->zleng + 3))
			{
				const uint32_t zleng = (uint32_t)voxptr->zleng;
				const uint32_t cull = (uint32_t)voxptr->backfacecull;
				const uint32_t ztop = (uint32_t)voxptr->ztop;
				if (zleng == 0)
				{
					continue;
				}
				const uint32_t capExposureMask =
					(buildExposureMask(x, y, (int)ztop) & 16u) |
					(buildExposureMask(x, y, (int)(ztop + zleng - 1u)) & 32u);

				if ((cull & 16u) != 0)
				{
					pushFace(x, y, (int)ztop, x + 1, y, (int)ztop, x, y + 1, (int)ztop, x + 1, y + 1, (int)ztop, voxptr->col[0]);
				}

				uint32_t colorRuns = 0;
				uint32_t adjacencySideFaceSpans = 0;
				const uint32_t sideCullBits[] = { 1u, 2u, 4u, 8u };
				const uint8_t* col = voxptr->col;
				uint32_t z = 0;
				const uint32_t colorRunOffset = outColorRuns != nullptr ? (uint32_t)outColorRuns->Size() : 0u;
				while (z < zleng)
				{
					const uint32_t lateralExposureMask = buildExposureMask(x, y, (int)(ztop + z)) & 15u;
					const uint32_t run = VoxelSurfaceRunLength(
						voxptr->col, z, zleng, lateralExposureMask,
						[&](uint32_t voxelOffset)
						{
							return buildExposureMask(x, y, (int)(ztop + voxelOffset)) & 15u;
						});
					++colorRuns;
					for (const uint32_t faceCullBit : sideCullBits)
					{
						if ((cull & faceCullBit) != 0u)
						{
							adjacencySideFaceSpans += CountVoxelAdjacencyNormalSpans(
								lateralExposureMask, capExposureMask, z, run, zleng, faceCullBit);
						}
					}
					if (outColorRuns != nullptr)
					{
						FVoxelRawColorRunRecord runRecord = {};
						runRecord.zOffset = z;
						runRecord.zLength = run;
						runRecord.color = col[0];
						runRecord.lateralExposureMask = lateralExposureMask;
						outColorRuns->Push(runRecord);
					}
					const int z0 = (int)(ztop + z);
					const int z1 = (int)(ztop + z + run);
					if ((cull & 1u) != 0)
					{
						pushFace(x, y, z0, x, y + 1, z0, x, y, z1, x, y + 1, z1, col[0]);
					}
					if ((cull & 2u) != 0)
					{
						pushFace(x + 1, y + 1, z0, x + 1, y, z0, x + 1, y + 1, z1, x + 1, y, z1, col[0]);
					}
					if ((cull & 4u) != 0)
					{
						pushFace(x + 1, y, z0, x, y, z0, x + 1, y, z1, x, y, z1, col[0]);
					}
					if ((cull & 8u) != 0)
					{
						pushFace(x, y + 1, z0, x + 1, y + 1, z0, x, y + 1, z1, x + 1, y + 1, z1, col[0]);
					}
					z += run;
					col += run;
				}
				if ((cull & 32u) != 0)
				{
					const int zz = (int)(ztop + zleng - 1u);
					pushFace(x + 1, y, zz + 1, x, y, zz + 1, x + 1, y + 1, zz + 1, x, y + 1, zz + 1, voxptr->col[zleng - 1]);
				}

				const uint32_t topFaces = (cull & 16u) != 0 ? 1u : 0u;
				const uint32_t bottomFaces = (cull & 32u) != 0 ? 1u : 0u;
				const uint32_t sideDirections =
					((cull & 1u) != 0 ? 1u : 0u) +
					((cull & 2u) != 0 ? 1u : 0u) +
					((cull & 4u) != 0 ? 1u : 0u) +
					((cull & 8u) != 0 ? 1u : 0u);
				const uint32_t sideFaceSpans = sideDirections * colorRuns;
				const uint32_t faceCount = topFaces + bottomFaces + sideFaceSpans;

				outStats.slabCount++;
				outStats.voxelCount += zleng;
				outStats.topFaceCount += topFaces;
				outStats.bottomFaceCount += bottomFaces;
				outStats.sideFaceSpanCount += sideFaceSpans;
				outStats.coalescedFaceCount += faceCount;
				outStats.unitSurfaceFaceCount += CountVoxelUnitSurfaceFaces(cull, zleng);
				outStats.adjacencySurfaceFaceCount += topFaces + bottomFaces + adjacencySideFaceSpans;

				if (outSlabs != nullptr)
				{
					FVoxelRawSlabRecord record = {};
					record.x = (uint32_t)x;
					record.y = (uint32_t)y;
					record.zTop = ztop;
					record.cullMask = cull;
					record.zLength = zleng;
					record.colorRunCount = colorRuns;
					record.colorRunOffset = colorRunOffset;
					record.capExposureMask = capExposureMask;
					outSlabs->Push(record);
				}
			}
		}
	}

	outStats.noDedupeVertexCount = outStats.coalescedFaceCount * 4u;
	outStats.indexCount = outStats.coalescedFaceCount * 6u;
	outStats.unitSurfaceVertexCount = outStats.unitSurfaceFaceCount * 4u;
	outStats.unitSurfaceIndexCount = outStats.unitSurfaceFaceCount * 6u;
	outStats.adjacencySurfaceVertexCount = outStats.adjacencySurfaceFaceCount * 4u;
	outStats.adjacencySurfaceIndexCount = outStats.adjacencySurfaceFaceCount * 6u;
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::BuildVertexBuffer(FModelRenderer *renderer)
{
	if (!GetVertexBuffer(renderer->GetType()))
	{
		Initialize();

		auto vbuf = renderer->CreateVertexBuffer(true, true);
		SetVertexBuffer(renderer->GetType(), vbuf);

		FModelVertex *vertptr = vbuf->LockVertexBuffer(mVertices.Size());
		unsigned int *indxptr = vbuf->LockIndexBuffer(mIndices.Size());

		memcpy(vertptr, &mVertices[0], sizeof(FModelVertex)* mVertices.Size());
		memcpy(indxptr, &mIndices[0], sizeof(unsigned int)* mIndices.Size());

		vbuf->UnlockVertexBuffer();
		vbuf->UnlockIndexBuffer();
		mNumIndices = mIndices.Size();

		// delete our temporary buffers
		mVertices.Clear();
		mIndices.Clear();
		mVertices.ShrinkToFit();
		mIndices.ShrinkToFit();
	}
}


//===========================================================================
//
// for skin precaching
//
//===========================================================================

void FVoxelModel::AddSkins(uint8_t *hitlist, const FTextureID*)
{
	hitlist[mPalette.GetIndex()] |= FTextureManager::HIT_Flat;
}

//===========================================================================
//
// 
//
//===========================================================================

bool FVoxelModel::Load(const char * fn, int lumpnum, const char * buffer, int length)
{
	return false;	// not needed
}

//===========================================================================
//
// Voxels don't have frames so always return 0
//
//===========================================================================

int FVoxelModel::FindFrame(const char* name, bool nodefault)
{
	return nodefault ? FErr_Voxel : 0; // -2, not -1 because voxels are special.
}

//===========================================================================
//
// Voxels need aspect ratio correction according to the current map's setting
//
//===========================================================================

float FVoxelModel::getAspectFactor(float stretch)
{
	return stretch;
}

//===========================================================================
//
// Voxels never interpolate between frames, they only have one.
//
//===========================================================================

void FVoxelModel::RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frame, int frame2, double inter, FTranslationID translation, const FTextureID*, const TArray<VSMatrix>& boneData, int boneStartPosition)
{
	renderer->SetMaterial(skin, true, translation);
	renderer->SetupFrame(this, 0, 0, 0, {}, -1);
	renderer->DrawElements(mNumIndices, 0);
}
