/*
 * Conformance harness (area hwencoders, spec S-050): hand-written stand-in for the NVIDIA
 * Video Codec SDK header (ffnvcodec/nvEncodeAPI.h, declared as SDK 13.0). The real SDK is not
 * available. Identifier NAMES were derived from what the obs-nvenc C sources use; struct
 * LAYOUTS, enum VALUES and GUID VALUES are fabricated (only identity matters: the runner prints
 * symbolic names). The pre-12.2 field names (pixelBitDepthMinus8 ...) are not declared, so the
 * #ifndef NVENC_12_2_OR_LATER branches of the plugin are never compiled. Test tooling, not OBS.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NVENCAPI
#define NVENCAPI_MAJOR_VERSION 13
#define NVENCAPI_MINOR_VERSION 0
#define NVENCAPI_VERSION (NVENCAPI_MAJOR_VERSION | (NVENCAPI_MINOR_VERSION << 24))

typedef int NVENCSTATUS;
enum {
	NV_ENC_SUCCESS = 0,
	NV_ENC_ERR_NO_ENCODE_DEVICE,
	NV_ENC_ERR_UNSUPPORTED_DEVICE,
	NV_ENC_ERR_INVALID_ENCODERDEVICE,
	NV_ENC_ERR_INVALID_DEVICE,
	NV_ENC_ERR_DEVICE_NOT_EXIST,
	NV_ENC_ERR_INVALID_PTR,
	NV_ENC_ERR_INVALID_EVENT,
	NV_ENC_ERR_INVALID_PARAM,
	NV_ENC_ERR_INVALID_CALL,
	NV_ENC_ERR_OUT_OF_MEMORY,
	NV_ENC_ERR_ENCODER_NOT_INITIALIZED,
	NV_ENC_ERR_UNSUPPORTED_PARAM,
	NV_ENC_ERR_LOCK_BUSY,
	NV_ENC_ERR_NOT_ENOUGH_BUFFER,
	NV_ENC_ERR_INVALID_VERSION,
	NV_ENC_ERR_MAP_FAILED,
	NV_ENC_ERR_NEED_MORE_INPUT,
	NV_ENC_ERR_ENCODER_BUSY,
	NV_ENC_ERR_EVENT_NOT_REGISTERD,
	NV_ENC_ERR_GENERIC,
	NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY,
	NV_ENC_ERR_UNIMPLEMENTED,
	NV_ENC_ERR_RESOURCE_REGISTER_FAILED,
	NV_ENC_ERR_RESOURCE_NOT_REGISTERED,
	NV_ENC_ERR_RESOURCE_NOT_MAPPED,
	NV_ENC_ERR_NEED_MORE_OUTPUT
};

typedef void *NV_ENC_INPUT_PTR;
typedef void *NV_ENC_OUTPUT_PTR;
typedef void *NV_ENC_REGISTERED_PTR;
typedef struct {
	uint32_t Data1;
	uint16_t Data2, Data3;
	uint8_t Data4[8];
} GUID;

/* GUID values are fabricated: Data1 = a distinct number per name. */
#define HW_GUID(n) {(n), 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}}
static const GUID NV_ENC_CODEC_H264_GUID = HW_GUID(1);
static const GUID NV_ENC_CODEC_HEVC_GUID = HW_GUID(2);
static const GUID NV_ENC_CODEC_AV1_GUID = HW_GUID(3);
static const GUID NV_ENC_PRESET_P1_GUID = HW_GUID(11);
static const GUID NV_ENC_PRESET_P2_GUID = HW_GUID(12);
static const GUID NV_ENC_PRESET_P3_GUID = HW_GUID(13);
static const GUID NV_ENC_PRESET_P4_GUID = HW_GUID(14);
static const GUID NV_ENC_PRESET_P5_GUID = HW_GUID(15);
static const GUID NV_ENC_PRESET_P6_GUID = HW_GUID(16);
static const GUID NV_ENC_PRESET_P7_GUID = HW_GUID(17);
static const GUID NV_ENC_H264_PROFILE_BASELINE_GUID = HW_GUID(21);
static const GUID NV_ENC_H264_PROFILE_MAIN_GUID = HW_GUID(22);
static const GUID NV_ENC_H264_PROFILE_HIGH_GUID = HW_GUID(23);
static const GUID NV_ENC_H264_PROFILE_HIGH_10_GUID = HW_GUID(24);
static const GUID NV_ENC_H264_PROFILE_HIGH_444_GUID = HW_GUID(25);
static const GUID NV_ENC_HEVC_PROFILE_MAIN_GUID = HW_GUID(31);
static const GUID NV_ENC_HEVC_PROFILE_MAIN10_GUID = HW_GUID(32);
static const GUID NV_ENC_HEVC_PROFILE_FREXT_GUID = HW_GUID(33);
static const GUID NV_ENC_AV1_PROFILE_MAIN_GUID = HW_GUID(41);

