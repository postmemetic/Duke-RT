#pragma once

// Internal CPU-path experiments and their reference observers are session-only.
// Ignore older persisted assignments so source defaults apply after an upgrade.
// This list deliberately excludes renderer quality, backend/API selection and
// general settings. Console commands, +set and exec do not consult this policy.
namespace nri_config
{
static constexpr const char* SessionOnlyCpuControls[] = {
	"nri_ptactorlightmaintenance",
	"nri_ptactorlightvalidate",
	"nri_ptatlasallocationreuse",
	"nri_ptatlasallocationvalidate",
	"nri_ptdecalreusevalidate",
	"nri_ptemissivecache",
	"nri_ptemissivecachevalidate",
	"nri_ptemissivediagnostichashgate",
	"nri_ptemissivefactresetreuse",
	"nri_ptemissiveresponselookup",
	"nri_ptemissiveresponseproduct",
	"nri_ptemissivestabilitytrace",
	"nri_ptemissivetailappend",
	"nri_ptemissivetailvalidate",
	"nri_ptfluidsignaturereuse",
	"nri_ptfluidsignaturevalidate",
	"nri_ptlightregistry",
	"nri_ptlightregistryvalidate",
	"nri_ptmapmotioncertificate",
	"nri_ptmapmotionvalidate",
	"nri_ptmapmotionworkers",
	"nri_ptmaterialcolorreuse",
	"nri_ptmaterialcolorvalidate",
	"nri_ptmaterialnamespacefast",
	"nri_ptmaterialpatch",
	"nri_ptmaterialpatchvalidate",
	"nri_ptmaterialpolicycache",
	"nri_ptmaterialpolicyvalidate",
	"nri_ptmaterialrangesvalidate",
	"nri_ptmaterialsegmentreuse",
	"nri_ptmutationbaselinepublication",
	"nri_ptmutationbaselinevalidate",
	"nri_ptmutationcausechunk",
	"nri_ptmutationcausechunks",
	"nri_ptmutationcausetrace",
	"nri_ptmutationsourcenotify",
	"nri_ptmutationworklistobserve",
	"nri_ptmutationworklistvalidate",
	"nri_ptpalettecompare",
	"nri_ptpalettesnapshot",
	"nri_ptpalettesnapshotvalidate",
	"nri_ptregistryvalidate",
	"nri_ptresidentblasbatch",
	"nri_ptresidentblasbatchvalidate",
	"nri_ptresidentprefixreuse",
	"nri_ptscenebufferdirtyrangegap",
	"nri_ptscenebufferrangeuploadmaxpercent",
	"nri_ptscenebufferrangeuploadmaxranges",
	"nri_ptscenedataring",
	"nri_ptscenedataringmaxbytes",
	"nri_ptsceneuploadvalidateinterval",
	"nri_ptsectorhueproduct",
	"nri_ptsectorhueproductvalidate",
	"nri_ptstaticfiltercache",
	"nri_ptstaticfiltervalidate",
	"nri_ptstaticmaterialpatch",
	"nri_ptstaticmaterialpatchvalidate",
	"nri_pttextureresolvevalidate",
	"nri_ptvoxelactorindexvalidate",
	"nri_ptvoxelactorlifecycle",
	"nri_ptvoxelarenapresize",
	"nri_ptvoxelbudgettestbytes",
	"nri_ptvoxelcachebounded",
	"nri_ptvoxelcachegraceframes",
	"nri_ptvoxelcacheintervalframes",
	"nri_ptvoxelcachematerialbytes",
	"nri_ptvoxelcachematerialrecords",
	"nri_ptvoxelcachemeshbytes",
	"nri_ptvoxelcachevalidate",
	"nri_ptvoxelcomputevalidatefullreadback",
	"nri_ptvoxelmaterialdigestcache",
	"nri_ptvoxelmaterialsharing",
	"nri_ptvoxelpublication",
	"nri_ptvoxelpublicationvalidate",
	"nri_ptzerotickreusevalidate",
	"perf_auditcpudetail",
	"perf_auditcpuframes",
};

inline bool IsSessionOnlyCpuControl(const char* key)
{
	if (key == nullptr) return false;
	for (const char* name : SessionOnlyCpuControls)
	{
		const char* candidate = key;
		while (*name != '\0' && *candidate != '\0')
		{
			const char c = *candidate >= 'A' && *candidate <= 'Z' ? static_cast<char>(*candidate + ('a' - 'A')) : *candidate;
			if (c != *name) break;
			++name;
			++candidate;
		}
		if (*name == '\0' && *candidate == '\0') return true;
	}
	return false;
}
}
