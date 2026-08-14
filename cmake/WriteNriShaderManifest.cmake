if(NOT DEFINED SHADER_DIR OR NOT DEFINED PROFILE)
	message(FATAL_ERROR "SHADER_DIR and PROFILE are required")
endif()

file(GLOB _canonical RELATIVE "${SHADER_DIR}"
	"${SHADER_DIR}/*.dxil" "${SHADER_DIR}/*.spirv")
list(FILTER _canonical EXCLUDE REGEX "^audit-task-")
list(SORT _canonical)
list(LENGTH _canonical _canonical_count)
if(PROFILE STREQUAL "PRODUCTION" AND NOT _canonical_count EQUAL 184)
	message(FATAL_ERROR "Production NRI shader set must contain exactly 184 canonical blobs; found ${_canonical_count}")
endif()

set(_entries "")
foreach(_path IN LISTS _canonical)
	file(SHA256 "${SHADER_DIR}/${_path}" _hash)
	if(_path MATCHES "\\.dxil$")
		set(_backend "dxil")
	else()
		set(_backend "spirv")
	endif()
	string(APPEND _entries "    {\"path\": \"${_path}\", \"variant\": \"production\", \"backend\": \"${_backend}\", \"sha256\": \"${_hash}\"},\n")
endforeach()

if(PROFILE STREQUAL "DEVELOPER")
	file(GLOB _diagnostic RELATIVE "${SHADER_DIR}"
		"${SHADER_DIR}/variants/diagnostic/*.dxil"
		"${SHADER_DIR}/variants/diagnostic/*.spirv")
	list(SORT _diagnostic)
	foreach(_path IN LISTS _diagnostic)
		file(SHA256 "${SHADER_DIR}/${_path}" _hash)
		if(_path MATCHES "\\.dxil$")
			set(_backend "dxil")
		else()
			set(_backend "spirv")
		endif()
		string(APPEND _entries "    {\"path\": \"${_path}\", \"variant\": \"diagnostic\", \"backend\": \"${_backend}\", \"sha256\": \"${_hash}\"},\n")
	endforeach()
endif()

string(REGEX REPLACE ",\n$" "\n" _entries "${_entries}")
file(WRITE "${SHADER_DIR}/nri-shaders.json"
"{\n  \"schema\": 1,\n  \"requestedProfile\": \"${REQUESTED_PROFILE}\",\n  \"resolvedProfile\": \"${PROFILE}\",\n  \"defaultVariant\": \"production\",\n  \"canonicalBlobCount\": ${_canonical_count},\n  \"entries\": [\n${_entries}  ]\n}\n")
