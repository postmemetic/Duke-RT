#pragma once

// Sharing changes physical alias ownership. Keep the first requested mode until
// all shared resources are reset, including across retained-resource map loads.
class NRIVoxelMaterialSharingMode
{
public:
	bool Observe(bool value)
	{
		const bool report = !latched || requested != value;
		requested = value;
		if (!latched) { enabled = value; latched = true; }
		return report;
	}
	bool Latched() const { return latched; }
	bool Enabled() const { return enabled; }
	bool Requested() const { return requested; }
	void Reset() { latched = false; enabled = requested = true; }
private:
	bool latched = false, enabled = true, requested = true;
};
