# openh264 (welsdec) software H.264 decoder, vendored from openh264 v2.2.0
# (codec/{api,common,decoder}); only the C fallback paths are compiled (no asm).

set(OPENH264_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/openh264")

set(OH264_SRCS
    "wels_dec.cpp"

    "${OPENH264_DIR}/codec/common/src/common_tables.cpp"
    "${OPENH264_DIR}/codec/common/src/copy_mb.cpp"
    "${OPENH264_DIR}/codec/common/src/cpu.cpp"
    "${OPENH264_DIR}/codec/common/src/crt_util_safe_x.cpp"
    "${OPENH264_DIR}/codec/common/src/deblocking_common.cpp"
    "${OPENH264_DIR}/codec/common/src/expand_pic.cpp"
    "${OPENH264_DIR}/codec/common/src/intra_pred_common.cpp"
    "${OPENH264_DIR}/codec/common/src/mc.cpp"
    "${OPENH264_DIR}/codec/common/src/memory_align.cpp"
    "${OPENH264_DIR}/codec/common/src/sad_common.cpp"
    "${OPENH264_DIR}/codec/common/src/utils.cpp"
    "${OPENH264_DIR}/codec/common/src/welsCodecTrace.cpp"
    "${OPENH264_DIR}/codec/common/src/WelsTaskThread.cpp"
    "${OPENH264_DIR}/codec/common/src/WelsThread.cpp"
    "${OPENH264_DIR}/codec/common/src/WelsThreadLib.cpp"
    "${OPENH264_DIR}/codec/common/src/WelsThreadPool.cpp"

    "${OPENH264_DIR}/codec/decoder/core/src/au_parser.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/bit_stream.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/cabac_decoder.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/deblocking.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/decode_mb_aux.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/decode_slice.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/decoder.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/decoder_core.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/decoder_data_tables.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/error_concealment.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/fmo.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/get_intra_predictor.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/manage_dec_ref.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/memmgr_nal_unit.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/mv_pred.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/parse_mb_syn_cabac.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/parse_mb_syn_cavlc.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/pic_queue.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/rec_mb.cpp"
    "${OPENH264_DIR}/codec/decoder/core/src/wels_decoder_thread.cpp"

    "${OPENH264_DIR}/codec/decoder/plus/src/welsDecoderExt.cpp"
)

idf_component_register(
    SRCS ${OH264_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "${OPENH264_DIR}/codec/api/svc"
        "${OPENH264_DIR}/codec/common/inc"
        "${OPENH264_DIR}/codec/decoder/core/inc"
        "${OPENH264_DIR}/codec/decoder/plus/inc"
)

# RISC-V builds use the portable C paths; make sure no x86/ARM detection kicks in.
# The vendored sources are third-party code with warning-noise (format strings,
# ignored qualifiers...), so keep warnings visible but non-fatal.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WELS_DECODER_BUILD=1
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-parameter)