typedef enum {
	NV_ENC_BUFFER_FORMAT_UNDEFINED = 0,
	NV_ENC_BUFFER_FORMAT_NV12 = 1,
	NV_ENC_BUFFER_FORMAT_YUV420_10BIT = 2,
	NV_ENC_BUFFER_FORMAT_YUV444 = 3
} NV_ENC_BUFFER_FORMAT;
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
typedef enum {
	NV_ENC_PARAMS_RC_CONSTQP = 0,
	NV_ENC_PARAMS_RC_VBR = 1,
	NV_ENC_PARAMS_RC_CBR = 2
} NV_ENC_PARAMS_RC_MODE;
typedef enum {
	NV_ENC_MULTI_PASS_DISABLED = 0,
	NV_ENC_TWO_PASS_QUARTER_RESOLUTION = 1,
	NV_ENC_TWO_PASS_FULL_RESOLUTION = 2
} NV_ENC_MULTI_PASS;
typedef enum {
	NV_ENC_TUNING_INFO_UNDEFINED = 0,
	NV_ENC_TUNING_INFO_HIGH_QUALITY = 1,
	NV_ENC_TUNING_INFO_LOW_LATENCY = 2,
	NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY = 3,
	NV_ENC_TUNING_INFO_LOSSLESS = 4,
	NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY = 5
} NV_ENC_TUNING_INFO;
typedef enum { NV_ENC_QP_MAP_DISABLED = 0, NV_ENC_QP_MAP_EMPHASIS = 1, NV_ENC_QP_MAP_DELTA = 2 } NV_ENC_QP_MAP_MODE;
typedef enum { NV_ENC_BIT_DEPTH_INVALID = 0, NV_ENC_BIT_DEPTH_8 = 8, NV_ENC_BIT_DEPTH_10 = 10 } NV_ENC_BIT_DEPTH;
typedef enum { NV_ENC_TIER_AV1_0 = 0, NV_ENC_TIER_AV1_1 = 1 } NV_ENC_TIER_AV1;
#define NV_ENC_LEVEL_AV1_AUTOSELECT 0
typedef enum { NV_ENC_PIC_STRUCT_FRAME = 1 } NV_ENC_PIC_STRUCT;
typedef enum {
	NV_ENC_PIC_TYPE_P = 0,
	NV_ENC_PIC_TYPE_B = 1,
	NV_ENC_PIC_TYPE_I = 2,
	NV_ENC_PIC_TYPE_IDR = 3,
	NV_ENC_PIC_TYPE_BI = 4,
	NV_ENC_PIC_TYPE_SKIPPED = 5,
	NV_ENC_PIC_TYPE_INTRA_REFRESH = 6,
	NV_ENC_PIC_TYPE_NONREF_P = 7,
	NV_ENC_PIC_TYPE_SWITCH = 8,
	NV_ENC_PIC_TYPE_UNKNOWN = 0xFF
} NV_ENC_PIC_TYPE;
#define NV_ENC_PIC_FLAG_EOS 8
typedef enum { NV_ENC_DEVICE_TYPE_DIRECTX = 0, NV_ENC_DEVICE_TYPE_CUDA = 1 } NV_ENC_DEVICE_TYPE;
typedef enum {
	NV_ENC_CAPS_SUPPORT_10BIT_ENCODE = 1,
	NV_ENC_CAPS_SUPPORT_YUV444_ENCODE,
	NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE,
	NV_ENC_CAPS_SUPPORT_LOOKAHEAD,
	NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ
} NV_ENC_CAPS;
typedef enum { NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY = 1 } NV_ENC_INPUT_RESOURCE_TYPE;

