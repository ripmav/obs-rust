/*
 * Conformance harness (area encopts): reconstruction of the parts of the NVIDIA
 * Video Codec SDK header (nvEncodeAPI.h, SDK 13.0 layout) that plugins/obs-nvenc's
 * option parser and property code compile against. The real header is not
 * available in the build environment. Only field *names and bit widths* matter
 * for the conformance vectors; reserved/unused fields are omitted. Test tooling,
 * not part of OBS.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NVENCAPI
#define NVENCAPI_MAJOR_VERSION 13
#define NVENCAPI_MINOR_VERSION 0

typedef int NVENCSTATUS;
typedef void *NV_ENC_INPUT_PTR;
typedef void *NV_ENC_REGISTERED_PTR;
typedef struct {
	uint32_t Data1;
	uint16_t Data2, Data3;
	uint8_t Data4[8];
} GUID;

typedef enum { NV_ENC_BUFFER_FORMAT_UNDEFINED = 0 } NV_ENC_BUFFER_FORMAT;
typedef enum {
	NV_ENC_BFRAME_REF_MODE_DISABLED = 0,
	NV_ENC_BFRAME_REF_MODE_EACH = 1,
	NV_ENC_BFRAME_REF_MODE_MIDDLE = 2
} NV_ENC_BFRAME_REF_MODE;
typedef enum {
	NV_ENC_SPLIT_AUTO_MODE = 0,
	NV_ENC_SPLIT_AUTO_FORCED_MODE = 1,
	NV_ENC_SPLIT_TWO_FORCED_MODE = 2,
	NV_ENC_SPLIT_THREE_FORCED_MODE = 3,
	NV_ENC_SPLIT_FOUR_FORCED_MODE = 4,
	NV_ENC_SPLIT_DISABLE_MODE = 15
} NV_ENC_SPLIT_ENCODE_MODE;
typedef enum { NV_ENC_LOOKAHEAD_LEVEL_0 = 0, NV_ENC_LOOKAHEAD_LEVEL_AUTOSELECT = 15 } NV_ENC_LOOKAHEAD_LEVEL;
typedef enum { NV_ENC_TEMPORAL_FILTER_LEVEL_0 = 0, NV_ENC_TEMPORAL_FILTER_LEVEL_4 = 4 } NV_ENC_TEMPORAL_FILTER_LEVEL;

typedef struct {
	uint32_t qpInterP;
	uint32_t qpInterB;
	uint32_t qpIntra;
} NV_ENC_QP;

typedef struct {
	NV_ENC_QP constQP;
	uint32_t averageBitRate;
	uint32_t maxBitRate;
	uint32_t vbvBufferSize;
	uint32_t vbvInitialDelay;
	uint32_t enableMinQP : 1;
	uint32_t enableMaxQP : 1;
	uint32_t enableInitialRCQP : 1;
	uint32_t enableAQ : 1;
	uint32_t reservedBitField1 : 1;
	uint32_t enableLookahead : 1;
	uint32_t disableIadapt : 1;
	uint32_t disableBadapt : 1;
	uint32_t enableTemporalAQ : 1;
	uint32_t zeroReorderDelay : 1;
	uint32_t enableNonRefP : 1;
	uint32_t strictGOPTarget : 1;
	uint32_t aqStrength : 4;
	uint32_t reservedBitFields : 16;
	NV_ENC_QP minQP;
	NV_ENC_QP maxQP;
	NV_ENC_QP initialRCQP;
	uint8_t targetQuality;
	uint8_t targetQualityLSB;
	uint16_t lookaheadDepth;
	int8_t cbQPIndexOffset;
	int8_t crQPIndexOffset;
	NV_ENC_LOOKAHEAD_LEVEL lookaheadLevel;
} NV_ENC_RC_PARAMS;

typedef struct {
	uint32_t level;
	uint32_t idrPeriod;
	NV_ENC_BFRAME_REF_MODE useBFramesAsRef;
	NV_ENC_TEMPORAL_FILTER_LEVEL tfLevel;
	uint32_t enableFillerDataInsertion : 1;
} NV_ENC_CONFIG_H264;

typedef struct {
	uint32_t level;
	uint32_t tier;
	uint32_t idrPeriod;
	NV_ENC_BFRAME_REF_MODE useBFramesAsRef;
	NV_ENC_TEMPORAL_FILTER_LEVEL tfLevel;
	uint32_t enableFillerDataInsertion : 1;
} NV_ENC_CONFIG_HEVC;

typedef struct {
	uint32_t level;
	uint32_t tier;
	uint32_t numTileColumns;
	uint32_t numTileRows;
	uint32_t idrPeriod;
	NV_ENC_BFRAME_REF_MODE useBFramesAsRef;
	NV_ENC_TEMPORAL_FILTER_LEVEL tfLevel;
	uint32_t enableBitstreamPadding : 1;
} NV_ENC_CONFIG_AV1;

typedef union {
	NV_ENC_CONFIG_H264 h264Config;
	NV_ENC_CONFIG_HEVC hevcConfig;
	NV_ENC_CONFIG_AV1 av1Config;
} NV_ENC_CODEC_CONFIG;

typedef struct {
	uint32_t gopLength;
	int32_t frameIntervalP;
	NV_ENC_RC_PARAMS rcParams;
	NV_ENC_CODEC_CONFIG encodeCodecConfig;
} NV_ENC_CONFIG;

typedef struct {
	uint32_t encodeWidth;
} NV_ENC_INITIALIZE_PARAMS;

typedef struct {
	uint32_t version;
} NV_ENCODE_API_FUNCTION_LIST;

typedef struct {
	uint16_t maxContentLightLevel, maxPicAverageLightLevel;
} CONTENT_LIGHT_LEVEL;
typedef struct {
	uint16_t displayPrimaryX[3];
} MASTERING_DISPLAY_INFO;
