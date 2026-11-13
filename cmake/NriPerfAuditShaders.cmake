# This optional target produces D3D12 audit variants beside the production
# shaders. It is deliberately independent of the game and default build targets.
set(_nri_audit_dir "${RAZE_NRI_SHADER_OUTPUT_DIR}/variants/perf-audit")
set(_nri_audit_args ${RAZE_NRI_COMMON_SHADER_ARGS})
list(REMOVE_ITEM _nri_audit_args -DNRI_SHADER_DIAGNOSTICS=0)
list(APPEND _nri_audit_args -DNRI_SHADER_DIAGNOSTICS=1 -DNRI_SHADER_PERF_AUDIT_ONLY=1)
set(_nri_audit_outputs "")
foreach(_family TraceOpaque TraceOpaqueLeanDebug TraceOpaqueTyped TraceOpaqueCache TraceOpaqueTypedCache)
	set(_defines "")
	if(_family STREQUAL "TraceOpaqueLeanDebug")
		list(APPEND _defines -DNRI_TRACE_GEOMETRY_DEBUG_ONLY=1)
	endif()
	if(_family MATCHES "Typed")
		list(APPEND _defines -DNRI_SPATIAL_ABSENCE_FORMAT=1)
	endif()
	if(_family MATCHES "Cache")
		list(APPEND _defines -DNRI_INDIRECT_RADIANCE_CACHE=1)
	endif()
	set(_output "${_nri_audit_dir}/${_family}.cs.dxil")
	add_custom_command(OUTPUT "${_output}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${_nri_audit_dir}"
		COMMAND "${RAZE_DXC_EXECUTABLE}" ${_nri_audit_args} ${_defines} -T cs_6_6 -E main
			-Fo "${_output}" "${CMAKE_CURRENT_SOURCE_DIR}/common/rendering/nri/shaders/TraceOpaque.cs.hlsl"
		DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/common/rendering/nri/shaders/TraceOpaque.cs.hlsl" ${NRI_SHARED_SHADER_FILES}
		COMMENT "Compiling performance-audit ${_family} (D3D12)"
		VERBATIM)
	list(APPEND _nri_audit_outputs "${_output}")
endforeach()

# Track the manifest as an output so deleting it or changing the writer repairs
# it on the next explicit audit-target build without recompiling every shader.
set(_nri_audit_manifest "${_nri_audit_dir}/perf-audit.json")
set(_nri_audit_manifest_writer "${CMAKE_CURRENT_LIST_DIR}/WriteNriPerfAuditManifest.cmake")
add_custom_command(OUTPUT "${_nri_audit_manifest}"
	COMMAND ${CMAKE_COMMAND} "-DAUDIT_SHADER_DIR=${_nri_audit_dir}" -P "${_nri_audit_manifest_writer}"
	DEPENDS ${_nri_audit_outputs} "${_nri_audit_manifest_writer}"
	COMMENT "Writing NRI performance-audit shader manifest"
	VERBATIM)
add_custom_target(raze_nri_perf_audit_shaders DEPENDS "${_nri_audit_manifest}")
set_target_properties(raze_nri_perf_audit_shaders PROPERTIES FOLDER "Shaders")