#define NV_ENC_CAPS_PARAM_VER 1
#define NV_ENC_CONFIG_VER 2
#define NV_ENC_PRESET_CONFIG_VER 3
#define NV_ENC_INITIALIZE_PARAMS_VER 4
#define NV_ENC_RECONFIGURE_PARAMS_VER 5
#define NV_ENC_PIC_PARAMS_VER 6
#define NV_ENC_LOCK_BITSTREAM_VER 7
#define NV_ENC_CREATE_BITSTREAM_BUFFER_VER 8
#define NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER 9
#define NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER 10
#define NV_ENCODE_API_FUNCTION_LIST_VER 11
#define NV_ENC_MAP_INPUT_RESOURCE_VER 12
#define NV_ENC_REGISTER_RESOURCE_VER 13

typedef struct {
	uint32_t qpInterP;
	uint32_t qpInterB;
	uint32_t qpIntra;
} NV_ENC_QP;

typedef struct {
	uint32_t version;
	NV_ENC_PARAMS_RC_MODE rateControlMode;
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
	NV_ENC_MULTI_PASS multiPass;
	NV_ENC_QP_MAP_MODE qpMapMode;
} NV_ENC_RC_PARAMS;

typedef struct {
	uint32_t videoSignalTypePresentFlag;
	uint32_t videoFullRangeFlag;
	uint32_t colourDescriptionPresentFlag;
	uint32_t colourPrimaries;
	uint32_t transferCharacteristics;
	uint32_t colourMatrix;
} NV_ENC_CONFIG_H264_VUI_PARAMETERS;

typedef struct {
	uint32_t videoSignalTypePresentFlag;
	uint32_t videoFullRangeFlag;
	uint32_t colourDescriptionPresentFlag;
	uint32_t colourPrimaries;
	uint32_t transferCharacteristics;
	uint32_t colourMatrix;
	uint32_t chromaSampleLocationFlag;
	uint32_t chromaSampleLocationTop;
	uint32_t chromaSampleLocationBot;
} NV_ENC_CONFIG_HEVC_VUI_PARAMETERS;

typedef struct {
	uint32_t level;
	uint32_t idrPeriod;
	NV_ENC_BFRAME_REF_MODE useBFramesAsRef;
	NV_ENC_TEMPORAL_FILTER_LEVEL tfLevel;
	uint32_t enableFillerDataInsertion : 1;
	uint32_t repeatSPSPPS : 1;
	uint32_t disableSPSPPS : 1;
	uint32_t outputAUD : 1;
	uint32_t outputBufferingPeriodSEI : 1;
	uint32_t outputPictureTimingSEI : 1;
	uint32_t qpPrimeYZeroTransformBypassFlag : 1;
	uint32_t sliceMode;
	uint32_t sliceModeData;
	uint32_t chromaFormatIDC;
	NV_ENC_BIT_DEPTH inputBitDepth;
	NV_ENC_BIT_DEPTH outputBitDepth;
	NV_ENC_CONFIG_H264_VUI_PARAMETERS h264VUIParameters;
} NV_ENC_CONFIG_H264;

