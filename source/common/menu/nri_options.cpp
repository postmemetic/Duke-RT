#include "menu.h"
#include "vm.h"
#ifdef HAVE_NRI
#include "../rendering/nri/renderer/nri_settings_availability.h"
#endif

DEFINE_ACTION_FUNCTION(_OptionMenuItemSupportedOption, UnavailableReason)
{
	PARAM_PROLOGUE;
	PARAM_NAME(setting);
	PARAM_INT(value);
#ifdef HAVE_NRI
	const std::string reason = NRIGetSettingUnavailableReason(setting.GetChars(), value);
	ACTION_RETURN_STRING(FString(reason.c_str()));
#else
	ACTION_RETURN_STRING(FString("Path tracing settings are unavailable in this build."));
#endif
}
