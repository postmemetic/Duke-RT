#pragma once

#include "tarray.h"
#include "basics.h"
#include "build.h"

struct HWDrawInfo;
class Clipper;

struct BunchDrawerCensusStats
{
	uint32_t sectionVisits = 0;
	uint32_t wallTests = 0;
	uint32_t bunchesStarted = 0;
	uint32_t bunchesProcessed = 0;
	uint32_t scratchArrayGrowths = 0;
	bool encounteredDraggedSector = false;
};

struct FBunch
{
	int sectornum;
	int startline;
	int endline;
	bool portal;
	angle_t startangle;
	angle_t endangle;
};

class BunchDrawer
{
	HWDrawInfo *di;
	Clipper *clipper;
	int LastBunch;
	int StartTime;
	TArray<FBunch> Bunches;
	TArray<int> CompareData;
	double viewx, viewy;
	float gcosang, gsinang;
	BitArray gotsector;
	BitArray gotsection;
	BitArray gotsection2;
	BitArray gotwall;
	BitArray blockwall;
	angle_t ang1, ang2, angrange;
	angle_t viewangle;
	float viewz;
	bool censusOnly = false;
	TArray<angle_t> censusClipAngles;
	BunchDrawerCensusStats censusStats;

	TArray<int> sectionstartang, sectionendang;

private:

	enum
	{
		CL_Skip = 0,
		CL_Draw = 1,
		CL_Pass = 2,
	};

	angle_t ClipAngle(int wal) { return (censusOnly ? censusClipAngles[wal] : wall[wal].clipangle) - ang1; }
	void StartScene();
	bool StartBunch(int sectnum, int linenum, angle_t startan, angle_t endan, bool portal);
	bool AddLineToBunch(int line, angle_t newan);
	void DeleteBunch(int index);
	bool CheckClip(walltype* wal, float* topclip, float* bottomclip);
	int ClipLine(int line, bool portal);
	void ProcessBunch(int bnch);
	int WallInFront(int wall1, int wall2);
	int ColinearBunchInFront(FBunch* b1, FBunch* b2);
	int BunchInFront(FBunch* b1, FBunch* b2);
	int FindClosestBunch();
	void ProcessSection(int sectnum, bool portal);

public:
	void Init(HWDrawInfo* _di, Clipper* c, const DVector2& view, angle_t a1, angle_t a2);
	void InitCensus(Clipper* c, const DVector3& view, angle_t canonicalSplitAngle);
	void RenderScene(const int* viewsectors, unsigned sectcount, bool portal);
	const BitArray& GotSector() const { return gotsector; }
	const BitArray& GotSection() const { return gotsection; }
	const BitArray& GotWall() const { return gotwall; }
	const BunchDrawerCensusStats& CensusStats() const { return censusStats; }
};