typedef struct {
	uint32_t level;
	uint32_t tier;
	uint32_t idrPeriod;
	NV_ENC_BFRAME_REF_MODE useBFramesAsRef;
	NV_ENC_TEMPORAL_FILTER_LEVEL tfLevel;
	uint32_t enableFillerDataInsertion : 1;
	uint32_t repeatSPSPPS : 1;
	uint32_t disableSPSPPS : 1;
	uint32_t outputAUD : 1;
	uint32_t outputBufferingPeriodSEI : 1;
	uint32_t outputPictureTimingSEI : 1;
	uint32_t outputMasteringDisplay : 1;
	uint32_t outputMaxCll : 1;
	uint32_t sliceMode;
	uint32_t sliceModeData;
	uint32_t chromaFormatIDC;
	NV_ENC_BIT_DEPTH inputBitDepth;
	NV_ENC_BIT_DEPTH outputBitDepth;
	NV_ENC_CONFIG_HEVC_VUI_PARAMETERS hevcVUIParameters;
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
	uint32_t repeatSeqHdr : 1;
	uint32_t outputMasteringDisplay : 1;
	uint32_t outputMaxCll : 1;
	uint32_t colorRange;
	uint32_t colorPrimaries;
	uint32_t transferCharacteristics;
	uint32_t matrixCoefficients;
	uint32_t chromaFormatIDC;
	NV_ENC_BIT_DEPTH inputBitDepth;
	NV_ENC_BIT_DEPTH outputBitDepth;
	uint32_t numFwdRefs;
	uint32_t numBwdRefs;
} NV_ENC_CONFIG_AV1;

typedef union {
	NV_ENC_CONFIG_H264 h264Config;
	NV_ENC_CONFIG_HEVC hevcConfig;
	NV_ENC_CONFIG_AV1 av1Config;
} NV_ENC_CODEC_CONFIG;

typedef struct {
	uint32_t version;
	GUID profileGUID;
	uint32_t gopLength;
	int32_t frameIntervalP;
	NV_ENC_RC_PARAMS rcParams;
	NV_ENC_CODEC_CONFIG encodeCodecConfig;
} NV_ENC_CONFIG;

typedef struct {
	uint32_t version;
	NV_ENC_CONFIG presetCfg;
} NV_ENC_PRESET_CONFIG;

typedef struct {
	uint32_t version;
	GUID encodeGUID;
	GUID presetGUID;
	uint32_t encodeWidth;
	uint32_t encodeHeight;
	uint32_t darWidth;
	uint32_t darHeight;
	uint32_t frameRateNum;
	uint32_t frameRateDen;
	uint32_t enableEncodeAsync;
	uint32_t enablePTD;
	NV_ENC_CONFIG *encodeConfig;
	NV_ENC_TUNING_INFO tuningInfo;
	NV_ENC_SPLIT_ENCODE_MODE splitEncodeMode;
} NV_ENC_INITIALIZE_PARAMS;

typedef struct {
	uint32_t version;
	NV_ENC_INITIALIZE_PARAMS reInitEncodeParams;
	uint32_t resetEncoder : 1;
	uint32_t forceIDR : 1;
} NV_ENC_RECONFIGURE_PARAMS;

typedef struct {
	uint16_t x, y;
} NV_ENC_XY;
typedef struct {
	uint16_t maxContentLightLevel, maxPicAverageLightLevel;
} CONTENT_LIGHT_LEVEL;
typedef struct {
	struct {
		uint16_t x, y;
	} r, g, b, whitePoint;
	uint32_t maxLuma, minLuma;
} MASTERING_DISPLAY_INFO;

typedef struct {
	CONTENT_LIGHT_LEVEL *pMaxCll;
	MASTERING_DISPLAY_INFO *pMasteringDisplay;
} NV_ENC_PIC_PARAMS_AV1;
typedef struct {
	CONTENT_LIGHT_LEVEL *pMaxCll;
	MASTERING_DISPLAY_INFO *pMasteringDisplay;
} NV_ENC_PIC_PARAMS_HEVC;
typedef union {
	NV_ENC_PIC_PARAMS_AV1 av1PicParams;
	NV_ENC_PIC_PARAMS_HEVC hevcPicParams;
} NV_ENC_CODEC_PIC_PARAMS;

typedef struct {
	uint32_t version;
	uint32_t inputWidth, inputHeight, inputPitch;
	uint32_t encodePicFlags;
	uint32_t frameIdx;
	uint64_t inputTimeStamp;
	NV_ENC_INPUT_PTR inputBuffer;
	NV_ENC_OUTPUT_PTR outputBitstream;
	NV_ENC_BUFFER_FORMAT bufferFmt;
	NV_ENC_PIC_STRUCT pictureStruct;
	int8_t *qpDeltaMap;
	uint32_t qpDeltaMapSize;
	NV_ENC_CODEC_PIC_PARAMS codecPicParams;
} NV_ENC_PIC_PARAMS;

