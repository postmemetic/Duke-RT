#include "voxelpolicy_editor.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

#include "c_cvars.h"
#include "i_time.h"
#include "keydef.h"
#include "lightoverlay_smoke_editor.h"
#include "printf.h"
#include "v_video.h"

EXTERN_CVAR(Bool, nri_ptactorlighteditmode)
EXTERN_CVAR(Bool, nri_ptmaplighteditmode)
EXTERN_CVAR(Bool, nri_ptemissivelighteditmode)

CVAR(Bool, nri_ptvoxelpolicyeditmode, false, 0)

namespace
{
	static constexpr uint64_t NotifyRepeatMs = 1000;
	static constexpr float EmissionScaleStep = 0.1f;

	struct VoxelPolicyEditorState
	{
		bool enabled = false;
		PathTracingVoxelPolicyEditTarget target;
		uint32_t selectedPaletteIndex = UINT32_MAX;
		uint32_t lastAimedPaletteIndex = UINT32_MAX;
		FString selectedResource;
		FString lastNotifyIdentity;
		uint64_t lastNotifyTimeMs = 0;
		bool printPressed = false;
		bool createPressed = false;
		bool cyclePreviousPressed = false;
		bool cycleNextPressed = false;
		bool emissionPressed = false;
		bool fullbrightPressed = false;
		bool castShadowPressed = false;
		bool receiveShadowPressed = false;
		bool scaleDownPressed = false;
		bool scaleUpPressed = false;
		bool reloadPressed = false;
		bool discardPressed = false;
	};

	static VoxelPolicyEditorState GEditor;

	static bool HasConflictingEditorMode()
	{
		return !!nri_ptactorlighteditmode ||
			!!nri_ptmaplighteditmode ||
			!!nri_ptemissivelighteditmode ||
			IsMapSmokeEmitterEditorEnabled();
	}

	static bool IsActionKey(const event_t* ev, char key)
	{
		if (ev == nullptr || (ev->type != EV_KeyDown && ev->type != EV_KeyUp))
		{
			return false;
		}
		const unsigned char ascii = static_cast<unsigned char>(ev->data2 & 0xff);
		return ascii != 0 && std::tolower(ascii) == key;
	}

