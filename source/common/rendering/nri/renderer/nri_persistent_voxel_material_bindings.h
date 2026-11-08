#pragma once

class NRIPersistentVoxelResidency;
struct NRIPersistentVoxelResetServices;

// Drain at entry and every exit, including failed/early reuse paths.
class NRIVoxelMaterialRetirementScope
{
public:
	NRIVoxelMaterialRetirementScope(NRIPersistentVoxelResidency& owner, const NRIPersistentVoxelResetServices& services);
	~NRIVoxelMaterialRetirementScope();
private:
	NRIPersistentVoxelResidency& owner;
	const NRIPersistentVoxelResetServices& services;
};