typedef struct {
	uint32_t version;
	uint32_t doNotWait : 1;
	NV_ENC_OUTPUT_PTR outputBitstream;
	void *bitstreamBufferPtr;
	uint32_t bitstreamSizeInBytes;
	uint64_t outputTimeStamp;
	NV_ENC_PIC_TYPE pictureType;
} NV_ENC_LOCK_BITSTREAM;

typedef struct {
	uint32_t version;
	NV_ENC_OUTPUT_PTR bitstreamBuffer;
} NV_ENC_CREATE_BITSTREAM_BUFFER;

typedef struct {
	uint32_t version;
	uint8_t *spsppsBuffer;
	uint32_t inBufferSize;
	uint32_t *outSPSPPSPayloadSize;
} NV_ENC_SEQUENCE_PARAM_PAYLOAD;

typedef struct {
	uint32_t version;
	NV_ENC_DEVICE_TYPE deviceType;
	void *device;
	uint32_t apiVersion;
} NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS;

typedef struct {
	uint32_t version;
	NV_ENC_CAPS capsToQuery;
} NV_ENC_CAPS_PARAM;

typedef struct {
	uint32_t version;
	NV_ENC_REGISTERED_PTR registeredResource;
	NV_ENC_BUFFER_FORMAT mappedBufferFmt;
	NV_ENC_INPUT_PTR mappedResource;
} NV_ENC_MAP_INPUT_RESOURCE;
typedef struct {
	uint32_t version;
	NV_ENC_REGISTERED_PTR registeredResource;
} NV_ENC_REGISTER_RESOURCE;

typedef struct {
	uint32_t version;
	NVENCSTATUS (*nvEncOpenEncodeSessionEx)(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS *, void **);
	NVENCSTATUS (*nvEncGetEncodeCaps)(void *, GUID, NV_ENC_CAPS_PARAM *, int *);
	NVENCSTATUS (*nvEncGetEncodePresetConfigEx)(void *, GUID, GUID, NV_ENC_TUNING_INFO, NV_ENC_PRESET_CONFIG *);
	NVENCSTATUS (*nvEncInitializeEncoder)(void *, NV_ENC_INITIALIZE_PARAMS *);
	NVENCSTATUS (*nvEncReconfigureEncoder)(void *, NV_ENC_RECONFIGURE_PARAMS *);
	NVENCSTATUS (*nvEncCreateBitstreamBuffer)(void *, NV_ENC_CREATE_BITSTREAM_BUFFER *);
	NVENCSTATUS (*nvEncDestroyBitstreamBuffer)(void *, NV_ENC_OUTPUT_PTR);
	NVENCSTATUS (*nvEncEncodePicture)(void *, NV_ENC_PIC_PARAMS *);
	NVENCSTATUS (*nvEncLockBitstream)(void *, NV_ENC_LOCK_BITSTREAM *);
	NVENCSTATUS (*nvEncUnlockBitstream)(void *, NV_ENC_OUTPUT_PTR);
	NVENCSTATUS (*nvEncGetSequenceParams)(void *, NV_ENC_SEQUENCE_PARAM_PAYLOAD *);
	NVENCSTATUS (*nvEncMapInputResource)(void *, NV_ENC_MAP_INPUT_RESOURCE *);
	NVENCSTATUS (*nvEncUnmapInputResource)(void *, NV_ENC_INPUT_PTR);
	NVENCSTATUS (*nvEncRegisterResource)(void *, NV_ENC_REGISTER_RESOURCE *);
	NVENCSTATUS (*nvEncUnregisterResource)(void *, NV_ENC_REGISTERED_PTR);
	NVENCSTATUS (*nvEncDestroyEncoder)(void *);
	const char *(*nvEncGetLastErrorString)(void *);
} NV_ENCODE_API_FUNCTION_LIST;
