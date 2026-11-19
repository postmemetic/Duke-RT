# This optional target produces D3D12 audit variants beside the production
# shaders. It is deliberately independent of the game and default build targets.
set(_nri_audit_dir "${RAZE_NRI_SHADER_OUTPUT_DIR}/variants/perf-audit")
raze_add_trace_opaque_shaders(_nri_audit_outputs "${_nri_audit_dir}" PERF_AUDIT)

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
