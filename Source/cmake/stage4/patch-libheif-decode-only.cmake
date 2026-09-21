cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED SOURCE_DIR OR NOT IS_DIRECTORY "${SOURCE_DIR}")
  message(FATAL_ERROR "SOURCE_DIR must point to the extracted libheif source")
endif()

function(replace_exactly_once relative_path original replacement)
  set(path "${SOURCE_DIR}/${relative_path}")
  file(READ "${path}" contents)
  if(NOT "${replacement}" STREQUAL "")
    string(FIND "${contents}" "${replacement}" replacement_position)
    if(NOT replacement_position EQUAL -1)
      string(REPLACE "${replacement}" "" remainder "${contents}")
      string(FIND "${remainder}" "${original}" original_remaining)
      string(LENGTH "${contents}" original_length)
      string(LENGTH "${replacement}" replacement_length)
      string(LENGTH "${remainder}" remainder_length)
      math(EXPR expected_length "${original_length} - ${replacement_length}")
      if(NOT original_remaining EQUAL -1 OR NOT remainder_length EQUAL expected_length)
        message(FATAL_ERROR "Duplicate libheif patch target: ${relative_path}")
      endif()
      return()
    endif()
  endif()
  string(FIND "${contents}" "${original}" original_position)
  if(original_position EQUAL -1)
    if(NOT "${replacement}" STREQUAL "")
      message(FATAL_ERROR "Pinned libheif source changed unexpectedly: ${relative_path}")
    endif()
    return()
  endif()
  string(LENGTH "${original}" match_length)
  math(EXPR after_match "${original_position} + ${match_length}")
  string(SUBSTRING "${contents}" ${after_match} -1 remainder)
  string(FIND "${remainder}" "${original}" duplicate_position)
  if(NOT duplicate_position EQUAL -1)
    message(FATAL_ERROR "Duplicate libheif patch target: ${relative_path}")
  endif()
  string(REPLACE "${original}" "${replacement}" contents "${contents}")
  file(WRITE "${path}" "${contents}")
endfunction()

replace_exactly_once(
  "libheif/plugin_registry.cc"
  "#include \"plugins/encoder_mask.h\"\n"
  "")
replace_exactly_once(
  "libheif/plugin_registry.cc"
  "  register_encoder(get_encoder_plugin_mask());\n"
  "")
replace_exactly_once(
  "libheif/plugins/CMakeLists.txt"
  [=[target_sources(heif PRIVATE
               encoder_mask.h
               encoder_mask.cc
               nalu_utils.h
               nalu_utils.cc)]=]
  [=[target_sources(heif PRIVATE
               nalu_utils.h
               nalu_utils.cc)]=])

# The viewer has no HDR tone-mapping policy. Reject bitstream-only PQ/HLG before
# libheif converts pixels to the requested SDR output, not after that metadata is lost.
replace_exactly_once(
  "libheif/plugins/decoder_dav1d.cc"
  [=[  // --- convert image to heif_image

  heif_chroma chroma;]=]
  [=[  if (frame.seq_hdr->trc == DAV1D_TRC_SMPTE2084 || frame.seq_hdr->trc == DAV1D_TRC_HLG) {
    dav1d_picture_unref(&frame);
    return {heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
            "PQ/HLG AVIF requires tone mapping"};
  }

  // --- convert image to heif_image

  heif_chroma chroma;]=])