	static void Notify(const FString& message)
	{
		Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "%s\n", message.GetChars());
	}

	static void ResetState(bool enabled)
	{
		GEditor = {};
		GEditor.enabled = enabled;
	}

	static bool HasEditableTarget()
	{
		return GEditor.target.valid && GEditor.target.hit &&
			!GEditor.target.voxelResource.IsEmpty() &&
			GEditor.selectedPaletteIndex < 256u;
	}

	static void PrintTarget(const char* prefix)
	{
		if (!HasEditableTarget())
		{
			Printf("NRI PT voxel policy editor: %s\n",
				GEditor.target.failureReason.IsEmpty() ? "no editable voxel surface is selected" : GEditor.target.failureReason.GetChars());
			return;
		}

		const uint32_t selected = GEditor.selectedPaletteIndex;
		const uint8_t* selectedRgb = GEditor.target.rawPaletteRgbByIndex[selected];
		const uint32_t selectedPolicy = GEditor.target.policyByPaletteIndex[selected];
		Printf(
			"NRI PT voxel policy editor: %s actor=%d pic=%d voxel=%d resource=%s index=%u aimed_index=%u raw=(%u,%u,%u) resolved=(%u,%u,%u) policy=0x%02x source=%s source_hash=0x%llx palette_hash=0x%llx\n",
			prefix != nullptr ? prefix : "target",
			GEditor.target.actorIndex,
			GEditor.target.sourcePicnum,
			GEditor.target.resolvedVoxelIndex,
			GEditor.target.voxelResource.GetChars(),
			GEditor.selectedPaletteIndex,
			GEditor.target.paletteIndex,
			(unsigned int)selectedRgb[0],
			(unsigned int)selectedRgb[1],
			(unsigned int)selectedRgb[2],
			(unsigned int)selectedRgb[0],
			(unsigned int)selectedRgb[1],
			(unsigned int)selectedRgb[2],
			(unsigned int)selectedPolicy,
			GEditor.target.policySource.IsEmpty() ? "(legacy)" : GEditor.target.policySource.GetChars(),
			(unsigned long long)GEditor.target.sourceHash,
			(unsigned long long)GEditor.target.paletteHash);
	}

	static bool SubmitEdit(PathTracingVoxelPolicyEditAction action, float scaleDelta = 0.0f)
	{
		if (!HasEditableTarget())
		{
			Notify("Voxel policy edit requires an aimed resident voxel surface.");
			return true;
		}
		if (screen == nullptr)
		{
			Notify("Voxel policy edit failed: no screen backend is active.");
			return true;
		}

		PathTracingVoxelPolicyEditRequest request = {};
		request.action = action;
		request.actorIndex = GEditor.target.actorIndex;
		request.sourcePicnum = GEditor.target.sourcePicnum;
		request.resolvedVoxelIndex = GEditor.target.resolvedVoxelIndex;
		request.paletteIndex = GEditor.selectedPaletteIndex;
		request.sourceHash = GEditor.target.sourceHash;
		request.paletteHash = GEditor.target.paletteHash;
		request.contentHash = GEditor.target.policyContentHash;
		request.emissionScaleDelta = scaleDelta;
		request.voxelResource = GEditor.target.voxelResource;

		PathTracingVoxelPolicyEditResult result = {};
		if (!screen->ApplyPathTracingVoxelPolicyEdit(request, result))
		{
			Notify(result.message.IsEmpty() ?
				"Voxel policy editing is not available from the active renderer." : result.message);
			return true;
		}

		FString message;
		message.Format(
			"Voxel policy %s: resource=%s index=%u policy=0x%02x path=%s reload=%s",
			result.changed ? "updated" : "unchanged",
			GEditor.target.voxelResource.GetChars(),
			GEditor.selectedPaletteIndex,
			(unsigned int)result.policyBits,
			result.writablePath.IsEmpty() ? "(none)" : result.writablePath.GetChars(),
			result.reloaded ? "ok" : "not-applied");
		Notify(message);
		return true;
	}

	static bool CycleSelectedIndex(int direction)
	{
		if (!HasEditableTarget() || GEditor.target.usedPaletteIndexCount == 0)
		{
			Notify("Voxel policy index cycling requires an aimed voxel with indexed-color data.");
			return true;
		}

		const uint32_t count = std::min<uint32_t>(GEditor.target.usedPaletteIndexCount, 256u);
		uint32_t current = 0;
		for (uint32_t i = 0; i < count; ++i)
		{
			if (GEditor.target.usedPaletteIndices[i] == GEditor.selectedPaletteIndex)
			{
				current = i;
				break;
			}
		}
		const uint32_t next = direction < 0 ? (current + count - 1u) % count : (current + 1u) % count;
		GEditor.selectedPaletteIndex = GEditor.target.usedPaletteIndices[next];
		PrintTarget("selected");
		return true;
	}

	static bool HandleLatchedAction(const event_t* ev, bool& pressed, PathTracingVoxelPolicyEditAction action, float scaleDelta = 0.0f)
	{
		if (ev->type == EV_KeyDown)
		{
			if (!pressed)
			{
				pressed = true;
				SubmitEdit(action, scaleDelta);
			}
		}
		else
		{
			pressed = false;
		}
		return true;
	}
}

bool IsVoxelPolicyEditorEnabled()
{
	return !!nri_ptvoxelpolicyeditmode;
}

void TickVoxelPolicyEditor()
{
	const bool requested = IsVoxelPolicyEditorEnabled();
	if (!requested)
	{
		if (GEditor.enabled)
		{
			ResetState(false);
		}
		return;
	}
	if (HasConflictingEditorMode())
	{
		Notify("Voxel policy edit mode cannot overlap another lighting or smoke editor mode.");
		nri_ptvoxelpolicyeditmode = false;
		ResetState(false);
		return;
	}
	if (!GEditor.enabled)
	{
		ResetState(true);
		Notify("Voxel policy edit mode enabled: o create, p status, [/] index, e emission, f fullbright, c cast, r receive, ,/. scale, l reload, d discard, Escape exit.");
	}

	PathTracingVoxelPolicyEditTarget target = {};
	if (screen == nullptr || !screen->BuildPathTracingVoxelPolicyEditTarget(target))
	{
		GEditor.target = target;
		const uint64_t now = I_msTime();
		if (!target.failureReason.IsEmpty() &&
			(GEditor.lastNotifyIdentity != target.failureReason || now - GEditor.lastNotifyTimeMs >= NotifyRepeatMs))
		{
			Notify(target.failureReason);
			GEditor.lastNotifyIdentity = target.failureReason;
			GEditor.lastNotifyTimeMs = now;
		}
		return;
	}

	const bool resourceChanged = GEditor.selectedResource.Compare(target.voxelResource) != 0;
	const bool aimedIndexChanged = GEditor.lastAimedPaletteIndex != target.paletteIndex;
	GEditor.target = target;
	if (resourceChanged || aimedIndexChanged || GEditor.selectedPaletteIndex >= 256u)
	{
		GEditor.selectedResource = target.voxelResource;
		GEditor.selectedPaletteIndex = target.paletteIndex;
		GEditor.lastAimedPaletteIndex = target.paletteIndex;
	}

	FString identity;
	identity.Format("%s:%u", target.voxelResource.GetChars(), GEditor.selectedPaletteIndex);
	const uint64_t now = I_msTime();
	if (identity != GEditor.lastNotifyIdentity || now - GEditor.lastNotifyTimeMs >= NotifyRepeatMs)
	{
		FString message;
		message.Format("Voxel %s palette index %u rgb=(%u,%u,%u) policy 0x%02x",
			target.voxelResource.GetChars(),
			GEditor.selectedPaletteIndex,
			(unsigned int)target.rawPaletteRgbByIndex[GEditor.selectedPaletteIndex][0],
			(unsigned int)target.rawPaletteRgbByIndex[GEditor.selectedPaletteIndex][1],
			(unsigned int)target.rawPaletteRgbByIndex[GEditor.selectedPaletteIndex][2],
			(unsigned int)target.policyByPaletteIndex[GEditor.selectedPaletteIndex]);
		Notify(message);
		GEditor.lastNotifyIdentity = identity;
		GEditor.lastNotifyTimeMs = now;
	}
}

