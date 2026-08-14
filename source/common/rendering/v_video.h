/*
** v_video.h
**
**---------------------------------------------------------------------------
** Copyright 1998-2008 Randy Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#ifndef __V_VIDEO_H__
#define __V_VIDEO_H__

#include <functional>
#include "basics.h"
#include "vectors.h"
#include "m_png.h"
#include "renderstyle.h"
#include "c_cvars.h"
#include "v_2ddrawer.h"
#include "intrect.h"
#include "hw_shadowmap.h"
#include "hw_levelmesh.h"
#include "buffers.h"
#include "files.h"
#include "zstring.h"


struct FPortalSceneState;
class FSkyVertexBuffer;
class IIndexBuffer;
class IVertexBuffer;
class IDataBuffer;
class FFlatVertexBuffer;
class HWViewpointBuffer;
class FLightBuffer;
struct HWDrawInfo;
class FMaterial;
class FGameTexture;
class FRenderState;
class BoneBuffer;
struct MapRecord;

struct PathTracingEmissiveLightEditTarget
{
	bool valid = false;
	bool hit = false;
	bool emissive = false;
	int sectorIndex = -1;
	int wallIndex = -1;
	int textureId = -1;
	int baseTextureId = -1;
	int materialIndex = -1;
	bool surfaceLightOverlay = false;
	uint32_t surfaceLightRuleId = 0;
	float position[3] = { 0.0f, 0.0f, 0.0f };
	float normal[3] = { 0.0f, 1.0f, 0.0f };
	FString textureName;
	FString materialTextureName;
	float sectorResponseIntensity = 1.0f;
	float sectorResponseMin = 0.25f;
	float sectorResponseMax = 3.0f;
	FString failureReason;
};

enum EHWCaps
{
	// [BB] Added texture compression flags.
	RFL_TEXTURE_COMPRESSION = 1,
	RFL_TEXTURE_COMPRESSION_S3TC = 2,

	RFL_SHADER_STORAGE_BUFFER = 4,
	RFL_BUFFER_STORAGE = 8,

	RFL_NO_CLIP_PLANES = 32,

	RFL_INVALIDATE_BUFFER = 64,
	RFL_DEBUG = 128,
};


extern int DisplayWidth, DisplayHeight;

void V_UpdateModeSize (int width, int height);
void V_OutputResized (int width, int height);
int V_GetBackend();
const char* V_GetStartupSetOverride(const char* name);
const char* V_GetStartupNriAPI();

EXTERN_CVAR(Bool, vid_fullscreen)
EXTERN_CVAR(Int, win_x)
EXTERN_CVAR(Int, win_y)
EXTERN_CVAR(Int, win_w)
EXTERN_CVAR(Int, win_h)
EXTERN_CVAR(Bool, win_maximized)

struct FColormap;
enum FTextureFormat : uint32_t;
class FModelRenderer;
struct SamplerUniform;

//
// VIDEO
//
//
class DCanvas
{
public:
	DCanvas (int width, int height, bool bgra);
	~DCanvas ();
	void Resize(int width, int height, bool optimizepitch = true);

	// Member variable access
	inline uint8_t *GetPixels () const { return Pixels.Data(); }
	inline int GetWidth () const { return Width; }
	inline int GetHeight () const { return Height; }
	inline int GetPitch () const { return Pitch; }
	inline bool IsBgra() const { return Bgra; }

protected:
	TArray<uint8_t> Pixels;
	int Width;
	int Height;
	int Pitch;
	bool Bgra;
};

class IHardwareTexture;
class FTexture;
class FGameTexture;

enum class PathTracingSmokeSourceKind : uint32_t
{
	Unspecified = 0,
	ProjectileTrail = 1,
	SurfaceImpact = 2,
	ActorAmbient = 3
};

struct PathTracingWeaponLightEvent
{
	FString eventId;
	int32_t emitterActorIndex = -1;
	bool hasEmitterActorIndex = false;
	int32_t ownerActorIndex = -1;
	bool hasOwnerActorIndex = false;
	DVector3 worldPosition;
	DVector3 basisRight;
	DVector3 basisForward;
	DVector3 basisUp;
	bool hasBasis = false;
	DVector3 incomingDirection;
	bool hasIncomingDirection = false;
	DVector3 surfaceNormal;
	bool hasSurfaceNormal = false;
	PathTracingSmokeSourceKind smokeSourceKind = PathTracingSmokeSourceKind::Unspecified;
	double absoluteTimeSeconds = 0.0;
	uint64_t serial = 0;
};

// Gameplay can publish smoke provenance without depending on a renderer backend.
// The base framebuffer adapts this event onto the existing shared PT effect queue.
struct PathTracingSmokeSourceEvent
{
	FString eventId;
	PathTracingSmokeSourceKind sourceKind = PathTracingSmokeSourceKind::Unspecified;
	int32_t sourceActorIndex = -1;
	bool hasSourceActorIndex = false;
	int32_t ownerActorIndex = -1;
	bool hasOwnerActorIndex = false;
	DVector3 worldPosition;
	DVector3 incomingDirection;
	bool hasIncomingDirection = false;
	DVector3 surfaceNormal;
	bool hasSurfaceNormal = false;
	double absoluteTimeSeconds = 0.0;
};

enum class PathTracingActorSpriteTraceStage : uint32_t
{
	Draw = 0,
	CaptureScene = 1,
	CaptureActorScene = 2
};

struct PathTracingActorSpriteTraceEvent
{
	PathTracingActorSpriteTraceStage stage = PathTracingActorSpriteTraceStage::Draw;
	int32_t actorIndex = -1;
	int32_t spriteStatnum = -1;
	int32_t spritePicnum = -1;
	int32_t baseTextureId = -1;
	int32_t resolvedTextureId = -1;
	int32_t palette = 0;
	int32_t shade = 0;
	uint32_t cstat = 0;
	uint32_t cstat2 = 0;
	uint32_t drawListType = 0;
	bool noAnimate = false;
	bool fullbright = false;
	const FGameTexture* resolvedGameTexture = nullptr;
	bool hasVoxelKeys = false;
	uint64_t voxelMeshKeyHash = 0;
	uint64_t voxelMaterialKeyHash = 0;
	uint64_t voxelInstanceKeyHash = 0;
	uint64_t voxelSurfaceSignature = 0;
	const char* voxelAction = nullptr;
};

enum class LevelTransitionReason : uint8_t
{
	Unknown = 0,
	NewGame,
	NextLevel,
	SaveGameLoad,
	Startup,
	MainMenu,
	Credits
};

struct LevelTransitionInfo
{
	LevelTransitionReason reason = LevelTransitionReason::Unknown;
	uint64_t serial = 0;
	MapRecord* oldLevel = nullptr;
	MapRecord* newLevel = nullptr;
	FString oldLevelName;
	FString newLevelName;
};

class DFrameBuffer
{
private:
	int Width = 0;
	int Height = 0;

	struct Queued2DTextureRender
	{
		FGameTexture* texture = nullptr;
		F2DDrawer* drawer = nullptr;
	};

	TArray<Queued2DTextureRender> mQueued2DTextureRenders;

public:
	// Hardware render state that needs to be exposed to the API independent part of the renderer. For ease of access this is stored in the base class.
	int hwcaps = 0;								// Capability flags
	float glslversion = 0;						// This is here so that the differences between old OpenGL and new OpenGL/Vulkan can be handled by platform independent code.
	int instack[2] = { 0,0 };					// this is globally maintained state for portal recursion avoidance.
	int stencilValue = 0;						// Global stencil test value
	unsigned int uniformblockalignment = 256;	// Hardware dependent uniform buffer alignment.
	unsigned int maxuniformblock = 65536;
	const char *vendorstring;					// We have to account for some issues with particular vendors.
	FSkyVertexBuffer *mSkyData = nullptr;		// the sky vertex buffer
	FFlatVertexBuffer *mVertexData = nullptr;	// Global vertex data
	HWViewpointBuffer *mViewpoints = nullptr;	// Viewpoint render data.
	FLightBuffer *mLights = nullptr;			// Dynamic lights
	BoneBuffer* mBones = nullptr;				// Model bones
	IShadowMap mShadowMap;

	int mGameScreenWidth = 0;
	int mGameScreenHeight = 0;
	IntRect mScreenViewport;
	IntRect mSceneViewport;
	IntRect mOutputLetterbox;
	float mSceneClearColor[4]{ 0,0,0,255 };

	int mPipelineNbr = 1;						// Number of HW buffers to pipeline
	int mPipelineType = 0;

public:
	DFrameBuffer (int width=1, int height=1);
	virtual ~DFrameBuffer();
	virtual void InitializeState() = 0;	// For stuff that needs 'screen' set.
	virtual bool IsVulkan() { return false; }
	virtual bool IsPoly() { return false; }
	virtual int GetShaderCount();
	virtual bool CompileNextShader() { return true; }
	void SetAABBTree(hwrenderer::LevelAABBTree * tree)
	{
		mShadowMap.SetAABBTree(tree);
	}
	virtual void SetLevelMesh(hwrenderer::LevelMesh *mesh) { }
	bool allowSSBO() const
	{
#ifndef HW_BLOCK_SSBO
		return true;
#else
		return mPipelineType == 0;
#endif
	}

	// SSBOs have quite worse performance for read only data, so keep this around only as long as Vulkan has not been adapted yet.
	bool useSSBO() 
	{
		return IsVulkan();
	}

	virtual DCanvas* GetCanvas() { return nullptr; }

	void SetSize(int width, int height);
	void SetVirtualSize(int width, int height)
	{
		Width = width;
		Height = height;
	}
	inline int GetWidth() const { return Width; }
	inline int GetHeight() const { return Height; }

	FVector2 SceneScale() const
	{
		return { mSceneViewport.width / (float)mScreenViewport.width, mSceneViewport.height / (float)mScreenViewport.height };
	}

	FVector2 SceneOffset() const
	{
		return { mSceneViewport.left / (float)mScreenViewport.width, mSceneViewport.top / (float)mScreenViewport.height };
	}

	// Make the surface visible.
	virtual void Update ();

	// Stores the palette with flash blended in into 256 dwords
	// Mark the palette as changed. It will be updated on the next Update().
	virtual void UpdatePalette() {}

	// Returns true if running fullscreen.
	virtual bool IsFullscreen () = 0;
	virtual void ToggleFullscreen(bool yes) {}

	// Changes the vsync setting, if supported by the device.
	virtual void SetVSync (bool vsync);

	// Delete any resources that need to be deleted after restarting with a different IWAD
	virtual void SetTextureFilterMode() {}
	virtual IHardwareTexture *CreateHardwareTexture(int numchannels) { return nullptr; }
	virtual void PrecacheMaterial(FMaterial *mat, int translation) {}
	virtual FMaterial* CreateMaterial(FGameTexture* tex, int scaleflags);
	virtual void BeginFrame() {}
	virtual void SetWindowSize(int w, int h) {}
	virtual void StartPrecaching() {}
	virtual FRenderState* RenderState() { return nullptr; }

	virtual int GetClientWidth() = 0;
	virtual int GetClientHeight() = 0;
	virtual void BlurScene(float amount) {}

	virtual void InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData) {}

    // Interface to hardware rendering resources
	virtual IVertexBuffer *CreateVertexBuffer() { return nullptr; }
	virtual IIndexBuffer *CreateIndexBuffer() { return nullptr; }
	virtual IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) { return nullptr; }
	bool BuffersArePersistent() { return !!(hwcaps & RFL_BUFFER_STORAGE); }

	// This is overridable in case Vulkan does it differently.
	virtual bool RenderTextureIsFlipped() const
	{
		return true;
	}

	// Report a game restart
	void SetClearColor(int color);
	virtual int Backend() { return 0; }
	virtual const char* DeviceName() const { return "Unknown"; }
	virtual bool SupportsQueued2DTextureRenders() const { return false; }
	virtual void AmbientOccludeScene(float m5) {}
	virtual void FirstEye() {}
	virtual void NextEye(int eyecount) {}
	virtual void SetSceneRenderTarget(bool useSSAO) {}
	virtual void UpdateShadowMap() {}
	virtual void WaitForCommands(bool finish) {}
	virtual void SetSaveBuffers(bool yes) {}
	virtual bool PrepareSavePicScene(int width, int height) { return true; }
	virtual void FinishSavePicScene() {}
	virtual void ImageTransitionScene(bool unknown) {}
	virtual void CopyScreenToBuffer(int width, int height, uint8_t* buffer)	{ memset(buffer, 0, width* height); }
	virtual bool FlipSavePic() const { return false; }
	virtual void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect&)> renderFunc) {}
	virtual void RenderTextureView(FGameTexture* tex, std::function<void(IntRect&)> renderFunc) {}
	virtual void SnapshotCurrentViewToCanvas(FCanvasTexture* tex) {}
	void Queue2DTextureRender(FGameTexture* tex, F2DDrawer* drawer);
	void FlushQueued2DTextureRenders();
	virtual void NotifyPathTracingCameraCut(const char* reason) {}
	virtual void SetPathTracingGuiCaptureState(bool active) {}
	virtual bool ShouldSkipSceneBuildForPathTracedScene(int drawmode, bool portal) const { return false; }
	virtual bool RenderPathTracedScene(HWDrawInfo& di, int drawmode, bool portal) { return false; }
	virtual bool HasActiveSceneFrame() const { return true; }
	virtual bool StartPathTracingLevelPreload() { return false; }
	virtual bool TickPathTracingLevelPreload() { return true; }
	virtual bool IsPathTracingLevelPreloadPending() const { return false; }
	virtual void CancelPathTracingLevelPreload() {}
	virtual void NotifyPathTracingLevelFirstFrameRelease() {}
	virtual void NotifyPathTracingLevelPreloadFinalCheckRelease() {}
	virtual void NotifyLevelUnloadBegin(const LevelTransitionInfo& info) {}
	virtual void NotifyLevelUnloadComplete(const LevelTransitionInfo& info) {}
	virtual void NotifyLevelLoadBegin(const LevelTransitionInfo& info) {}
	virtual void SetActiveRenderTarget() {}
	virtual void EmitPathTracingWeaponLightEvent(const PathTracingWeaponLightEvent& event);
	virtual void EmitPathTracingSmokeSourceEvent(const PathTracingSmokeSourceEvent& event);
	virtual void EmitPathTracingActorSpriteTraceEvent(const PathTracingActorSpriteTraceEvent& event);
	virtual void PrintPathTracingSurfaceProbeStatus() const;
	virtual bool BuildPathTracingEmissiveLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const { outTarget = {}; return false; }
	virtual bool BuildPathTracingSurfaceLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const { outTarget = {}; return false; }
	virtual bool ProjectPathTracingEditorLine(const float renderStart[3], const float renderEnd[3],
		DVector2& outStart, DVector2& outEnd) const { outStart = {}; outEnd = {}; return false; }
	virtual bool SetPathTracingEditorPointLight(const DVector3& worldPosition, const float color[3], float intensity, float radius) { return false; }
	virtual void ClearPathTracingEditorPointLight() {}

	// Screen wiping
	virtual FTexture *WipeStartScreen();
	virtual FTexture *WipeEndScreen();

	virtual void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D) { if (afterBloomDrawEndScene2D) afterBloomDrawEndScene2D(); }

	void ScaleCoordsFromWindow(int16_t &x, int16_t &y);

	virtual void Draw2D() {}

	virtual void SetViewportRects(IntRect *bounds);
	int ScreenToWindowX(int x);
	int ScreenToWindowY(int y);

	void FPSLimit();

	// Retrieves a buffer containing image data for a screenshot.
	// Hint: Pitch can be negative for upside-down images, in which case buffer
	// points to the last row in the buffer, which will be the first row output.
	virtual bool QueueScreenshot(FileWriter* file, const char* filename = nullptr) { return false; }
	virtual TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma) { return TArray<uint8_t>(); }

	static float GetZNear() { return 5.f; }
	static float GetZFar() { return 65536.f; }

	// The original size of the framebuffer as selected in the video menu.
	uint64_t FrameTime = 0;

private:
	uint64_t fpsLimitTime = 0;

	bool isIn2D = false;
};


// This is the screen updated by I_FinishUpdate.
extern DFrameBuffer *screen;

#define SCREENWIDTH (screen->GetWidth ())
#define SCREENHEIGHT (screen->GetHeight ())

EXTERN_CVAR (Float, vid_gamma)


// Allocates buffer screens, call before R_Init.
void V_InitScreenSize();
void V_InitScreen();

// Initializes graphics mode for the first time.
void V_Init2 ();

void V_Shutdown ();
int V_GetBackend();

inline bool IsRatioWidescreen(int ratio) { return (ratio & 3) != 0; }
extern bool setsizeneeded, setmodeneeded;


#endif // __V_VIDEO_H__
