#pragma once

#include "serializer.h"
#include "coreplayer.h"
#include "d_net.h"
#include "input_lineage.h"

enum : unsigned
{
	CS_CANCROUCH = 1,
	CS_DISABLETOGGLE = 2,
};

enum : unsigned
{
	VEH_CANMOVE = 1,
	VEH_CANTURN = 2,
	VEH_SCALETURN = 4,
};

class GameInput
{
	enum class LateMouseMode : uint8_t
	{
		None,
		Movement,
		Vehicle,
	};

	struct LateMouseRoute
	{
		LateMouseMode mode;
		double turnscale;
		double vehicleBaseVel;
		double vehicleVelScale;
		unsigned vehicleFlags;
		bool yawLook;
		bool pitchLook;
	};

	/*
	// Turbo turn time.
	Blood:     24 * 30 = 720;
	Duke: 120 / 8 * 30 = 450;
	SW:   120 / 8 * 40 = 600;
	Exhumed: N/A;
	Average: 590.;
	*/
	enum
	{
		BUILDTICRATE = 120,
		TURBOTURNBASE = 590,
	};

	// Input received from the OS.
	float joyAxes[NUM_JOYAXIS];
	FVector2 mouseInput;
	DRotator lateAppliedMouseAngles;
	LateMouseRoute lateMouseRoute;

	// Internal variables when generating a packet.
	InputPacket inputBuffer;
	ESyncBits ActionsToSend;
	double turnheldtime;
	double scaleAdjust;
	bool syncinput;
	int WeaponToSend;
	int dpad_lock;
	int keymove;

	// Turn speed doubling after x amount of tics.
	void updateTurnHeldAmt()
	{
		turnheldtime += getTicrateScale(BUILDTICRATE * scaleAdjust);
	}
	bool isTurboTurnTime()
	{
		return turnheldtime >= getTicrateScale(TURBOTURNBASE);
	}

	// Prototypes for private member functions.
	void processInputBits();
	DRotator getDesiredLateMouseAngles();
	void applyLocalCameraDelta(const DRotator& delta);
	void reconcileLocalCamera(
		const DRotator& commandAngles,
		const DRotator& commandMouseAngles,
		bool yawLook,
		bool pitchLook);

public:
	// Bit sender updates.
	void SendWeapon(const int weapon)
	{
		WeaponToSend = weapon;
	}
	void SendAction(const ESyncBits action)
	{
		ActionsToSend |= action;
	}

	// Clear all values within this object.
	void Clear()
	{
		CancelLateMouseLook();
		PerfInputLineageDiscardPendingMouse();
		memset(this, 0, sizeof(*this));
	}

	// Receives mouse input from OS for processing.
	void MouseAddToPos(float x, float y)
	{
		mouseInput.X += x;
		mouseInput.Y += y;
	}

	// Receives the current input scale from the engine's main loop.
	void UpdateInputScale()
	{
		const double frac = I_GetInputFrac();
		scaleAdjust = !SyncInput() ? frac : 1;
	}

	// Handling of whether to allow unsynchronised input.
	bool SyncInput()
	{
		return syncinput || cl_syncinput || cl_capfps;
	}
	bool LateMouseLatchEnabled() const;
	bool ApplyLateMouseLook();
	void CancelLateMouseLook();
	double GetInputScale() const
	{
		return scaleAdjust;
	}
	void ForceInputSync(const int pnum)
	{
		if (pnum == myconnectindex)
		{
			syncinput = true;
		}
	}
	void ResetInputSync()
	{
		syncinput = false;
	}

	// Prototypes for large member functions.
	void processMovement(const double turnscale = 1, const bool allowstrafe = true, const int drink_amt = 0);
	void processVehicle(const double baseVel, const double velScale, const unsigned flags);
	void getInput(InputPacket* packet = nullptr);
	void resetCrouchToggle();
};

extern GameInput gameInput;