bool VoxelPolicyEditorResponder(event_t* ev)
{
	if (!IsVoxelPolicyEditorEnabled() || ev == nullptr || HasConflictingEditorMode())
	{
		return false;
	}
	if (ev->type != EV_KeyDown && ev->type != EV_KeyUp)
	{
		return false;
	}

	if (ev->data1 == KEY_ESCAPE)
	{
		if (ev->type == EV_KeyDown)
		{
			nri_ptvoxelpolicyeditmode = false;
			ResetState(false);
			Notify("Voxel policy edit mode disabled.");
		}
		return true;
	}
	if (IsActionKey(ev, 'p'))
	{
		if (ev->type == EV_KeyDown && !GEditor.printPressed)
		{
			GEditor.printPressed = true;
			PrintTarget("target");
		}
		if (ev->type == EV_KeyUp) GEditor.printPressed = false;
		return true;
	}
	if (IsActionKey(ev, 'o')) return HandleLatchedAction(ev, GEditor.createPressed, PathTracingVoxelPolicyEditAction::CreateSidecar);
	if (IsActionKey(ev, '['))
	{
		if (ev->type == EV_KeyDown && !GEditor.cyclePreviousPressed)
		{
			GEditor.cyclePreviousPressed = true;
			CycleSelectedIndex(-1);
		}
		if (ev->type == EV_KeyUp) GEditor.cyclePreviousPressed = false;
		return true;
	}
	if (IsActionKey(ev, ']'))
	{
		if (ev->type == EV_KeyDown && !GEditor.cycleNextPressed)
		{
			GEditor.cycleNextPressed = true;
			CycleSelectedIndex(1);
		}
		if (ev->type == EV_KeyUp) GEditor.cycleNextPressed = false;
		return true;
	}
	if (IsActionKey(ev, 'e')) return HandleLatchedAction(ev, GEditor.emissionPressed, PathTracingVoxelPolicyEditAction::ToggleEmission);
	if (IsActionKey(ev, 'f')) return HandleLatchedAction(ev, GEditor.fullbrightPressed, PathTracingVoxelPolicyEditAction::ToggleFullbright);
	if (IsActionKey(ev, 'c')) return HandleLatchedAction(ev, GEditor.castShadowPressed, PathTracingVoxelPolicyEditAction::ToggleShadowCast);
	if (IsActionKey(ev, 'r')) return HandleLatchedAction(ev, GEditor.receiveShadowPressed, PathTracingVoxelPolicyEditAction::ToggleShadowReceive);
	if (IsActionKey(ev, ',')) return HandleLatchedAction(ev, GEditor.scaleDownPressed, PathTracingVoxelPolicyEditAction::AdjustEmissionScale, -EmissionScaleStep);
	if (IsActionKey(ev, '.')) return HandleLatchedAction(ev, GEditor.scaleUpPressed, PathTracingVoxelPolicyEditAction::AdjustEmissionScale, EmissionScaleStep);
	if (IsActionKey(ev, 'l')) return HandleLatchedAction(ev, GEditor.reloadPressed, PathTracingVoxelPolicyEditAction::Reload);
	if (IsActionKey(ev, 'd')) return HandleLatchedAction(ev, GEditor.discardPressed, PathTracingVoxelPolicyEditAction::DiscardAndReload);
	return false;
}
